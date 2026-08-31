# ADR-152: WorkflowSupervisor's fine-grained, genuinely-live event stream (issue #29)

## 1. The question

`WorkflowSupervisor`'s only observability surface, `enable_live_view()`, emits one coarse
`WorkflowLiveEvent` per superstep round — no per-executor content, no token-level streaming, no
visibility into a round while it is running, only a summary once it is done. Meanwhile
`AgentSession::enable_event_stream()` already has a rich, per-run event vocabulary (real
per-token `model_delta` streaming among it) — but that stream exists only per-`AgentSession`, with
no wiring into `WorkflowSupervisor` at all. Filed as issue #29, applying to every 014 §3 pattern
(Sequential, Concurrent, Handoff, Group Chat, Planner/Magentic, Map-Reduce, Router, Reflection), not
Magentic-specific.

Project-owner direction for this pass: **every event any node produces during a run must be
yielded live, the instant it happens** — genuine per-token, per-tool-call, per-route-decision
streaming, multiplexed from however many nodes are concurrently in flight within one round, not a
richer round summary. Also, explicitly: **this repo hasn't shipped, so a fresh architecture is fine
if it's genuinely better than patching the first draft's approach into working.**

## 2. Scope

One piece: `WorkflowSupervisor::enable_event_stream()` (`include/agentengine/rt/
workflow_supervisor.hpp`), returning `agentengine::workflow::WorkflowEventStream`
(`include/agentengine/workflow/workflow_event.hpp`, new) — a merged, poll-only consumer over two
buckets:

1. **Structural** (routing/lifecycle/checkpoint/merge decisions) — `workflow_run_started/
   _suspended/_resumed/_completed/_failed`, `superstep_started/_completed`, `executor_dispatched/
   _completed`, `message_routed`, `fan_out_dispatched`, `fan_in_aggregated`, `route_selected`,
   `request_port_opened/_resolved`, `checkpoint_saved`, `merge_completed/_conflict`. Produced only
   by `execute()`'s own single coroutine, at existing/new hook points — the same single-writer
   channel shape `enable_live_view()` already uses, zero new concurrency risk.
2. **Multiplexed per-node** (`agent_turn_event`, `moderator_stream_delta`) — an agent-kind node's
   real per-token `RunEvent`s, or a function-kind moderator's own forwarded `chat_stream()` deltas,
   produced concurrently from whichever `ThreadPool` worker thread is running that node's call, tagged
   with `{executor_id, round, attempt}`.

## 3. Two red-team passes and how each finding was addressed

### Pass 1 — design draft, before any code existed (fresh agent, zero prior context)

The first draft's central question was whether concurrent `push()` calls, from multiple
`ThreadPool` worker threads, into ONE retained `channel_producer<T,E>` instance, were memory-safe.
**Confirmed yes, empirically** — a scratchpad stress test (8–32 threads, ~2.7M pushes,
`/fsanitize=address`) found zero corruption, zero lost pushes, zero deadlocks. That confirmation
turned out not to matter:

1. **MUST-FIX — the real hazard was never memory safety, it was liveness.**
   `channel_producer<T,E>::push()` deliberately *blocks the calling OS thread* when its queue is
   full (correct for a dedicated I/O thread; wrong for a `ThreadPool` worker thread also needed for
   real workflow compute). Enough concurrently-streaming nodes in one fan-out round, each blocked
   waiting on a lagging UI consumer, could exhaust the pool and stall actual workflow execution —
   unacceptable for a purely-additive observability feature. **Fixed**: built a dedicated,
   purpose-built, non-blocking MPSC primitive (`workflow::multiplex_sink<T>`,
   `include/agentengine/workflow/multiplex_sink.hpp`) for the multiplexed bucket only — `push()`
   never blocks, an overflow is dropped (not an already-queued item evicted), with a running drop
   count for diagnostics. The structural bucket keeps the original channel-based design (single
   writer, never at risk).
