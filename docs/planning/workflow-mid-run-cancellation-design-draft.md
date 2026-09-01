# Design draft: mid-run cancellation for `WorkflowSupervisor` (GitHub issue #37)

Status: **red-teamed once (fresh agent, zero prior context); central mechanism confirmed sound, one
real MUST-FIX bug found (a second exhaustive `switch(workflow_status)` this draft's own file list
missed) and one proof-plan gap treated as blocking (round-boundary proof needs a deterministic
liveness shape, not a timing race). §3/§4/§5/§6 below are revised to incorporate every finding.
Written per CLAUDE.md's `design -> red-team -> prove -> judge` discipline and the user's own
explicit preference for this pipeline over a quick patch on architectural work (see memory
`feedback_design_over_patch`).

## 1. The question

MAF's `workflow_cancellation.py` sample cancels a running workflow mid-execution and the run stops
before completing its remaining steps. `WorkflowSupervisor` (`include/agentengine/rt/
workflow_supervisor.hpp`) has `TerminationBound::max_rounds`/`deadline_ms`/`max_stalls`/`max_resets`
— all DECLARED limits, checked only at round boundaries — but nothing lets a caller signal "stop
this run now" from OUTSIDE, imperatively, for its own reasons (a user closing a UI, an upstream
request being aborted). Confirmed via direct grep: zero matches for "cancel" anywhere in that file
today.

## 2. An already-established precedent exists in this codebase — reuse it

`agentengine::stream<T>`/`stream_producer<T>` (`core/stream.hpp`) already solved the identical
shape of problem for a different subsystem: "an out-of-band cancellation signal a consumer raises
that a producer running on a different thread must observe," citing ADR-017. The mechanism: a
`std::stop_source`, held by BOTH sides via `std::stop_source`'s own copy semantics (copying shares
the associated stop-state, not duplicates it — "no extra allocation, no plumbing" per that file's
own comment), with `cancel()` calling `request_stop()` and the producer side exposing a
`std::stop_token` for cooperative polling.

