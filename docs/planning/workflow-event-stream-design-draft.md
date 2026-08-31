# Design draft: `WorkflowSupervisor` per-executor event streaming (issue #29)

Status: **superseded — implemented as `decisions/ADR-152-workflow-event-streaming.md`.** This
document is kept as the historical design-and-red-team record (§2–§4 below); the ADR is the
current source of truth for what actually shipped, including two further findings from the
implementation pass itself (ADR-152 §3, "Pass 2") that this draft's own §4 did not anticipate.

Project-owner direction carried through the red-team pass unchanged: **this repo hasn't shipped,
so a fresh architecture is fine if it's genuinely better — don't patch the first draft's mechanism
into working if patching it just moves the failure mode somewhere else.** The red team took that
seriously: it confirmed the first draft's central technical bet (see §2) but concluded the bet was
answering the wrong question, and the revision below reflects that, not a patch.

## 1. The question, sharpened

Issue #29 as filed leaves the fix's shape open. Project-owner direction for this pass: **every
event any node produces during a run must be yielded live, the instant it happens** — not batched
to round (superstep) boundaries the way `enable_live_view()`'s `WorkflowLiveEvent` already is.
The bar is genuine per-token, per-tool-call, per-route-decision streaming out of
`WorkflowSupervisor`, multiplexed from however many nodes are concurrently in flight within one
round, not a richer round summary.

## 2. What the first draft got right, and what the red-team overturned

The first draft's central open question was: is it memory-safe to call
`channel_producer<T,E>::push()` concurrently, from multiple `ThreadPool` worker threads, through
ONE retained producer instance? **Empirically confirmed yes** — a scratchpad stress test (8-32
threads, ~2.7M pushes, MSVC `/fsanitize=address`, three contention profiles) found zero lost
pushes, zero corruption, zero deadlocks. `push()`'s entire body is under one `std::mutex`; the
implementation is correct for this.

**That confirmation turned out not to matter, because memory safety was never the real risk.**
Two findings overturn the first draft's direction entirely:

1. **The file's own banner already says this usage is out of contract**, more directly than the
   first draft's reading of it: *"Multiple PRODUCERS are similarly unsupported... a real fan-in
   point, if ever needed, is a caller-side concern, not this channel's."* That is the type's
   author naming this exact scenario and routing it to caller-side fan-in — not an ambiguous
   silence the way the first draft's §2 characterized it.
2. **The actual hazard is liveness, not memory safety, and it is severe.** `push()` deliberately
   *blocks the calling OS thread* when the queue is at capacity (default 256) — a correct design
   for `channel.hpp`'s intended caller (a dedicated SSE-read thread with nothing else to do). It
   is the wrong contract for a `ThreadPool` worker thread that is also needed for real workflow
   compute. `execute()`'s round loop submits every ready delivery to a **fixed-size** pool, then
   collects with blocking `std::future::get()` calls. If a worker thread's forwarding closure
   blocks inside `push()` because a live-view consumer (a UI, under load — precisely the scenario
   this feature exists for) is draining too slowly, that worker thread is gone. Enough
   concurrently-streaming nodes in one fan-out round can exhaust the pool and stall the actual
   workflow run — not merely degrade its observability. An additive observability feature must
   never be able to do that to the compute it's observing, and the first draft's mechanism could.

Separately, the red team found the first draft's own named "safe fallback" (private per-delivery
buffers drained by `execute()`'s own coroutine "between yield points") **does not exist as a real
option**: `execute()`'s round-collection loop calls blocking `std::future::get()` in a tight loop
with no yield points until every job in the round has finished. That fallback's real behavior is
"buffer everything privately during the round, dump it all when the round ends" — which is
byte-for-byte the round-boundary batching `enable_live_view()` already does today, i.e. a no-op
fix for issue #29 dressed up as a weaker-but-safe version of streaming. Recorded here so it is
never picked under time pressure as if it were a lesser-but-working alternative.

## 3. Revised architecture

### 3a. Structural events — unchanged from the first draft, confirmed safe as designed

