# ADR-150: A `Workflow`-as-participant adapter (adjacent to issue #35, filed as issue #36)

## 1. The question

Can a whole built `Workflow` (plus its `ExecutorBody` set) be reused as ONE ordinary participant
inside ANOTHER workflow's builder — "no graph node, no message-type wiring", mirroring MAF's
`workflow.as_agent()` in spirit (`python/samples/03-workflows/agents/sequential_workflow_as_agent.py`,
`agent-framework` clone, commit `4c0bff8`) — as pure additive sugar over `WorkflowSupervisor`, with no
new `executor_kind`, no change to `check_workflow_executable()`, and without introducing a real
concurrency or capability-sourcing hazard along the way?

Started as an attempt to close GitHub issue #35 ("no adapter lets a built Workflow satisfy the
Agent/ChatClient interface"). A red-team pass on the first design draft found this framing was wrong
— #35 as filed is centered on a `ChatClient`-shaped adapter, not an `ExecutorBody`-shaped one, and its
own text names hand-writing a bespoke `ExecutorBody` shim as the *inadequate status quo* it wants
obsoleted, not the fix. This work was re-scoped mid-pass and filed as its own issue, #36; #35 remains
open, unrelated in mechanism to what shipped here.

## 2. Scope

One piece: `agentengine::rt::workflow_as_executor_body()` (`include/agentengine/rt/
workflow_as_executor.hpp`), two overloads —

1. `result<ExecutorBody> workflow_as_executor_body(std::shared_ptr<WorkflowSupervisor> inner)` — the
   primary, owning surface.
2. `result<ExecutorBody> workflow_as_executor_body(WorkflowSupervisor& inner)` — an advanced,
   reference-capture surface for a caller with an already-guaranteed-stable `inner`.

Both: refuse construction (a real error, nothing built) if `inner`'s graph contains any
`executor_kind::request_port` node; otherwise return a closure that runs `inner`'s workflow fresh, to
completion, on every call, serialized by the adapter's own `std::mutex` (independent of
`WorkflowSupervisor::run_mutex_`).

## 3. Red-team findings and how each is addressed

An independent red-team pass (fresh agent, no prior context) reviewed the first design draft BEFORE
any code existed. Full findings in `docs/planning/workflow-as-executor-body-adapter-design-draft.md`
§10; 4 of them were MUST-FIX:

1. **The concurrency claim was actively wrong, not just optimistic.** The first draft assumed
   `WorkflowSupervisor::run_mutex_` would safely serialize a second concurrent call against the same
   `inner`. Traced by the red-team: `run_mutex_` is an `AsyncMutex` whose contended path parks the
   *awaiting coroutine's own handle*, resumable only by the matching `unlock()`'s own trampoline — the
   adapter's own `drive()` "resume until done" loop has no way to honor that. Two deliveries to the
   same wrapped node in one outer round (ordinary topology for a `function`-kind node — no
   agent-kind-only dedup applies) would resume each other's parked handles, racing on shared state and
   risking a double-resume of an already-destroyed coroutine frame. **Fixed**: the adapter now owns
   its own `std::mutex`, independent of `run_mutex_`, serializing calls safely.
2. **Scope/issue misattribution** — see §1. **Fixed**: re-scoped, filed as issue #36, #35 left open.
3. **Fail-closed on `suspended` silently and permanently orphaned the inner run's open interaction**
   the next time the same adapter was called (`run_workflow()` unconditionally resets `ports_`).
   **Fixed**: construction now refuses any graph containing a `request_port` node outright, so
   `suspended` can never occur through this adapter — the state-loss hazard is eliminated
   structurally, not merely documented.
4. **Reference-capture lifetime was a real trap, not a hypothetical.** `WorkflowSupervisor` turns out
   to be immovable (embeds an `AsyncMutex`, whose deleted copy ops suppress the implicit move
   constructor too), and the adapter's own headline use case — a reusable, composable, possibly
   factory-built participant — actively invites exactly the dangling-reference and
   container-reallocation patterns that break reference capture. **Fixed**: added the `shared_ptr`
   overload as the primary documented surface.