`std::stop_token`/`std::stop_source` is not a one-off here either — already used across
`rt/thread_pool.hpp` (`std::jthread`'s own built-in stop mechanism for worker shutdown),
`core/model_call_gateway.hpp`, both `chat_client.hpp` protocol adapters, `sandbox/
provider_http_client.hpp`, and `sandbox/net_egress_proxy.hpp`. This is this codebase's own
established, idiomatic cancellation primitive — the design below reuses it rather than inventing a
bespoke `CancellationToken` type, which would be new machinery for a solved problem.

## 2b. Red-team pass: findings and resolution

An independent red-team pass (fresh agent, zero prior context) confirmed the central mechanism is
sound — thread safety, call-site completeness, checkpoint isolation, I1/I2/I3, and the
`run_mutex_`-acquiring alternative's rejection were all traced against the real code and confirmed
correct, not merely plausible. Two real problems, both fixed here before implementation:

- **MUST-FIX — a second exhaustive `switch (workflow_status)` this draft's own §6 file list
  missed**: `workflow_as_executor_detail::status_tag()` (`include/agentengine/rt/
  workflow_as_executor.hpp:103-117`) has NO `default:` case, unlike `finish()`'s own switch. This
  project builds under `-Wall -Wextra -Werror` (gcc/clang) as a hard CI/local gate
  (`CONVENTIONS.md`) — adding `workflow_status::cancelled` without a case here breaks the build on
  those compilers (MSVC alone would not have caught this). Fixed: §6's file list now includes this
  file; `status_tag()` gets a `cancelled` case.
- **MUST-FIX (proof-plan) — §5 proof #1 needs this codebase's own established deterministic-
  liveness shape**, not a timing race, or it risks the exact false-negative class this session has
  now caught three times (S12's racy background sampler; ADR-157 Pass 2's `fan_in`-merge false
  negative; and this would be a third instance if shipped as originally scoped). Nothing in the
  original wording forced the run to still genuinely be executing when `cancel()` lands — a run
  that happens to finish naturally before the call arrives would pass the check for the wrong
  reason. Fixed: proof #1 now mirrors `test_rt_workflow_event_stream.cpp`'s own W12 shape exactly —
  a body deliberately blocked on a `condition_variable` (bounded wait, machine-safety-compliant)
  past round 1, `cancel()` issued and CONFIRMED to have landed while the body is still blocked, then
  released, then confirmed round 2 never dispatched (`rounds == 1`, not merely
  `status == cancelled`, which alone wouldn't rule out "happened to finish naturally at round 1
  anyway").

Two SHOULD-FIX refinements, both incorporated:

- §4's "cheap, round-boundary" framing is sharpened below: for a wide fan-out round or any round
  containing a sub_workflow dispatch (which runs an entire nested multi-round sub-run synchronously
  inside one job before the outer loop ever rechecks `cancel_source_`), "next round boundary" can be
  an effectively UNBOUNDED wait, not a uniformly cheap one — worth saying plainly given the issue's
  own "stop this run NOW" framing.
- The nested-cancellation residual is reframed: a host that already holds a `shared_ptr<
  WorkflowSupervisor> inner` (required to call `bind_sub_workflow()` in the first place) can call
  `inner->cancel()` directly, TODAY, with zero new machinery — confirmed safe by the same thread-
  safety trace as the outer case. The real, still-open gap is only AUTOMATIC fan-out from one
  outer `cancel()` call to every bound descendant, not "nested cancellation" as a whole.

## 3. The accepted mechanism

- `WorkflowSupervisor` gains a private `std::stop_source cancel_source_;` member (default-
  constructed, always present — same "always valid, no null state" shape `multiplex_sink_` already
  established for a different ADR-152 field).
- Two new public methods:
  - `void cancel() noexcept { cancel_source_.request_stop(); }` — callable from ANY thread while a
    run is in flight on another (`std::stop_source::request_stop()` is documented thread-safe by
    the standard itself), matching the issue's own "signal from another thread" requirement.
  - `[[nodiscard]] std::stop_token cancellation_token() const noexcept { return cancel_source_.
    get_token(); }` — a caller can hold this and check it independently, or hand it to unrelated
    code that wants to observe the same cancellation, mirroring `stream_producer<T>::stop_token()`'s
    own exposed-handle shape.
- New `workflow_status::cancelled` enumerator (+ `workflow_status_tag()` case) — an honest, expected
  non-error termination, the same shape `bound_max_rounds`/`bound_max_stalls` already are. `finish()`
  needs ZERO changes: its `switch (status)` already has a `default:` case that pushes
  `workflow_run_failed` with the right tag for any status it doesn't special-case — this is exactly
  why every one of the existing bound-triggered statuses (`bound_max_rounds`, `bound_deadline`,
  `bound_max_stalls`, `bound_max_resets`) already composes for free through that one choke point,
  and `cancelled` does too.
- `execute()`'s round loop: ONE new check, placed in the EXACT SAME spot every other termination
  bound is already checked (top of the `while (!state_.pending.empty())` loop, right after the
  existing `deadline_ms` check) — never a special early gate before the port-resolution prologue,
  deliberately: `max_rounds`/`deadline_ms` don't gate that block either, so cancellation doesn't
  either, for consistency (an already-resolved port's answer still gets folded even if cancellation
  arrived in between — the SAME "cheap, round-boundary" characteristic the issue's own scope note
  asks for, not a stronger guarantee than any existing bound already offers):
  ```cpp
  if (cancel_source_.stop_requested()) {
      status = workflow_status::cancelled;
      break;
  }
  ```
- Cooperative mid-call opt-out: `EffectContext` (`core/effect_context.hpp`) gains a new field,
  `std::stop_token cancellation;` — default-constructed (a default `std::stop_token` always reports
  `stop_requested() == false`, `stop_possible() == false`, so this is always safe to read even when
  nobody ever wires it — the SAME "optional-but-always-safe-to-call" idiom this file already
  establishes for `report_progress`/`agent_turn_sink`, just via a cheap value type instead of a
  `std::function`). Implementation refinement over adding a new `run_executor_job()` parameter:
  `execute()`'s dispatch loop instead builds a per-delivery COPY of `contexts_[idx]` with
  `ctx.cancellation = cancel_source_.get_token()` set before passing it in — `EffectContext` already
  IS the per-call vehicle for exactly this kind of opt-in signal (mirrors `capabilities`/
  `bound_capabilities` riding along on `ctx` rather than as separate parameters), so no new
  function-signature plumbing was needed at all. Unconditional for every delivery — no enable/
  disable gate needed
  (unlike the multiplexed event sink's `workflow_event_stream_enabled_` gate): a `std::stop_token` is
  cheap to hold and check regardless of whether anyone ever calls `cancel()`. A long-running body
  that wants to abort early checks `ctx.cancellation.stop_requested()` itself — matching how
  cancellation composes with arbitrary user code in every other engine this issue's own text already
  names as the expected shape (cooperative, not preemptive).

## 4. What this draft does not claim

- Does NOT preempt an already-dispatched, in-flight executor body that never checks
  `ctx.cancellation` — the SAME already-accepted characteristic every existing bound
  (`max_rounds`/`deadline_ms`) has today (the issue's own text names this explicitly: "even
  `deadline_ms` only takes effect at the next round check, not mid-call"). Not a new limitation
  introduced by this mechanism, an existing one this mechanism inherits by design. **For a wide
  fan-out round, or any round containing a sub_workflow dispatch (which runs an entire nested
  multi-round sub-run synchronously inside one job before the outer loop ever rechecks
  `cancel_source_` — `run_sub_workflow_job()`'s `drive(inner->run_workflow(...))`), "next round
  boundary" can be an effectively UNBOUNDED wait, not a uniformly cheap one.** Said plainly here
  rather than left implicit, given the issue's own "stop this run NOW" framing.
- Does NOT automatically fan out ONE outer `cancel()` call into every bound NESTED `sub_workflow`
  instance (`bind_sub_workflow()`, ADR-157). This is narrower than it first appears: a host that
  already holds a `shared_ptr<WorkflowSupervisor> inner` (required to call `bind_sub_workflow()` at
  all) can call `inner->cancel()` directly, TODAY, with zero new machinery — the same thread-safety
  guarantee that makes the outer case sound applies identically to any instance, nested or not. The
  real, still-open gap is only AUTOMATIC propagation from a single outer `cancel()` call to every
  descendant it doesn't itself hold a direct handle to — a real follow-on for a future issue,
  reusing the `ScopedForwardedEventSink`-style dispatch-time-wiring pattern issue #42 item 3 just
  established, not attempted here to keep this pass scoped to what issue #37 actually asks for.
- Does NOT take an extra checkpoint AT the cancellation point. Traced directly: the existing
  per-round `checkpoint_hook_(rounds_, to_record())` call already fires at the END of the round
  BEFORE cancelled, right after that round's results are folded (`execute()`'s own "Checkpoint at
  superstep boundaries" comment) — cancellation is checked at the START of the NEXT iteration, after
  that hook already ran. So a cancelled run's last completed round is always already checkpointed
  if a hook is set, with zero new checkpoint call needed. Answers the issue's own open question
  ("whether a checkpoint is taken at the cancellation point") without adding a new mechanism.
- Does NOT add `cancel`/`cancellation_token` to `TerminationBound` (`workflow/graph.hpp`). Every
  existing `TerminationBound` field is a DECLARED, graph-authored limit; cancellation is an
  imperative, caller-driven external signal, structurally the same shape `stream<T>::cancel()`
  already is, not a graph property. Kept as a `WorkflowSupervisor`-level runtime feature, mirroring
  `set_checkpoint_hook()`'s own "opt-in configuration call, not a graph field" precedent.
- Does NOT persist cancellation state through checkpoint/resume — a fresh/restored instance's
  `cancel_source_` is always unrequested; a caller wanting a restored run to stay cancelled must
  call `cancel()` again after restore. Named, matching every other non-persisted runtime member
  this file already documents this way (`nesting_depth_`, `event_path_prefix_`).

## 5. Required proof before this ships

1. **Positive, round-boundary — deterministic liveness, not a timing race** (per §2b's own
   MUST-FIX): a body deliberately blocked on a `condition_variable` (bounded wait) past round 1,
   driven on a real `std::thread`; `cancel()` issued from the main thread and CONFIRMED to have
   landed (`stop_requested()` observed true) while the body is still genuinely blocked; then
   released; then confirmed the run stops with `workflow_status::cancelled` at `rounds == 1` — round
   2 never dispatches. Mirrors `test_rt_workflow_event_stream.cpp`'s own W12 shape exactly.
2. **Positive, cooperative mid-call**: a body that checks `ctx.cancellation.stop_requested()` and
   returns early observes a `cancel()` call made from another thread while it is running.
3. **Non-preemption is real, not accidentally stronger than claimed**: a body that does NOT check
   the token runs to its own natural completion for that one call, even after `cancel()` was called
   mid-call — confirming the mechanism is genuinely cooperative, not something that silently became
   preemptive by accident.
4. **Zero-cost-when-unused**: a run that never calls `cancel()` completes with its ordinary status,
   byte for byte unaffected — matching this file's own "additive, existing callers unaffected"
   convention for every optional hook.
5. **Negative/mutation**: temporarily make the round-loop check a no-op and confirm a run that
   SHOULD have stopped early keeps running instead — ruling out a test that would pass regardless of
   whether cancellation actually works (the same class of false-negative this session has now caught
   three times already).
6. **Non-regression**: every existing workflow-family test still passes unchanged.

## 6. Files (planned)

- `include/agentengine/rt/workflow_supervisor.hpp` — `cancel_source_`, `cancel()`,
  `cancellation_token()`, `workflow_status::cancelled`, `workflow_status_tag()`'s new case, the
  round-loop check, `execute()`'s dispatch loop wiring `ctx.cancellation` onto its per-delivery
  `EffectContext` copy (no new `run_executor_job()` parameter needed).
- `include/agentengine/rt/workflow_as_executor.hpp` — `status_tag()`'s own new case (§2b MUST-FIX;
  a SECOND exhaustive switch over `workflow_status`, missed in the first draft's own file list).
- `include/agentengine/core/effect_context.hpp` — `EffectContext::cancellation`.
- `tests/test_rt_workflow_cancellation.cpp` (new) — the proofs from §5.
- `decisions/ADR-159-workflow-mid-run-cancellation.md` (once implemented and proven — 159 is the
  next free number as of this draft; verify against `decisions/` before committing in case another
  session claimed it concurrently, matching this repo's own established numbering-collision
  recovery pattern).

## Status

**Implemented and proven** (see the paired ADR for final evidence). Design drafted, independently
red-teamed once (fresh agent, zero prior context) before any code was written — one real MUST-FIX
(a second exhaustive switch this draft's own file list initially missed) and one proof-plan MUST-FIX
(round-boundary proof needed a deterministic liveness shape), both resolved above before
implementation began.
