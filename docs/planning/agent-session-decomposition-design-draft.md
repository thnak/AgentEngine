# Design draft: decomposing `rt::AgentSession`'s responsibility clusters

**Status:** Design draft, Revision 2. Not an ADR yet — this is the design phase of the
`design → red-team → prove → judge` gate CLAUDE.md requires before touching hot-path/security-
critical code (`AgentSession` owns I1's session-serialization guard, I2's per-request capability
wiring, and I3's trust-enforcement points). No implementation has happened yet; this draft is what
gets red-teamed first.

## Revision 2 — three independent red-team passes, findings folded in

Three agents red-teamed Revision 1 in parallel, each reading the whole real `agent_session.hpp`
directly rather than trusting this draft's paraphrase — a correctness/lifetime lens, a
security/invariant lens, and a scope/overclaim lens. No fatal defects; one MAJOR correction to the
draft's own central argument, and several real refinements. All folded in below rather than left as
a separate addendum, so this draft stays the single source of truth.

**MAJOR, changes §1's own reasoning:** the "mutually recursive, therefore unsplittable" framing
overstated the case. The scope/overclaim reviewer traced every call site directly: `run_rounds()`
itself never calls back into `resolve_codeact_ask`/`resolve_hook_decision`/
`finish_hook_processed_round`/`resolve_interaction`/`start_run` — it only ever *returns* a sentinel
error (`kSuspendedForApproval`/`kSuspendedForHookDecision`/`kSuspendedForCodeActAsk`); resumption
happens later via a brand-new, externally-initiated `resolve_interaction()` call, not an in-process
callback. The real call graph is a DAG (`resolve_interaction → {resolve_codeact_ask,
resolve_hook_decision, finish_hook_processed_round, run_rounds}`; `resolve_hook_decision →
finish_hook_processed_round → run_rounds` — also correcting a factual slip in Revision 1, which
said `resolve_hook_decision` tail-calls `run_rounds()` directly; it tail-calls
`finish_hook_processed_round()`, which then calls `run_rounds()`). That means `run_rounds()`
*could* be extracted as a free function taking explicit state parameters, called by the five
methods that stay as members, with no back-pointer needed. §1 below now states this precisely
instead of overclaiming impossibility, and explains why this draft still declines to pursue it —
a considered rejection, not an overlooked option.

**MAJOR (independently found by two separate lenses — strong signal):** the correctness/lifetime
and security/invariant reviewers both independently flagged the same real gap in §2a's
`drain_ready()` proposal: today, `drain_background_completions_locked()` erases one completed
effect from `standing_effects_` and emits its event *immediately*, interleaved, one at a time.
Revision 1's proposal collects ALL drained completions inside the registry call first, erasing
every matching entry from `effects_`, and only afterward has `AgentSession` iterate the returned
vector and emit. Final event *order* is preserved, but Revision 1's claim of "zero behavioral
difference beyond that" was wrong: this widens (from effectively zero to up to N-1 iterations) the
window in which a concurrent *unlocked* reader (`list_standing_effects()`/`due_standing_effects()`/
`cancel_standing_effect()` — already documented as racing `standing_effects_` directly, file
banner `agent_session.hpp:125-137`) can observe an effect already erased from the list whose
`ToolCallFinished` event hasn't fired yet. Folded into §3 below as a named, bounded widening of an
*already-accepted* race class, not a new hazard — but stated honestly, not asserted away.

**Also folded in:** an explicit guard-lifetime requirement (the `AsyncMutex::Guard` must stay live
across `drain_ready()` *and* the subsequent emit loop, in the same function scope — a refactor slip
that factors the emit loop out from under the guard would reopen a real I1 window); a rename for
the registry's unlocked `schedule_wakeup` method (Revision 1 gave it the same bare name as
`AgentSession`'s own public, *locked* `schedule_wakeup()` wrapper — exactly the confusion ADR-061
§20.5 already split `schedule_wakeup()`/`schedule_wakeup_impl()` to prevent once); an explicit pin
that `now` must stay caller-supplied on the registry's method (I5); an explicit §2c bullet that
`emit_run_event()`/`emit_run_event_for()` (and their backing `run_event_producer_`/
`run_event_seq_by_run_`) stay `AgentSession` members — Revision 1 rerouted three call sites through
an `EmitFn` callback but never said what the callback's target does or where it lives; and a citation
for *why* moving state into a member collaborator adds no lifetime risk: `AgentSession` is
structurally immovable (`rt::AsyncMutex`'s deleted copy ctor with no declared move ctor,
`async_mutex.hpp:108-110`, suppresses every implicit move member on `AgentSession`) — already an
established, previously-discovered fact in this codebase (`session_builder.hpp:67-79`, on why
`QuickstartSessionBuilder` heap-owns its session via `unique_ptr` instead of storing it by value),
not a new claim this draft needed to prove from scratch.

