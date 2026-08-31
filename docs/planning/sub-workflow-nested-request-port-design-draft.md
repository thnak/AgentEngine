# Design draft: `executor_kind::sub_workflow`'s real runtime bridge, with nested `request_port`
# proxying (issues #33 and #38)

Status: **superseded — implemented as `decisions/ADR-157-sub-workflow-nested-request-port.md`.**
This document is kept as the historical design-and-red-team record; the ADR is the current source
of truth for what actually shipped, including a second red-team-style finding caught during
implementation itself (a test using `fan_in` edges silently prevented the intended concurrency
hazard from ever being exercised — a false-negative in the first verification attempt, caught and
fixed before trusting the result; see the ADR's own §2 "Pass 2").

## 1. The question and scope decision (unchanged from the first draft)

Build `executor_kind::sub_workflow`'s real runtime bridge, closing #33 (declared but unconditionally
refused) and #38 (no nested `request_port` proxying) together, as one mechanism, rather than trying
to extend `workflow_as_executor_body()`'s (ADR-150) synchronous `ExecutorBody` contract to express
suspension. Full reasoning unchanged from the first draft's §1 — `ExecutorBody` is load-bearing for
every other node kind; suspension is a structural round-loop concern, not an observability side
channel. `workflow_as_executor_body()` remains completely untouched by this work (confirmed by the
red team's own direct read of `workflow_as_executor.hpp` — nothing here touches it).

## 2. What the red team overturned and why

The first draft's §3 (dispatch/resume mechanism) and §3d (extending `OpenPort`) were both found
unsound on direct trace against the real `execute()`/`resume_workflow()` code:

1. **The I1 concurrency hazard was real** (two sub_workflow deliveries to the SAME bound inner
   supervisor in one outer round — ordinary topology, nothing prevents it). The first draft named
   this as an open question and vaguely suggested "its own per-node mutex." The red team traced the
   EXISTING, already-proven OQ-19 same-round-duplicate-delivery quarantine (`execute()`,
   `workflow_supervisor.hpp` ~line 1054, today scoped to `executor_kind::agent` only) and confirmed
   it generalizes cleanly: extending it to also cover `sub_workflow`-kind closes the hazard with a
   ~2-line change to already-tested machinery, no new primitive, no new adversarial-verification
   burden. **Adopted as-is.**
2. **Extending `OpenPort` with an `inner_interaction_id` field (first draft §3d) was actively wrong,
   not just inelegant.** The red team traced `resume_workflow()`'s real sequencing:
   `port->response = request.response;` (the RAW human answer) is written UNCONDITIONALLY before any
   branch could check a marker field — meaning a naive implementation of the first draft's own
   described flow would fold the raw human answer to a NESTED question as if it were the
   sub-workflow's own completed output, silently skipping whatever the inner workflow does after
   that interaction resolves. Worse: this exact confusion survives a checkpoint/restore cycle (since
   `OpenPortRecord` mirrors `OpenPort`, and the marker distinguishing "sub_workflow-pending" from
   "ordinary port" would round-trip as inert data with no code path checking it after restore) —
   producing exactly the failure `graph.hpp`'s own design principle warns against: "A refused graph
   is recoverable; a quietly reinterpreted one is not." **`OpenPort` is NOT touched by this revision
   at all** — see §3c below for the actual fix, which also resolves this checkpoint hazard as a side
   effect, not a separate patch.
3. **"Third dispatch bucket, folded into the existing reply-collection" (first draft §3b) understated
   the real diff.** The red team enumerated five separate touch points in `execute()`'s round loop
   that all need an explicit "is this a pending sub_workflow suspension, not a real reply" branch:
   eventing, the fold loop, the routing loop, the stall-reporter loop, and port materialization. §3
   below is written against all five explicitly, not glossed over.
4. **A sub_workflow suspension followed by an UNRELATED round abort (a different executor's routing
   failure, a merge conflict, a stall trip) in the SAME round would silently orphan the live, real
   inner suspension** — the first draft never considered this interleaving. Fixed in §3c/§4 by making
   `open_interactions()` unconditionally reflect true pending state, not gated behind
   `status == suspended`.
5. **The multiplexed event bridge (ADR-152) does not compose for free**, contrary to the first
   draft's "looks exactly like an ordinary request_port suspension from the outside" framing — an
   inner graph's own agent-kind/moderator streaming has no path to the outer's
   `enable_event_stream()` consumer without new, unbuilt forwarding. **Explicitly out of scope for
   this pass** (§6) — the STRUCTURAL suspension signal (open/resolved) still composes via reusing
   `request_port_opened`/`_resolved`, which is a real, if partial, win.
6. **Multi-level nesting is not a coroutine-reentrancy hazard** (each `WorkflowSupervisor` owns its
   own independent `ThreadPool`, confirmed by the red team's direct read of `thread_pool.hpp`), **but
   IS a real, unbounded-thread-consumption hazard**: `workflow_as_executor_detail::drive()`'s
   "resume until done" loop busy-spins a full CPU core for the inner run's whole duration, and this
   compounds at every nesting level with no shared ceiling. Addressed in §5 with an explicit, bounded
   test rather than an unbounded nesting redesign (out of scope — see §6).
7. The I3 claim that `WorkflowResult::partial` already discloses "what is the nested workflow
   asking" was found FALSE (`record_partial()` is never called for anything that doesn't produce a
   real reply) — but on further inspection this exactly matches the EXISTING, pre-existing behavior
   for an ordinary `request_port` node (which also gets no `partial` entry until resolved). The
   claim in the first draft was simply wrong and is removed, not replaced with new machinery — see
   §6.

## 3. The revised mechanism

### 3a. Binding — unchanged from the first draft

`WorkflowSupervisor::bind_sub_workflow(std::string const& executor_id,
std::shared_ptr<WorkflowSupervisor> inner)` — a new opt-in call, same shape as
`set_checkpoint_hook()`/`set_merge_on_join_hook()`. `check_workflow_executable()`'s `sub_workflow`
refusal narrows to "refused unless bound" (mirroring the existing agent-kind
`agent_kind_bodies_are_structurally_agent_backed()` precedent). An unbound `sub_workflow` node still
refuses exactly as today.

Per the red team's settled §7 Q5: no bind-time validation of `inner`'s own `valid_` state (matching
`workflow_as_executor_body()`'s own precedent of deferring to call-time). The dispatch path's
handling of a bound-but-invalid `inner` (§3b) is written as a genuine catch-all (`else`/default),
never an enumerated switch that could silently miss `workflow_status::invalid` or a future status.

### 3b. Dispatch — a separate `sub_workflow_deliveries` list, its own job function, the SAME
### concurrency dedup as agent-kind

`execute()`'s round loop gathers a third list, `sub_workflow_deliveries` (parallel to the existing
`port_deliveries` split, gathered from `state_.pending` by kind, same as today).

**Concurrency (closes red-team finding 1)**: the existing OQ-19 same-round quarantine block
(currently: `if (graph_.executors[idx].kind != executor_kind::agent) continue;`) is generalized to
also scan `sub_workflow_deliveries` the identical way — only the FIRST same-round delivery to a given
`sub_workflow` executor_index is ever dispatched; a second is synthetically quarantined
(`contract`-class, never retried, routed through the existing `EdgeFailurePolicy` machinery like any
other failure) — reusing the already-proven mechanism, not a new mutex.

Each surviving `sub_workflow_deliveries[i]` is submitted via `pool_.submit(run_sub_workflow_job(...))`
— a NEW sibling to `run_executor_job` (not a reuse of it: there is no `ExecutorBody`/`bodies_[idx]`
for a `sub_workflow`-kind node, exactly like `request_port` has none today). `run_sub_workflow_job`:
- If no pending inner interaction is tracked for this executor_index (see §3c): calls
  `inner->run_workflow(RunWorkflow{payload})`.
- If one IS tracked (this delivery is the routed-down human answer, not a fresh task — only reachable
  via §3c's resume path, never via ordinary round dispatch): this branch does not exist in
  `run_sub_workflow_job` at all — resuming a pending inner interaction happens ENTIRELY inside
  `resume_workflow()` (§3c), never through the ordinary per-round dispatch path, because a pending
  sub_workflow interaction is not part of `state_.pending` at all (see §3c) — it is not something
  `execute()`'s round loop redelivers, structurally ruling out the confusion between "fresh task" and
  "routed-down answer" the first draft's dispatch-branch design invited.
- Inspects the inner `WorkflowResult`, genuine catch-all (not enumerated):
  - `completed` → `ExecuteReply{ok=true, payload=inner_result.output, ...}` — an ORDINARY reply.
  - `suspended` → a NEW, explicit marker on `ExecuteReply` (see below), never a "normal" reply.
  - anything else (`executor_failed`/`routing_failed`/`merge_conflict`/`bound_*`/`invalid`, and any
    future status) → `ExecuteReply{ok=false, ...}` — an ORDINARY failure reply, routed through the
    EXISTING `EdgeFailurePolicy` machinery unchanged.

`ExecuteReply` gains one new field, mirroring how `stalled` was already added as a trailing field for
ADR-149 (round-local, transient, never persisted/checkpointed — NOT the same class of change as
touching `OpenPort`, which the red team correctly flagged as hazardous specifically because `OpenPort`
IS persisted):
```cpp
struct ExecuteReply {
    agentengine::Message     payload;
    std::vector<std::string> routes;
    bool                     ok    = true;
    agentengine::failure_class klass = agentengine::failure_class::fatal;
    bool stalled = false;
    // NEW: set only by run_sub_workflow_job, only when the inner run's status was `suspended`.
    // `ok`/`payload`/`routes`/`klass` are all UNUSED and left default when this is set -- a
    // suspended entry produces no real reply at all (closes red-team finding 3: every one of the
    // five per-index loops below checks this FIRST and skips entirely, exactly like they already
    // skip `port_deliveries` today).
    std::optional<std::string> pending_sub_workflow_inner_interaction_id;
};
```

### 3c. The pending-interaction tracker — a SEPARATE structure, `OpenPort` untouched (closes
### red-team finding 2)

New member, NOT part of `ports_`:
```cpp
struct PendingSubWorkflow {
    agentengine::Interaction interaction;      // the OUTER-visible interaction (mint_interaction(), unchanged format)
    std::size_t              executor_index = 0;
    std::string               inner_interaction_id;  // NEVER exposed outside WorkflowSupervisor
};
std::unordered_map<std::string, PendingSubWorkflow> pending_sub_workflows_;  // keyed by OUTER interaction_id
```

When `run_sub_workflow_job` reports `pending_sub_workflow_inner_interaction_id` for
`sub_workflow_deliveries[i]`: mint a fresh outer `Interaction` (existing `mint_interaction()`,
unchanged), insert into `pending_sub_workflows_`, fire `request_port_opened` (ADR-152; reuses the
existing event kind — from the outside this is indistinguishable from an ordinary `request_port`
opening, closing the "composes for free" claim for the STRUCTURAL signal specifically). Do NOT touch
`ports_`, do NOT push this delivery into `next` — it produces nothing for this round's routing.

**`resume_workflow(ResumeWorkflow{interaction_id, response, routes})` checks `pending_sub_workflows_`
FIRST, before touching `ports_` at all**:
- Not found there → falls through to the EXISTING, completely unmodified `ports_` scan (ordinary
  `request_port` resume, byte-for-byte unchanged).
- Found → erase the entry from `pending_sub_workflows_`, call
  `inner->resume_workflow(ResumeWorkflow{stored.inner_interaction_id, response, routes})` (a plain
  synchronous call — this happens on whatever thread called `resume_workflow()`, not inside a
  `ThreadPool` job; no new concurrency surface, since only one `resume_workflow()` call can be in
  flight at a time per I1's `run_mutex_` guard, already held for this whole call). Inspect the
  result, genuine catch-all:
  - `completed`/any-terminal-failure → construct an ORDINARY, freshly-built `OpenPort{interaction=
    stored.interaction, executor_index=stored.executor_index, response=inner_result.output (or a
    failure marker), routes={}, resolved=true}`, push it onto `ports_`, then fall through to the
    EXISTING `execute()` call exactly as an ordinary port resolution already does. **`OpenPort`'s own
    fields mean exactly what they always meant** — the derived value is computed BEFORE the
    `OpenPort` is ever constructed, never written into a pre-existing one, which is precisely what
    closes red-team finding 2's semantic-overload hazard.
  - `suspended` again → mint a NEW outer `Interaction`, insert a NEW `PendingSubWorkflow` entry
    (updated `inner_interaction_id`), and return — mirroring the EXISTING early-return shape
    `resume_workflow()` already has for "other ports still unresolved" (still-suspended, caller sees
    a fresh interaction to answer, no special "second time" handling needed on the caller's side).

### 3d. The five per-round-loop touch points (closes red-team finding 3, made explicit rather than
### hand-waved)

Every one of these already has an existing, analogous "skip this index" or "skip this bucket"
shape for `port_deliveries` today — `sub_workflow_deliveries` entries with
`pending_sub_workflow_inner_interaction_id` set get the identical treatment, added explicitly:

1. **Eventing** (`executor_completed`, ADR-152): skipped for a pending entry; `request_port_opened`
   fires instead (§3c), mirroring exactly what the `port_deliveries` block already does for an
   ordinary port.
2. **Fold loop** (`record_partial`/output-selection/merge-hook): skipped entirely for a pending
   entry — it produced no reply to record. (This is also why `WorkflowResult::partial` has no entry
   for a currently-suspended sub_workflow node — see §6, this MATCHES existing `request_port`
   behavior, not a new gap.)
3. **Routing loop** (`route_from`): skipped entirely for a pending entry — nothing to route.
4. **Stall-reporter loop**: skipped for a pending entry (a suspended sub_workflow has not "not
   progressed," it has not run to completion at all — reporting `stalled=false` for it, the default,
   would be a category error, not merely harmless).
5. **Port/interaction materialization**: as described in §3c — a pending entry feeds
   `pending_sub_workflows_`, never `ports_`.

### 3e. Orphan handling when an unrelated round abort follows a same-round sub_workflow suspension
### (closes red-team finding 4)

Because the fold/routing loops (where `broke`/`merge_failed`/the stall-trip are decided) run AFTER
the per-index processing above, a sub_workflow entry can be recorded into `pending_sub_workflows_`
and then a DIFFERENT executor's failure aborts the round with a non-`suspended` status in the same
pass. Fix: `pending_sub_workflows_` is a **persistent member**, not round-local — it is never cleared
by an unrelated abort. `finish()`'s gating changes from
`if (status == workflow_status::suspended) r.open_interactions = open_interactions();` to
**unconditional**: `r.open_interactions = open_interactions();` always, and `open_interactions()`
itself is extended to union `ports_` (existing, unresolved) with `pending_sub_workflows_` (all
entries, by construction always "open"). A caller can now see "this round ended `routing_failed`,
AND there is still a real, live nested interaction with id X" — honest disclosure of true state
rather than silently dropping it, matching the existing `unopened_ports` disclosure precedent's own
spirit rather than inventing a new one.

## 4. Checkpoint/resume — fails closed, not silently corrupting (closes red-team finding 5)

`RunStateRecord` is genuinely unchanged — `pending_sub_workflows_` is NOT persisted (matching
`agent_session_as_executor_body()`'s own already-accepted "fresh bindings supplied by the caller at
initialize()-time" precedent for `bodies_`/`contexts_`, extended here to `sub_workflows_`/
`pending_sub_workflows_`). The critical difference from the FIRST draft: because §3c never puts a
sub_workflow-pending entry into `OpenPort`/`ports_` at all, there is no in-band marker that could be
misread as "an ordinary resolvable port" after restore. A restored `WorkflowSupervisor` has an EMPTY
`pending_sub_workflows_` (default-constructed, matching every other fresh member) — calling
`resume_workflow()` with what WAS a pending sub_workflow interaction_id finds nothing in
`pending_sub_workflows_` (empty) and nothing in `ports_` (never was there), falling through to the
EXISTING "port not found or already resolved" branch: **`workflow_status::invalid`, a safe, honest
failure — never silent misrouting.** This must be a real, tested claim (§5), not asserted.

Two layers of protection, both real:
1. **First line**: `WorkflowCheckpointManager::resume_or_start()`'s existing fail-closed guard
   (`workflow_checkpoint_manager.hpp`, confirmed by the red team to already cover `sub_workflow`-kind
   alongside `agent`-kind) refuses to resume at all without `acknowledge_agent_history_reset`.
2. **Second line, new** (closes the red team's own follow-up note that a host bypassing the manager
   and calling `restore_from_record()` directly gets none of the first line's protection today, for
   EITHER agent-kind or sub_workflow-kind): the fail-closed `resume_workflow()` behavior above holds
   regardless of which path restored the state — it is a property of `pending_sub_workflows_` always
   starting empty on a fresh/restored instance, not something that needs its own separate check.

## 5. The bounded-nesting/thread-consumption test (closes red-team's settled §7 Q2)

Not a redesign of `ThreadPool` sharing (out of scope — see §6) — a real, bounded, CLAUDE.md-compliant
test (machine safety: every loop bounded) proving a SPECIFIC nesting shape (2 levels deep, 3-wide
fan-out at the outer level, each sub_workflow node itself a small graph) completes in bounded wall-
clock time and peak thread count, so the busy-spin `drive()` cost the red team identified is measured,
not merely asserted safe. Documented explicitly: nested `WorkflowSupervisor` instances each own an
independent, default-sized `ThreadPool` with no shared ceiling — a host nesting deeply or widely is
responsible for bounding that itself (matching `ThreadPool`'s own existing "system default, caller's
job to size for its own workload" contract, unchanged, not newly invented here).

## 6. What this draft does not claim

- Does not touch `workflow_as_executor_body()` (ADR-150) — confirmed by the red team, unaffected.
- Does not forward an inner graph's own multiplexed ADR-152 events (agent-kind/moderator streaming)
  to the outer's `enable_event_stream()` consumer — only the structural `request_port_opened/
  _resolved` signal for the suspension itself composes. Real, disclosed limitation (red-team finding
  7), not solved here — a future pass would need `bind_sub_workflow()` to also wire an explicit
  forwarding tap from `inner->enable_event_stream()` into the outer's own `multiplex_sink_`/
  structural producer, genuinely new integration work.
- Does not redesign `ThreadPool` sharing/ceilings across nested `WorkflowSupervisor` instances
  (§5) — named, bounded-tested at a specific shape, not solved generally.
- Does not give `sub_workflow`-kind nodes anything beyond what `TypedExecutor<In,Out>`'s existing
  compile-time check already provides — #33's "typed message passing" is treated as already
  satisfied.
- Does not claim `WorkflowResult::partial` discloses a suspended sub_workflow's own nested question —
  this matches EXISTING `request_port` behavior (no entry until resolved), not a new gap this draft
  introduces or needs to close.
- Does not attempt multi-level (3+) nesting as a tested, claimed-safe shape — §5's test is bounded to
  2 levels; deeper nesting is not asserted safe or unsafe, simply untested.

## 7. Files (planned)

- `docs/planning/sub-workflow-nested-request-port-design-draft.md` (this file)
- `include/agentengine/rt/workflow_supervisor.hpp` (edit — `bind_sub_workflow()`, `sub_workflows_`,
  `pending_sub_workflows_`, `ExecuteReply::pending_sub_workflow_inner_interaction_id`,
  `run_sub_workflow_job()`, the generalized OQ-19 quarantine, the five §3d touch points,
  `resume_workflow()`'s new pending-check branch, `finish()`'s unconditional `open_interactions()`)
- `include/agentengine/workflow/graph.hpp` (edit — narrow the `sub_workflow` refusal to "refused
  unless bound", mirroring the existing agent-kind precedent exactly)
- `tests/test_rt_workflow_sub_workflow.cpp` (new) — including: a basic nested suspend/resume/
  complete round-trip; the OQ-19-generalization concurrency proof (mirroring ADR-150's own
  adversarial mutation-test discipline — temporarily narrow the quarantine back to agent-only,
  confirm a real crash/race reproduces, then revert); the checkpoint-restore fail-closed proof
  (§4); the round-abort-orphan disclosure proof (§3e); the bounded-nesting test (§5).
- `examples/27_*.cpp` (new) — mirroring MAF's `sub_workflow_request_interception.py`/
  `sub_workflow_parallel_requests.py` in spirit.
- `decisions/ADR-157-sub-workflow-nested-request-port.md` (new, once implemented and proven)

## 8. Recommended next step

Implementation, directly against this revised design — every MUST-FIX finding from the red-team pass
has a concrete, code-grounded resolution above, not just an acknowledged open question. Proceed to
build, test (including the adversarial mutation-test discipline ADR-150/152 both used), and write the
ADR.
