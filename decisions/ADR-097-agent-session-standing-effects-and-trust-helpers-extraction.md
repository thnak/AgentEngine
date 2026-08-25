# ADR-097 — Which parts of `rt::AgentSession`'s ten responsibility clusters can be safely extracted into real collaborators, and which can't?

**Status:** Proposed — design and red-team phases complete (three independent rounds, 2026-08-25);
**implementation is done and proven** (2026-08-25, post-design): `StandingEffectRegistry`
(`include/agentengine/rt/standing_effect_registry.hpp`) and the I3 trust/streaming free functions
(`include/agentengine/rt/agent_session_trust.hpp`) are real code, wired into `AgentSession` in
place of the state and logic they absorbed. Full project rebuild (240/240 targets) and the full
`ctest` suite (252/252) green — zero regressions, zero test files modified. Still awaiting the
project owner's own `Judged` sign-off; this session cannot self-Judge a change to hot-path/
security-critical code per CLAUDE.md.

**Relates to:** `docs/planning/agent-session-decomposition-design-draft.md` (the full record — two
revisions, three red-team rounds, summarized not duplicated below); ADR-061 (session-mutex I1
discipline, the `schedule_wakeup()`/`schedule_wakeup_impl()` locked/unlocked naming split this ADR
preserves); ADR-053 (`schedule_wakeup`'s own original design); the `worktree.hpp` split (an
unrelated, already-landed, much lower-risk precedent for splitting an oversized header — cited in
the design draft only as a contrast, not as this change's own justification).

## 1. The question

**Stated so it has a wrong answer:** `include/agentengine/rt/agent_session.hpp` is the largest file
in the tree (3055 lines before this change), and `class AgentSession` alone spans ~2200 of them,
owning roughly ten distinct responsibility clusters behind one `session_mutex_`. Is that actually a
single god-class that a real OO decomposition can fix wholesale, or does a full read of the class
reveal that only *some* of those clusters are genuinely separable — and if so, which, and is the
rest of the "god class" complaint actually irreducible given what the code has to do?

## 2. What a full read of the real code found (not assumed from a line-count summary)

Almost every "big" method (`start_run`, `resolve_interaction`, `resolve_codeact_ask`,
`resolve_hook_decision`, `finish_hook_processed_round`, `run_rounds`) touches almost all of
`history_`, `effect_context_`, `open_interactions_`, `pending_codeact_asks_`,
`pending_hook_decisions_`, the four decider/hook members, and `emit_run_event()`. A red-team pass
traced the real call graph among these six methods and found it is a DAG, not the mutually
recursive structure an earlier revision of the design draft claimed — `run_rounds()` never calls
back into any of the five resolution methods, it only ever returns a sentinel error and resumption
happens via a brand-new externally initiated call. That correction matters: it means
`run_rounds()` *could* be extracted as a free function with no back-pointer needed. This ADR's own
implementation still declines to do that (§3 below) — not because it's structurally impossible, but
because the ~15-parameter surface such a free function would need is isomorphic to `AgentSession&`
in every way that matters for coupling, while `run_rounds()` is the single function with the most
red-team-found-bug history in this file (the `resolved_interaction_id` copy-not-reference/ASan
dangling-reference fix, the round-8 red-team's finding 16/17 leaks, the codeact-ask `max_turns_`
bypass). Threading fifteen-plus references through an explicit parameter list is exactly the kind
of mechanical change where a dropped or miscopied reference reintroduces one of those same bug
classes, for a benefit (independent testability of `run_rounds()` alone) this ADR judges isn't
worth that risk.

Three clusters, by contrast, genuinely are close to disjoint from that core and from each other:
standing effects/background tasks (Slice 3 + Slice 4 of the file's own banner), I3 trust
enforcement (`force_tainted`/`filter_cross_provider_reasoning`), and the streaming-response drain
loop. These are what this ADR extracts.

## 3. What changed

**`StandingEffectRegistry`** (new type, `include/agentengine/rt/standing_effect_registry.hpp`) now
owns `StandingEffect` storage, the mint-handle-id/count-by-kind/cancel/list/due operations, the
`schedule_wakeup_impl()` capability-enforcement logic (verbatim), and the background-completion
queue (`BackgroundCompletionQueue`/`BackgroundTaskDone`, also moved here). `AgentSession` holds one
as a plain member (`standing_effects_registry_`, replacing the three members it absorbed:
`standing_effects_`, `standing_effect_counter_`, `background_completions_`) and its six public
methods in this area (`start_background_task`, `list_standing_effects`, `cancel_standing_effect`,
`schedule_wakeup`, `due_standing_effects`, `drain_background_completions`/
`drain_background_completions_locked`) become thin wrappers: same locking (locked/unlocked split
per ADR-061 §20.5/§24.2, unchanged), same authority/capability resolution, delegate the bookkeeping,
emit whatever the original emitted, return. `fork_from()`/`clear_in_process_state()` now call
`standing_effects_registry_.reset()` in place of the two-line `clear()`/`= 0` pair they used to
do directly.

The registry deliberately knows nothing about `ToolTable`/`ApprovalDecider`/capability-checking
machinery — that stays in `AgentSession`, which is the only caller and the only place per-call
authority/capabilities get resolved (I2). It also has no locking of its own opinion; `AgentSession`'s
`session_mutex_` is still what serializes every access, exactly as before.

The registry's unlocked method is named `schedule_wakeup_impl()`, not a bare `schedule_wakeup` —
deliberately, so it doesn't collide with `AgentSession`'s own public, *locked* `schedule_wakeup()`
wrapper and throw away the exact naming signal ADR-061 §20.5 introduced the `_impl` split to give a
future maintainer editing the `ScheduleWakeupTool` dispatch closure inside `run_rounds()`.

`drain_ready()` (the registry's replacement for `drain_background_completions_locked()`'s own
inline erase+emit loop) erases every matched completion first, then returns what to emit;
`AgentSession`'s wrapper iterates the result and emits, still inside the same locked function scope
the caller already holds `session_mutex_` through. This is an honest, small, disclosed behavioral
change from the original interleaved erase-then-emit-per-item shape: two independent red-team
lenses (correctness and security) each found it independently, which is why it's called out
explicitly rather than left as an internal implementation detail. It widens (from effectively zero
to up to N-1 iterations in an N-completion batch) the window in which an already-documented,
already-accepted unlocked-reader race (`list_standing_effects()`/`due_standing_effects()`/
`cancel_standing_effect()` racing a locked mutator — file banner, `agent_session.hpp:125-137`, a
named, pre-existing-in-kind precondition, not a new one) could observe an effect already erased
whose `ToolCallFinished` event hasn't fired yet. Judged acceptable: it's a bounded widening of an
*already-accepted* race class, not a new hazard category, and the alternative (emitting from inside
the registry, which would require giving the registry an `emit` capability and widening its
interface back toward "knows about session-level concerns") was rejected as a worse trade.

**I3 trust helpers and the streaming drain** (`include/agentengine/rt/agent_session_trust.hpp`, new
free functions in `agentengine::rt::detail`): `force_tainted()` (already stateless, moved verbatim),
`filter_cross_provider_reasoning()`, and `drain_streaming_response()` now take an `EmitFn`
(`std::function<void(run_event_kind, RunEventPayload)>`) instead of calling
`AgentSession::emit_run_event()` directly. `AgentSession`'s own `emit_run_event()`/
`emit_run_event_for()` and their backing state (`run_event_producer_`/`run_event_seq_by_run_`) are
unchanged and unmoved — every one of the six call sites across the class constructs a thin
`[this](k, p){ emit_run_event(k, std::move(p)); }` closure at the point of use. This is a real, if
modest, win specifically for I3 auditability: `force_tainted`'s recursive-taint behavior and
`filter_cross_provider_reasoning`'s provenance-matching logic had zero direct test coverage before
this change (only indirect coverage through `run_rounds()`/`resolve_interaction()` integration
tests) — extracting them to free functions makes writing that coverage straightforward without
needing a whole `AgentSession` fixture. (Dedicated unit tests for these three functions are a named
follow-up, not built in this pass — see §5.)

## 4. What does NOT stay, named explicitly, matching this file's own "residuals named, not silently
assumed complete" convention

Everything else stays exactly where it was: the configuration/setter surface, the two entry points,
lifecycle bookkeeping, `run_model_call()`, `apply_dispatch_authority()`, and the entire
interaction/hook-resolution/`run_rounds()` cluster (§2 above explains why, in more depth than a
one-line "too coupled" dismissal). `AgentSession` is roughly 170 lines smaller
(3055 → 2884) and three member variables lighter; the real win is not the line count (modest — this
was never a line-count exercise, see the design draft's own framing) but that the standing-effects
cluster is now an independently-instantiable, independently-testable type with a narrow interface,
and the I3-relevant trust logic is no longer only reachable through a full `AgentSession` fixture.

## 5. Verification performed

- Full project rebuild from the modified state: 240/240 targets, zero errors, only the pre-existing
  unrelated `skill_provider.hpp`/`tool_optimizer_provider.hpp` C4458 warnings (present before this
  change, unrelated to it).
- Full `ctest` suite: 252/252 passed, including every `AgentSession`-touching test
  (`test_rt_agent_session_schedule_wakeup`, `test_rt_agent_session_tool_call_hook`,
  `test_rt_agent_spawn*`, `test_rt_agent_session_streaming_and_events`,
  `test_rt_agent_session_suspend_approval`, `test_agent_session_tool_call_progress`, and every other
  `test_rt_agent_session_*`/`test_agent_session_*` file) with **zero test files modified** — every
  public method signature and observable behavior this ADR touches is unchanged from the caller's
  perspective, confirmed rather than merely claimed.
- Three independent red-team passes against the design draft before implementation (correctness/
  lifetime, security/invariant, scope/overclaim lenses), each reading the whole real
  `agent_session.hpp` directly rather than trusting the draft's paraphrase. No fatal defects found.
  One MAJOR correction to the draft's own central argument (the mutual-recursion claim, §2 above);
  one MAJOR issue found independently by two separate lenses (the `drain_ready()` visibility-window
  widening, §3 above); several smaller refinements (the guard-lifetime requirement across
  `drain_ready()` + its emit loop, the `schedule_wakeup_impl` naming to avoid colliding with the
  public locked wrapper, the `now`-must-stay-caller-supplied pin for I5, an explicit statement that
  `emit_run_event()`/`emit_run_event_for()` stay members, and the grounding fact that `AgentSession`
  is structurally immovable — `rt::AsyncMutex`'s deleted copy ctor with no declared move ctor
  suppresses every implicit move member on `AgentSession`, so a registry held as a plain member
  subobject introduces no new lifetime risk versus the state it absorbed). All folded into
  Revision 2 of the design draft before implementation began.

## 6. Residuals, named not silently assumed complete

- No dedicated unit tests for `force_tainted()`/`filter_cross_provider_reasoning()`/
  `drain_streaming_response()` were written in this pass — they remain covered only indirectly
  through existing `AgentSession` integration tests. Extracting them as free functions makes writing
  direct coverage straightforward; doing so is a real, tracked follow-up, not done here.
- No dedicated test asserts `drain_ready()`'s event-emission *ordering* survives the split (the
  design draft's own §3 item 3 names this — today's `drain_background_completions_locked()` had no
  such test before this change either, so this is a pre-existing coverage gap this ADR does not
  close, not a regression it introduces).
- `run_rounds()` and the four resolution methods remain unextracted, for the reasons in §2 — a
  future session with a concrete reason to want `run_rounds()` independently testable can revisit
  that call with its own red-team pass; this ADR does not recommend for or against that in advance.
- This ADR does not attempt any further split of `AgentSession`'s remaining ~2000-line body. The
  configuration surface, entry points, and interaction/hook/round-loop cluster are named residuals,
  not silently claimed fixed.
