# 013 — UI and Streaming Surfaces

**Status:** Draft · **Depends on:** 001, 003, 006, 012 · **Gate:** §6

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

**`ToolCallDelta`'s producer**, since it is the one event in this list a tool implementation emits
rather than the engine: a call to `EffectContext.report_progress` during `invoke()` (006 §6a) is the
only source. It is distinct from the tool-call argument streaming folded into `ModelDelta` above —
that is the *model* incrementally producing a call's arguments before invocation starts;
`ToolCallDelta` is the *tool* reporting on work already in flight.

**Properties:**

- **Ordered and monotonic** per run, with a sequence number; a consumer that reconnects resumes by
  sequence where the transport allows and re-syncs by snapshot where it does not.
- **Backpressure-aware**: emission rides Quark's credit-controlled streams (Quark 024/ADR-018) — a
  slow UI stalls the producer rather than growing an unbounded buffer.
- **Sensitive-content aware**: events carry the same taint and capture policy as telemetry (016);
  a surface configured for metadata-only never sees content it should not.
- **Replayable**: an event stream replayed from a recording drives a UI identically.

## 2. AG-UI projection

### 2.0 Maturity — stated plainly, because it changes what conformance can mean

**AG-UI has no formal specification document.** Its normative content is the TypeScript SDK's Zod
schemas plus a mirrored protobuf definition and prose docs — there is no clause-numbered RFC-2119
text to conform *to*. It is **pre-1.0** (`@ag-ui/core` `0.0.57`, Python `0.1.19` as of 2026-07-31;
the deprecated `THINKING_*` events are slated for removal "in 1.0.0", which has not shipped), **MIT**
licensed, and **informally governed** — vendor-led out of CopilotKit, with no foundation, TSC, or
maintainers file. **And there is no conformance suite, TCK, or validator** that can be pointed at an
independent implementation.

That is a materially different risk profile from MCP and A2A, and it changes how **I7** applies here:

- We **pin to a schema version** (the `@ag-ui/core` release whose Zod/protobuf schemas we generate
  against) rather than to a spec revision, and record it as a build constant.
- We **author our own conformance suite** from those schemas, seeded by the invariants that AG-UI's
  own client-side `verifyEvents` middleware enforces and by the feature matrix its reference app
  exercises. That suite is ours to maintain; §6 G5 names it.
- We **claim compatibility, not conformance**, until an upstream suite exists — the distinction
  matters and this RFC will not blur it.
- The community C++ SDK inside AG-UI's own monorepo (`sdks/community/c++`, with an `event_verifier`,
  an SSE parser, and a JSON-Patch applier) is the closest reference implementation for a C++ engine
  and is worth diffing against, while remaining community-maintained rather than official.

Adoption is nonetheless real and worth serving: AWS Bedrock AgentCore publishes an AG-UI runtime
contract, and Microsoft Agent Framework ships an integration.

### 2.1 Event mapping

Its categories map onto §1 with no structural gaps:

| AG-UI | Source |
|---|---|
| `RunStarted/Finished/Error`, `StepStarted/Finished` | `RunStarted/Finished/Failed`, `TurnStarted/Finished` |
| `TextMessageStart/Content/End/Chunk` | `ModelDelta` (text) |
| `ToolCallStart/Args/End/Result/Chunk` | `ToolCall*`, `CHUNK` from `ToolCallDelta` (006 §6a) |
| `StateSnapshot/StateDelta/MessagesSnapshot` | `StateChanged` + session view |
| `ActivitySnapshot/Delta` | `SandboxExec*`, long-running tool progress |
| `Reasoning*` (incl. `ReasoningEncryptedValue`) | `ModelDelta` (reasoning); encrypted reasoning passes through opaque (003 §1) |
| `Raw`, `Custom` | escape hatch, namespaced |