`workflow_run_started`/`_suspended`/`_resumed`/`_completed`/`_failed`, `superstep_started`/
`_completed`, `executor_dispatched`/`_completed`, `message_routed`, `fan_out_dispatched`,
`fan_in_aggregated`, `route_selected`, `request_port_opened`/`_resolved`, `checkpoint_saved`,
`merge_completed`/`_conflict`. Produced ONLY by `execute()`'s own single coroutine, at existing
hook points, pushed through an ordinary `channel_producer`-backed `stream<WorkflowEvent>` exactly
like `live_view_producer_` today — same single-writer shape, zero new concurrency risk, the
red-team's stress test and reasoning both confirm this bucket is fine unmodified.

**One real gap the red team found here**: `run_workflow()`'s `!valid_` early return,
`resume_workflow()`'s invalid-interaction-id and still-suspended branches, and
`continue_workflow()`'s `!valid_` check all construct a `WorkflowResult` WITHOUT going through
`execute()`/`finish()` — a naive implementation would silently never emit `workflow_run_*` for
these paths. The eventual implementation needs an explicit map of every `WorkflowResult`-producing
return site to which structural event (if any) fires there, not just the round-loop happy path.
Also: neither `live_view_producer_` today nor the new producer is ever explicitly `close()`d on
completion (pre-existing behavior — it fires via the move-only destructor's fire-default, or a
later `enable_*()` call replacing it). A consumer that wants "the run is over" as a stream event
rather than stream termination needs `workflow_run_completed`/`_failed` genuinely wired at every
return site, not assumed to fall out of the round loop alone.

### 3b. `WorkflowSupervisor::enable_event_stream(mr, cfg) -> stream<WorkflowEvent>`

Same construction pattern as `enable_live_view()`, additive — `enable_live_view()` stays as-is.

### 3c. Multiplexed per-node events — the mechanism the red team rejected and replaced

`agent_turn_event{executor_id, round, attempt, inner: RunEvent}` (an `attempt` field is new — see
§4 below) and `moderator_stream_delta{executor_id, round, attempt, text_delta}` still exist as
event kinds, but they are **not delivered through `channel_producer<T,E>`.** Per the red team's
recommendation, adopted here:

- **A dedicated, purpose-built, non-blocking MPSC primitive** carries this bucket only — worker
  threads never block on it. Bounded, with drop-or-coalesce-on-overflow under sustained lag, never
  backpressure into a compute thread. This is a smaller, new type (e.g.
  `workflow/multiplex_sink.hpp`), not a reuse or a stretch of `channel_producer<T,E>`'s documented
  contract — matching the file banner's own instruction that a fan-in point like this is a
  caller-side concern, built by the caller, not the channel. A dropped delta under lag is an
  accepted cost, consistent with this codebase's existing acceptance of unbounded `tool_call_delta`
  volume from `report_progress` (`effect_context.hpp`'s own comment: *"No rate limiting... any
  consumer needs to throttle on its own"* — this design extends that acceptance one hop upstream,
  to the producer side, instead of letting a lagging consumer become a compute stall).
- **A dedicated `EffectContext` field**, NOT `report_progress`. The red team found a real, not
  hypothetical, collision: a plain `function`-kind executor body that calls `Tool<>::invoke()`
  directly (a normal, already-supported pattern, no `AgentSession` involved) also reads
  `ctx.report_progress` — if the workflow bridge overwrote that field, a node's own tool progress
  would be silently reinterpreted as a workflow-bridge event. New field, own default no-op, own
  bracket discipline, matching this file's existing "one field per audience" precedent
  (`report_progress`/`codeact_preseeded_answers`/`blob_sink` are all separately scoped for exactly
  this reason).
