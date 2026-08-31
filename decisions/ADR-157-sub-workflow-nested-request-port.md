# ADR-157: `executor_kind::sub_workflow`'s real runtime bridge, with nested `request_port`
# proxying (issues #33, #38)

## 1. The question and scope decision

Two open issues, closed together as one mechanism:

- **#33**: `executor_kind::sub_workflow` was declared in `Executor` (`workflow/graph.hpp`) and named
  in RFC 014 §1, but `check_workflow_executable()` refused ANY graph containing one, unconditionally.
  No runtime bridge existed.
- **#38**: `workflow_as_executor_body()` (ADR-150) deliberately refuses to wrap any inner graph
  containing a `request_port` node, because `ExecutorBody`'s contract (one synchronous call, no
  "still pending" concept) cannot express suspension. MAF's own equivalent
  (`WorkflowHostExecutor`, `_pendingResponsePorts`, `IExternalRequestSink`) genuinely supports this.

**Scope decision**: build this as `executor_kind::sub_workflow`'s real runtime bridge, not as an
extension of `workflow_as_executor_body()`'s synchronous `ExecutorBody` contract. `ExecutorBody` is
load-bearing for every other node kind in this engine; suspension is a structural round-loop
concern the engine must know about directly, matching how `request_port` already gets special
structural treatment rather than being expressed through a body's return value.
`workflow_as_executor_body()` (ADR-150) is completely unaffected by this work.

## 2. Two red-team passes, and what each one found

### Pass 1 — design draft, before any code existed (fresh agent, zero prior context)

Found the first draft's central mechanism (extending `OpenPort` with an `inner_interaction_id`
field, gathering sub_workflow deliveries as a loosely-described "third bucket") was unsound on
direct trace against the real `execute()`/`resume_workflow()` code:

1. **MUST-FIX — the I1 concurrency hazard was real**: two sub_workflow deliveries to the SAME bound
   inner supervisor in one outer round (ordinary topology, nothing prevented it) would dispatch
   concurrently into the same `WorkflowSupervisor::run_workflow()`, the identical class of hazard
   ADR-150's own red team found for a structurally similar case. **Fixed**: generalized the
   already-proven OQ-19 same-round-duplicate-delivery quarantine (previously scoped to
   `executor_kind::agent` only) to also cover `sub_workflow`-kind — no new primitive.
2. **MUST-FIX — extending `OpenPort` was actively wrong, not just inelegant**: `resume_workflow()`
   writes `port->response = request.response` (the raw human answer) UNCONDITIONALLY before any
   marker field could be checked — a naive implementation would fold a nested question's raw human
   answer as if it were the sub-workflow's own completed output, silently skipping whatever the
   inner workflow does afterward. Worse, this confusion would survive a checkpoint/restore cycle
   undetected. **Fixed**: `OpenPort` is not touched at all. A completely separate
   `pending_sub_workflows_` tracker (keyed by the outer interaction_id) holds suspended state; an
   `OpenPort` is only ever constructed AFTER the derived value is already computed, never mutated
   into after the fact.
3. **MUST-FIX — "third dispatch bucket, folded into existing reply-collection" understated the real
   diff**: five separate points in `execute()`'s round loop needed explicit handling. Made explicit
   in the revised design (§3 below), not left hand-wavy.
4. **MUST-FIX — a sub_workflow suspension in the same round as an unrelated executor's failure would
   silently orphan the live, real inner suspension.** **Fixed**: `pending_sub_workflows_` is a
   persistent member, never cleared by an unrelated round abort; `finish()`'s own
   `open_interactions()` population is now unconditional, not gated behind
   `status == suspended`.