Exact identifiers, since they are the contract: `RUN_STARTED` · `RUN_FINISHED` · `RUN_ERROR` ·
`STEP_STARTED` · `STEP_FINISHED` · `TEXT_MESSAGE_{START,CONTENT,END,CHUNK}` ·
`TOOL_CALL_{START,ARGS,END,RESULT,CHUNK}` · `STATE_SNAPSHOT` · `STATE_DELTA` · `MESSAGES_SNAPSHOT` ·
`ACTIVITY_{SNAPSHOT,DELTA}` · `REASONING_{START,END}` · `REASONING_MESSAGE_{START,CONTENT,END,CHUNK}` ·
`REASONING_ENCRYPTED_VALUE` · `RAW` · `CUSTOM`. The `THINKING_*` family is deprecated for removal at
1.0.0 and **we do not emit it**; we accept it on ingest for compatibility.

**Rules:**

- The projection is total for what we emit, and lossless — anything AG-UI cannot express is carried
  in `CUSTOM` with a namespaced type id, never silently dropped.
- A run **begins** with `RUN_STARTED` and ends with **exactly one** of `RUN_FINISHED` / `RUN_ERROR`.
  `RUN_ERROR` is the *sole* error event; no other error shape exists.
- `REASONING_ENCRYPTED_VALUE` carries an opaque blob that we **store and forward without
  decrypting**, matching 003 §1's rule for encrypted reasoning.
- **`STATE_DELTA` and `ACTIVITY_DELTA` use RFC 6902 JSON Patch.** This settles Q1 below: JSON Patch
  is the wire format, so we generate patches at the projection rather than inventing an internal
  delta form and translating twice.
- Snapshots are **all-or-nothing per role**: a `MESSAGES_SNAPSHOT` containing any message of a role
  is authoritative for that role, and entries it omits are deleted client-side. A partial snapshot is
  therefore a data-loss bug, and is a test case.

### 2.2 Interrupts — AG-UI's human-in-the-loop is a *third* shape

AG-UI does not pause a run. A run needing human input **ends** with
`RUN_FINISHED { outcome: { type: "interrupt", interrupts: [...] } }`, where each `Interrupt` is
`{id, reason, message?, toolCallId?, responseSchema?, expiresAt?, metadata?}` and `reason` is
`tool_call` | `input_required` | `confirmation` (plus a `<framework>:<name>` extension namespace).
Resumption is a **new run** carrying `resume[] = {interruptId, status, payload?}`.

Rules we must honour when projecting `InputRequired` (001 §2) onto this: the same `threadId`; **every
open interrupt must be covered by a single resume** (no partial resumes); a new input on a thread
with pending interrupts that omits `resume` **must** produce `RUN_ERROR`; resumes are idempotent;
stale resumes past `expiresAt` are rejected. For a tool-bound interrupt the agent does **not** re-emit
`TOOL_CALL_START/ARGS/END` — it emits `TOOL_CALL_RESULT` against the original `toolCallId`.

**And a hard ordering obligation:** whatever `STATE_SNAPSHOT` / `MESSAGES_SNAPSHOT` a resume will
need must be emitted **before** the interrupt-bearing `RUN_FINISHED`. Emitting it after is
unrecoverable, because the run is over.

