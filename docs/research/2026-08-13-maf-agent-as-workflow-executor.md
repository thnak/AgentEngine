# How MAF (.NET) wires a real agent into its Workflow graph as an executor node

**Date:** 2026-08-13. **Source:** direct reading of the local MAF clone, `D:\GitSrc\agent-framework`
(dotnet). All claims below cite real file:line in that repo, not memory or MAF's public docs —
per this project's own research discipline (`CLAUDE.md`: "do not assert what a protocol/framework
does from memory"). **Why this doc exists:** AgentEngine's own `workflow/graph.hpp` ported MAF's
`Workflow`/`Executor`/`Edge` graph model, including a closed `executor_kind` enum with an `agent`
member — but nothing in AgentEngine's runtime (`rt::WorkflowSupervisor`) actually wires an
`executor_kind::agent` node to a real agent yet (see the companion gap doc,
`docs/planning/agent-as-workflow-executor-gap.md`). Before designing AgentEngine's own version of
that bridge, this doc records exactly how MAF — the project AgentEngine is deliberately shaped after
— actually built theirs, so the AgentEngine design can follow a real, shipped shape rather than
invent an incompatible one.

## 1. The type that hosts an `AIAgent` as an executor

`AIAgentHostExecutor` (internal, `Microsoft.Agents.AI.Workflows.Specialized` namespace) —
`dotnet/src/Microsoft.Agents.AI.Workflows/Specialized/AIAgentHostExecutor.cs:27`. Derives from
`ChatProtocolExecutor` (`ChatProtocolExecutor.cs:34`), itself derived from
`StatefulExecutor<List<ChatMessage>>`. Users never construct it directly — it's produced through
`AIAgentBinding` (`AIAgentBinding.cs:14-18`) or the extension method
`AIAgentExtensions`/`ExecutorBindingExtensions.BindAsExecutor(this AIAgent, ...)`
(`ExecutorBindingExtensions.cs:422,431`). There is an **implicit conversion**,
`ExecutorBinding(AIAgent agent) => agent.BindAsExecutor()` (`ExecutorBinding.cs:128`) — this is why
MAF samples can write `builder.AddEdge(frenchAgent, spanishAgent)`, passing raw `AIAgent` instances
directly as graph nodes: the graph-building API itself absorbs the wrapping step.

## 2. Bridging the async gap: chat-protocol message handlers, not a single call-in/call-out

`ChatProtocolExecutor.ConfigureProtocol` (`ChatProtocolExecutor.cs:70-90`) registers handlers for
`ChatMessage`/`IEnumerable<ChatMessage>` (buffered into `List<ChatMessage>` state) and a `TurnToken`
handler that flushes the buffer. `AIAgentHostExecutor` overrides
`TakeTurnAsync(List<ChatMessage>, IWorkflowContext, bool? emitEvents, CancellationToken)`
(`AIAgentHostExecutor.cs:212-216`), which calls `ContinueTurnAsync` (lines 179-210). That method:

- optionally forwards incoming messages verbatim (`context.SendMessageAsync`, line 184);
- calls `InvokeAgentAsync` (line 191) — the real bridge (lines 218-298): non-streaming does one
  blocking `await this._agent.RunAsync(messages, session, cancellationToken: ...)` (lines 282-285,
  `Task<AgentResponse>`); streaming iterates `this._agent.RunStreamingAsync(...)`
  (`IAsyncEnumerable<AgentResponseUpdate>`, lines 226-229) and calls
  `context.YieldOutputAsync(update, ...)` per chunk (line 272);
- filters non-portable content (reasoning tokens, provider-raw items) via `FilterForwardableMessages`
  (lines 197, 325-354) before re-sending the agent's reply as `List<ChatMessage>` through
  `context.SendMessageAsync` (line 200) — this literally IS "route to the next node," since MAF's
  edges route by message type;
- sends a `TurnToken` back out (`context.SendMessageAsync(new TurnToken(...))`, line 207) **only if
  there are no outstanding tool/approval requests** — this is what lets the *next* agent executor's
  own `TurnToken` handler fire, i.e. turn-passing is itself gated on a pending-request check, not
  unconditional.

## 3. Session/context ownership: one session per workflow RUN, not per call, not shared

`AIAgentHostExecutor` caches a single `AgentSession` field (`_session`, line 31), lazily created via
`this._agent.CreateSessionAsync(cancellationToken)` in `EnsureSessionAsync` (lines 137-138) —
**reused across turns within one run**, never recreated per call. Isolation across DIFFERENT runs
comes from the binding layer, not the session object itself: `AIAgentBinding.IsSharedInstance =>
false` (`AIAgentBinding.cs:31`), and the constructor passes `declareCrossRunShareable: false` with an
explicit comment: *"we maintain turn state on the instance"* (`AIAgentHostExecutor.cs:47-51`). Each
`Run`/`StreamingRun` gets its OWN executor instances via `ExecutorBinding.CreateInstanceAsync(sessionId)`
(`ExecutorBinding.cs:61-65`), invoked from `InProcessRunnerContext` keyed by `_sessionId`
(`InProc/InProcessRunnerContext.cs:25,43,64,97`) — so a fresh `AIAgentHostExecutor` (and thus a fresh
`AgentSession`) is minted per workflow run, structurally preventing cross-run session leakage rather
than relying on callers to remember to reset one.

Session state is checkpointed via `_agent.SerializeSessionAsync`/`DeserializeSessionAsync` inside
`OnCheckpointingAsync`/`OnCheckpointRestoredAsync` (lines 144-173) — the agent-hosting executor rides
the SAME checkpoint mechanism every other stateful executor uses, not a special case.