5. Also settled: multi-level nesting is not a coroutine-reentrancy hazard (each `WorkflowSupervisor`
   owns its own independent `ThreadPool`), but IS a real, unbounded-thread-consumption hazard
   (`drive()`'s busy-spin loop, compounding per nesting level) — addressed with a real, bounded,
   CLAUDE.md-compliant test (§5 below) rather than an unbounded-nesting redesign.
6. Settled: the multiplexed ADR-152 event bridge does NOT compose for free across nesting — only
   the structural `request_port_opened`/`_resolved` signal does (this mechanism reuses those event
   kinds for a sub_workflow suspension, matching an ordinary `request_port`'s own shape exactly).
   Forwarding an inner graph's own per-token streaming outward is real, unbuilt future work — not
   attempted here.

Full findings: `docs/planning/sub-workflow-nested-request-port-design-draft.md`.

### Pass 2 — adversarial empirical verification during implementation

The revised design's central safety claim (the generalized quarantine prevents genuine concurrent
dispatch to the same inner supervisor) was proven, not just reasoned about — and the FIRST attempt
to prove it gave a false negative that had to be caught and fixed:

- An initial concurrency test used `fan_in` edges to converge two deliveries onto the same
  sub_workflow node. **This was a test bug, not a design bug**: `route_from()`'s `fan_in` handling
  MERGES converging replies into ONE delivery entry before dispatch — so the "duplicate" never
  actually reached the quarantine at all, and the quarantine-bypass mutation test showed zero
  crashes across 55 trials (including a deliberately widened 15ms race window) purely because there
  was never anything to race in the first place. An explicit atomic-counter instrumentation
  confirmed: `genuine concurrent overlap observed = NO`.
- Rebuilt the test using `direct` edges (which do NOT merge — each firing edge unconditionally
  produces its own delivery entry), matching the quarantine's own real target case exactly as the
  original agent-kind OQ-19 comment describes it ("two ordinary, non-fan_in edges converging"). With
  genuine duplicate deliveries: the quarantine-bypass mutation now **segfaulted 20/20 runs**,
  confirming the hazard is real and the fix is load-bearing. Reverted; the same test passes cleanly
  across 15 repeated trials with the quarantine restored, confirming zero genuine concurrent overlap
  occurs once it's back in place.
- A second, structural bug was found and fixed during this same pass, independent of the red team's
  findings: the round loop's own bottom-of-loop suspend check (`if (!ports_.empty()) { status =
  suspended; break; }`) never consulted `pending_sub_workflows_` — a round whose ONLY "open" thing
  was a freshly-suspended nested interaction (no ordinary `ports_`, `state_.pending` now empty)
  fell through the `while` loop's own exit condition and silently reported `completed` instead of
  `suspended`. Fixed by extending that one check.

## 3. The accepted design

- `WorkflowSupervisor::bind_sub_workflow(executor_id, shared_ptr<WorkflowSupervisor>)` — an opt-in
  call after `initialize()`, mirroring `set_checkpoint_hook()`. Recomputes `valid_` immediately from
  a cached `valid_base_` (everything valid_ depends on except sub_workflow binding) plus a fresh
  `sub_workflow_kind_nodes_are_bound()` check, so a caller does NOT need to call `initialize()` a
  second time after binding every sub_workflow node.
- `check_workflow_executable(Workflow const&, vector<EffectContext> const&)` (`workflow/graph.hpp`)
  no longer unconditionally refuses `sub_workflow`-kind — mirrors agent-kind's own two-layer shape:
  this graph-only function accepts the kind unconditionally (no capability-ceiling enforcement,
  matching `workflow_as_executor_body()`'s own "zero implicit capability flow" I2 answer);
  `WorkflowSupervisor::sub_workflow_kind_nodes_are_bound()` is the separate structural check that
  actually refuses an unbound node. The contexts-FREE overload (`check_workflow_executable(Workflow
  const&)` alone) is unchanged — still conservatively refuses both agent- and sub_workflow-kind,
  since it has no way to see runtime bindings at all.
- Dispatch (`execute()`): `sub_workflow_deliveries` gathered as its own list, own generalized OQ-19
  quarantine, own retry loop (`run_sub_workflow_job()`, a new sibling to `run_executor_job()` — no
  `ExecutorBody`/`bodies_[idx]` exists for this kind, exactly like `request_port`). A resolved
  (completed or terminally-failed) reply is appended onto `exec_deliveries`/`replies` before the
  existing fold loop runs, so every downstream loop (fold/merge-hook/routing/stall-report/eventing)
  needed ZERO new branches — a resolved sub_workflow entry looks exactly like an ordinary
  `exec_deliveries` entry to all of them. A PENDING (suspended) reply is never folded at all — it
  mints an outer `Interaction` and records a `PendingSubWorkflow{interaction, executor_index,
  inner_interaction_id}` entry, keyed by the outer interaction_id, in `pending_sub_workflows_`.
- Resume (`resume_workflow()`): checks `pending_sub_workflows_` FIRST, before the ordinary `ports_`
  scan at all. Found → erase, call `inner->resume_workflow(...)`, inspect the result: `completed`/
  failed → construct a fresh, ordinary `OpenPort` (the derived value computed BEFORE construction,
  never written into a pre-existing one) and fall through to the existing `execute()` call exactly
  like an ordinary port resolution; `suspended` again → mint a new outer interaction, re-track,
  return early exactly like "other ports still unresolved" already does. Not found → falls through,
  completely unmodified, to the existing `ports_` scan.
- `open_interactions()` unions `ports_` (unresolved) with `pending_sub_workflows_` (all entries,
  always "open" by construction). `finish()` populates `r.open_interactions` unconditionally, not
  gated behind `status == suspended` — closing the round-abort-orphan hazard.
- Checkpoint/resume: `RunStateRecord` is genuinely unchanged. `pending_sub_workflows_` is never
  persisted — a restored instance starts with it empty, which is precisely what makes
  `resume_workflow()`'s fail-closed behavior against a stale pending-sub-workflow interaction_id
  work: it falls through to "not found" in both trackers, returning `workflow_status::invalid`,
  never silently misrouting. Two layers of protection: `WorkflowCheckpointManager::resume_or_start()`
  already refuses without `acknowledge_agent_history_reset` for any graph containing a
  sub_workflow-kind node (confirmed by direct trace); the fail-closed `resume_workflow()` behavior
  above is a second, independent layer that also protects a host that bypasses the manager and
  calls `restore_from_record()` directly.
- I1/I2/I3: `inner->run_workflow()`/`resume_workflow()` calls only ever happen while the OUTER
  supervisor's own `run_mutex_` is held (both dispatch paths require it), so at most one thread can
  ever be driving a given `inner` from THIS supervisor at a time; the OQ-19-generalized quarantine
  additionally prevents two deliveries in the SAME round from ever targeting the same `inner` via
  the same executor_index. Capability sourcing mirrors `workflow_as_executor_body()`'s own I2 answer
  exactly (zero implicit flow — `inner`'s own executors run under whatever `EffectContext`s were
  passed to `inner->initialize(...)` before binding). The routed-back human response crosses the
  boundary as a plain `Message`, identical to an ordinary `request_port` response — no new trust
  decision.

Full design: `docs/planning/sub-workflow-nested-request-port-design-draft.md`.

## 4. What this ADR does not claim

- Does not touch `workflow_as_executor_body()` (ADR-150) — confirmed unaffected by both the red team
  and direct implementation review.
- ~~Does not forward an inner graph's own multiplexed ADR-152 events (agent-kind/moderator
  streaming) to the outer's `enable_event_stream()` consumer~~ **RESOLVED — see claims 18-22.** A
  nested run's own `agent_turn_event`/`moderator_stream_delta` events now surface live on the
  outer's stream, tagged with the full lineage of sub_workflow executor_ids from outer to
  originating node (a new `path` field, `workflow_event.hpp`) — wired transiently at DISPATCH time
  (`ScopedForwardedEventSink`, `workflow_supervisor.hpp`), never at bind/enable time, per an
  independent red-team pass that found bind-time propagation would have produced wrong paths under
  this codebase's own real (bottom-up) construction order AND a genuine new cross-thread data race.
  The structural bucket still does not forward beyond `request_port_opened`/`_resolved` — see
  `docs/planning/nested-workflow-event-forwarding-design-draft.md` §2 for why that bucket
  structurally cannot share this mechanism (a single-writer channel, not a multi-producer sink).
  **This closes the last of ADR-157's four originally-named residuals — issue #42 is now fully
  closed.**
- ~~Does not redesign `ThreadPool` sharing/ceilings across nested `WorkflowSupervisor` instances~~
  **RESOLVED — see claims 15/16.** A static per-instance worker budget (`WorkflowSupervisor(
  worker_budget)`), a fail-closed `split_worker_budget()` helper, and a mandatory, automatic
  `nesting_depth_`/`kMaxNestingDepth` cap now bound real, live OS thread count across an arbitrarily
  deep (up to the cap) nested tree — not just the specific bounded 2-level/3-wide shape originally
  tested.
- Does not give `sub_workflow`-kind nodes anything beyond `TypedExecutor<In,Out>`'s existing
  compile-time type check.
- Does not claim `WorkflowResult::partial` discloses a suspended sub_workflow's own nested
  question — this matches EXISTING `request_port` behavior (no `partial` entry until resolved), not
  a new gap.
- ~~Does not enforce that a single `inner` instance is bound to at most one executor_index~~
  **RESOLVED — see claim 13.**
- ~~Does not attempt or claim safety for 3+ level nesting~~ **RESOLVED — see claim 14.**
- Does not persist `worker_budget`/`nesting_depth_` through checkpoint/resume — a host resuming a
  checkpointed run must reconstruct `inner` with the SAME budget it originally used, or the
  effective ceiling silently drifts across a resume cycle. Named, not solved (mirrors the
  already-disclosed "bindings are not checkpoint-durable" limitation for `sub_workflows_` itself).
- Does not make `split_worker_budget()` recursive/depth-aware across multiple nesting levels — a
  caller composing several levels calls it once per level, explicitly.

## 5. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | An unbound `sub_workflow` node refuses at `initialize()`. | CORRECT | `test_rt_workflow_sub_workflow.cpp` S1 |
| 2 | `bind_sub_workflow()` alone flips `valid_` true, with no second `initialize()` call needed. | CORRECT | S2 |
| 3 | A basic nested suspend/resume/complete round-trip works end to end: the outer suspends on the inner's own `request_port`, the outer caller resumes with a plain answer (no knowledge of the inner's own interaction_id), the inner genuinely completes, and the outer graph continues past the sub_workflow node with the inner's real result. | CORRECT | S3 |
| 4 | A terminally-failing inner run (never completes) folds as an ordinary `ok=false` reply, routed through the existing `EdgeFailurePolicy` machinery unchanged. | CORRECT | S4 |
| 5 | A sub_workflow suspension in the same round as an unrelated executor's routing failure does not silently orphan the live nested interaction — `open_interactions()` still discloses it even though the round's own status is a failure. | CORRECT | S5 |
| 6 | A restored `WorkflowSupervisor` (pending_sub_workflows_ never persisted) fails closed (`invalid`) on a stale pending-sub-workflow interaction_id — never silently misroutes the raw human answer as the sub-workflow's own completed output. | CORRECT | S6 |
| 7 | The generalized OQ-19 quarantine is genuinely load-bearing: with it bypassed, two GENUINE duplicate deliveries (direct edges, not fan_in-merged) to the same sub_workflow executor_index reliably crash. With it in place, the same scenario never crashes and never produces genuine concurrent overlap. | CORRECT (adversarially verified) | 20/20 segfaults with the quarantine bypassed (direct-edge duplicates); 15/15 clean with it restored, confirmed via atomic-counter instrumentation that zero overlap occurs |
| 8 | A bounded, real nesting shape (2 levels, 3-wide fan-out at the middle level, each branch its own independently-bound inner supervisor) completes in bounded wall-clock time. | CORRECT | S8 |
| 9 | The example (`examples/27_sub_workflow_nested_request_port.cpp`) proves the same properties end-to-end against a realistic "approval pipeline wrapped inside a publishing workflow" scenario, mirroring MAF's `sub_workflow_request_interception.py` in spirit. | CORRECT | Run directly: suspends, resumes, completes with the inner's real resolution routed through the outer's own downstream node |
| 10 | Every pre-existing test still passes; the wider repo-wide suite is unaffected except for one test that needed updating to match the intentionally-changed `check_workflow_executable()` contract. | CORRECT | Full `ctest -C Debug`: 310/312 passed. `test_workflow_agent_executor_gate`'s own G6 case asserted the OLD "sub_workflow refused unconditionally by the contexts-aware overload" behavior this ADR deliberately changes — updated to assert the new, intentional contract, confirmed passing. The two failures are both pre-existing and unrelated: `test_reference_agent_task_corpus` (the same long-documented matplotlib/pandas environment gap named in ADR-149's own row) and `test_rt_spawn_cost_budget` (confirmed genuinely flaky by direct rerun — 2/3 clean, 1/3 fails on its own T2 concurrency assertion under real 8-thread contention — a pre-existing, unrelated subsystem this pass never touches). |
| 11 | The full project builds clean, including under `-Werror`/`/WX`. | CORRECT | Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC), exit code 0, zero errors |
| 12 | **Follow-up** (post-ship, closing §4's own named-but-unenforced caller contract): binding a non-`sub_workflow`-kind `executor_id` is refused — the node stays unbound, not silently mis-targeted. This was already CLAIMED by `bind_sub_workflow()`'s own comment at ship time but never actually checked in code; caught during a later audit, not by any red team. | CORRECT (fixed) | `test_rt_workflow_sub_workflow.cpp` S9 |
| 13 | **Follow-up**: binding the SAME `inner` instance to a SECOND `sub_workflow` executor_index in one graph is refused — closing the exact gap §4 disclosed ("does not enforce (only documents) that one `inner` instance must be bound to at most one executor_index"), which would otherwise defeat the OQ-19-generalized quarantine's own per-executor_index concurrency guarantee. A genuinely distinct second `inner` instance still binds normally. | CORRECT (fixed) | S10 |
| 14 | **Follow-up** (issue #42 item 1): the mechanism genuinely generalizes to 3-level nesting (outer → a 3-wide fan-out middle level → each branch its own further-nested sub_workflow), not just claimed by extrapolation from the 2-level shape claim 8 proves — completes correctly, in bounded wall-clock time. | CORRECT | S11 |
| 15 | **Follow-up** (issue #42 item 2): a `WorkflowSupervisor(worker_budget)`-constructed nested tree's real, live OS worker-thread count never exceeds the declared per-level budget sum, measured via a process-wide `live_worker_thread_count()` gauge, both immediately after constructing the whole tree and again after the run completes. | CORRECT | S12 |
| 16 | **Follow-up**: claim 15's own check is genuinely load-bearing, not vacuous — adversarially verified by mutation. | CORRECT (adversarially verified) | Temporarily changed the constructor to `pool_(0 * worker_budget)` (ignoring the requested budget, forcing every instance to the system default). Rebuilt and reran: live thread count after constructing the same 5-instance tree was 21 against a declared ceiling of 7 — a real, measured breach, not a hypothetical one. The mutation ALSO caught a real bug in claim 15's own first implementation: an earlier version of S12 sampled the peak via a background thread racing a fast, synchronous `drive()` call, which could legitimately take zero samples before the run finished and silently pass regardless of the true peak — the same class of false-negative ADR-157 Pass 2 (§2 finding 6 / this table's claim 7) already found once with a `fan_in`-merged test topology. Fixed by replacing the racy sampler with a deterministic direct sample: `ThreadPool` creates its worker `jthread`s once, in its own constructor, and they live for the whole `WorkflowSupervisor` instance's lifetime (never created/destroyed per round or per dispatch), so a sample taken any time after full construction (while every instance is still in scope) already reflects the true peak. Reverted the mutation; reran — clean, 34/34 checks passing across the whole file. |
| 17 | **Follow-up**: the mandatory `kMaxNestingDepth` cap (=16) — the load-bearing precondition that makes claim 15's budget arithmetic a genuine ceiling at all, not merely "recommended defense in depth" — actually refuses a bind at its declared boundary, not just claimed. Tested via a genuinely bounded configuration (`worker_budget=1` per level, an 18-level chain), never risking unbounded real resource use even to prove the cap works, per CLAUDE.md's own machine-safety discipline. | CORRECT | S13: binding succeeds through exactly depth 16, refused at depth 17 |
| 18 | **Follow-up** (issue #42 item 3): single-level forwarding — a nested run's own `moderator_stream_delta` is observed on the OUTER's `enable_event_stream()` consumer; `executor_id` keeps its existing meaning (the real originating node's own local id); `path` carries exactly the outer's own sub_workflow node id; the real payload content survives the hop unchanged. | CORRECT | `test_rt_workflow_event_stream.cpp` W10 |
| 19 | **Follow-up**: multi-level forwarding — at 3 levels of nesting, the innermost node's own event carries the FULL path (both intermediate sub_workflow node ids, in order), not just the immediate parent — proving the mechanism's claimed automatic cascade (no recursive method, no cascade code; each level just reads its own current, freshly-wired state) actually works past 2 levels. | CORRECT | W11 |
| 20 | **Follow-up — the central property this whole mechanism exists to provide**: a nested node's own event is observed on the outer's stream WHILE the inner run is STILL genuinely in progress, not merely present once the whole nested run already finished — ruling out the design draft's own rejected "batch after drive() returns" alternative (Approach 3) having shipped by accident. Proven deterministically, not by a timing race: a deliberately-blocked inner node (parked on a bounded-wait `condition_variable`) is driven on a background thread; the main thread polls and observes the event while a `run_finished` flag is STRUCTURALLY guaranteed still false (it can only become true after the test itself releases the block, which has not happened yet at the assertion point). | CORRECT | W12 |
| 21 | **Follow-up**: bind/enable construction order (bind-then-enable vs. enable-then-bind) produces identical forwarding for the same tree shape — confirming the dispatch-time mechanism genuinely has no order dependency, unlike the bind-time-propagation design the red team rejected (claim 22). | CORRECT | W13a/W13b |
| 22 | **Follow-up — the mechanism's central design decision was independently red-teamed BEFORE implementation, and found genuinely wrong as first drafted**: a bind-time/enable-time propagation design (`adopt_multiplex_sink()`, cascading at `bind_sub_workflow()`/`enable_event_stream()` time) would have produced WRONG `path` values under this codebase's own real bottom-up construction order (every real nested tree, including S8/S11, binds innermost-first — a bind-time-computed prefix can only reflect what an ancestor's own prefix was AT THAT MOMENT, before any of ITS OWN later binds happen) — traced by the red team to break claim 19's own validation scenario specifically. The SAME pass also found a genuine, NEW cross-thread data race the bind-time design would have introduced: `enable_event_stream()`'s own existing doc comment already blesses being called more than once, and nothing prevents `bind_sub_workflow()`/`enable_event_stream()` from running on one thread while a previous run against the same `inner` is mid-flight on another — a bind-time cascade would plainly write into a live object's members while its own dispatch loop concurrently reads them, a hazard that cannot happen in the shipped code today (nothing reassigns `multiplex_sink_` post-construction at all). **Both findings were confirmed genuine, not hypothetical, and the mechanism was redesigned before any of it shipped** — dispatch-time wiring (`ScopedForwardedEventSink`, wired immediately before and restored immediately after each nested `drive()` call, on the calling thread) closes both by construction: nothing is ever precomputed at bind/enable time, and the write always immediately precedes, on the same thread, the one call whose own worker threads are the only readers. | CORRECT (caught pre-implementation) | Independent red-team pass (fresh agent, zero prior context); full findings in `docs/planning/nested-workflow-event-forwarding-design-draft.md` §3b |
| 23 | **Follow-up**: claims 18-21's own forwarding checks are genuinely load-bearing, not vacuous — adversarially verified by mutation. | CORRECT (adversarially verified) | Temporarily short-circuited `ScopedForwardedEventSink`'s constructor (`if (false && sink)`, disabling all forwarding). Rebuilt and reran: every forwarding-dependent check in W10/W11/W13a/W13b failed (8 failures) exactly as expected; W12's own presence check failed too (its "still in progress" check passed vacuously in that state, as expected — the presence check failing is what correctly signals the mutation broke forwarding, not a flaw in that proof). Reverted; rebuilt; reran — clean, 53/53 checks passing in `test_rt_workflow_event_stream.cpp` again. |

## 6. Files changed

**New:**
- `docs/planning/sub-workflow-nested-request-port-design-draft.md`
- `tests/test_rt_workflow_sub_workflow.cpp`
- `examples/27_sub_workflow_nested_request_port.cpp`

**Edited:**
- `include/agentengine/workflow/graph.hpp` — narrowed `check_workflow_executable()`'s
  contexts-aware overload's `sub_workflow` refusal (contexts-free overload unchanged).
- `include/agentengine/rt/workflow_supervisor.hpp` — `bind_sub_workflow()`, `valid_base_`,
  `sub_workflows_`, `pending_sub_workflows_`, `PendingSubWorkflow`, `ExecuteReply::
  pending_sub_workflow_inner_interaction_id`, `drive()`, `run_sub_workflow_job()`, the generalized
  OQ-19 quarantine, `execute()`'s sub_workflow dispatch block, the bottom-of-round suspend check
  fix, `resume_workflow()`'s new pending-check branch, `finish()`'s unconditional
  `open_interactions()`.
- `tests/test_workflow_agent_executor_gate.cpp` — G6 updated to match the intentionally-changed
  contract (see claim 10).
- `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new target registrations.

