# ADR-214 — What a workflow cancel does, wherever it lands, and for how long

**Status:** **Proposed (2026-10-01).** Design, one self red-team pass (§4), implemented and proven (§6).
Judge is the project owner's.

**Relates to:** GitHub issue #156. `014-Workflow-and-Orchestration.md` §2/§4. ADR-159 (mid-run
cancellation, the mechanism this amends). ADR-178 (`AgentSession::cancel()` is per run — the precedent
followed here). ADR-210 §7 C1 (where #156 was found). ADR-157 (nested sub-workflows). ADR-152 (event
stream). `include/agentengine/rt/workflow_supervisor.hpp`, `src/rt/workflow_supervisor.cpp`,
`include/agentengine/rt/async_mutex.hpp`, `include/agentengine/rt/workflow_run_state_record.hpp`.

## 1. The question

ADR-159 gave `WorkflowSupervisor` a `cancel()` that the round loop checked only at the top of each
round. Issue #156 showed what a host actually got:

1. **Cancel during an agent step ended `executor_failed`.** The step's `AgentSession` sees
   `ctx.cancellation` (ADR-193's bridge), ends its own run `run.canceled` (class `fatal`), and the
   default `fail` edge policy turned that into a step failure before the loop ever reached its check.
2. **Cancel in the round that opens a port ended `suspended`.** The port opened anyway.
3. **Cancel while suspended did nothing.** The run kept its open interactions.
4. **Cancel was permanent.** `cancel_source_` was never replaced, so every later run or resume on the
   supervisor ended `cancelled`.

Which of 1-3 a host saw depended on timing. A host could not tell "I cancelled it" from "a step broke".

## 2. Decision

1. **A cancel observed anywhere in a round ends the run `cancelled`.** After a round's steps are
   collected and folded (outputs into `partial`, usage, branch merges — exactly as in any round), the
   supervisor checks the cancel BEFORE routing. If set, the run ends `cancelled`; a step that failed in
   that round is not reported (`failed_executor` empty); ports reached that round go to
   `unopened_ports`. A merge conflict in the same round still wins (it needs a human regardless).
   No failed step is retried once the cancel is set. A cancel that lands after that check but before
   the run suspends turns the `suspended` result into `cancelled`.
2. **Cancel while suspended ends the run `cancelled` and closes its interactions — at once.**
   `cancel()` takes the run lock with a new non-blocking `AsyncMutex::try_lock()`. If it gets it and
   the run has open interactions, it settles the run on the calling thread: `finish(cancelled)` pushes
   `workflow_run_failed{cancelled}`, drops the ports, cancels each pending nested sub-workflow (which
   settles that inner run the same way) and discards undelivered messages. If an entry point holds the
   lock instead, that entry point observes the cancel (`refuse_if_cancelled()` on entry, the round
   checks, and a check on every `suspended` result in the public wrappers) and settles the run before
   it returns.
3. **A cancelled run never moves again.** `resume_workflow()` returns `cancelled` and pushes
   `request_port_rejected` (reason `cancelled`, the #155 refusal event); `continue_workflow()` returns
   `cancelled`. Neither runs anything. `RunStateRecord` gains `cancelled`, so a checkpoint of a
   cancelled run restored and continued is still `cancelled`, not `completed` (§4 F5).
4. **Cancel is per run, not permanent.** `run_workflow()` and `restore_from_record()` give the run a
   fresh `std::stop_source` (replaced under a small `cancel_mutex_`, since `cancel()` may run on
   another thread at that instant — ADR-178's exact shape). A cancel aimed at an earlier run, or one
   that arrived after it ended, cannot cancel the next run.
5. **An outside token can be linked into a run.** Per-run sources mean a cancel issued before
   `run_workflow()` takes the lock is dropped (§3). A caller that is itself cancellable passes its token
   as the new trailing field `RunWorkflow::cancellation`; `run_workflow()` registers a `stop_callback`
   on it for the call's duration (an already-stopped token cancels the run before round 1). The engine
   uses this for every nested run it starts: a `sub_workflow` dispatch passes the outer run's token,
   `workflow_as_executor_body()` and `WorkflowChatClient` pass `ctx.cancellation`. `cancel()` on a run
   in flight also cancels the bound inners, so a nested resume in progress stops too.

## 3. Choice: per-run reset, not documented permanence

Issue #156 allowed either. Permanence was rejected:

- It is the bug's fourth symptom: a host that cancels one run must build a new supervisor (re-binding
  bodies, contexts, sub-workflows, admission) to run again. Nothing else on this class is single-use.
- `AgentSession` already decided this (ADR-178, "a stale cancel can never poison it"); a workflow whose
  agent nodes are per-run while the workflow is permanent is two contracts for one word.
- The cost of per-run — a cancel issued before the run starts is lost — is closed by §2.5 for the
  callers that need it (adapters with their own cancellation), and is the documented contract for a
  host calling `cancel()` directly: cancel after starting. The old "permanent" behaviour was not
  relied on: the test driver enforces "a cancelled workflow never runs again" in its own monitor.

## 4. Self red-team (adversarial pass on this design before landing)

- **F1 — lost pre-cancel through `WorkflowChatClient` (found, fixed).** Its bridge calls
  `inner->cancel()` when the caller's context stops — including at registration, for a caller already
  cancelled. With per-run sources, the `run_workflow()` that followed replaced the source and the run
  went ahead. Fixed by §2.5 (`RunWorkflow::cancellation`), proven by C13 for the engine side.
- **F2 — nested run in flight ran to completion (found, fixed).** A `sub_workflow` dispatch calls
  `inner->run_workflow()`, which now replaces the inner's source, so an outer cancel never reached it
  and the nested run finished all its rounds first. Fixed by linking the outer token into the
  dispatch and by `cancel()` propagating to bound inners while a run is in flight. C14.
- **F3 — `cancel()` racing `run_workflow()` on the source handle.** Assigning a `std::stop_source`
  while another thread copies it is a data race. Both happen under `cancel_mutex_`; `request_stop()`
  runs on the copy, outside the lock (stop-state is itself thread-safe).
- **F4 — settling from `cancel()` racing an entry point.** `try_lock()` never queues, so `cancel()`
  either owns the run lock (no entry point can be mid-run; any that arrives queues behind it and then
  sees the run already cancelled) or leaves the run to the entry point that owns it. Every return path
  of the three entry points is covered: entry (`refuse_if_cancelled`), in-round (§2.1), and the
  wrappers' check on a `suspended` result (the early returns in `resume_workflow` that never reach
  `execute()`, e.g. one of two ports answered).
- **F5 — checkpoint of a cancelled run (found, fixed).** `close_run_for_cancel()` discards undelivered
  messages, so a restored copy continued with nothing pending and reported `completed`. `RunStateRecord`
  now carries `cancelled` (optional on read; absent = false).
- **F6 — a cancel that lands during a round that completes the run.** It now ends `cancelled` instead
  of `completed` (the outputs are still in `partial`/`output`). Accepted: "a cancel observed anywhere in
  a round ends the run cancelled" is what #156 asks for, and a timing-dependent `completed` vs
  `cancelled` is no worse than the timing-dependent `executor_failed` it replaces.
- **F7 — I2/I3.** Nothing here widens authority. `cancel()` and `RunWorkflow::cancellation` are host
  calls; model output can only stop a run if host code wires it to, as before. A cancelled run refuses
  answers; it never routes one.

## 5. Residuals (named, not fixed)

- **R1 — cancel before start.** A direct `cancel()` issued before `run_workflow()` takes the run lock
  does not cancel that run (§3). Callers that need it link a token (§2.5).
- **R2 — settling can block on the event stream.** Settling pushes structural events; the stream is
  credit-controlled and blocks a producer on a full queue. Like every entry point, `cancel()` then
  waits until the host drains it. A host that calls `cancel()` from the thread that drains the stream,
  with the stream already full, deadlocks — the same hazard `resume_workflow()` has on that thread.
- **R3 — `snapshot_record()` holding the lock.** If `cancel()` arrives while only a snapshot holds the
  run lock, the suspended run is not settled until the next entry point (which settles it on entry).
  `open_interactions()` read in between still lists the ports.
- **R4 — `sub_workflows_` read without a lock.** `cancel()` walks the bound inners from any thread.
  Binding is configuration-time (`bind_sub_workflow()` is not safe against a concurrent run either);
  binding while another thread cancels is unsupported.
- **R5 — an inner cancelled on its own.** If a host cancels a nested inner directly while the outer
  waits on it, the outer's next resume of that interaction gets a terminal inner outcome and folds it
  as a failure marker (ADR-157 S4's shape), not as a cancel of the outer. The outer was not cancelled.

## 6. Proof

`tests/workflow/test_rt_workflow_cancellation.cpp` (C1-C6 unchanged and passing):

- **C7** — a real `AgentSession` step parked in its chat client; `cancel()`; the run ends `cancelled`
  with no failed executor and the next step never runs. **Positive control:** with the in-round check
  disabled the same test reports `executor_failed` (C7, C10, C11 fail) — verified.
- **C8** — cancel while suspended: open interactions empty at once, `workflow_run_failed{cancelled}`
  on the stream; a later resume is `cancelled` + `request_port_rejected{cancelled}` and no second run
  event; `continue_workflow()` runs nothing; a checkpoint round-trip stays `cancelled`.
- **C9** — per run: the next `run_workflow()` suspends and completes normally; a cancel after a run
  completed does not poison the next.
- **C10** — cancel in the round that reaches a port: `cancelled`, port in `unopened_ports`.
- **C11** — a function step that fails because it saw the cancel: `cancelled`, not `executor_failed`.
- **C12** — cancelling an outer run suspended in a nested sub-workflow closes the inner run too.
- **C13** — a linked token stopped before `run_workflow()` cancels that run at round 0.
- **C14** — an outer cancel stops a nested run in flight at its next round.

Every workflow test plus the test driver's (W8 cancel cases) pass: `ctest -R "workflow|magentic|
test_agentengine_test_driver"` 38/38 on Windows (clang, Debug). C1-C14 passed 10/10 repeated runs.