**No separate capability/DI context object is threaded through.** Per-call context is the ambient
`IWorkflowContext` (`IWorkflowContext.cs:13`: `SendMessageAsync`, `YieldOutputAsync`, `AddEventAsync`,
`ReadStateAsync`, all taking a `CancellationToken`) plus `AIAgentHostOptions`
(`AIAgentHostOptions.cs:10-47`: `ForwardIncomingMessages`, `ReassignOtherAgentsAsUsers`,
`EmitAgentUpdateEvents`/`EmitAgentResponseEvents`) supplied once, at BINDING time, by whoever built
the graph. The `CancellationToken` is the only thing that flows live, straight from the workflow run
call down into `_agent.RunAsync`/`RunStreamingAsync`.

## 4. Streaming vs. blocking are two distinct code paths, both surfaced through one event stream

`InvokeAgentAsync` (`AIAgentHostExecutor.cs:218-298`) branches explicitly on `emitUpdateEvents`
(derived from `TurnToken.EmitEvents` merged with `AIAgentHostOptions.EmitAgentUpdateEvents` via
`TurnExtensions.ShouldEmitStreamingEvents`, lines 17-24): the streaming branch drives
`RunStreamingAsync` and calls `context.YieldOutputAsync(update, ...)` per update (line 272); the
non-streaming branch does one `await RunAsync(...)` (line 282). Either way, if
`EmitAgentResponseEvents` is set, the FINAL aggregated `AgentResponse` is also yielded (line 292).
`IWorkflowContext.YieldOutputAsync`'s outputs become typed workflow events in
`InProcessRunnerContext.YieldOutputAsync` (`InProc/InProcessRunnerContext.cs:253-299`):
`AgentResponseUpdate` → `AgentResponseUpdateEvent`, `AgentResponse` → `AgentResponseEvent` (lines
268-269, 294-295), both subclasses of `WorkflowOutputEvent` — so agent output surfaces through the
exact same `run.WatchStreamAsync()`/`NewEvents` pipeline as any other workflow output, no parallel
channel.

## 5. Working sample

`dotnet/samples/03-workflows/_StartHere/02_AgentsInWorkflows/Program.cs` — three `ChatClientAgent`s
chained `AddEdge(frenchAgent, spanishAgent).AddEdge(spanishAgent, englishAgent)` (lines 41-44), run via
`InProcessExecution.RunStreamingAsync`, driven by
`run.TrySendMessageAsync(new TurnToken(emitEvents: true))` (line 52), consuming
`AgentResponseUpdateEvent` from `run.WatchStreamAsync()` (lines 53-58). The plain (non-agent)
executor/edge baseline these samples build on is `01-get-started/05_first_workflow/Program.cs` — the
same file AgentEngine's own `examples/04_first_workflow.cpp` already mirrors.

## 6. Documented gotchas / design rationale, in MAF's own comments

- **Explicit non-shareable declaration**, because turn state (`_session`, `_currentTurnEmitEvents`,
  pending tool/approval requests) lives on the instance — comment at `AIAgentHostExecutor.cs:47`.
- `FilterForwardableMessages` strips reasoning tokens / provider-specific raw items before forwarding
  to the next agent because "these item types are not valid as input items" for some providers'
  Responses API — comments at lines 193-196, 319-324, 341-344.
- `InProcessRunnerContext.YieldOutputAsync` deliberately bypasses the executor's declared-output-type
  check for agent-shaped payloads, with an explicit rationale comment that the host executor "relays
  the agent's output without declaring `AgentResponse(Update)` in its Yields set" (lines 279-283),
  plus a legacy-compat branch gated by `Futures.EnableAgentResponseOutputTaggingAndFiltering` (lines
  260-274).
- **Unterminated tool calls / approval requests are intercepted, not silently blocked.**
  `AIAgentUnservicedRequestsCollector` tracks pending `ToolApprovalRequestContent`/
  `FunctionCallContent` (`AIAgentUnservicedRequestsCollector.cs:37-77`), and the executor withholds
  its outgoing `TurnToken` while `HasOutstandingRequests` is true (`AIAgentHostExecutor.cs:175-176,
  205`), resuming via `HandleUserInputResponseAsync`/`HandleFunctionResultAsync` (lines 82-135).

## What this means for AgentEngine (summary only — full mapping in the companion gap doc)

MAF's shape maps onto AgentEngine's own vocabulary reasonably directly: `AIAgentHostExecutor` ~
`rt::ExecutorBody` wrapping an `AgentSession`; `AgentSession` (MAF's, the per-run conversation state)
~ AgentEngine's own `AgentSession` instance, minted fresh per workflow run exactly the way MAF's
`ExecutorBinding.CreateInstanceAsync(sessionId)` does; `IWorkflowContext` ~ AgentEngine's own
`EffectContext`/routing return value; `TurnToken`/pending-request gating ~ AgentEngine's own
`Interaction`/suspend-for-approval mechanism (ADR-029), already built at the `AgentSession` layer.
The one genuine open question MAF's design does NOT answer for AgentEngine: MAF has no in-process
capability/authority system analogous to AgentEngine's `CapabilitySet`/`EffectContext::capabilities`
— `IWorkflowContext` carries no such thing, so MAF's own design gives zero precedent for "where does
an agent-executor's `CapabilitySet` come from" (workflow-level config? per-executor binding? derived
from the graph's own capability ceiling?). This is AgentEngine-specific work the MAF port cannot
just copy — named explicitly in the companion gap doc rather than glossed over.