This is the third of three incompatible shapes for one idea (012 §5a) — MCP retries a request, A2A
continues a task, AG-UI restarts a run — and it is the strongest argument that our internal
`InputRequired` needs a correlation identity that survives all three. **Resolved (OQ-4):** that
identity is the `request_id`-shaped token defined in 001 §2; for AG-UI specifically, it maps
directly onto `interruptId` — and, per MAF's own precedent for this binding
(`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §2), the *same* token may simultaneously
back a `toolCallId` for a tool-bound interrupt without needing a second identity.

## 3. Other surfaces

| Surface | Projection |
|---|---|
| **A2A streaming** (012) | Task status + artifact updates from the same stream |
| **MCP progress** (011) | `notifications/progress` on the originating request's response stream, when serving a tool call — sourced from `ToolCallDelta` (006 §6a) exactly like the AG-UI projection above |
| **OpenAI-compatible SSE** | Chat-completion-shaped chunks, for drop-in clients that already speak it |
| **Terminal / CLI** | Direct consumption, no projection |
| **Recording** | Verbatim, for replay |

**A2UI** (agent-generated declarative UI) is explicitly deferred; when adopted it becomes another
projection, which is the entire reason for this design.

## 4. Transport

- **Server-Sent Events** over HTTP for the default web surface (widest client support, proxy
  friendly, one-way is enough). This is also AG-UI's default: one `data: <JSON event>` frame per
  event, against a `POST` carrying a `RunAgentInput`.
- **AG-UI binary framing** where throughput matters: negotiated by
  `Accept: application/vnd.ag-ui.event+proto`, framed as a **4-byte big-endian length prefix followed
  by the protobuf-encoded event**, with graceful fallback to SSE. Worth implementing — it is a
  natural fit for a C++ engine, and it avoids JSON encoding on the hot streaming path (028 §1).
- **WebSocket** where bidirectional interaction (mid-run input, approvals) justifies it.
- **In-process** for embedded hosts — no serialization on the local path.

**AG-UI capability discovery is `getCapabilities()`, a method — not a well-known URL** — and its
stated principle is **discovery only, no negotiation**: an absent field means "not declared", not
"false". There is no caching or signing story, unlike A2A's card. We therefore treat a peer's
declared capabilities as a hint, never as an authorization or integrity signal.

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
- **G5** — our own AG-UI compatibility suite (§2.0) passes against a pinned schema version, covering
  every event kind, the interrupt/resume lifecycle including the pre-`RUN_FINISHED` snapshot
  ordering, all-or-nothing snapshot semantics, and JSON-Patch delta application; and the same fixture
  stream is accepted by AG-UI's own client-side verifier without violations.
- **G6** — binary framing round-trips identically to SSE for the same run, and content negotiation
  falls back cleanly when the client does not accept protobuf.

## 7. Open questions

- ~~**Q1** — Whether `StateDelta` should use JSON Patch internally.~~ **Resolved:** AG-UI's
  `STATE_DELTA` and `ACTIVITY_DELTA` are RFC 6902 JSON Patch, so we generate patches at the
  projection rather than maintaining an internal delta form and translating twice.
- **Q2** — Multi-consumer streams: two UIs attached to one run need either fan-out with independent
  credit or a shared cursor. Quark's `Topic<M>` is best-effort at-most-once, which is *wrong* here —
  and A2A makes it a **MUST** that every concurrent subscriber to a task receives identical events in
  identical order, so best-effort fan-out is not merely undesirable, it is non-conformant. This needs
  a real primitive. **Partially addressed for the embedded-host case**: 020 §3a licenses `Topic<M>`
  for *secondary, in-process-only* observers of a run (a debug pane alongside a primary view, where a
  dropped UI frame is not a correctness bug), explicitly not as an answer for A2A's stricter
  requirement — that half of this question is still open.

  **Upstream primitive requested and scoped**: filed as
  [QuarkCpp#10](https://github.com/thnak/QuarkCpp/issues/10), pre-registered as
  [Quark ADR-039](https://github.com/thnak/QuarkCpp/blob/master/decisions/ADR-039-ordered-reliable-multi-subscriber-fanout.md)
  (Draft — sketch only, no red-team/prove pass yet). Checking our own §2.3/§2.4 against Quark's
  proposed two-policy design (`EvictAfter<N>` vs `Block`) settled which one we actually need: A2A's
  ordering MUST applies only to *currently attached* subscribers, and §2.4 explicitly disclaims
  gap-free delivery on reconnect (`GetTask`, not the stream, is the source of truth). So
  `EvictAfter<N>` alone — bounded buffer, then evict with an explicit gap signal, treated by our
  client exactly like any other A2A disconnect/resubscribe — is sufficient; `Block` is not required
  for this need. Still blocked on Quark actually proving and shipping the primitive.
- **Q3** — How much history a late-attaching consumer receives (snapshot + tail, or full replay).