**Follow-up edits** (closing three of ADR-157's own four named residuals — see claims 12-17):
- Bind-contract enforcement (claims 12/13): further changes to `bind_sub_workflow()` (the missing
  `kind` check, and the new duplicate-`inner`-instance refusal); `test_rt_workflow_sub_workflow.cpp`
  gained S9, S10.
- 3-level nesting (claim 14): `test_rt_workflow_sub_workflow.cpp` gained S11.
- `ThreadPool` resource budgeting (issue #42 item 2; claims 15-17): `include/agentengine/rt/
  thread_pool.hpp` gained `split_worker_budget()` and `live_worker_thread_count()` (plus the
  `LiveWorkerCountGuard` instrumentation each worker thread now carries); `workflow_supervisor.hpp`
  gained the `WorkflowSupervisor(worker_budget)` constructor, `max_nesting_depth()`, `nesting_depth_`,
  and `kMaxNestingDepth`, with `bind_sub_workflow()`'s depth-ceiling refusal wired in;
  `test_rt_workflow_sub_workflow.cpp` gained S12, S13; `test_rt_thread_pool.cpp` gained T7
  (`split_worker_budget()`) and T8 (`live_worker_thread_count()`). Design:
  `docs/planning/nested-workflow-threadpool-budget-design-draft.md` (red-teamed, now implemented and
  proven).

**Follow-up edits, pass 3** (closing the last of ADR-157's own four named residuals — issue #42
item 3, claims 18-23):
- `include/agentengine/workflow/workflow_event.hpp` — `AgentTurn::path`/`ModeratorDelta::path`.
- `include/agentengine/rt/workflow_supervisor.hpp` — `event_path_prefix_` member,
  `ScopedForwardedEventSink` (new private nested RAII class), `run_executor_job()`'s new
  `path_prefix` parameter, `run_sub_workflow_job()`'s new `sink`/`path_prefix` parameters and
  guard-wrapped `drive()` call, `execute()`'s two dispatch sites (ordinary exec_deliveries +
  sub_workflow) now passing the new parameters, `resume_workflow()`'s pending-sub-workflow branch's
  guard-wrapped `drive()` call.
- `tests/test_rt_workflow_event_stream.cpp` — W10-W13, plus `<atomic>`/`<condition_variable>`/
  `<mutex>`/`<thread>` includes.
- Design: `docs/planning/nested-workflow-event-forwarding-design-draft.md` (independently
  red-teamed BEFORE implementation — two MUST-FIX findings, both closed by redesigning the
  mechanism before any of it shipped, see claim 22 — then implemented and proven).

**All four of ADR-157's originally-named residuals are now closed. Issue #42 is fully closed.**

## Status

**Proposed — implemented across three passes, ALL FOUR originally-named residuals now closed,
issue #42 fully closed, pending project-owner sign-off.**

Red-teamed three times total across this ADR's lifetime — once before any code existed (the
original mechanism, §2), and once more specifically for the event-forwarding follow-up (fresh
agent, zero prior context, BEFORE any of that code was written) which found the first-drafted
bind-time propagation design was genuinely wrong (claim 22: would have produced incorrect `path`
values under this codebase's own real bottom-up construction order, AND introduced a genuine new
cross-thread data race) — caught and redesigned before shipping, not discovered after. Two
independent classes of false-negative test bug were also caught and fixed via adversarial mutation
across this ADR's own history, never trusted on a first clean pass: the original `fan_in`-edge
concurrency test (§2 Pass 2), the `ThreadPool` budget work's racy background sampler (claim 16), and
most recently the event-forwarding proofs themselves (claim 23) — each confirmed genuinely
load-bearing by temporarily breaking the mechanism and observing the expected failures before
reverting.

Test evidence: 34/34 checks in `test_rt_workflow_sub_workflow.cpp` (S1-S13), 4 in
`test_rt_thread_pool.cpp` (T7/T8), 53/53 in `test_rt_workflow_event_stream.cpp` (W1-W13, up from
28 pre-existing W1-W9 checks). The new example and every mechanism-specific proof (ThreadPool
budget: claims 15/16; event forwarding: claims 18-23) run directly and pass. Full project building
clean (zero errors, MSVC/Visual Studio 18, Debug). Full repo-wide `ctest`: 315/317 passing — the
two failures both pre-existing and confirmed unrelated (the long-documented matplotlib/pandas
environment gap; `test_rt_spawn_cost_budget`, this ADR's own already-documented flaky concurrency
assertion under real thread contention, confirmed clean 3/3 on direct rerun).