- **A dedicated `AgentSession` callback tap for the agent-kind bridge, NOT a second
  `enable_event_stream()` call.** The red team found that having the bridge call
  `AgentSession::enable_event_stream()` on the inner session would silently evict any consumer an
  application had already attached directly to that same session (single-consumer "second call
  replaces the producer" contract) — a real, legitimate usage pattern (an app wanting both a
  workflow-level dashboard AND a focused per-agent debug stream on the same node) breaks silently,
  no error, just a stalled orphaned consumer. Fix: add a second, independent, plain callback field
  to `AgentSession` (`std::function<void(RunEvent const&)>`, default no-op, same call-scoped
  bracket discipline `report_progress` already uses) — composes with `enable_event_stream()`
  instead of contending with it for the one producer slot.
- **`agent_session_as_executor_body()`'s `drive()` loop needs real restructuring, not plumbing.**
  It is today a blind `while (!t.done()) t.resume();` spin loop
  (`rt/agent_workflow_executor.hpp`), documented as safe only because of the OQ-19 quarantine
  guarantee that no cross-thread wakeup happens inside one call. Draining/forwarding mid-flight
  means interleaving a non-blocking drain with each `resume()`: "resume, drain-and-forward, check
  done, repeat." This touches the single most concurrency-fragile file this design reaches into —
  it must be its own explicitly reviewed sub-change, with the existing CONCURRENCY CONTRACT
  comment's safety argument re-verified (it should still hold, since the drain runs on the same
  worker thread doing the resuming — no new thread hop — but that needs an explicit test, not an
  assumption carried over from the pre-drain version).
- **`function`-kind nodes** (moderator/router bodies) that want to stream: same pattern as before —
  call `chat_stream()`, forward `ChatResponseUpdate`s through the new dedicated field into the same
  MPSC sink as `moderator_stream_delta`. Still opt-in, still no engine enforcement (I3).

## 4. Resolved open questions from the first draft

1. **`report_progress` reuse** — resolved: dedicated field, not reuse (§3c). Real collision found,
   not hypothetical.
2. **Ordering/sequencing** — resolved, simpler than first framed: the structural-event bucket's
   channel is already fully mutex-serialized, so queue arrival order is already a valid race-free
   total order for that bucket — no separate atomic sequence counter needed. The multiplexed
   bucket's own MPSC sink needs its own arrival-order guarantee (part of its design, not a shared
   concern with the structural bucket). Explicitly document in the eventual ADR: cross-node
   interleaving order in a genuinely live multi-producer stream reflects real wall-clock/scheduling
   races between worker threads and is **not run-to-run reproducible** — expected and fine for a
   live stream, not an I5 violation (I5 concerns nondeterminism that affects a run's actual
   decisions/outputs; the round loop's own deterministic fixed-index `todo`/`replies` collection is
   unaffected by what order the *observability* stream happens to interleave in) — but must be
   stated so nobody later assumes this stream is replay-deterministic.
3. **Standalone `agent_session_as_executor_body()` (no outer workflow stream)** — confirmed a
   genuine no-op by construction, PROVIDED the dedicated-callback-tap fix in §3c ships (it sidesteps
   the `enable_event_stream()` collision that would otherwise make this case fragile too).
4. **v1 scope: is `moderator_stream_delta` in this pass?** Still open — project-owner's "all
   events" direction argues yes; not re-litigated by the red team (out of its mandate), still a
   real size/risk trade-off for the implementation plan.
5. Backgrounded-tool progress gap (ADR-060 §4) — still explicitly out of scope, orthogonal.
6. Nested `workflow_as_executor_body()` (ADR-150) event propagation — checked directly by the red
   team this pass (not deferred blind): the inner supervisor's own event producer, if wired, is
   completely independent of the outer one, and `run_once()`'s existing `call_mutex` already
   serializes concurrent deliveries to the wrapped node — no new hazard found. Confirmed appropriate
   to leave as issue #38 territory, not folded into this work.

### New findings from the red-team pass (not questions in the first draft at all)

- **Retry-attempt aliasing**: `execute()`'s retry loop can dispatch the same `executor_index`
  multiple times in one round (`EdgeFailurePolicy::retry`); each attempt gets a fresh
  `run_executor_job` and, for an agent-kind node, a fresh `AgentSession::start_run()`/`run_id`. The
  wrapper needs an `attempt` field (added in §3c above) — without it, a live consumer sees a failed
  first attempt's partial tokens and a retried second attempt's tokens arrive tagged with the exact
  same `{executor_id, round}`, no signal to discard/reset rather than append.