2. **MUST-FIX — the first draft's own named "safe fallback" wasn't a fix at all.** It proposed
   private per-delivery buffers, drained by `execute()`'s own coroutine "between yield points." Read
   directly: `execute()`'s round-collection loop calls blocking `std::future::get()` in a tight loop
   with no yield points until every job in the round finishes — that fallback's real behavior is
   "buffer everything privately during the round, dump it all at once when the round ends," i.e.
   exactly the round-boundary batching issue #29 was filed to eliminate. **Fixed**: not used; the
   multiplex_sink design above delivers genuinely instant per-event forwarding instead.
3. **MUST-FIX — reusing `EffectContext::report_progress` for the bridge was a real collision, not
   hypothetical.** A `function`-kind body that calls `Tool<>::invoke()` directly (a normal,
   already-supported pattern, no `AgentSession` involved) also reads `report_progress`; overwriting
   it for the workflow bridge would silently reinterpret that tool's own progress as a workflow
   event. **Fixed**: two new, dedicated `EffectContext` fields (`agent_turn_sink`,
   `moderator_delta_sink`, `core/effect_context.hpp`), matching this file's existing "one field per
   audience" precedent.
4. **MUST-FIX — bridging via a second `AgentSession::enable_event_stream()` call would silently
   evict any consumer an app already attached directly to that session** (the channel's own
   "second call replaces the producer" contract). **Fixed**: a second, independent, plain callback
   tap (`AgentSession::set_run_event_tap()`, `rt/agent_session.hpp`) — both a direct
   `enable_event_stream()` consumer and the workflow bridge, if both wired, independently observe
   every event, verified directly in §6 claim 8.
5. Also resolved: an `attempt` discriminator added to the multiplexed payloads (a retried delivery
   to the same `executor_id`/`round` needs to be tellable apart from its failed predecessor);
   confirmed `channel_producer::push()`'s own queue arrival order is already a valid race-free total
   order for the structural bucket, so no second sequence counter was needed; documented that
   cross-node interleaving order in the multiplexed bucket reflects real scheduling races and is not
   run-to-run reproducible (expected for a genuinely live stream, not an I5 violation — the round
   loop's own deterministic `todo`/`replies` collection is unaffected by observability-stream
   ordering).

Full findings: `docs/planning/workflow-event-stream-design-draft.md` §2–§4.

### Pass 2 — implementation review (self-identified during build; see §4 below for what this closed)

Two structural gaps were found and fixed directly during implementation, before any test was
written against them:

6. `agent_session_as_executor_body()`'s wiring needed to forward RunEvents **synchronously from
   inside `emit_run_event_for()`** rather than via a polling drain loop — simpler than the first
   draft's own proposed "interleave drain with resume()" restructuring of that adapter's
   concurrency-fragile `drive()` loop, and provably equivalent: the tap fires on whatever thread is
   already running that coroutine's resumption, no new thread hop, no polling latency at all. This
   is a genuine simplification over the design draft's own plan, not a shortcut — verified directly
   in §6 claim 8 (both a direct tap and the workflow bridge see the identical live sequence).
