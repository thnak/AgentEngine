# Streaming and event support matrix (2026-09-01)

No such table existed anywhere in this repo before this pass (checked: RFC 013-UI-and-Streaming-
Surfaces.md's own §1/§2 name the event vocabulary and its AG-UI projection but carry no per-type
support matrix; `decisions/README.md` has no dedicated row for this either). Built by direct code
read, not from memory (CLAUDE.md: "do not assert what a protocol/component does from memory") —
every claim below cites a real file:line. Treat this as dated research, same discipline
`docs/research/` applies: a real snapshot of 2026-09-01, subject to going stale as new conformers are
added — re-verify before trusting a row for a type not touched since this date.

## Two separate axes — do not conflate them

This codebase has TWO independent event/streaming mechanisms, easy to confuse because both use the
word "streaming":

- **Axis A — `ChatClient`-level content streaming.** `chat_stream()`/`call_stream()` push raw
  `ChatResponseUpdate{delta: ContentItem, is_final, usage}` fragments — text deltas, tool-call-argument
  fragments (`core/chat_client.hpp:150-167`). This is what "does this backend stream tokens live" means.
  A `ChatClient` has NO concept of "tool call started" as a distinct event — that's synthesized one
  layer up.
- **Axis B — `RunEvent`/`run_event_kind` granular turn events.** `run_started`, `turn_started`,
  `model_call_started`, `model_delta`, `model_call_finished`, `tool_call_started`, `tool_call_delta`,
  `tool_call_finished`, `sandbox_exec_started`, `sandbox_exec_finished`, `input_required`/`resolved`,
  `auth_required`/`resolved`, `approval_requested`/`resolved`, `codeact_ask_requested`,
  `hook_decision_requested`, `warning`, `policy_decision` (`core/run_event.hpp:41-57`, the full,
  current enum). This is what "tool calling / tool called" actually means in this codebase's own
  vocabulary. **Only `AgentSession` itself ever emits these** (`emit_run_event()` — every other type
  either forwards them, silences them, or was never wired to see them in the first place). There is no
  such thing as a `ChatClient` conformer emitting a `tool_call_started` event; skill-mount/script-start
  events do not exist as distinct kinds either — mounting a skill is an ordinary tool call
  (`mount_skill`), so it surfaces as `tool_call_started`/`tool_call_finished` with that tool name, not
  its own kind.

