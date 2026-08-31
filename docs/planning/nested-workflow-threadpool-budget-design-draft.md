# Design draft: bounding `ThreadPool` resource usage across nested `WorkflowSupervisor` instances
# (ADR-157 §4's own named residual)

Status: **implemented and proven** — `split_worker_budget()`, the `WorkflowSupervisor(worker_budget)`
constructor, the automatic `nesting_depth_`/`kMaxNestingDepth` cap, and `live_worker_thread_count()`
all shipped; both §6 proofs executed (positive: S12 in `tests/test_rt_workflow_sub_workflow.cpp`;
negative/mutation: a temporary `pool_(0 * worker_budget)` mutation reproduced a real ceiling breach
— 21 live threads against a declared budget of 7 — confirming S12's assertion is load-bearing, not
vacuous; the mutation also caught and fixed a real bug in S12 itself: a background sampler thread
racing a fast, synchronous `drive()` call could legitimately take zero samples before the run
finished, silently passing regardless of the actual peak — replaced with a deterministic direct
sample, since `ThreadPool` workers are created once at construction and live for the whole
`WorkflowSupervisor` instance's lifetime, not per-round). S13 additionally proves the
`kMaxNestingDepth` boundary is real and reachable, not dead code. Full local regression run pending
confirmation; ADR write-up (addendum to ADR-157) in progress. Written per CLAUDE.md's
`design → red-team → prove → judge` discipline.

## 1. The question

ADR-157 shipped `executor_kind::sub_workflow`'s real runtime bridge but explicitly did not solve
one thing it named: every `WorkflowSupervisor` owns its own, independently-sized `ThreadPool`
(default `hardware_concurrency()` workers), with zero shared ceiling across a nested tree — a real,
unbounded-OS-thread-creation hazard, architecturally the same class CLAUDE.md's own "Machine
safety" section names.

## 2. Why literal pool sharing is dangerous, not just inefficient (confirmed by red-team)

Traced directly against `include/agentengine/rt/thread_pool.hpp`: `ThreadPool::run_job()` resumes
a submitted job **exactly once** and fails loud if it isn't `done()` afterward — this contract is
satisfied today only because `execute()`'s dispatch loop's `std::future::get()` calls are plain
OS-thread blocks inside already-running synchronous code, never a coroutine suspension. If a nested
supervisor's own round loop submitted to the SAME pool an ancestor's worker is running on, that
worker would block on a future serviced by a pool with no free worker to run it (all of them
similarly occupied) — genuine, structural self-starvation, not hypothetical. **Verdict: sharing one
live pool across nesting levels is unsound. Only budgeting — separate pools, each sized to stay
within some ceiling — is safe by construction.**

## 3. The decision: ship a static per-instance worker budget; drop the shared-semaphore option

Two candidates were weighed (a static per-`WorkflowSupervisor` worker-count budget vs. a shared
counting-semaphore gating worker creation). **Settled: ship the static budget only.** The
semaphore's entire advantage over a static budget — adapting to real usage, never holding permits
for work that's bound but never dispatched — does not survive contact with how this codebase
actually constructs nested supervisors: every real call site (`tests/test_rt_workflow_sub_workflow.
cpp`, `examples/27_sub_workflow_nested_request_port.cpp`) constructs `inner` FULLY, pool and all,
BEFORE `bind_sub_workflow()` is ever called — so a semaphore's permits would be held for the whole
object lifetime exactly like a static budget's threads sit idle, for no adaptivity payoff, at the
cost of a genuinely new three-way failure-policy question (block/degrade/refuse on exhaustion) with
no clean precedent for the specific shape this would need (this codebase's own closest analog,
`rt::multi_agent::Budget::acquire_in_flight_slot()`, is safe specifically because it's acquired from
caller-side DRIVING code before submission, never from inside an already-dispatched job — a
constraint a construction-time semaphore for `ThreadPool` sizing can't cleanly inherit). Rejected.

A `AsyncMutex`/`channel<T>`-based coroutine-native scheduler was also considered and rejected as a
false lead for this specific problem: those primitives park a coroutine handle for later resumption,
a genuinely different kind of suspension than the OS-thread-blocking `.get()` calls actually driving
this hazard — building a coroutine-native scheduler that avoids wasting OS threads on blocked
`.get()` calls is exactly the "genuinely separate, harder problem" `thread_pool.hpp`'s own file
banner already declares out of scope for this type, correctly, not a gap this design should try to
route around.

A "reserved worker" scheme (one worker per pool set aside specifically for re-entrant submissions)
was also considered and rejected: it only defers the same hazard one level deeper (a job running on
the reserved worker that itself needs to re-enter has no SECOND reserved worker), and generalizing
it to depth K is isomorphic to K separate per-level pools (i.e., the static budget already ships)
while requiring genuinely new `ThreadPool` machinery (tiered scheduling, reentrant-job tagging) for
no benefit over the simpler shape.

## 4. The accepted mechanism

- `WorkflowSupervisor` gains a constructor: `explicit WorkflowSupervisor(std::size_t worker_budget =
  0)`, forwarding directly to `pool_`'s own existing constructor (`ThreadPool pool_{worker_budget}` —
  `ThreadPool`'s own `worker_count == 0` sentinel already means "use `default_worker_count()`," so
  this is source-compatible with every existing `WorkflowSupervisor sup;`/`make_shared<
  WorkflowSupervisor>()` call site, zero breakage). **Not** a `bind_sub_workflow()` parameter as the
  first draft proposed — mechanically impossible given `pool_`'s workers already exist, already
  running at default size, by the time any `bind_sub_workflow()` call could happen (confirmed: no
  real or plausible call site in this codebase ever constructs-then-binds in the other order).
- A new, small, engine-provided helper near `default_worker_count()`
  (`thread_pool.hpp`): `result<std::vector<std::size_t>> split_worker_budget(std::size_t total,
  std::size_t child_count)` — evenly distributes `total` across `child_count`, remainder to the
  first few, and **fails closed** (`result` carrying a real error) if `child_count > total`, rather
  than silently flooring any child's share to 0. This is the single most important correction from
  red-team: `ThreadPool`'s own `worker_count == 0` already means "use the system default" — passing
  an unfloored, computed `0` for a starved child would silently hand it a full, UNBOUNDED
  default-sized pool, invisibly defeating the entire ceiling this mechanism exists to enforce. Never
  silently floor; fail the bind attempt instead, matching this codebase's existing fail-closed idiom
  (`CapabilitySet::attenuate()`, `Budget::try_reserve()`).
- **Promoted from "recommended defense in depth" to a load-bearing precondition** (the first draft
  under-stated this): a mandatory nesting-depth cap is what keeps the budget arithmetic a genuine
  ceiling at all — without it, a subtree's real worst-case thread count is `max(configured_total,
  number_of_sub_workflow_children)` recursively, i.e. not actually bounded once fan-out is
  unbounded. Implemented as automatic, structural tracking, not caller discipline: `WorkflowSupervisor`
  gains a private `nesting_depth_ = 0` member; `bind_sub_workflow()` sets `inner`'s
  `nesting_depth_` to `this->nesting_depth_ + 1` and refuses the bind (leaving `sub_workflows_`
  untouched, so the existing `sub_workflow_kind_nodes_are_bound()` structural check already catches
  it as "unbound," matching how a wrong `executor_id` is already silently refused today — no new
  API surface, no signature change) if that would exceed a fixed ceiling. No caller bookkeeping
  needed; the depth is inherited automatically through the SAME `bind_sub_workflow()` call that
  already wires everything else together.

## 5. What this draft does not claim

- Does not persist `worker_budget`/`nesting_depth_` through checkpoint/resume — inherits the SAME,
  already-disclosed "bindings are not checkpoint-durable, caller re-supplies fresh at
  construction/bind time" limitation ADR-157 §4 already names for `sub_workflows_` itself. A host
  resuming a checkpointed run must reconstruct `inner` with the SAME budget it originally used, or
  the effective ceiling silently drifts across a resume cycle — named here, not solved.
- Does not make `split_worker_budget()` recursive/depth-aware across multiple nesting levels — a
  caller composing several levels calls it once per level, explicitly; auto-splitting across an
  entire tree would need the whole tree's shape known up front, which is the kind of speculative
  generality this codebase's own stated engineering values (and `thread_pool.hpp`'s own banner)
  reject building before it's proven necessary.
- Does not attempt a dynamic/adaptive budget — confirmed not worth the complexity given real usage
  patterns (§3).

## 6. Required proof before this ships (not yet executed — queued for the implementation pass)

Per this session's own established discipline (ADR-157 Pass 2's fan_in-merge false-negative catch):
both a positive and a negative/mutation proof, not just "the mechanism exists":

1. **Positive**: a peak-thread-count instrumented test (an atomic high-water-mark counter in
   `ThreadPool`'s own worker-spawn loop, test-only) across a deliberately wide/deep nested tree
   (extending ADR-157 S8's own shape) with a small budget, asserting peak count never exceeds the
   declared ceiling.
2. **Negative/mutation**: temporarily reintroduce the exact hazard §2 identifies (either literal
   pool sharing, or passing an unfloored raw `0` for a computed split) and confirm a real hang or a
   real unbounded-thread-count blowup reproduces — mirroring ADR-157 Pass 2's own 20/20
   segfault-then-15/15-clean methodology — before trusting the fix is load-bearing.

## 7. Files (planned, not yet created)

- `include/agentengine/rt/thread_pool.hpp` — `split_worker_budget()`.
- `include/agentengine/rt/workflow_supervisor.hpp` — the new constructor, `nesting_depth_` member,
  `bind_sub_workflow()`'s depth-ceiling check.
- `tests/test_rt_workflow_sub_workflow_budget.cpp` (or extend `test_rt_workflow_sub_workflow.cpp`)
  — the two proofs from §6, plus a depth-limit-refusal test and a `split_worker_budget()`
  fail-closed test.
- `decisions/ADR-15y-nested-workflow-threadpool-budget.md` (once implemented and proven).

## Status

**Design settled, red-teamed, ready for implementation as its own future pass.** Not started —
queued behind #1 of ADR-157's four named residuals (enforcing one `inner` per executor_index) per
explicit project-owner sequencing.