Plus 4 MINOR/RESIDUAL findings, also resolved (see §10 of the design draft): the I2 argument was
analogy-only, strengthened with a direct citation (`check_workflow_executable()` never
engine-enforces `capability_ceiling` for `function`-kind nodes at all); no "ROUTING NOTE" mirroring
the sibling adapter's own I3 disclosure, added; no explicit statement that the outer
`TypedExecutor<In,Out>`'s type pairing has zero visibility into the wrapped workflow's real shape,
added; `MagenticWorkflowBuilder`'s own surface wasn't checked for extra constraints before the
composability example was written — checked directly (none found) before §4 fix 4 below.

## 4. The accepted design

- `workflow_as_executor_body()` (both overloads) refuses at construction, `failure_class::contract`,
  code `rt.workflow_as_executor.request_port_unsupported`, if `inner`'s graph contains any
  `request_port` executor.
- The returned closure captures the supervisor (by `shared_ptr` or reference, per overload) plus a
  fresh `std::shared_ptr<std::mutex>`, held for the full duration of one `drive(inner->run_workflow(...))`
  call — safe because this always runs on an ordinary `ThreadPool` worker thread, never inside a
  suspended coroutine frame.
- On `workflow_status::completed`, returns `ExecutorOutcome{r.output}` (empty `routes`, output
  metadata unchanged — the same "no routing concept" disclosure `agent_session_as_executor_body()`
  makes for its own equivalent seam). On any other status, fails closed with
  `rt.workflow_as_executor.inner_run_not_completed.<status>` — the status is embedded in the code so a
  caller can distinguish "hit a configured bound" from "routing failed inside the inner graph" from
  "the inner graph is malformed" programmatically, even though `failure_class` stays uniformly
  `contract` for all of them (nothing in this codebase's edge-policy machinery keys off message text).
- Capability sourcing (I2): the outer `EffectContext` this body receives is unused. The inner
  workflow's own executors run under whatever `EffectContext`s the caller already passed to
  `inner->initialize(..., contexts, ...)` before wrapping — zero implicit capability flow across the
  adapter boundary, grounded directly in `check_workflow_executable()` never engine-enforcing
  `capability_ceiling` for `function`-kind nodes regardless of this adapter's existence.
- Every call to the same wrapped adapter is an independent, fresh, complete run of the inner workflow
  from its own `start` — `run_workflow()` unconditionally resets round/port state at its own top, so
  there is no cross-call memory, by design (MAF's own "one call in, one final output out" semantics,
  the opposite of `agent_session_as_executor_body()`'s own "accumulates history across rounds"
  contract).

Full design: `docs/planning/workflow-as-executor-body-adapter-design-draft.md`.

## 5. What this ADR does not claim

- Does not close issue #35 (the `ChatClient`-shaped adapter question — a different mechanism with its
  own different open questions, e.g. what a multi-turn `ChatRequest` means against something with no
  "turn" concept). Left open.
- Does not touch issue #33 (`executor_kind::sub_workflow` as a first-class graph NODE with typed
  message passing and intermediate-result visibility). Unrelated mechanism, unrelated status.