7. Every non-`execute()` `WorkflowResult` return site (`run_workflow`'s/`resume_workflow`'s/
   `continue_workflow`'s own `!valid_`/invalid-interaction-id early returns) needed its own
   `workflow_run_failed`/`_suspended` event — `finish()` alone does not see those paths. Wired
   explicitly (see `run_workflow`/`resume_workflow`/`continue_workflow` in
   `rt/workflow_supervisor.hpp`), closing the gap the first design draft's own §3a named but did not
   resolve.

## 4. The accepted design

- `workflow::WorkflowEvent`/`workflow_event_kind`/`WorkflowEventPayload` (new,
  `workflow/workflow_event.hpp`) — mirrors `RunEvent`/`run_event_kind`'s own shape without touching
  that already-locked RFC 013 type; lives in `workflow/`, not `rt/`, so `RunFailed`'s payload carries
  a plain `status_tag` string (`workflow_status_tag()`, `rt/workflow_supervisor.hpp`) rather than
  `rt::workflow_status` itself — `workflow/` does not depend on `rt/`.
- `workflow::multiplex_sink<T>` (new, `workflow/multiplex_sink.hpp`) — bounded (default 1024),
  mutex-guarded, multi-producer/single-consumer, `push()` never blocks/never fails (drops on
  overflow, tracks a drop count), `try_pop()` non-blocking poll. Deliberately not built on
  `rt::channel.hpp`'s `channel_producer<T,E>` — see §3 finding 1.
- `workflow::WorkflowEventStream` (new, same header) — the consumer-facing handle
  `enable_event_stream()` returns: merges the structural channel-backed `stream<WorkflowEvent>` and
  the multiplexed `multiplex_sink<WorkflowEvent>` behind one `next()`/`done()` poll surface, draining
  the multiplexed bucket first (the latency-sensitive one).
- `EffectContext::agent_turn_sink`/`moderator_delta_sink` (new fields, `core/effect_context.hpp`) —
  the per-call bridge fields; both default no-op, wired per-delivery by
  `WorkflowSupervisor::run_executor_job()` only when `enable_event_stream()` has been called
  (`workflow_event_stream_enabled_` gate — zero cost otherwise).
- `AgentSession::set_run_event_tap()` (new, `rt/agent_session.hpp`) — the independent callback tap;
  `emit_run_event_for()` calls it unconditionally alongside (never instead of) the existing
  `run_event_producer_` push, guarded by a combined "either sink attached" check so an untouched
  session still pays nothing.
- `agent_session_as_executor_body()` (`rt/agent_workflow_executor.hpp`) wires
  `session.set_run_event_tap([&ctx](RunEvent const& ev){ ctx.agent_turn_sink(ev); })` immediately
  before `start_run()` and resets it with a genuinely empty `std::function` (not another no-op
  lambda — needed so `AgentSession::run_event_tap_attached_` correctly flips back to false)
  immediately after — call-scoped, mirroring `report_progress`'s own ADR-060 discipline. No
  restructuring of the existing `drive()` spin loop was needed (§3 finding 6).
- `route_from()` (`rt/workflow_supervisor.hpp`) now emits `message_routed` per firing edge,
  `fan_out_dispatched` per call with firing `fan_out` edges, and `route_selected` per call with
  `switch_case`/`multi_selection` edges present; it also appends to a round-scoped
  `fan_in_edges_this_round_` member for every firing `fan_in` edge, drained into one
  `fan_in_aggregated` event per target (aggregated across the WHOLE round, not per-edge) by
  `push_fan_in_aggregated_events()`, called by `execute()` right after each of its two routing loops.
- Capability sourcing (I2) and I3: unaffected. The bridge fields are pure outbound sinks — `void`
  return, never awaited or inspected by the executor body, never a new effect-execution or decision
  path back into a body. Every event in this vocabulary is observation-only; `route_selected`'s
  payload is a passive mirror of a decision `route_from()` already made through the pre-existing
  `ExecuteReply::routes` mechanism, never a second, independent source of truth for it.

Full design: `docs/planning/workflow-event-stream-design-draft.md`.

## 5. What this ADR does not claim

- Does not touch `enable_live_view()`, `RunEvent`/`run_event_kind` (RFC 013, locked), or any
  existing `EffectContext` field's behavior for a caller that never wires the new bridge.
- Does not standardize a `Custom.type_id` convention for generic tool-progress UI rendering (issue
  #29's own Group F, a separate, non-blocking follow-up).
- Does not address the backgrounded-tool progress gap (ADR-060 §4's own already-named residual) —
  orthogonal to workflow-level streaming.
- Does not touch nested `workflow_as_executor_body()` (ADR-150) event propagation — checked
  directly (the inner supervisor's own event producer, if wired, is completely independent of the
  outer one; `run_once()`'s existing `call_mutex` already serializes concurrent deliveries to the
  wrapped node) — no new hazard found, appropriately left as issue #38 territory.
- Does not fan out to multiple simultaneous `enable_event_stream()` consumers — same "second call
  replaces the producer" single-consumer convention `enable_live_view()` already uses, unchanged
  here.
- `moderator_stream_delta` forwarding is opt-in machinery only (I3: a body that never calls
  `moderator_delta_sink` is simply coarser-grained observability for that node, never a violation) —
  no engine enforcement that any real moderator/router body actually streams; that remains an
  application authoring choice, demonstrated in `examples/25_workflow_event_stream_live.cpp`.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | Structural events (`workflow_run_started`, `superstep_started/_completed`, `executor_dispatched/_completed`, `message_routed`, `workflow_run_completed`) fire, in a plausible order, for a plain linear graph, with the real output unaffected by whether the stream is attached. | CORRECT | `test_rt_workflow_event_stream.cpp` W1 |
| 2 | A run that never calls `enable_event_stream()` behaves identically (same status, same output) to one that does. | CORRECT | W2 |
| 3 | `request_port_opened`/`request_port_resolved` fire correctly around a real suspend/resume cycle, with resolution only firing once the port is actually resumed. | CORRECT | W3 |
| 4 | `checkpoint_saved` fires exactly once per real `checkpoint_hook_` invocation, never more, never fewer. | CORRECT | W4 |
| 5 | `fan_out_dispatched` carries every real target of one source's fan-out; `fan_in_aggregated` is ONE event per target, aggregated across the whole round, listing every real contributing source — not one event per converging edge. | CORRECT | W5 |
| 6 | `route_selected` reports the real chosen case against the real full set of available cases on a `switch_case` graph. | CORRECT | W6 |
| 7 | A retried delivery's multiplexed events carry `attempt=0` then `attempt=1` for the identical `{executor_id, round}` — a live consumer can tell a failed attempt's partial output apart from the retry's. | CORRECT | W7 |
| 8 | An agent-kind node's real `RunEvent` sequence reaches the workflow-level `agent_turn_event` bridge AND a direct `AgentSession::enable_event_stream()` consumer wired simultaneously on the same session — both observe the identical sequence, neither evicts the other. | CORRECT | W8 |
| 9 | The central liveness property: a fan-out round with several concurrently-dispatched bodies, each pushing far more multiplexed events than the sink's capacity, into a stream NO ONE DRAINS, still completes in bounded time — no worker thread ever blocks in `push()` — and the sink's own drop counter confirms overflow was actually dropped, not silently unbounded. | CORRECT (adversarially verified) | W9; with `multiplex_sink::push()` temporarily mutated to spin-block on a full queue, the SAME test hung and was killed by a 15s process timeout (exit 124), never reaching W9's own output — reverted, re-verified clean (all 35 checks pass) before this ADR was written |
| 10 | The example (`examples/25_workflow_event_stream_live.cpp`) proves the same properties end-to-end against a real concurrent 3-way fan-out/fan-in graph, not just the minimal unit-test shapes above. | CORRECT | 9 real moderator_stream_delta events observed live (3 participants × 3 steps), exactly 1 aggregated fan_out_dispatched, exactly 1 aggregated fan_in_aggregated |
| 11 | A real, live model call (OpenRouter, `openai/gpt-4o-mini`) through `agent_session_as_executor_body()`, with `stream_model_calls_` engaged, delivers its actual per-token `model_delta` sequence to the workflow-level `agent_turn_event` bridge live — the reassembled text from the streamed deltas matches the model's real answer. | CORRECT | `examples/26_workflow_event_stream_live_openrouter.cpp`, run live: 39 real `model_delta` events observed, one word/sub-word fragment at a time ("Quantum", " ent", "ang", "lement", ...), reassembling byte-for-byte into the model's real sentence-length answer about quantum entanglement — see run transcript, ADR write time |
| 12 | Every pre-existing workflow-family and agent-session test still passes; the wider repo-wide test suite is unaffected. | CORRECT | Full `ctest -C Debug`: **308/309 passed** (2 skipped: `test_shell_runner_no_process_creation`/`test_mediated_shell_runner_no_process_creation`, pre-existing environment-gated skips, unrelated). The one failure, `test_reference_agent_task_corpus`, is the same long-documented, pre-existing matplotlib/pandas environment gap ADR-149's own `decisions/README.md` row already names (ADR-081 et al.) — not a regression from this work; confirmed by reading its failure output directly (a missing `matplotlib` install in this environment, not a code defect). |
| 13 | The full project builds clean, including under `-Werror`/`/WX`. | CORRECT | Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC), exit code 0, zero errors across the whole tree |

## 7. Files changed

**New:**
- `docs/planning/workflow-event-stream-design-draft.md`
- `include/agentengine/workflow/workflow_event.hpp`
- `include/agentengine/workflow/multiplex_sink.hpp`
- `tests/test_rt_workflow_event_stream.cpp`
- `examples/25_workflow_event_stream_live.cpp`
- `examples/26_workflow_event_stream_live_openrouter.cpp` — the live-model counterpart to 25;
  needs `AGENTENGINE_OPENROUTER_API_KEY`, SKIPs (exit 0) when unset, same convention as
  `examples/16/19_*_live.cpp`.

**Edited:**
- `include/agentengine/core/effect_context.hpp` — `agent_turn_sink`/`moderator_delta_sink` fields.
- `include/agentengine/rt/agent_session.hpp` — `set_run_event_tap()`, `run_event_tap_`/
  `run_event_tap_attached_` members, `emit_run_event_for()` wiring.
- `include/agentengine/rt/agent_workflow_executor.hpp` — the tap bracket around `start_run()`.
- `include/agentengine/rt/workflow_supervisor.hpp` — `enable_event_stream()`, `workflow_status_tag()`,
  `push_structural_event()`/`push_fan_in_aggregated_events()`, `run_executor_job()`'s bridge wiring,
  `route_from()`'s per-edge event emission, every structural-event call site listed in §4.
- `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new target registrations.

No existing engine invariant (I1–I8) was relaxed; `enable_live_view()`, `RunEvent`/`run_event_kind`,
and `WorkflowLiveEvent` are all untouched.

**Incidental, disclosed fixes found while driving a genuinely clean full build+`ctest` run** (none
touch this ADR's own mechanism; each is a pre-existing latent issue in an unrelated file, surfaced
only because this pass's own full rebuild was the first to exercise it against a `-Werror`/`/WX`
level a concurrent session had recently tightened):
- `tests/test_rt_multi_agent.cpp` — an unconditional `throw` followed by an unreachable `co_return`
  (needed only to keep the lambda a valid `task<T>`-returning coroutine) tripped MSVC C4702.
  Fixed with a `static volatile bool` guard so the compiler can no longer prove the branch
  unconditional — not a `#pragma` suppression, which CONVENTIONS.md/`pal/env.hpp`'s own precedent
  explicitly forbids ("a warning is fixed at its site... this project does not allow" pragma
  suppression). Re-verified the throwing-thunk test (P5) still genuinely exercises the throw path.
- `include/agentengine/core/skill_provider.hpp` — a parameter named `name` shadowed a class member
  of the same name (C4458). Renamed to `skill_name`.
- `examples/17_planner_live.cpp` — a `std::toupper()` result (`int`) implicitly narrowed to `char`
  inside `std::transform` (C4244) — the identical bug `examples/19_magentic_builder_live.cpp`'s own
  `upper()` helper already had fixed with an explicit `static_cast<char>`; applied the same fix here.
- `tools/cli_chat.cpp` — a diagnostic banner narrowed a `std::wstring` mount path to `std::string`
  via raw iterator construction (C4244, byte-truncating for non-ASCII). Made the truncation
  explicit via a per-character `static_cast<char>` loop — preserves the pre-existing (already
  lossy) display behavior byte-for-byte, just makes the narrowing an intentional statement instead
  of an implicit conversion; this is diagnostic-only output, never used for a real filesystem path.

## Status

**Proposed — implemented, red-teamed twice (design draft before any code existed, plus two
structural gaps closed during implementation), the single most severe claim (§6 #9) adversarially
verified post-fix (mutation-tested, not merely re-reasoned about), 35/35 new test checks passing,
a real live-model run (§6 #11) proving the bridge end to end against OpenRouter, the full project
building clean and 308/309 `ctest` passing repo-wide (the one failure pre-existing and unrelated —
§6 #12), pending project-owner sign-off.**