- **Taint preservation needs an explicit test, not an assumption**: §3a's "wraps... unchanged"
  language must become a real test asserting a wrapped `agent_turn_event`'s inner content is still
  `tainted=true` end to end (ADR-060's `force_tainted()` precedent is the pattern to inherit, not
  just cite).
- **I2/I3 compliance**: confirmed by direct trace — the bridge is a pure outbound sink, `void(...)`,
  never awaited or inspected by the executor body itself (no new effect-execution or decision path
  back into a body); `route_selected`'s payload is a passive mirror of a decision already made
  through the existing `route_from()`/`ExecuteReply::routes` mechanism. State explicitly in the
  eventual ADR that every event in this design is observation-only and must never be fed back into
  a policy/authorization decision — matches I3's existing discipline, not a new exception to it.

## 5. What this draft does not claim

- Does not decide whether `moderator_stream_delta` ships in v1 or as a fast-follow (open question
  4 above) — an implementation-planning decision, not a red-team finding.
- Does not touch `enable_live_view()`, `RunEvent`/`run_event_kind` (RFC 013, locked), or
  `EffectContext`'s existing fields' behavior for any caller that never wires the new bridge.
- Does not standardize a `Custom.type_id` convention for generic tool-progress UI rendering (issue
  #29 Group F, separate non-blocking follow-up).
- Does not design the new MPSC primitive's own internals (drop policy, bound size, coalescing
  rule) — that is implementation work for the next pass, scoped here only as "non-blocking on the
  producer side, bounded, lossy-under-lag, never backpressure-propagating."

## 6. Files (planned, not yet created)

- `docs/planning/workflow-event-stream-design-draft.md` (this file)
- `include/agentengine/workflow/workflow_event.hpp` (new — `WorkflowEvent`/`workflow_event_kind`,
  including the `attempt` field)
- `include/agentengine/workflow/multiplex_sink.hpp` (new — the dedicated non-blocking MPSC
  primitive for the multiplexed bucket; NOT built on `channel_producer<T,E>`)
- `include/agentengine/core/effect_context.hpp` (edit — new dedicated field for the bridge)
- `include/agentengine/rt/agent_session.hpp` (edit — new independent callback tap, parallel to
  `enable_event_stream()`, not replacing it)
- `include/agentengine/rt/workflow_supervisor.hpp` (edit — `enable_event_stream()`, structural-
  event push sites mapped to EVERY `WorkflowResult` return site including the non-`execute()` ones,
  the per-delivery bridge wiring in `run_executor_job`)
- `include/agentengine/rt/agent_workflow_executor.hpp` (edit — restructure `drive()` to interleave
  drain-and-forward with `resume()`, with the CONCURRENCY CONTRACT comment re-verified and a test
  proving it still holds)
- `tests/test_rt_workflow_event_stream.cpp` (new — including a taint-preservation test, a
  retry-attempt-aliasing test, and the concurrent-push/backpressure stress test adapted from the
  red team's scratchpad version into a real, repo-tracked test)
- `examples/24_*.cpp` (new — a fan-out round with 2+ concurrently-streaming agent-kind nodes,
  proving live interleaving under a deliberately slow consumer, not just presence of events)
- `decisions/ADR-152-workflow-event-streaming.md` (shipped)

## 7. Recommended next step

Design is now settled enough to scope an implementation plan against, pending one project-owner
call: **is `moderator_stream_delta` in v1** (open question 4, §4). Once that's decided, this moves
to implementation + its own `prove` pass (the taint-preservation test, the retry-attempt test, and
a repo-tracked version of the red team's backpressure/liveness stress test — this time proving the
NEW non-blocking primitive never blocks a worker thread under a saturated/absent consumer, which is
the property the whole revised architecture exists to guarantee).