## 0. Why this exists

`include/agentengine/rt/agent_session.hpp` is 3055 lines, the largest file in the tree by a wide
margin, and `class AgentSession` alone spans ~2200 of them (`agent_session.hpp:572`–`2769`). A
prior code-smell pass mapped it into roughly ten responsibility clusters (configuration surface,
the two entry points, lifecycle bookkeeping, background-task draining, run-event emission, I3 trust
enforcement, the model-invocation wrapper, the interaction/hook resolution cascade, and
`run_rounds()` itself) sharing one `session_mutex_`-guarded object. The user asked for a real
decomposition — actual collaborators, not just moving text around a file boundary the way the
`worktree.hpp` split (an unrelated, already-landed, lower-risk change) did.

## 1. What a full read of the class actually shows

Re-reading `agent_session.hpp:572`–`2769` in full (not just signatures) to design against real
code, not the earlier line-count summary, surfaces a structural fact that changes what "split"
should mean here: **almost every "big" method touches almost all of the class's state.**
Concretely:

- `start_run()`, `resolve_interaction()`, `resolve_codeact_ask()`, `resolve_hook_decision()`,
  `finish_hook_processed_round()`, and `run_rounds()` all read/write `history_`,
  `effect_context_`, `chat_client_` (via `history_provider_.on_context()`/`run_model_call()`),
  `open_interactions_` (via `open_interaction()`/`resolve_interaction_record()`),
  `approval_decider_`/`policy_decider_`/`tool_call_hook_`/`turn_middleware_hook_`, and
  `emit_run_event()`.