A third, unrelated vocabulary exists at the WORKFLOW level: `workflow_event_kind`
(`workflow/workflow_event.hpp`) — structural events (`executor_dispatched`, `request_port_opened`,
`workflow_run_started`, etc.) plus two payload kinds that CARRY Axis-B events across the workflow
boundary: `AgentTurn` (wraps a real forwarded `RunEvent`) and `ModeratorDelta` (wraps a raw text delta
from a `function`-kind node's own `chat_stream()` call). Do not confuse `workflow_event_kind` with
`run_event_kind` — they are sibling, not overlapping, enums.

## Axis A — real `ChatClient`/gateway conformers, `chat_stream()`/`call_stream()`

| Type | Real incremental push? | Evidence | Notes |
|---|---|---|---|
| `OpenAIChatClient` | **YES** — real SSE/chunked decode, pushed as bytes arrive | `protocol/openai/chat_client.hpp` `run_stream_worker` | The live baseline every other row is measured against. |
| `AnthropicChatClient` | **YES** — same shape | `protocol/anthropic/chat_client.hpp:1146` | |
| `ModelCallGateway<Primary, Fallback...>` | **YES** — forwards the live winning tier's own chunks, pre-commit chunks never exposed | `core/model_call_gateway.hpp:258` (`call_stream()`), file banner `:57-70` | Retry/failover stay invisible to the caller; a mid-attempt failure after any chunk has been shown is terminal, matching `call()`'s own single-attempt contract for that case. |
| `MiddlewareModelCallGateway` | **NO** — `call()` only | `model_call_gateway.hpp:66-67`'s own comment, confirmed by direct read | **Named, disclosed residual**, not new — from `unified-streaming-design-draft.md` §3. `AgentSession::run_model_call()` already has a fallback path and a warning for exactly this case. |
| `ContentReplayGateway` | **NO** — `call()` only | same citation | Same disclosed residual. |
| `RoutingModelCallGateway<SelectFn, Routes...>` | Depends on Route 0's own type | `core/routing_model_call_gateway.hpp:78` (`capabilities()` reads Route 0 only) | Scoped, disclosed limitation (ADR-148) — heterogeneous-capability routing is real, separate, unbuilt work. |
| `ReplayChatClient` | **YES**, but REPLAYED not live — real chunk-by-chunk push with real inter-chunk timing reproduction | `core/replay_chat_client.hpp:93-114` (`run_replay_worker`) | Deterministic playback of a recorded `ChatCallRecording`; genuinely multi-push, not a single-shot stub. |
| `RecordingChatClient<Inner>` | **Inherits `Inner`'s own answer** — forwards `Inner`'s real `chat_stream()` while recording | `core/recording_chat_client.hpp:167-266` | Gates on `LegacyChatClient` (needs `Inner::chat()` too) — see Axis A's own "must have chat()" note below. |
| `WorkflowChatClient` | **NO, by design** — one batch of N pushes (one per output `ContentItem`) after the WHOLE wrapped workflow run completes, `is_final` only on the last | `rt/workflow_as_chat_client.hpp` (ADR-162) | Declares `capabilities().streaming = false` — honest, not a gap: a whole `Workflow` run has no natural token-by-token granularity at ITS OWN boundary. That granularity exists one layer down, inside any `agent`-kind node's own real model call — routed through Axis B instead (see below), deliberately kept separate. |

**`chat()` (the `LegacyChatClient`-only method) support**, for completeness since `RecordingChatClient`
specifically requires it: `OpenAIChatClient`/`AnthropicChatClient`/`ReplayChatClient` all have it;
`ModelCallGateway` uses `call()` (a different name, same shape) so does not itself satisfy
`LegacyChatClient`; `WorkflowChatClient` deliberately has NEITHER `chat()` NOR anything named `call()` —
see ADR-162 §3/§5 for why re-adding one would silently reintroduce a closed defect.

## Axis B — `RunEvent`/`run_event_kind` emission and forwarding

| Type | Emits real events? | Forwards them onward? | Evidence |
|---|---|---|---|
| `AgentSession<...>` | **YES — the only real emitter** | N/A (source) | `emit_run_event()`, called at every real lifecycle point (`run_started`/`model_call_started`/`tool_call_started`/etc., `agent_session.hpp`, dozens of call sites); consumed by `enable_event_stream()` (a real, direct stream) or `set_run_event_tap()` (a plain callback). |
| `agent_session_as_executor_body()` | forwards, does not originate | **YES** — bridges the wrapped session's live `RunEvent`s into `ctx.agent_turn_sink` for the duration of exactly one call, call-scoped (reset to empty immediately after) | `rt/agent_workflow_executor.hpp:101-118` (ADR-152) |
| `WorkflowSupervisor::enable_event_stream()` | receives forwarded events, wraps in `AgentTurn`/`ModeratorDelta` | **YES**, per-node, live, multiplexed across concurrent dispatches within one round | `workflow/workflow_event.hpp`, ADR-152 |
| `workflow_as_executor_body()` (ADR-150) | — | **NO** — the outer `EffectContext` (including `agent_turn_sink`) is entirely unused by this adapter, neither read nor written | `rt/workflow_as_executor.hpp` file banner, "CAPABILITY SOURCING" paragraph (same "outer ctx unused" stance, extended here to cover events too — confirmed by direct read: no `ctx.agent_turn_sink` reference anywhere in that file) |
| `WorkflowChatClient` (ADR-162) | — | **NO, deliberately severed** — `sanitize_for_detached_worker()` resets `agent_turn_sink`/`moderator_delta_sink`/`report_progress` to no-ops before the detached worker ever runs | `rt/workflow_as_chat_client.hpp`, `sanitize_for_detached_worker()` |
| `OpenAIChatClient`/`AnthropicChatClient`/`ModelCallGateway`/etc. (Axis A conformers) | — | **N/A — structurally cannot**; a `ChatClient` has no `EffectContext::agent_turn_sink`-shaped seam at all, only `chat_stream()`'s own content deltas | `core/chat_client.hpp` — no such field/parameter exists on the concept |

## The policy this project already follows (made explicit, not invented here)

Both closed adapters above (`workflow_as_executor_body()`, `WorkflowChatClient`) independently arrived
at the SAME rule when their own red-team passes examined this question, which is worth stating as an
explicit, followable policy rather than leaving it implicit in two separate ADRs:

1. **A type that wraps or crosses a THREAD or LIFETIME boundary (a detached worker, a call that outlives
   the caller's own stack frame) MUST NOT forward a caller-supplied event callback across that boundary
   unless the callback is itself an owned, thread-safe, lifetime-independent value.** `ctx.agent_turn_sink`/
   `report_progress`/`moderator_delta_sink` are ordinary `std::function`s captured by whatever closure the
   ORIGINAL caller built — reaching back into a since-returned frame from a detached thread is exactly the
   hazard `tool_pipeline.hpp::background_task()` (ADR-060 §4) and `WorkflowChatClient`'s own
   `sanitize_for_detached_worker()` both close by resetting to a no-op, never by trusting the caller to
   have kept everything alive.
2. **A type whose OWN embedding boundary has no natural per-token/per-event granularity (an adapter whose
   "one call" is actually a whole multi-step orchestration) MUST NOT fabricate synthetic granularity at
   that boundary — it reports coarse, honest outcomes only, and names the SEPARATE, already-existing
   channel (`WorkflowSupervisor::enable_event_stream()`) a caller can reach independently if it wants the
   real granularity.** This is why `WorkflowChatClient` declares `streaming=false` rather than inventing
   fake incremental updates, and why it doesn't try to smuggle Axis-B events through Axis-A's own
   `ChatResponseUpdate` shape (which has no field for them anyway).
3. **A type that owns the ORIGINAL model call and stays within the calling thread's own lifetime
   (`agent_session_as_executor_body()`, still on the coroutine that dispatched it) SHOULD forward real
   events, and already does.** The one-field-per-audience rule (ADR-152's own red-team finding) applies
   here too: `report_progress` (a tool's OWN progress), `agent_turn_sink` (an agent-kind node's real
   RunEvents), and `moderator_delta_sink` (a function-kind node's own forwarded content deltas) are three
   SEPARATE fields specifically because reusing one for multiple audiences was found to silently
   misattribute one audience's data as another's.
4. **A new adapter added to either table above should be classified by asking**: (a) does it cross a
   thread/lifetime boundary a caller's own callback could dangle across (Axis B: if yes, sanitize/reset,
   don't forward blindly); (b) does its own embedding boundary have real per-token granularity of its own,
   or is it a coarse batch/summary by nature (Axis A: report honestly either way, never fabricate).

## Worked examples

Real, cited code, not paraphrase — each snippet below is quoted verbatim (trimmed for length only)
from the file:line named beside it, matching this doc's own "every claim cites a real file:line"
discipline.

### Axis A, live incremental push — draining a real `chat_stream()`

`examples/28_workflow_as_chat_client.cpp:63-78`, the same polling shape
`rt/agent_session_trust.hpp`'s own `drain_streaming_response()` uses internally for every real Axis-A
conformer (`OpenAIChatClient`/`AnthropicChatClient`/`ModelCallGateway`/`ReplayChatClient` all get
drained this way by `AgentSession` itself — this is the canonical consumer loop, not one adapter's own
invention):

```cpp
[[nodiscard]] std::vector<ChatResponseUpdate> drain(stream<ChatResponseUpdate> s, char const* label) {
    std::vector<ChatResponseUpdate> updates;
    while (!s.done()) {
        while (auto upd = s.next()) {
            std::printf("[%s] update (is_final=%s)\n", label, upd->is_final ? "true" : "false");
            updates.push_back(std::move(*upd));
        }
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (s.terminal() != stream_terminal::closed) {
        std::printf("[%s] stream ended with an error: %s\n", label, s.fail_error().message.c_str());
    }
    return updates;
}
```

### Axis A, coarse batch push — `WorkflowChatClient`'s own honest non-streaming shape

`include/agentengine/rt/workflow_as_chat_client.hpp:375-393` (the `completed` branch of
`chat_stream()`'s terminal push): every `ContentItem` of the wrapped workflow's final output message
becomes its own `ChatResponseUpdate`, `is_final` only on the last — a real, ordered multi-push, but
built AFTER the whole run finishes, never incrementally as the run progresses (that's exactly what
`capabilities().streaming = false` discloses):

```cpp
for (std::size_t i = 0; i < r.output.content.size(); ++i) {
    agentengine::ChatResponseUpdate upd{};
    upd.delta = r.output.content[i];
    upd.is_final = (i + 1 == r.output.content.size());
    if (upd.is_final) upd.usage = usage_delta;
    if (producer.push(std::move(upd)) != agentengine::stream_push::ok) return;
}
producer.close();
```

### Axis B, call-scoped forwarding — `agent_session_as_executor_body()`'s live `RunEvent` tap

`include/agentengine/rt/agent_workflow_executor.hpp:113-118` (ADR-152): a real, per-call bridge from
the wrapped `AgentSession`'s own `emit_run_event()` calls to whatever `ctx.agent_turn_sink` the
enclosing `WorkflowSupervisor` wired up — bracketed to exactly one `start_run()` call, then reset to a
genuinely empty function (not another no-op lambda) so a later, unrelated call into the same session
never inherits a closure captured by reference into a since-returned `EffectContext`:

```cpp
session.set_run_event_tap([&ctx](agentengine::RunEvent const& ev) { ctx.agent_turn_sink(ev); });

agentengine::result<AgentResponse> driven =
    agent_executor_detail::drive(session.start_run(StartRun{in}));

session.set_run_event_tap({});
```

### Axis B, workflow-level consumption — `WorkflowSupervisor::enable_event_stream()`

`tests/test_rt_workflow_event_stream.cpp:157-173` (W1): opting in, running, then draining the
structural events a plain linear graph produces with no agent-kind node involved at all —
`workflow_run_started` → `executor_dispatched`/`executor_completed` per node → `message_routed` →
`workflow_run_completed`, the same channel `agent_turn_event`/`moderator_stream_delta` ride when an
agent-kind or function-kind node's own events get forwarded onto it:

```cpp
WorkflowSupervisor sup;
sup.initialize(linear_graph(), linear_bodies());
WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));

std::vector<WorkflowEvent> evs = drain_all(stream);
check(contains_kind(evs, workflow_event_kind::workflow_run_started), "workflow_run_started fired");
check(contains_kind(evs, workflow_event_kind::executor_dispatched), "executor_dispatched fired");
check(contains_kind(evs, workflow_event_kind::workflow_run_completed), "workflow_run_completed fired");
```

Calling `enable_event_stream()` is opt-in and zero-cost when skipped (W2, same test file) — a run that
never wires it up proceeds with byte-identical output/status, no behavior change.

## What this table does NOT cover

- MCP/A2A/AG-UI protocol-surface event projection (013 §2's own "AG-UI projection" mapping) — a
  separate, further translation layer downstream of `run_event_kind`, out of scope here.
- Whether a given `Tool<>` itself calls `ctx.report_progress()` — that's per-tool, not per-`ChatClient`/
  adapter, and already has its own real audit surface (`tool_pipeline.hpp`'s own dispatch).
- Sandbox-level events (`sandbox_exec_started`/`finished`) beyond confirming they're real `run_event_kind`
  values — which backend/path actually fires them is a separate question this pass didn't chase.
