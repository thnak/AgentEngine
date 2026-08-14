# Design draft: resolving the external-process-hooks gap's six open questions

**Status:** Design draft, red-teamed once — **not an ADR, no code written.** Seeds a future ADR once a
milestone claims this work; per the standing project-owner posture on the two sibling gap docs
(`agent-as-workflow-executor-gap.md`, `batch-inference-coalescing-gap.md`): document, do not implement
yet, pending explicit direction. Companion docs: `external-process-hooks-gap.md` (the six original open
questions), `docs/research/2026-08-14-claude-code-hooks-mechanics.md` (Claude Code's and the OpenAI
Agents SDK's real hook mechanics, cited).

## The resolution, in one sentence

**Scope narrowly to ONE real gating-hook consumer — the tool-call point, the single clean choke point
`invoke_tool()` already provides — by extending the already-proven `ApprovalDecider` seam rather than
adding a parallel chain-runner; route any hook that needs to reach an external process through the
EXISTING suspend/resume `Interaction` mechanism (ADR-029) instead of an inline blocking call; and leave
`RunEvent` untouched as the complete, already-real answer for every *observational* (non-gating) hook.**
This deliberately narrows the gap doc's original "maybe wire all of run/turn/tool-call" framing for
THIS specific pass — see Q1 below for why that is a refinement forced by real code shape, not a
contradiction of the gap doc's own text (which already flagged the model-call chain-runner as "the
nearer, better-proven template" without claiming it was a drop-in for the other three points).

An independent red-team pass (`Agent` tool, fresh context, real file:line citations) found the
original six-point draft had **two fatal problems** — no cancellation/suspend story for a hook that
blocks on an external process, and no concrete insertion point for the provenance-downgrade guard at
the tool-call level — plus four must-fix gaps. All six are addressed below; the resolution below is the
corrected design, not the original draft.

## Q1 — Mechanism split: which interception points get real gating hooks this pass

**First answer rejected by red-team.** Reusing `middleware.hpp`'s `run_before`/`run_after` chain-runner
pattern for run/turn-level hooks assumed `run_rounds()` has one clean before→call→after cycle, the same
shape the model-call point has. It doesn't: `run_rounds()` (`rt/agent_session.hpp:901-1021`) has **seven
distinct `co_return` exit points** inside its `for` loop (context-unavailable, no-chat-client,
chat-failed, token-budget-exceeded, no-tool-calls/done, suspend-for-approval, max-turns-exceeded), and
only two of them (`turn_finished` at lines 953 and 1011) actually emit a matching lifecycle event before
returning. A generic N-middleware onion-unwind around this loop either silently skips `after_turn` on
five of seven exit paths (breaking the exact symmetry the model-call version relies on for correctness)
or requires restructuring a coroutine 002 §5's own text already declined to touch for this reason
("large, mature, heavily tested... separately-scoped, larger work").

**Corrected: scope this pass to the tool-call point only.** `invoke_tool()` (`core/tool_pipeline.hpp:
348-462`) is a single, non-templated function with one entry and one clean gate (step 5, lines
400-420) — already exactly the shape a chain-runner needs, and already has a real, proven, synchronous
consumer (`ApprovalDecider`) to extend rather than duplicate. This is also, independently, where
Claude Code's own most-used hook pair (`PreToolUse`/`PostToolUse`) lives — the highest-value point to
get right first. Run/turn-level **gating** hooks (a `Stop`-block or `UserPromptSubmit`-modify analog)
are named, explicitly, as future work needing its own pass against `run_rounds()`'s real shape — not
solved here, not silently dropped. Run/turn-level **observational** hooks need no new design at all:
they're already real (Q4 below).

## Q2 — Where "run an external process" actually lives

**Holds up under red-team, essentially unchanged**, now scoped to the tool-call point: nowhere in
first-party engine code. The generalized decision seam (Q3) is a host-injected callback, matching
`ApprovalDecider`/`MiddlewareTraceHook`/`WorkflowSupervisor::CheckpointHook`'s existing idiom — the
HOST's own callback body decides whether and how to reach an external process (raw unsandboxed exec is
the host's own risk acceptance, matching 008 §2a's "custom backend is host-trust-tier code" framing; or
via `native-jail`'s `SandboxBackend::exec` for a capability-scoped, bounded version). AgentEngine ships
zero subprocess-spawning code as part of this mechanism itself.

**Real gap found and now named, not solved**: `SandboxBackend::exec` (`sandbox/sandbox.hpp:166-179`)
is synchronous today — its own file comment states plainly `ae::task<T> is not yet wired into this
header`. A reference dispatcher built on it would therefore hard-block whatever thread calls it, not
cooperatively yield — this is a real, independent prerequisite (`SandboxBackend`'s methods becoming
genuinely awaitable) for any reference implementation that wants to use the sandboxed path safely, not
something this design can route around.

**Carried over from ADR-039 §3e, which the original draft borrowed the shape of but not the stated
residual**: if a reference `ExternalCommandMiddleware`-shaped example ships under `examples/` (built on
`native-jail`, never raw host exec), it inherits ADR-039's own named risk — "the example quietly becomes
the de facto production path" — and the same mitigation direction: whatever tests exist for this
mechanism must run against the abstract decision-seam interface, not be hardcoded to the reference
dispatcher, so a future hardened adapter can swap in without re-litigating what "supports hooks" means.

## Q3 — The generalized decision seam and the I2/ADR-033 discipline

**First answer rejected by red-team.** A brand-new `before_tool`/`after_tool` Middleware pair running
alongside the existing `ApprovalDecider` at the same call site creates two independent, unordered gates
with no stated precedence — the exact "two decision points that can drift" shape ADR-023/ADR-033 both
already had to close once.

**Corrected: extend the seam, don't duplicate it, with sequencing fixed by construction.** A new,
optional hook stage runs *before* `ApprovalDecider` is ever consulted, immediately after
`tool_call_request_of()` builds the `ToolCallRequest` from the model's real `ToolCall`
(`agent_session.hpp:995`, `provenance = vendor_structured` at construction) and before it reaches
`invoke_tool()`. Its context type, `ToolCallHookContext`, follows `ModelCallContext`'s exact I2
discipline (`middleware.hpp:72-78`) — no `EffectContext&`, no capability type, ever — enumerated
concretely (the red-team's must-fix #8, closed here rather than left asserted):

```cpp
struct ToolCallHookContext {
    std::string  tool_name;
    json::Value  arguments;                       // the request's current (possibly hook-rewritten) args
    std::optional<json::Value> rewritten_arguments; // set by a hook that wants to change them
    std::optional<error>       denial;             // set by a hook that wants to deny outright
};
```

`ApprovalDecider` is **unchanged** — it still runs exactly where it runs today (`tool_pipeline.hpp:
411-420`), on whatever request survives the hook stage. Ordering is fixed by construction, not
documented convention: hook stage runs first, its output (rewrite or denial) is folded into the real
`ToolCallRequest` before `ApprovalDecider` ever sees it, so there is exactly one gate's worth of
approval semantics for a caller to reason about, with the hook stage able to deny outright (skipping
`ApprovalDecider` entirely, matching Claude Code's own `permissionDecision: "deny"` short-circuit) or
rewrite-then-still-require-approval (matching `updatedInput` staying subject to normal approval).

**The fatal finding this closes**: `enforce_hook_rewritten_tool_call_provenance(ToolCallRequest& req,
json::Value const& original_arguments)` — called unconditionally, immediately after the hook stage
runs, before `invoke_tool()` is ever called. If `req.arguments` is not byte-identical to
`original_arguments`, `req.provenance` is forced to `call_provenance::text_derived` — the same
fail-closed rule `enforce_backend_tool_call_provenance` (`middleware.hpp:227-244`) already uses for the
model-call point, simpler here because there is no "final message" to diff against: the comparison is
just the request's own arguments, before vs. after the hook ran. This routes any hook-rewritten call
through `tool_call_requires_approval`'s stricter `text_derived` gate (`tool_pipeline.hpp:325-331`,
`is_auto_declassifiable_text_derived_call`) — overriding even a tool's own `approval_mode::
never_require` for anything with a real capability ceiling or non-pure effect class, exactly like the
model-call case. Without this, a rewriting hook reopens the confused-deputy hole ADR-023/ADR-033
already closed once, one call site down — and per the research doc, this is precisely Claude Code's own
`updatedInput` shape, arriving over a JSON round-trip through an arbitrary host process, a strictly
*less* trustworthy path than the in-process C++ content-rewrite ADR-033 already treats this strictly.

## Q3a — The blocking hazard: the fatal finding the original draft never engaged with

Not one of the gap doc's original six questions, but the red-team's most serious finding, so it gets
its own heading rather than being buried in Q3's fix.

**The fatal problem**: `start_run()` acquires `session_mutex_` (`agent_session.hpp:407`) into a guard
that lives across the *entire* `co_await run_rounds()` call — every model call and every tool call in a
potentially multi-turn round. `AsyncMutex::lock()` has no timeout. A hook stage that dispatches to an
external process and is `co_await`ed inline (the original draft's implicit assumption, copying
`run_before`/`run_after`'s shape) therefore blocks *every other entry point on the session* —
`resolve_interaction()`, `snapshot_record()`, the next `start_run()` — for as long as the external
process takes, with no cancellation mechanism. This is 009 §6's "the host never blocks a session
activation on a plugin call" hazard, reborn at a new call site the original draft didn't check against.

**Corrected: an external-dispatching hook never runs inline — it goes through the EXISTING suspend/
resume `Interaction` mechanism, the same one `suspend_for_approval_` already uses for exactly this
"this might take arbitrarily long, a human/external system is involved" shape** (`agent_session.hpp:
959-990`, ADR-029). A fourth `interaction_reason` value is added alongside the current three
(`interaction_reason { input, auth, approval }`, `core/interaction.hpp:33`) — e.g. `hook_decision` — and
a tool call whose hook stage needs external input suspends the round exactly the way an
approval-required call does today: `open_interaction()`, emit the analogous `RunEvent`, `co_return`
a sentinel error, no mutex held while the host's external process runs. The host resolves it later via
the existing `resolve_interaction()` path once its own dispatch (subprocess, HTTP call, whatever)
completes — arbitrarily long, no session-wide stall, no new cancellation primitive needed because none
is needed: this is exactly the shape ADR-029 already built and proved.

**What this means concretely**: a **purely in-process** hook (ordinary C++, no external dispatch, same
justification the model-call point already relies on for staying synchronous) may still run inline,
synchronously, in the hook stage described in Q3 — it's ordinary code, not a new blocking risk. A hook
that reaches outside the process **must** be written as a suspend-and-resume hook, using the
`interaction_reason::hook_decision` path, never as an inline `co_await`. This is a real constraint on
how a host authors an external-dispatching hook, not an implementation detail — it needs to be a stated
contract the same way `MiddlewareChatClient`-outer/`ResilientChatClient`-inner composition ordering
(ADR-033 §3 finding 3) is a stated, not compile-time-enforced, caller contract.

**Q3a also resolves the red-team's checkpoint-boundary question, as a byproduct, not separately**:
since `session_mutex_` is already held for the whole run today (finding above), there is no live
per-turn checkpoint an inline hook could double-fire against — 001 §3's "every iteration is a
checkpoint boundary" does not describe the current implementation's real locking behavior. The
question the red-team's finding actually sharpens is a *cost*, not a correctness hazard: routing
external dispatch through suspend/resume, rather than inline blocking, is what keeps this design from
*adding* to that already-existing "whole run is atomically un-checkpointable" window — confirming the
suspend/resume correction is the right one for this reason too, not only for the mutex hazard.

## Q4 — Event taxonomy

**First answer needed disambiguation, now resolved as red-team's must-fix required.** "Reuse
`run_event_kind` to drive which hook fires" was ambiguous between shared *vocabulary* and shared
*trigger mechanism*. Resolved as **vocabulary only, mechanism kept fully separate**:
`emit_run_event()` (`agent_session.hpp:817-829`) stays exactly as it is today — `void`, fire-and-forget,
observation-only, unchanged by anything in this design. The new tool-call hook stage (Q3) is
independent code, run at its own call site, never wired through `emit_run_event`'s call sites. The only
thing shared is naming: the hook stage's two natural moments line up with the *already-real*
`tool_call_started`/`tool_call_finished` events for a host author's convenience when correlating logs,
nothing more. Every OTHER lifecycle point Claude Code names an event for (`SessionStart`, `SessionEnd`,
`PostToolUse`-as-pure-notification, etc.) is **already fully served today** by subscribing to the real
`RunEvent` stream — zero core change needed for the observational half of "hooks," which is most of
Claude Code's own 24-event taxonomy (`docs/research/2026-08-14-claude-code-hooks-mechanics.md` §2's own
table: 16 of 24 events cannot block/gate anything and are pure observation).

Scoped OUT, as the gap doc already named: Claude Code's async-standalone events (`FileChanged`,
`ConfigChange`, `WorktreeCreate`) have no AgentEngine subsystem to hang off yet.

## Q5 — Vocabulary mapping

**Confirmed clean by red-team, unchanged.** AgentEngine's **Run** (`start_run()`) ≈ Claude Code's
session-scoped "one user turn" (`UserPromptSubmit`→`Stop`). AgentEngine's **Turn** is a per-round
internal concept with no Claude Code analog — verified directly against the real loop:
`effect_context_.turn_index` increments once per `for`-loop iteration inside a single `start_run()`
call (`agent_session.hpp:905-906`), i.e. once per tool-calling round, potentially many times per Run.
A host binding to `turn_started` expecting Claude Code's per-user-prompt cadence will see it fire once
per round instead — this mapping must be documented wherever hook events are, not left implicit.

## Q6 — Configuration surface

**Confirmed clean by red-team, unchanged.** Hook registration stays compile-time, host/deployer-
assembled — the tool-call hook stage's callback is supplied the same way `ApprovalDecider` already is
(a constructor/builder parameter on `AgentSession`'s configuration), never something an agent's own
declarative YAML/JSON can add unilaterally. This matches 002 §3's existing "not a runtime axis" framing
for `Middleware` and CLAUDE.md's locked "v1 authoring surfaces are C++ CRTP and declarative YAML/JSON"
split — attaching a process-dispatching hook is an operator/deployment trust decision, the same
category ADR-039 already put transport selection in. A declarative, config-file-driven hook loader
(closer to Claude Code's own `settings.json`) is real, wanted, and explicitly out of scope for this
first pass — named here so it isn't silently assumed solved.

## Q7 — Run/turn-level internal (in-process) hooks (addendum, 2026-08-14, red-teamed once)

Q1 scoped run/turn-level **gating** hooks out entirely, citing the mismatch between `run_before`/
`run_after`'s onion-unwind chain-runner and `run_rounds()`'s seven-exit-path loop (only one exit,
line 953-956, is immediately preceded by a matching `turn_finished`). This addendum asks specifically
about **internal** (in-process, no external dispatch) run/turn hooks, since Q3a's blocking-hazard fix
doesn't apply to a hook that never leaves the process — is the chain-runner mismatch the only real
blocker, and is there a different mechanism that sidesteps it?

**First answer rejected by red-team, on more fundamental grounds than Q1's own.** The proposed fix was
an RAII type (`TurnBoundaryGuard`/`RunBoundaryGuard`) constructed once per loop iteration (or once per
`run_rounds()` call), whose *destructor* fires an `after_turn`/`after_run` hook exactly once regardless
of which exit path was taken — reusing the exact mechanism `AsyncMutex::Guard` already relies on to
release `session_mutex_` correctly across all seven of `run_rounds()`'s existing exits, with zero
control-flow restructuring (confirmed real and precedented, not just plausible: `agent_session.hpp:
407`, `async_mutex.hpp`). That mechanical claim survives red-team intact. **Two of the capabilities
proposed on top of it do not:**

- **A hook cannot be invoked from a destructor if it needs to be `co_await`ed.** C++20
  `[dcl.fct.def.coroutine]` disallows destructors from being coroutines at all — `co_await`ing an async
  hook (the `task<std::monostate>` shape every other hook in this codebase uses, `middleware.hpp:97-104`)
  inside `TurnBoundaryGuard::~TurnBoundaryGuard()` is a hard compile error, not a design choice. This
  independently kills any version of this mechanism that routes external dispatch through
  `interaction_reason::hook_decision` (Q3a's mechanism) from inside the guard — a destructor structurally
  cannot suspend.
- **An `after_turn` hook cannot override an outcome that already happened.** The `completed` exit
  (`agent_session.hpp:953-956`) evaluates `co_return AgentResponse{...}` — which finalizes the
  coroutine's stored return value — *before* any local's destructor runs. By the time
  `TurnBoundaryGuard`'s destructor fires, there is no mechanism to redirect the coroutine back into the
  `for` loop it already left. The Claude-Code-`Stop`-hook-shaped capability this addendum's own first
  draft proposed ("an `after_turn` hook can force another round instead of finishing") is not an RAII
  pattern at all — delivering it needs an explicit check placed *before* the `co_return`, which is
  exactly the control-flow restructuring this whole approach was trying to avoid.

**Corrected, narrower resolution: the RAII guard survives, scoped to purely synchronous, non-throwing,
non-overriding, observation-only hooks — a real, smaller thing, not a rescue of the original ambition.**
`after_turn(TurnBoundaryContext{turn_index, TurnOutcome})` / `after_run(RunBoundaryContext{run_id,
RunOutcome})` fire exactly once per iteration/run, ordinary synchronous C++ calls (no `co_await`, no
suspend/resume, no override power), wrapped in an internal try/catch inside the guard's own destructor
(`CONVENTIONS.md`'s "hot paths are `noexcept`" rule makes this mandatory, not optional — a throwing
hook escaping a destructor during coroutine-frame unwinding is `std::terminate`, not a caught-and-
converted `error` the way `middleware.hpp`'s non-destructor call sites already handle it). This closes
a real, standing gap independent of anything else in this document: **today, 5 of `run_rounds()`'s 7
exit paths never emit a matching `turn_finished` `RunEvent` at all** — a host building anything that
counts on "every started turn eventually reports how it ended" has no reliable signal for those 5 paths
today. The guard's `after_turn` fires on all 7, unconditionally, closing that gap for good — this is a
real value on its own, separate from hooks being useful for policy.

**Must-fix corrections found and closed for the buildable version:**

- `TurnOutcome`/`RunOutcome` need an explicit `unwound_via_exception` (or equivalent unknown) sentinel,
  default-initialized before each iteration — a genuine thrown C++ exception (`bad_alloc` from
  `history_.push_back`, or a throwing `ContextProvider` call) unwinds the frame without passing through
  any of the "set outcome, then `co_return`" sites this design's outcome vocabulary otherwise covers.
- `RunBoundaryGuard` must be placed in `start_run()`, not `run_rounds()`, to also observe `start_run()`'s
  own two early exits that happen before `run_rounds()` is ever called — `run.admission_denied`
  (`agent_session.hpp:414-416`) and `run.approval_pending` (`agent_session.hpp:423-428`) — both need
  their own `RunOutcome` values, not silently unobserved.
- Must be documented explicitly, not left implicit: `after_turn` firing does **not** reconcile with or
  imply the pre-existing `turn_started`/`turn_finished` `RunEvent` asymmetry (the same 5-of-7 gap this
  guard closes independently) — a host correlating the two streams by name/timing (as Q4 already invites
  for the tool-call point) must not assume they stay in lockstep.

**What this leaves genuinely unresolved, restated plainly**: gating (deny/override) power at run/turn
boundaries, and any external-dispatching run/turn hook, both still need the SAME real control-flow
restructuring of `run_rounds()`'s seven exit paths that Q1 originally scoped out — an explicit hook
call placed before each `co_return`, not an RAII trick. `before_run`/`before_turn` **denial** (run
`session_mutex_`-safe, ordinary inline synchronous calls before work starts, confirmed clean by
red-team, unaffected by any of the above) stays available and buildable today, unchanged from Q1's
original framing — only the *after*-side observation gained a real design this pass, and only in its
synchronous, non-overriding form.

## What's left for a real ADR (punch list)

1. Design and implement `ToolCallHookContext` and the hook-stage insertion point in `run_rounds()`
   (between `tool_call_request_of()` and `invoke_tool()`), including `enforce_hook_rewritten_tool_call_
   provenance()` — the Q3 fix, not yet code.
2. Add `interaction_reason::hook_decision` (`core/interaction.hpp:33`) and the suspend/resume wiring in
   `run_rounds()` mirroring `suspend_for_approval_`'s existing shape (`agent_session.hpp:959-990`) — the
   Q3a fix. Requires a corresponding 001 §2 RFC text update, since `interaction_reason` is named there
   normatively (`interaction.hpp:33`'s own lint-allow comment).
3. Make `SandboxBackend`'s methods genuinely awaitable (`ae::task<T>`-returning) before any reference
   dispatcher built on `native-jail` can safely use it without hard-blocking its calling thread — a
   real, independent prerequisite named in Q2, not this design's own job to build.
4. Decide and build (or explicitly defer) the reference `ExternalCommandMiddleware`-shaped example
   under `examples/`, with the ADR-039 §3e drift-mitigation (abstract-interface-scoped tests) stated
   from the start, not retrofitted later.
5. Implement `TurnBoundaryGuard`/`RunBoundaryGuard` (Q7) — synchronous, non-throwing,
   observation-only `after_turn`/`after_run`, with the `unwound_via_exception` outcome sentinel and
   `RunBoundaryGuard`'s placement in `start_run()` (covering `admission_denied`/`approval_pending` too).
   The one piece of run/turn-level hooking this pass actually closed.
6. Run/turn-level **gating/override** hooks and any **external-dispatching** run/turn hook (Q7's
   remaining open half) — needs real restructuring of `run_rounds()`'s seven exit paths (an explicit
   call before each `co_return`), not an RAII trick. Still explicitly future work, not solved here.
6. A declarative/config-file-driven hook registration surface (Q6's named-out scope), if ever wanted —
   separate, later work.

Items 1-2 together are the minimum real, coherent slice: a tool-call gating hook that can deny, rewrite
(with the provenance guard), or suspend-for-external-dispatch, without the session-wide blocking hazard
the original draft missed. Everything else is real, named follow-on work, not silently dropped.