- **Corrected in Revision 2** (Revision 1 overclaimed this; a red-team pass traced every call site
  directly and found the real shape is weaker but the conclusion still holds, for a different
  reason). `resolve_codeact_ask()` and `finish_hook_processed_round()` do `co_return co_await
  run_rounds()` at their tail; `resolve_hook_decision()` does not call `run_rounds()` directly — it
  tail-calls `finish_hook_processed_round()`, which then calls `run_rounds()`. And critically,
  `run_rounds()` itself never calls back into any of the five resolution methods — it only ever
  *returns* one of three sentinel errors (`kSuspendedForApproval`/`kSuspendedForHookDecision`/
  `kSuspendedForCodeActAsk`) to suspend; resumption happens later via a brand-new, externally
  initiated `resolve_interaction()` call, not an in-process callback. The real call graph is a DAG
  (`resolve_interaction → {resolve_codeact_ask, resolve_hook_decision, finish_hook_processed_round,
  run_rounds}`; `resolve_hook_decision → finish_hook_processed_round → run_rounds`), not mutual
  recursion in the call-graph sense — so `run_rounds()` genuinely *could* be extracted as a free
  function taking `history_`/`effect_context_`/the deciders/etc. as explicit reference parameters,
  called by the five methods that stay as members, with no back-pointer needed at all.

  This draft still declines to do that extraction, for a reason narrower than "impossible":
  `run_rounds()` touches on the order of fifteen distinct pieces of session state (`effect_context_`,
  `history_`, `chat_client_`, `tool_call_hook_`, `turn_middleware_hook_`, `approval_decider_`,
  `policy_decider_`, `static_instructions_`, `output_schema_json_`/`_strategy_`/`_validate_`,
  `token_budget_`/`run_tokens_consumed_`, `max_turns_`, the standing-effects registry for its
  `ScheduleWakeupTool` closure, `open_interaction()`/`pending_codeact_asks_`/
  `pending_hook_decisions_` for suspending, and `emit_run_event()`). A free-function signature
  carrying that many reference parameters (or an equally large parameter-bundling struct built
  solely to avoid a long parameter list) is isomorphic to `AgentSession&` in every way that matters
  for coupling — it changes the syntax, not the actual dependency graph — while adding real risk:
  `run_rounds()` is the single function with the most red-team-found-bug history in this file (the
  `resolved_interaction_id` copy-not-reference/ASan dangling-reference fix at
  `agent_session.hpp:1011`–`1017`, the round-8 red-team's `finding 16`/`17` leaks, the codeact-ask
  `max_turns_` bypass fixed in `resolve_codeact_ask()`'s own comment). Threading fifteen-plus
  pieces of state through an explicit parameter list is exactly the kind of mechanical change where
  a single dropped-or-miscopied reference reintroduces one of those same bug classes, for a benefit
  (independent unit-testability of `run_rounds()` in isolation from `AgentSession` construction)
  this draft judges not worth that risk given the function's history. Named here as a **considered
  and declined** option, not an overlooked one — a future session with a concrete reason to want
  `run_rounds()` independently testable can revisit this call with its own red-team pass.

  The other four resolution methods (`start_run`, `resolve_interaction`, `resolve_codeact_ask`,
  `resolve_hook_decision`, `finish_hook_processed_round`) genuinely do all need direct access to
  `history_`, `open_interactions_`, `pending_codeact_asks_`, `pending_hook_decisions_`, and each
  other — extracting any one of them alone would still need most of the same state, for the same
  reason. **Conclusion (narrower than Revision 1's): this draft splits neither `run_rounds()` nor
  the four resolution methods out of `AgentSession`. `run_rounds()`'s extraction is technically
  possible but declined on risk/reward; the four resolution methods have no clean cut at all.**

- By contrast, three clusters genuinely are close to disjoint from the above and from each other:

  1. **Standing effects / background tasks** (Slice 3 + Slice 4 of the file's own banner):
     `standing_effects_`, `standing_effect_counter_`, `background_completions_`
     (`BackgroundCompletionQueue`), and the methods `start_background_task()`,
     `list_standing_effects()`, `cancel_standing_effect()`, `schedule_wakeup()`/
     `schedule_wakeup_impl()`, `due_standing_effects()`, `drain_background_completions()`/
     `drain_background_completions_locked()`. None of these read `history_`,
     `open_interactions_`, `pending_codeact_asks_`, or `pending_hook_decisions_`. They need
     `session_id_` (for handle-id minting), and per-call `effect_context_.run_id`/`.principal`/
     `.capabilities` passed in as parameters (already the pattern `schedule_wakeup_impl()` uses).
     `run_rounds()`'s only touchpoint is the dynamically-injected `ScheduleWakeupTool` closure,
     which already calls `schedule_wakeup_impl()` with explicit parameters, not member access.

  2. **I3 trust-enforcement helpers**: `force_tainted()` (already `static`, no member state at
     all) and `filter_cross_provider_reasoning()` (needs only `emit_run_event()` capability, no
     other member state).

  3. **Streaming response drain**: `drain_streaming_response()` (needs `stream_model_calls_` and
     `emit_run_event()`).

## 2. Proposed decomposition

Only extract what §1 shows is safe. Everything else in `AgentSession` (configuration surface,
entry points, bookkeeping, the interaction/hook/round-loop cluster, `run_model_call()`) **stays
exactly where it is** — named explicitly as a residual, not silently dropped, matching this
project's own "residuals named, not silently assumed complete" convention (this file's own banner
uses that phrase about itself).

### 2a. `StandingEffectRegistry` (new type, real collaborator with real ownership)

A new class, `agentengine::rt::StandingEffectRegistry` (new header
`include/agentengine/rt/standing_effect_registry.hpp`), owning:
- `std::vector<agentengine::StandingEffect> effects_` (was `standing_effects_`)
- `std::uint64_t counter_` (was `standing_effect_counter_`)
- `std::shared_ptr<BackgroundCompletionQueue> completions_` (was `background_completions_` —
  `BackgroundCompletionQueue`/`BackgroundTaskDone` move into this new header too, since nothing
  outside this cluster references them by name except `start_background_task()`'s closure, which
  moves with it)

Methods (all taking the specific pieces of caller state they need as parameters, never reaching
back into `AgentSession`):
- `mint_handle_id(std::string const& session_id) -> std::string`
- `count_of(standing_effect_kind) const -> std::size_t`
- `record_background_task(StandingEffect) -> void` / a combined
  `submit_background_task(table, held, request, ctx, approve, on_complete_closure_factory)`
  wrapper mirroring today's `start_background_task()` body, OR (simpler, less risk) keep the
  `background_task()` call and closure-construction in `AgentSession::start_background_task()`
  exactly as today, and have that method call `registry_.record(effect)` /
  `registry_.completion_queue()` (exposed so the closure can still capture a `weak_ptr` to it) —
  **this draft picks the second, narrower shape**: `StandingEffectRegistry` owns the data and the
  small pure operations on it (mint id, count-by-kind, add, erase-by-handle-with-ownership-check,
  drain-ready-completions-against-effects), `AgentSession::start_background_task()` keeps doing
  its own capability/authority checks and constructing the `background_task()` closure, then calls
  into the registry for the bookkeeping half. This avoids widening `StandingEffectRegistry`'s
  interface to know about `ToolTable`/`ApprovalDecider`/capability checking at all — it becomes a
  small, densely testable value-ish type instead of a second "everything" object.
- `schedule_wakeup_impl(delay, label, now, held, principal, run_id) -> result<StandingEffect>`
  (moves `schedule_wakeup_impl()`'s body verbatim — it already takes everything as parameters).
  **Corrected in Revision 2**: keep the `_impl` suffix rather than the bare `schedule_wakeup` name
  Revision 1 gave it — `AgentSession` still exposes a public, *locked* `schedule_wakeup()` wrapper
  (§2a below), and giving the registry's unlocked method the identical bare name would throw away
  exactly the naming signal ADR-061 §20.5 introduced the `_impl` split to provide in the first
  place (a future maintainer editing the `ScheduleWakeupTool` closure in `run_rounds()` must be
  able to tell at a glance which one is safe to call from inside an already-held lock). `now` stays
  a required parameter here, never read internally from `steady_clock::now()` — load-bearing for
  I5 (file banner, `agent_session.hpp:174-179`); this is a hard constraint on the implementation,
  not just today's signature shape.
- `due(now) const -> std::vector<StandingEffect>` (moves `due_standing_effects()`'s body verbatim)
- `cancel(handle_id, caller_principal) -> result<void>` (moves `cancel_standing_effect()`'s body
  verbatim)
- `list() const -> std::vector<StandingEffect> const&` (moves `list_standing_effects()`'s body)
- `struct DrainedCompletion { std::string owner_run_id; std::string call_id; ToolResult result; };`
  `drain_ready() -> std::vector<DrainedCompletion>` — moves `drain_background_completions_locked()`'s
  body, EXCEPT the `emit_run_event_for(...)` call: that stays in `AgentSession`, which now iterates
  the returned vector and emits for each entry. **Revision 1 claimed this was "zero behavioral
  difference" beyond emission order — a red-team pass (independently, via two separate lenses)
  found that overstated: see §3 item 3 below for the honest version of this claim.**
- `void reset()` — clears `effects_`/`counter_` back to empty/0, used by `fork_from()` and
  `clear_in_process_state()` (today's `standing_effects_.clear(); standing_effect_counter_ = 0;`
  pairs, both call sites). Deliberately does **not** reset `completions_` (the shared_ptr) — matches
  `clear_in_process_state()`'s own existing comment on why `background_completions_` is left alone
  (a worker thread may hold a `weak_ptr` to it; a stale completion for a since-cleared effect is
  already a harmless no-op via `drain_ready()`'s own `find_if` miss).

`AgentSession` keeps a `StandingEffectRegistry standing_effects_registry_` member (replacing the
three members it absorbs) and every one of the six public methods in this cluster becomes a thin
wrapper: lock (or don't, matching today's exact locked/unlocked split — `start_background_task()`
and `schedule_wakeup()` lock, `cancel_standing_effect()`/`list_standing_effects()`/
`due_standing_effects()` stay unlocked, exactly as ADR-061 §20.5/§24.2 left them), do whatever
per-call authority/capability resolution today's body does, delegate the actual bookkeeping to
`standing_effects_registry_`, emit whatever events the original emitted, return.

**Net effect on `AgentSession`:** removes ~150 lines of bookkeeping logic and 3 member variables;
the six public method *signatures* and their locking/authority/event-emission behavior are
unchanged (verified by keeping the same tests passing unmodified — no test should need to change,
since nothing about the public API moves).

### 2b. Trust-enforcement and streaming-drain as free functions

New header `include/agentengine/rt/agent_session_trust.hpp` (or a `detail` sub-namespace inline in
`agent_session.hpp` if a red-teamer finds a real reason a separate header is worse — named as an
open choice, not a foregone conclusion):

- `void force_tainted(ContentItem&)` — moves verbatim, already stateless.
- `void filter_cross_provider_reasoning(ContextContribution&, std::string const& current_chat_client_id, EmitFn const& emit)`
  where `EmitFn` is `std::function<void(run_event_kind, RunEventPayload)>` or a template parameter
  — takes an emit callback instead of calling `this->emit_run_event()` directly. `AgentSession`'s
  call site becomes `detail::filter_cross_provider_reasoning(*contribution, chat_client_->producer_chat_client_id(), [this](auto k, auto p){ emit_run_event(k, std::move(p)); })`.
- `result<ChatResponse> drain_streaming_response(stream<ChatResponseUpdate>, bool stream_model_calls, EmitFn const& emit)`
  — same callback shape.

**Net effect:** these three functions (~140 lines combined) leave the class body entirely and
become independently unit-testable (today none of them has a dedicated test — they're only
exercised indirectly through `run_rounds()`/`resolve_interaction()` integration tests). This is a
real, if modest, win: `force_tainted`'s recursive-taint behavior and
`filter_cross_provider_reasoning`'s provenance-matching logic are both I3-relevant and currently
have zero direct test coverage; extracting them to free functions makes writing that coverage
straightforward without needing a whole `AgentSession` fixture.

### 2c. What stays, named explicitly

Per §1's finding, the following do **not** move, and this draft does not propose a follow-up that
moves them either without a lot more design work this draft is not attempting:

- Configuration/setter surface (`initialize`, every `set_*`/accessor pair) — stays; it's the
  class's actual public contract, not the smell.
- `start_run()`, `resolve_interaction()`, `resolve_codeact_ask()`, `resolve_hook_decision()`,
  `finish_hook_processed_round()`, `run_rounds()` — stay, for the mutual-recursion reason in §1.
- `run_model_call()` — stays; it's the one clean per-turn seam but pulling it out would need
  `chat_client_`, `scan_response_format_leaks_`, and `emit_run_event` threaded through, and its
  only caller is `run_rounds()`, which isn't moving — no benefit to relocating it alone.
- `apply_dispatch_authority()` — stays; central to I2, called from four of the six entry-point-ish
  methods, tiny (~25 lines), not a size problem.
- Lifecycle bookkeeping (`fork_from`, `clear_in_process_state`, `to_record`/`restore_from_record`,
  `snapshot_record`, `clear_in_process_state_locked`) — stays; each of these now needs to also
  call `standing_effects_registry_.reset()` where it previously did the two-line
  `standing_effects_.clear(); standing_effect_counter_ = 0;` — a **mechanical** substitution, not a
  design change, but real code that changes and must be verified (§3).
- **Added in Revision 2** (a real gap a red-team pass found: §2b reroutes three call sites through
  an `EmitFn` callback but Revision 1 never said where the callback's *target* lives): `emit_run_event()`/
  `emit_run_event_for()` and their backing state (`run_event_producer_`, `run_event_seq_by_run_`)
  stay `AgentSession` members, unchanged. The `EmitFn` callbacks §2b's free functions take are thin
  closures (`[this](k, p){ emit_run_event(k, std::move(p)); }`) constructed at each of the three
  call sites — nothing about event emission itself moves.

## 3. What must be verified before/while implementing (for the red-team / prove phase)

Named up front, not discovered mid-implementation:

1. **`fork_from()` and `clear_in_process_state()` both currently reset `standing_effects_`/
   `standing_effect_counter_` but leave `background_completions_` alone** — `StandingEffectRegistry::reset()`
   must reproduce exactly that asymmetry (reset the vector+counter, never reallocate/clear the
   queue's shared_ptr identity), or a worker thread's already-captured `weak_ptr` silently starts
   pointing at a queue nothing drains from anymore.
2. **Locking asymmetry must be preserved exactly per-method**, not "mostly": `start_background_task()`
   and `schedule_wakeup()` acquire `session_mutex_`; `cancel_standing_effect()`,
   `list_standing_effects()`, `due_standing_effects()` do not (ADR-061 §20.5/§24.2's own deliberate,
   documented choice). Moving the *data* into a registry must not accidentally change which
   `AgentSession` wrapper methods lock — the registry itself has no locking of its own opinion; it's
   plain data+logic, exactly as `standing_effects_`/`standing_effect_counter_` are today.
3. **`drain_ready()`'s honest behavioral delta (Revision 2 — corrects Revision 1's overstated "zero
   difference" claim, found independently by two red-team lenses).** Today,
   `drain_background_completions_locked()` erases one completed effect from `standing_effects_`
   and calls `emit_run_event_for(...)` immediately, interleaved, one entry at a time. The
   `drain_ready()` split erases every matching entry from `effects_` first (inside one registry
   call), then `AgentSession` emits for the whole returned vector afterward — so event *order* is
   preserved (`std::vector` preserves `drain_ready()`'s append order, which is
   `background_completions_->pending`'s own FIFO order, unchanged) but the window in which a
   *concurrent, unlocked* reader (`list_standing_effects()`/`due_standing_effects()`/
   `cancel_standing_effect()` — already documented, file banner `agent_session.hpp:125-137`, as
   racing `standing_effects_` directly, a pre-existing accepted hazard) can observe an effect
   already erased with its `ToolCallFinished` event not yet on the stream widens from effectively
   zero to up to N-1 iterations for a batch of N. **Judgment call, stated explicitly rather than
   hidden in an "unchanged" claim**: this is a bounded widening of an *already-accepted* race class
   (the file banner already says a host racing these unlocked accessors against a locked mutator is
   a real, named, pre-existing-in-kind precondition, not a new one), not a new hazard category —
   but it must be named in the eventual ADR as a real, small, honestly-disclosed behavioral change,
   not asserted away. Write a test that submits ≥2 background tasks whose completions land in a
   known order and asserts `run_event`s fire in that same order after the refactor (today's
   `drain_background_completions_locked()` has no dedicated ordering test either, per a repo grep —
   a coverage gap either way, worth closing regardless of which way the refactor goes).
4. **Guard-lifetime requirement (Revision 2 — a red-team pass found this was implicit, not
   stated).** Today, `drain_background_completions_locked()` mutates `standing_effects_` and emits
   in the SAME function body, so the whole drain+emit sequence runs inside one
   `AsyncMutex::Guard`'s lifetime. After the split, `AgentSession`'s wrapper (whichever of
   `start_run()`/`resolve_interaction()`/`drain_background_completions()` calls
   `standing_effects_registry_.drain_ready()`) must keep the `AsyncMutex::Guard` alive across BOTH
   the `drain_ready()` call AND the subsequent emit loop, in the same function scope — not merely
   "call the registry, then emit from wherever." Factoring the emit loop into a separate helper
   called after the guard's scope ends (an easy, plausible-looking refactor during implementation)
   would reopen a genuine new I1 window. Call this out as a code-review checklist item, not just a
   test to write — a test proves the happy path; this is a structural property to visually confirm
   at every call site.
5. **`ScheduleWakeupTool`'s dispatch closure** (`run_rounds()`, `agent_session.hpp:2216`–`2231`)
   captures `this` and calls `schedule_wakeup_impl()` directly to avoid a self-deadlock on the
   non-reentrant `session_mutex_`. After the move, it must call
   `standing_effects_registry_.schedule_wakeup_impl(...)` instead — same non-reentrancy reasoning
   applies identically (the registry has no lock of its own to deadlock on), but the call site
   changes and needs the existing `test_rt_agent_spawn*`/scheduler tests to still pass unmodified.
6. **`std::weak_ptr<BackgroundCompletionQueue>` capture in `start_background_task()`'s closure**
   must keep capturing a weak_ptr to the SAME queue instance the registry owns — verify the
   registry exposes the shared_ptr (or a method that hands back what's needed to construct the
   weak_ptr) without giving the closure a way to keep the registry itself alive by accident (the
   whole point of the weak_ptr design is "session-gone => silently drop"; a registry-owning
   shared_ptr accidentally captured strong would reintroduce a lifetime bug this file already
   solved once). **Grounding added in Revision 2** (a red-team pass established this rather than
   leaving it an open question): `AgentSession` is structurally immovable —
   `rt::AsyncMutex`'s deleted copy constructor with no declared move constructor
   (`async_mutex.hpp:108-110`) suppresses every implicit move member on `AgentSession`, and every
   real call site constructs one either heap-owned via `unique_ptr` (already established for this
   exact reason in `session_builder.hpp:67-79`, on why `QuickstartSessionBuilder` couldn't store its
   session by value) or in-place, never relocated. So `standing_effects_registry_` as a plain member
   subobject introduces no *new* lifetime risk versus today's members — it is pinned for
   `AgentSession`'s whole life exactly as `standing_effects_`/`background_completions_` already are.

## 4. Not yet an ADR

This stays a design draft until it's been red-teamed and the findings are folded back in (matching
`session-sandbox-lifecycle-wiring-design-draft.md`'s own process, cited above as the template).
Once stable, the actual ADR goes in `decisions/ADR-097-*.md`, Proposed status, awaiting the
project owner's own Judged sign-off — this session cannot self-Judge a change to hot-path/
security-critical code per CLAUDE.md.
