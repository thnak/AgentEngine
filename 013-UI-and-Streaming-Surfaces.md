# 013 — UI and Streaming Surfaces

**Status:** Draft · **Depends on:** 001, 003, 012 · **Gate:** §6

## Goal

One internal run event stream, projected onto every external surface — AG-UI for frontends, A2A
streaming for peers, MCP progress for tool callers, OpenAI-compatible SSE for drop-in clients — so
that adding a surface is writing a projection, not a second event model.

## 1. The internal run event stream

Every run emits an ordered, typed event sequence. It is the single source for streaming, telemetry
(016), recording (001 §7), and checkpoint boundaries (019).

```
RunStarted · RunFinished · RunFailed · RunCanceled
TurnStarted · TurnFinished
ModelCallStarted · ModelDelta · ModelCallFinished       (text, reasoning, tool-call deltas)
ToolCallStarted · ToolCallDelta · ToolCallFinished
SandboxExecStarted · SandboxExecFinished
StateChanged · ArtifactProduced
InputRequired · InputResolved
ApprovalRequested · ApprovalResolved
Warning · PolicyDecision
```

**Properties:**

- **Ordered and monotonic** per run, with a sequence number; a consumer that reconnects resumes by
  sequence where the transport allows and re-syncs by snapshot where it does not.
- **Backpressure-aware**: emission rides Quark's credit-controlled streams (Quark 024/ADR-018) — a
  slow UI stalls the producer rather than growing an unbounded buffer.
- **Sensitive-content aware**: events carry the same taint and capture policy as telemetry (016);
  a surface configured for metadata-only never sees content it should not.
- **Replayable**: an event stream replayed from a recording drives a UI identically.

## 2. AG-UI projection

AG-UI is the frontend-facing standard (CopilotKit, MIT; AWS Bedrock AgentCore added support in
March 2026). Its categories map onto §1 with no structural gaps:

| AG-UI | Source |
|---|---|
| `RunStarted/Finished/Error`, `StepStarted/Finished` | `RunStarted/Finished/Failed`, `TurnStarted/Finished` |
| `TextMessageStart/Content/End/Chunk` | `ModelDelta` (text) |
| `ToolCallStart/Args/End/Result/Chunk` | `ToolCall*` |
| `StateSnapshot/StateDelta/MessagesSnapshot` | `StateChanged` + session view |
| `ActivitySnapshot/Delta` | `SandboxExec*`, long-running tool progress |
| `Reasoning*` (incl. `ReasoningEncryptedValue`) | `ModelDelta` (reasoning); encrypted reasoning passes through opaque (003 §1) |
| `Raw`, `Custom` | escape hatch, namespaced |

**Rule:** the projection is total for what we emit, and lossless — anything AG-UI cannot express is
carried in `Custom` with a namespaced type id, never silently dropped.

## 3. Other surfaces

| Surface | Projection |
|---|---|
| **A2A streaming** (012) | Task status + artifact updates from the same stream |
| **MCP progress** (011) | `notifications/progress` on the originating request's response stream, when serving a tool call |
| **OpenAI-compatible SSE** | Chat-completion-shaped chunks, for drop-in clients that already speak it |
| **Terminal / CLI** | Direct consumption, no projection |
| **Recording** | Verbatim, for replay |

**A2UI** (agent-generated declarative UI) is explicitly deferred; when adopted it becomes another
projection, which is the entire reason for this design.

## 4. Transport

- **Server-Sent Events** over HTTP for the default web surface (widest client support, proxy
  friendly, one-way is enough).
- **WebSocket** where bidirectional interaction (mid-run input, approvals) justifies it.
- **In-process** for embedded hosts — no serialization on the local path.

**Resumability is a per-transport property and must be stated, not assumed.** Notably, MCP's
2026-07-28 revision *removed* SSE resumability and message redelivery: a broken stream loses the
in-flight request and the client re-issues with a new request id. Our own surfaces state their own
guarantee explicitly, and where a surface is not resumable, the client contract says so.

## 5. Human-in-the-loop

`InputRequired` and `ApprovalRequested` are stream events *and* run states (001 §2), so a UI can
render them inline while a headless caller polls or receives them over A2A/MCP. One mechanism,
four renderings.

Approval payloads carry the **exact validated arguments** and the hash the approval is bound to
(006 §4), so a UI cannot display one thing while another executes.

## 6. Promotion gate

- **G1** — a scripted run drives an AG-UI reference client end to end, with no dropped or reordered
  events, including reasoning and tool-call streaming.
- **G2** — a slow consumer applies backpressure to the provider read; the host's memory stays flat
  under a 10× producer/consumer speed mismatch (measured, not asserted).
- **G3** — the same run projects to AG-UI, A2A streaming, and OpenAI-compatible SSE with equivalent
  content, proven by a cross-surface comparison test.
- **G4** — a recorded stream replays into a UI identically to the live run.

## 7. Open questions

- **Q1** — Whether `StateDelta` should use JSON Patch (AG-UI's convention) internally, or convert at
  the projection.
- **Q2** — Multi-consumer streams: two UIs attached to one run need either fan-out with independent
  credit or a shared cursor. Quark's `Topic<M>` is best-effort at-most-once, which is *wrong* for a
  UI that must not miss an event — this needs a decision.
- **Q3** — How much history a late-attaching consumer receives (snapshot + tail, or full replay).
