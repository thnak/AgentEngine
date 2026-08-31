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
- Does not forward an inner graph's own multiplexed ADR-152 events (agent-kind/moderator streaming)
  to the outer's `enable_event_stream()` consumer — only the structural
  `request_port_opened`/`_resolved` signal composes. Real, disclosed limitation, not solved here.
- Does not redesign `ThreadPool` sharing/ceilings across nested `WorkflowSupervisor` instances — a
  host nesting deeply or widely is responsible for bounding that itself; only a specific, bounded
  2-level/3-wide-fan-out shape is tested and claimed safe.
- Does not give `sub_workflow`-kind nodes anything beyond `TypedExecutor<In,Out>`'s existing
  compile-time type check.
- Does not claim `WorkflowResult::partial` discloses a suspended sub_workflow's own nested
  question — this matches EXISTING `request_port` behavior (no `partial` entry until resolved), not
  a new gap.
- Does not enforce that a single `inner` instance is bound to at most one executor_index across the
  whole outer graph — a documented caller contract (mirroring `workflow_as_executor_body()`'s own
  reference-overload lifetime contract), not runtime-checked. Binding the same `inner` to two
  different sub_workflow nodes would defeat the per-index quarantine's own dedup and is unsupported.
- Does not attempt or claim safety for 3+ level nesting — untested, not asserted either way.

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

## Status

**Proposed — implemented, red-teamed once (design draft before any code existed, revised against
every MUST-FIX finding), the single most severe claim (§5 #7) adversarially verified — including
catching and fixing a false-negative in the FIRST verification attempt (a `fan_in`-edge test
topology that silently prevented the hazard from ever being exercised) before trusting a clean
result — 22/22 new test checks passing, the new example run directly and passing, full project
building clean, 310/312 repo-wide `ctest` passing (both failures pre-existing and confirmed
unrelated), pending project-owner sign-off.**
