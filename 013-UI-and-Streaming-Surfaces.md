# 013 — UI and Streaming Surfaces

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 003, 006, 012, 019 · **Gate:** §6

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
AuthRequired · AuthResolved
ApprovalRequested · ApprovalResolved
Warning · PolicyDecision
```

**`AuthRequired`/`AuthResolved` are the run-state pair 001 §2 defines alongside `InputRequired`** —
entering either state mints an `Interaction{interaction_id, run_id, reason ∈ {input, auth}, ...}`
(001 §2), so the two event pairs carry the same shape and differ only in which state, and which
`reason`, produced them. They exist as their own pair rather than folding into `InputRequired` with
a payload flag because A2A already has a distinct wire state for exactly this case (`AUTH_REQUIRED`,
012 §1) — collapsing them internally would force every adapter to re-derive what A2A already tells
us for free. §2.2 below extends the `interaction_id` mapping to state how each protocol surfaces the
`auth` reason specifically.

**`ToolCallDelta`'s producer**, since it is the one event in this list a tool implementation emits
rather than the engine: a call to `EffectContext.report_progress` during `invoke()` (006 §6a) is the
only source. It is distinct from the tool-call argument streaming folded into `ModelDelta` above —
that is the *model* incrementally producing a call's arguments before invocation starts;
`ToolCallDelta` is the *tool* reporting on work already in flight.

**`StandingEffect` visibility rides `StateChanged`, not a new event pair.** Registering, resolving
(fired, completed, or expired), or cancelling a `schedule_wakeup`/`watch_resource`/`background_task`
handle (006 §6b) is a change to the run's visible state — the same category `StateChanged` already
covers — so a run's set of active `StandingEffect`s is exposed as part of that state rather than
inventing `StandingEffectRegistered`/`Resolved` as siblings to `InputRequired`/`ApprovalRequested`.
This closes the observability gap §6b otherwise leaves — today a UI can only see a standing effect's
eventual `ToolCallFinished` (for a background task) or the run's own suspend/resume (for a wakeup or
watch), with nothing showing what is currently outstanding — for free, through the projection every
surface already has for `StateChanged` (§2.1, §3), rather than a second wire mapping to design and
maintain per surface.

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

**Resolved (OQ-4): `interaction_id` (001 §2) is that identity, and each adapter carries it in the
shape its protocol demands, never in a shape the others share:**

- **AG-UI** — `interruptId` **is** the `interaction_id`, verbatim; AG-UI already mints one identifier
  per pause and already requires every open one to be covered by a single `resume[]`, so no
  encoding step is needed here, only a naming equivalence. The projection's pre-`RUN_FINISHED`
  snapshot obligation above is the general rule 001 §2 states for every adapter, applied to the one
  protocol whose spec makes it explicit.
- **MCP** — `interaction_id` is embedded in `requestState`, HMAC/AEAD-protected per 011 §8a's
  "`requestState` is attacker-controlled input" rule; a retry decodes and verifies it, looks up the
  `Interaction`, and resolves it with `inputResponses`. The *new* JSON-RPC request id MCP mints on
  every retry carries no correlation meaning for us — all of it rides in `requestState`, which is
  exactly the shape MCP's stateless design already expects a client to use (011 §3.4).
- **A2A** — needs no encoding at all. A task can hold at most one outstanding `INPUT_REQUIRED` at a
  time (its lifecycle is sequential), so `taskId` — which already **is** our `run_id` (001 §2) —
  already disambiguates it; a new message on that `taskId` resolves the run's one currently-open
  `Interaction` directly. This is not a gap OQ-4 needed to close, only a confirmation that A2A's
  existing 1:1 task/run mapping already carries what A2A needs.

None of this changes the internal state machine — a run in `InputRequired`/`Suspended` behaves
identically regardless of which surface it is being observed through. What differs is entirely
adapter-local: which field carries `interaction_id`, and whether that adapter additionally demands
every open `Interaction` resolve together (AG-UI) or lets each resolve independently (MCP, A2A).

**The `auth` reason (`AuthRequired`/`AuthResolved`, §1) rides the same `interaction_id` mapping, but
each protocol surfaces it differently, matching how much of "this is specifically an auth wait" each
wire shape already knows how to say:**

- **A2A** — needs no encoding at all, the same as its `input` case above: `TASK_STATE_AUTH_REQUIRED`
  is a distinct, native task state (012 §1, §4), so the `reason: auth` tag is redundant with — not
  additional to — what the wire already carries. `taskId` disambiguates exactly as it does for
  `INPUT_REQUIRED`.
- **AG-UI** — has no native auth reason; its `Interrupt.reason` enum is `tool_call` | `input_required`
  | `confirmation` with no `auth` member (§2.2 above). We carry it in the `<framework>:<name>`
  extension namespace the enum already reserves for this — e.g. `ae:auth_required` — rather than
  overloading `input_required`, which would erase the distinction A2A and MCP both preserve.
- **MCP** — has no native auth-specific MRTR shape either; an `AuthRequired` interaction is carried
  through the same `requestState`-embedded, HMAC/AEAD-protected `interaction_id` a §2.2 `input`
  interaction uses (011 §3.4, §8a), with `reason: auth` living inside the looked-up `Interaction`
  record rather than in any MCP wire field — MCP's MRTR shape does not distinguish the two at retry
  time, so we don't invent a distinction it has no slot for.

**This is invariant-touching (I2/I3), not a wording fix, and closing the textual contradiction here
is not the same as this being Reviewed.** `AuthRequired` is a credential-granting path, and per
CLAUDE.md's "contested, hot-path, or security-critical designs go through design→red-team→prove→
judge" rule and the sign-off workflow's §3 (`docs/planning/v1-review-signoff-workflow.md`), this
addition and its per-protocol mapping still owe that full track and an ADR before being treated as
settled — this edit only removes the gap 012 §8 G3 had nothing to implement against.

## 3. Other surfaces

| Surface | Projection |
|---|---|
| **A2A streaming** (012) | Task status + artifact updates from the same stream |
| **MCP progress** (011) | `notifications/progress` on the originating request's response stream, when serving a tool call — sourced from `ToolCallDelta` (006 §6a) exactly like the AG-UI projection above |
| **OpenAI-compatible SSE** | Chat-completion-shaped chunks, for drop-in clients that already speak it — best-effort compatibility, not gated (§6 G3) |
| **Terminal / CLI** | Direct consumption, no projection |
| **Recording** | Verbatim, for replay |

**A2UI** (agent-generated declarative UI) is explicitly deferred; when adopted it becomes another
projection, which is the entire reason for this design — **resolves OQ-9's A2UI part**
(OpenQuestions.md, 2026-08-04): the projection architecture already makes adoption additive by
construction, so nothing here needs to preemptively account for it, and no further design work is
owed ahead of A2UI actually stabilizing.

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
- **G3** — the same run projects to AG-UI and A2A streaming with equivalent content, proven by a
  cross-surface comparison test. **OpenAI-compatible SSE is not part of this gate.** Unlike AG-UI
  (§2.0's pinned `@ag-ui/core` schema version) and A2A/MCP (each a named protocol revision, 012 §0 /
  011 header), "OpenAI-compatible" names no version we pin and no field-mapping table we maintain —
  it is a widely-imitated de facto chunk shape, not a spec with a conformance-vs-compatibility
  distinction to draw. It is served best-effort for drop-in clients and stays ungated until it has
  the same rigor (a pinned version and an event/field mapping) the other three surfaces already have.
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
- ~~**Q2** — Multi-consumer streams: two UIs attached to one run need either fan-out with independent
  credit or a shared cursor. Quark's `Topic<M>` is best-effort at-most-once, which is *wrong* here —
  and A2A makes it a **MUST** that every concurrent subscriber to a task receives identical events in
  identical order, so best-effort fan-out is not merely undesirable, it is non-conformant.~~
  **Resolved (2026-08-04), fully — see below.** For the embedded-host case, 020 §3a licenses
  `Topic<M>` for *secondary, in-process-only* observers of a run (a debug pane alongside a primary
  view, where a dropped UI frame is not a correctness bug); A2A's own stricter MUST is closed by the
  locally-built ordered fan-out below, not by `Topic<M>`.

  **Upstream primitive requested and scoped**: filed as
  [QuarkCpp#10](https://github.com/thnak/QuarkCpp/issues/10), pre-registered as
  [Quark ADR-039](https://github.com/thnak/QuarkCpp/blob/master/decisions/ADR-039-ordered-reliable-multi-subscriber-fanout.md)
  (Draft — sketch only, no red-team/prove pass yet). Checking our own §2.3/§2.4 against Quark's
  proposed two-policy design (`EvictAfter<N>` vs `Block`) settled which one we actually need: A2A's
  ordering MUST applies only to *currently attached* subscribers, and §2.4 explicitly disclaims
  gap-free delivery on reconnect (`GetTask`, not the stream, is the source of truth). So
  `EvictAfter<N>` alone — bounded buffer, then evict with an explicit gap signal, treated by our
  client exactly like any other A2A disconnect/resubscribe — is sufficient; `Block` is not required
  for this need.

  ~~Still blocked on Quark actually proving and shipping the primitive.~~ **Resolved, don't wait
  on it (2026-08-04):** A2A conformance (012 §8 G1, zero MUST-level failures) can't be hostage to an
  external, unscheduled primitive landing in a dependency's own roadmap. What A2A's MUST actually
  needs is narrower than the generic problem Quark's `Topic<M>`/ADR-039 is solving for: ordered
  fan-out with bounded eviction for exactly **one** already-ordered internal stream (013 §1), not an
  arbitrary-topic generic pub/sub primitive. That's implementable as ordinary AgentEngine-owned code
  — a small per-subscriber cursor plus a bounded ring buffer, held by the run's supervising actor or
  a dedicated fan-out actor, applying `EvictAfter<N>` directly — with no dependency on Quark shipping
  anything new. The upstream ask (QuarkCpp#10) stays filed, since a general primitive would still
  benefit other Quark consumers and let this local implementation be replaced later, but it is no
  longer this RFC's gate blocker.
- ~~**Q3** — How much history a late-attaching consumer receives (snapshot + tail, or full replay).~~
  **Resolved, snapshot + tail, confirming what the two most detailed protocol mappings already do
  (2026-08-04):** this wasn't actually undecided so much as unstated as a cross-cutting rule — §2.1's
  "all-or-nothing per role" `MESSAGES_SNAPSHOT` and 012 §2.3's "`SubscribeToTask`'s first event MUST
  be the current `Task` snapshot" are both already snapshot-then-tail, not full replay. Generalized:
  a late-attaching consumer on any surface gets a current-state snapshot (as that surface's own
  projection already defines it) followed by live events, never a replay of every intermediate
  event — which would make live-attach cost scale with a run's entire history, a bad property for
  hour/day-scale suspended-and-resumed runs (019). Full historical replay stays available as a
  **separate, explicit** operation — 001 §7's offline recording/replay mechanism — never something a
  live attach implicitly triggers.
