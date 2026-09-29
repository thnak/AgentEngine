# ADR-210 — Test driver: the workflow tool group (drive a `WorkflowSupervisor` run, answer its request ports)

- **Status**: **Proposed — implemented and proven (2026-09-29): every W check and both scenarios pass, and all 13 positive controls fail their claim (§9). Not yet Judged.** §7 supersedes §2–§4 where they differ; §8 supersedes §7 R2 (no pump).
- **Date**: 2026-09-29
- **Origin**: ADR-182 §3.4 ("`workflow_start`, `workflow_wait_for`, `request_port_list`, `request_port_resolve`
  over `WorkflowSupervisor`, with the same shape"), ADR-182 §11 Q1 (workflows after the session phases), the
  ADR-182 red-team note that `agent_session_as_executor_body` resets the tap; GitHub #109.
- **Touches**: `tools/test_driver/test_driver.hpp` (a workflow table beside the session table, compiled workflow
  fixtures, the new tools, export and replay), `tools/CMakeLists.txt` (link `agentengine_rt_workflow` if not already
  transitive), `tests/testing/test_agentengine_test_driver.cpp`, new scenarios under `tests/scenarios/`.
- **Invariants**: I1 (one run per supervisor at a time; an agent step's session is used by the workflow only), I2
  (the run's owner and every grant are driver-host constants; a fixture names executors, never authority), I3 (a
  port answer is data; its `routes` only select among edges the graph already declared, ADR-169), I4 (every resolve
  names its caller), I5 (the scripted model is the only nondeterministic input and it is recorded, with C8 request
  digests per agent step).

## 1. The problem

The driver drives one `rt::AgentSession` at a time. Nothing lets a tester drive a workflow: start a run, watch its
supersteps, answer a request port (014 §4), check where the answer routed the run, and export that as a
deterministic scenario. Workflow HITL behaviour is proven only by C++ tests written against the supervisor directly
(`tests/workflow/test_rt_workflow_supervisor_request_port.cpp`), which a tester cannot extend without writing C++.

## 2. Decision

### 2.1 Tools

Same shape as the session group. Every workflow tool takes `workflow_id` (driver-minted, `w<n>`).

| Tool | Does |
|---|---|
| `workflow_start {fixture}` | builds the graph and a fresh supervisor from a compiled workflow fixture; returns `workflow_id` and the executor list (id, kind) |
| `workflow_script_push {workflow_id, executor_id, turns}` | appends scripted model turns to one agent step (same turn shape as `model_script_push`, including `request_digest`) |
| `workflow_run {workflow_id, text}` | starts `run_workflow` with a text message on the driver's worker; returns at once |
| `workflow_wait_for {workflow_id, until}` | `settled` \| `suspended` \| `event` (kind, optional `executor_id`); ≤ 60 s |
| `request_port_list {workflow_id}` | open interactions: `interaction_id`, port executor id, the ask's text |
| `request_port_resolve {workflow_id, interaction_id, text, routes?, caller?}` | `resume_workflow`; returns at once |
| `workflow_snapshot {workflow_id}` | status, rounds, output, partial, failed executor, unopened ports, open interactions, per-step script and C8 counters |
| `workflow_events {workflow_id, since?}` | the structural event log (below) |
| `workflow_cancel {workflow_id}` / `workflow_close {workflow_id}` | cancel; close and discard |
| `scenario_export {workflow_id, name}` | the existing tool, extended to take a workflow |

`scenario_replay` and `agentengine_scenario_runner` replay workflow scenarios too (§2.5).

### 2.2 Fixtures: compiled only, agent steps are driver sessions

- Workflow fixtures are compiled into the driver, like the P1 session fixtures. File fixtures (015 `kind: Workflow`)
  are deferred: `compile_workflow_document` drops the `agent:` reference and has no `switch_case`, so the driver would
  have to invent the missing half. A later ADR can add them.
- First fixtures:
  - `wf_review`: `draft` (agent) → `review` (request_port) → switch_case on the answer's route: `approve` →
    `publish` (agent), `revise` → back to `draft`. Bounded `max_rounds`.
  - `wf_fanout`: `split` (function) fans out to `a` and `b` (agents), `fan_in` to `join` (function).
  - `wf_two_ports`: two ports open in the same round in different branches (014 §4 OQ-4).
- Each agent step is a driver `Session` (the P1 type: `DriverChatClient` scripted, test tools only), bound with
  `agent_session_as_executor_body`. A step's session is never listed as a driver session and no session tool can
  reach it (I1).
- **Refused in a workflow fixture:** real tools (ADR-208; no per-step sandbox in this ADR), live mode, and gated
  tools. A gated tool in an agent step cannot surface as an interaction today: `start_run` returns
  `run.suspended_for_approval` and the step fails as `executor_failed` (engine gap, filed separately; 014 §4 says
  port and approval are "one shape"). The driver refuses rather than build a fixture whose only outcome is that
  failure.

### 2.3 Authority and admission (I2, I3, ADR-169)

- The run's owner is a driver constant principal (`test-driver:workflow-owner`). Grants per step are the fixture's
  compiled ones (test tools need none).
- `request_port_resolve` passes `caller` to `ResumeWorkflow`. Default: the owner. A tester may name another caller
  (same syntax check as ADR-196's `approver_id`) to exercise the engine's admission refusal
  (`admission_denied`); that choice never widens anything, because admission is the engine's own predicate against
  the owner the driver fixed.
- `routes` are passed through as given. The engine accepts them only on a `switch_case`/`multi_selection` edge and
  fails an invented label as `routing_failed` (E5, IQ5). The driver adds no routing of its own.

### 2.4 Events and determinism

- **Structural stream:** `enable_event_stream` on the supervisor, drained by a driver pump thread into a
  `WorkflowMonitor` (the `SessionMonitor` shape: seq-numbered log, waiters). Its order is the supervisor's own
  (fixed index order within a round), so it is deterministic.
- **Agent-step events:** the `AgentTurn{executor_id, attempt, RunEvent}` events of the same stream, stored in one log
  **per executor**. Across executors they interleave by thread timing; within one executor they are ordered. Export
  and replay compare each executor's log on its own and never the interleaving. The session tap is not used (the
  adapter overwrites it; ADR-182 note, confirmed).
- **Drops:** the multiplex sink drops when full. The driver sizes it for its caps and counts drops; a workflow with a
  drop is not exportable (`nondeterministic_reason`), as for sessions.
- **Deadline bound:** fixtures set no `deadline_ms` (wall-clock nondeterminism). `max_rounds` only.

### 2.5 Scenario export and replay

- Format 3, `"target": "workflow"`: `fixture`, per-executor `model_turns` (each with `request_digest`), `steps`
  (`run {text}`, `resolve {interaction_id, text, routes, caller}`, `cancel`), and `expected`: the structural event
  list, each executor's event list, and the final `WorkflowResult` projection (status, output, partial,
  failed_executor, unopened_ports, open interaction ids).
- Interaction ids are deterministic (`run_id:port:<executor>:<round>`), so steps name them verbatim.
- Replay builds a fresh driver and supervisor, pushes each executor's turns, applies the steps, and compares. A
  request mismatch in step X fails as `test.replay_mismatch at <executor> model call N`.

### 2.6 Cancel and lifetime

- `workflow_cancel` calls `supervisor.cancel()`, which is permanent for that supervisor. After it the workflow only
  accepts snapshot, events, export and close; `workflow_run`/`request_port_resolve` are refused
  (`test.workflow_cancelled`) instead of returning the engine's `cancelled` again with no run.
- Caps: 4 open workflows per driver, a supervisor `worker_budget` of 2, `max_rounds` ≤ 16 in every fixture, the
  existing per-step `kMaxTurnsPerRun`, 32 open interactions listed at most.

## 3. Why this shape

- **Per-executor scripts, not one global script.** Parallel agent steps request the model in timing order; one
  shared script would hand turns out by race. Each step's script is consumed only by its own session.
- **Per-executor event logs.** The same reason, for events. The structural stream carries the cross-step order that
  the supervisor guarantees.
- **Compiled fixtures first.** The declarative compiler cannot express the review loop (switch_case) today.
- Rejected: **exposing each step's session through the session tools.** It would let a tester send to a session the
  workflow is running (I1) and would need the tap the adapter overwrites.
- Rejected: **letting a gated tool in an agent step through** to watch it fail. That tests the engine gap, not the
  workflow; the gap gets an issue and its own ADR.

## 4. Claims (to prove)

| # | Claim | Positive control |
|---|---|---|
| W1 | A run reaching a port suspends; `request_port_list` shows exactly the open ids; `request_port_resolve` resumes it and the answer's route decides the next step | route ignored by the driver → the review loop takes the wrong branch |
| W2 | An unknown or already-answered id, or an invented route, fails as the engine decides (`invalid`, `routing_failed`) with no state change | a driver pre-check that "fixes" ids or routes → the check fails |
| W3 | A caller other than the owner is refused (`admission_denied`), recorded as such | owner reused for every resolve → the check fails |
| W4 | Each agent step's C8 digests are checked on replay; a changed request in step X names X | per-step expectations merged → the failure names the wrong step or none |
| W5 | Export and replay compare the structural stream and each step's log exactly; 200 runs of `wf_fanout` export identically | comparing the interleaved stream → the repeat check fails |
| W6 | A dropped event makes the workflow non-exportable | the drop counter ignored → export succeeds |
| W7 | A step's session is unreachable from session tools; gated tools, real tools and live mode are refused in workflow fixtures | each refusal removed → its check fails |
| W8 | Cancel ends the run `cancelled`; later run/resolve are refused `test.workflow_cancelled`; caps hold | each cap or refusal removed → its check fails |

## 5. Open questions (decided with the proposed default, delegated per the owner's 2026-09-25 instruction)

- **Q1: file fixtures for workflows?** Default: not in this ADR (§2.2).
- **Q2: a tester-chosen `caller`?** Default: allowed, syntax-checked, only to exercise admission (§2.3).
- **Q3: gated tools in agent steps?** Default: refused, engine gap filed (§2.2).

## 6. Residuals

- The engine's same-round port-id collision (two deliveries to one port node in one round mint the same id) is
  inferred from code, not yet reproduced; the fixtures avoid it, and it is to be checked and filed.
- Nested sub-workflow ports and `open_interactions()` over an `unordered_map` (order) are out of scope; no fixture
  nests.

## 7. Red-team pass 1 (2026-09-29, `general-purpose` agent, no prior context) and the revisions

Result: 2 Critical, 10 Real gaps, 7 Minor; no I2/I3 hole (a tester's `caller`, `routes` and text reach the engine
only as data it admits or rejects). Verified true: the adapter overwrites and clears the session tap; `AgentTurn` is
`{executor_id, attempt, inner, path}`; structural events come only from the supervisor thread in fixed order; the
multiplex sink drops and counts; `cancel()` is permanent; admission runs first on run/resume/continue; routes fire only
on `switch_case`/`multi_selection`; `run.suspended_for_approval` is `contract` and ends `executor_failed`; `partial`
keeps one entry per executor; steps call `chat()`; `RunEvent` carries no timing; `max_rounds` bounds cycles. Every
finding is accepted.

- **C1 (Critical): cancel does not reliably end `cancelled`.** The engine checks cancel only at the top of a round.
  A cancel during an agent step reaches the session, which fails `run.canceled` (fatal), so the run ends
  `executor_failed`; a cancel in the round that opens a port ends `suspended`; a cancel while suspended does nothing;
  and which of these happens is timing. *Revision:* a `workflow_cancel` while a run is in flight marks the workflow
  non-exportable (`nondeterministic_reason`), as `session_cancel` does. A cancel while suspended is recorded by the
  driver as `cancelled_by_driver` (labelled as the driver's, not the engine's). Either way the workflow is never
  runnable again. W8 is restated below. The agent-step case is an engine finding, to be filed.
- **C2 (Critical): a bad resolve is not "no state change".** `resume_workflow` marks the port resolved and stores its
  routes before validating them: an invented-only label consumes the port and the run ends `routing_failed`; with two
  ports open the bad route is stored and the failure appears on the other port's resolve; a mixed label
  (`["approve","bogus"]`) is accepted; on `direct` or `multi_selection` edges invented labels are ignored. An unknown
  id and an admission denial each write `workflow_run_failed` into the event stream while the run stays suspended.
  *Revision:* W2 is split per case (below) and asserts exactly this behaviour; `wf_two_ports` gets the delayed case.
  The consume-before-validate and the `run_failed` event on a still-suspended run are engine findings, to be filed.
- **R1: the sink cannot be sized** (fixed 1,024, no setter; the drop count is per supervisor). *Revision:* the pump
  drains promptly; export reads `multiplexed_dropped_count()`; W6 uses an in-process seam that pauses the pump.
- **R2: the pump.** The stream is a non-blocking try-pop, never closed; a full structural channel blocks the
  supervisor; the final structural event can be popped while agent events are still queued. *Revision:* one pump per
  workflow polls with a bounded back-off; after the run job returns (every step joined), the pump drains both queues
  to empty before `settled`/`suspended` is signalled; close cancels the stream first so a blocked push returns.
- **R3:** waiters settle on the run job's returned `WorkflowResult`, never on `workflow_run_failed` events.
- **R4: reads while running race.** `open_interactions()`, `usage()` and friends are unlocked; `snapshot_record()`
  blocks for the whole run. *Revision:* while running, `workflow_snapshot` and `request_port_list` answer from the
  monitor (`partial: true`); the driver keeps its own copy of the last `WorkflowResult`.
- **R5: one run per workflow.** A second `run_workflow` wipes the ports and reuses step sessions against the
  adapter's one-session-per-run contract. *Revision:* `workflow_run` is refused after the first
  (`test.workflow_already_run`).
- **R6: scripts.** Pushing during a run races, and an exhausted script is `contract` class (never retried), so the
  step fails. *Revision:* `workflow_script_push` only while idle or suspended; the guide says to push each step's
  turns before `workflow_run`/`request_port_resolve`; the snapshot shows each step's remaining turns.
- **R7: grants.** The adapter sets the session's capabilities from `ctx` on every call, so each step's grant is
  `contexts[i].capabilities`. *Revision:* each step gets an empty `grant_root({})` owned by the workflow entry, and an
  empty ceiling; claim W10.
- **R8: ids.** Interaction ids are `<graph.id>:run:<n>:port:<exec>:<round0>`: `graph.id` is the fixture name, and
  each workflow has a fresh supervisor, so ids repeat across record and replay. Step sessions get fixed ids
  (`<fixture>/<executor>`) passed to `DriverChatClient` for `normalize_ids`; `AgentTurn` payloads are normalised per
  step. Step sessions are not built by `make_session` (no `sessions_` entry, no `kMaxSessions` slot, no own pool).
- **R9: W5's control could not fail** with instant models. *Revision:* a test-only latching backend forces `b`
  before `a` in one run and `a` before `b` in another; the check that per-step logs match holds, and a comparison of
  the interleaved stream fails.
- **R10: lifetime.** The adapter captures the session by reference. *Revision:* close and driver exit do: `cancel()`,
  join the run job, destroy the supervisor (joins its pool), cancel the stream, stop the pump, then destroy the step
  sessions. Threads per workflow: 2 pool, 1 run job, 1 pump; 4 workflows is about 16 threads.
- **M1:** a resolve's message is `user_message(text)` (user role and origin, untainted); tester-supplied structure is
  refused. **M2:** `caller` is an id only (fixed tenant, no `on_behalf_of`); `workflow_run` passes the owner
  explicitly; W3 also checks the port is still open after a denial, the owner can then resolve it, and
  `admission_denied_count()` rose. **M3:** `worker_budget` does not affect results (fixed index order). **M4:** the
  same-round port-id collision is confirmed by reading; to be reproduced and filed. **M5:** W1's control yields
  `routing_failed`, reworded. **M6:** per-step `model_requests` and `--stamp-requests`; no CMake change
  (`agentengine_rt_workflow` comes through `agentengine::core`). **M7:** `workflow_start` never falls back to a file.

Revised claims (replacing §4):

| # | Claim | Positive control |
|---|---|---|
| W1 | a run reaching a port suspends; `request_port_list` shows exactly its ids; a resolve with route `approve` runs `publish`, `revise` runs `draft` again | routes dropped by the driver → the resolve fails `routing_failed` and the check fails |
| W2 | per case: unknown id → `invalid`, port still open, one `workflow_run_failed{invalid}` event; invented-only label → `routing_failed`, `failed_executor = review`; mixed label → accepted; with two ports, a bad route surfaces on the other port's resolve | a driver pre-check that rewrites ids or routes → a case fails |
| W3 | another caller → `admission_denied`; the port stays open; the owner then resolves it; the denial counter rose | the driver always passes the owner → the check fails |
| W4 | each step's C8 digests are checked on replay; a changed request in step X names X | one shared expectation set → the failure names the wrong step |
| W5 | per-step logs and the structural stream match across forced opposite interleavings | comparing the interleaved stream → fails under the latch |
| W6 | a dropped event makes the workflow non-exportable | the drop count ignored → export succeeds with the pump paused |
| W7 | step sessions are unreachable by session tools; gated tools, real tools, live mode and file fallback are refused for workflows | each refusal removed → its check fails |
| W8 | a cancel in flight makes the workflow non-exportable and never runnable again; a cancel while suspended is `cancelled_by_driver` | each refusal removed → its check fails |
| W9 | close and driver exit with a run in flight or suspended leave no thread and no use-after-free | exit order reversed → the check (or ASan) fails |
| W10 | every step's grant is empty | a non-empty step grant → the check fails |
| W11 | run, resolve and script-push while running are refused | each refusal removed → its check fails |
| W12 | after `settled`, every step's log is complete | the post-join drain removed → the check fails |

## 8. Implementation findings (2026-09-29)

- **No pump thread.** The structural stream's capacity is set to 8,192 (`kWorkflowStructuralCapacity`), above
  what any fixture can emit under its round cap, so the supervisor never blocks on it. Both queues are drained on
  the driver's job thread right after each `run_workflow`/`resume_workflow` call returns, when every step has been
  joined. That removes §7 R2's pump and its lifetime rules: a workflow has 2 supervisor workers and 1 job thread.
  The step-event queue (the engine's multiplex sink, fixed at 1,024) still drops when a step emits more than that
  in one call; W6 shows the drop is counted and export refused.
- **Port ids are 1-based by round** (`wf_review:run:1:port:review:1`, then `:3` after a revise), not 0-based as §7
  R8 read the code. They repeat across record and replay, which is what matters.
- **The engine behaviour W2 records** (§7 C2), all observed through the driver: an unknown id returns `invalid`,
  leaves the port open and still logs `workflow_run_failed`; an invented-only route consumes the port and ends
  `routing_failed` at `review`; a mixed route (`approve` plus `bogus`) completes; with two ports, a bad route on
  `p1` is stored while the run stays suspended and the failure surfaces on `p2`'s resolve, naming `p1`.
- **A cancel while a step is running ended `executor_failed`** (W8's run), not `cancelled`, as §7 C1 predicted.
- **`caller` is checked with `is_attributable_id`** (no control characters, not only invisible characters) and a
  128-byte cap; spaces are allowed, as for ADR-196's `approver_id`.
- **The scenario runner does not stamp workflow scenarios** (`--stamp-requests`): export always writes every
  step's digests.
- **W9 is a smoke check.** Closing a suspended workflow and destroying the driver with a run in flight return
  (0.2 s, the gate the test opens); no ASan run on this Windows build.
- **A gated tool in a step is denied, not a step failure** (probe, temporary fixture): the driver runs steps with
  `suspend_for_approval` off, so a `gated_echo` call returns "approval required and not granted" and the run
  completes. No approval inside a step can reach the tester either way, which is why the fixture rules refuse gated
  tools (§2.2's "fails the step" applies only with suspension on; not reproduced here).
- **The same-round port-id collision is real** (probe, temporary fixture `split` → `x`, `y` → one port): two open
  interactions share `wf_collide:run:1:port:port:2`; the first resolve closes one, the second resolve of that id is
  `invalid`, and the other port can never be answered. No compiled fixture routes two same-round deliveries to one
  port. Engine issue drafted.

## 9. Evidence and positive controls (2026-09-29)

`tests/testing/test_agentengine_test_driver.cpp`, section W (all pass): fixture rules (W7, W8 round cap); W1 approve
and revise on `wf_review`, two ports routed and fanned in on `wf_two_ports`; W2 per case (unknown id, invented-only,
mixed, delayed on two ports); W3 admission denial, open port, counter, owner resolve, bad caller id; W4 export and
replay (3 requests checked), a changed answer names `publish`, a changed input names `draft`, a changed route
fails; W5 a-first and b-first (forced by a latching backend) export the same scenario and replay; W6 760 echo calls in one step:
524 step events dropped and export refused; W7 step sessions unreachable, session fixtures refused; W8 cancel while
suspended (`cancelled_by_driver`, exports and replays) and while running (`executor_failed`, non-exportable); W9
close and driver exit with a run in flight (smoke); W10 both steps ran under the workflow's grant holding nothing;
W11 push and port listing refused while running, snapshot partial. Scenarios `tests/scenarios/
workflow_review_approve.json` and `workflow_two_ports_go.json` replay in ctest.

Each mutant was applied to `tools/test_driver/test_driver.hpp`, the driver test rebuilt and run, and the named check
seen to fail; the header was restored from a copy and its sha256 checked.

| Claim | Mutant | Seen to fail |
|---|---|---|
| W1 | resolve drops the routes | W1 approve, W1 revise |
| W2 | the driver pre-checks the interaction id | W2 unknown id |
| W3 | the driver always passes the owner | W3 denial, W3 bad caller |
| W4 | the replay does not name the step | W4 publish, W4 draft |
| W5 | step events carry their arrival order | W5 same export, W5 replay |
| W6 | export ignores the drop count | W6 |
| W7 | gated tools allowed in steps | W7 gated |
| W7 | real tools allowed in steps | W7 real tool |
| W8 | a cancelled workflow can still be resolved | W8 resolve after cancel |
| W8 | a cancel while running is not marked | W8 running cancel |
| W10 | steps get a non-empty grant | W10 |
| W11 | script push allowed while running | W11 |
| W12 | no drain after the call returns | W1 structural log, W1 draft's log |

W9 has no control (smoke only; §8).