- No nested `request_port` proxying (mirroring MAF's `WorkflowHostExecutor::_pendingResponsePorts`) —
  named, real, unbuilt future work; graphs needing it are refused outright by this adapter today, not
  silently mishandled.
- No compile-time or runtime check that the outer `TypedExecutor<In,Out>` declared for the wrapping
  node actually corresponds to what the inner workflow's own `start`/`output_selection` produce — the
  author's manual responsibility, same as any hand-written `ExecutorBody` already has.
- No new `executor_kind`, no change to `check_workflow_executable()`, no change to
  `WorkflowBuilder`/`MagenticWorkflowBuilder`.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | A completed inner run's output reaches the caller unchanged, with empty `routes`. | CORRECT | `test_rt_workflow_as_executor.cpp` W1 |
| 2 | A second call to the same wrapped adapter starts fully fresh — identical input produces identical output, proving no `WorkflowSupervisor`-internal state leaked across calls. | CORRECT | W2 |
| 3 | Construction is refused, both overloads, for an inner graph containing a `request_port` executor, with the specific error code. | CORRECT | W3 |
| 4 | An inner run that does not complete (`routing_failed`) fails closed with a status-specific error code, not a crash or default value. | CORRECT | W4 |
| 5 | Two genuinely concurrent deliveries to the SAME wrapped node in one outer round (fan-out topology) complete correctly, serialized by the adapter's own mutex, across repeated trials. | CORRECT | W5 (5 trials, each confirming exactly 2 calls served, zero crash) |
| 6 | The concurrency fix (finding 1, §3) is genuinely load-bearing, not decorative. | CORRECT (adversarially verified) | With the adapter's `std::mutex` guard temporarily removed and rebuilt, W5 segfaults reproducibly (3/3 runs); restored, W5 passes cleanly again (5/5 trials) — see §8 |
| 7 | The `shared_ptr` overload survives its local `WorkflowSupervisor` variable going out of scope in a factory function. | CORRECT | W6 |
| 8 | A whole inner `Workflow`, wrapped, composes as ONE ordinary node inside an OUTER `WorkflowBuilder`-shaped graph, driven through a real `WorkflowSupervisor::run_workflow()` call, with the final output reflecting both the outer and the full inner processing. | CORRECT | W7 |
| 9 | The same composability claim holds against `MagenticWorkflowBuilder` specifically (ADR-149's own convenience builder), not just a hand-built `Workflow`. | CORRECT | `examples/21_workflow_as_participant.cpp` — a wrapped inner workflow used as one `.participant()` alongside an ordinary function participant, real `run_workflow()` run, `completed`, output carries both the wrapped workflow's real two-step processing and the sibling participant's own output |
| 10 | Every pre-existing workflow-family test still passes after this addition (header-only, no shared-file edits beyond `Executor`-designated-init consistency fixes already required by an unrelated, concurrent gcc `-Werror` fix). | CORRECT | Full `ctest -R "workflow"`: 24/24 passed, zero regressions |
| 11 | The full project builds clean, including under the newly-enforced `/WX`/`-Werror`. | CORRECT | Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC), exit code 0, zero errors, zero warnings |

## 7. Files changed

**New:**
- `docs/planning/workflow-as-executor-body-adapter-design-draft.md`
- `include/agentengine/rt/workflow_as_executor.hpp`
- `tests/test_rt_workflow_as_executor.cpp`
- `examples/21_workflow_as_participant.cpp`

**Edited:**
- `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new target registrations.

No existing engine file (`workflow_supervisor.hpp`, `graph.hpp`, `magentic.hpp`, `WorkflowBuilder`)
was touched — this is genuinely additive, exactly as scoped.

## 8. Adversarial verification of the concurrency fix

Given how severe red-team finding 1 (§3) was, the fix was verified directly rather than trusted on
reasoning alone, the same "adversarial mutation test" discipline this codebase's own ADRs use (e.g.
ADR-144's port-range-before-cast reordering test): the adapter's `std::lock_guard<std::mutex>` line
was temporarily removed (a bare `(void)call_mutex;` in its place), the test target rebuilt, and run 3
times. **All 3 runs segfaulted reproducibly**, exactly at W5 (the concurrency test) — proving the
original race was real, not theoretical, and that the fix is what prevents it, not the test's own
happenstance timing. The change was then reverted (confirmed byte-identical to the pre-mutation file)
and the target rebuilt and re-run clean (5/5 trials passed, zero crash) before this ADR was written.

## Status

**Proposed — implemented, independently red-teamed once (design draft, before any code existed), the
single most severe finding adversarially verified post-fix (not merely re-reasoned about), all
evidence executed and passing, pending project-owner sign-off.** Filed as GitHub issue #36 (not #35 —
see §1). Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC) and the full workflow-family
`ctest` suite (24/24) both clean; `examples/21_workflow_as_participant.cpp` run directly, `OK`.
