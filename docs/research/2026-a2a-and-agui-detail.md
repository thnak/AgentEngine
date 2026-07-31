# Research record — A2A v1.0 and AG-UI normative detail

**Compiled:** 2026-07-31 · **Status:** dated snapshot · **Feeds:** RFC 012, RFC 013

Two protocols at very different maturity levels. Recording that difference is the most important
thing in this document: A2A has a normative spec, a proto source of truth, RFC-2119 clauses, LF
governance, and an official conformance kit. **AG-UI has none of those.**

---

# Part A — A2A v1.0

## A.1 Status and normative source

- **Version `1.0.0`**; wire negotiation uses `Major.Minor` only (`"1.0"`) and patch numbers **MUST
  NOT** appear in requests, responses, or Agent Cards.
- Linux Foundation project (proto package `lf.a2a.v1`), Apache-2.0, TSC with AWS/Cisco/Google/IBM/
  Microsoft/Salesforce/SAP/ServiceNow.
- **`specification/a2a.proto` is the single authoritative normative definition.** A generated
  `a2a.json` **MAY** be published but is explicitly a *non-normative build artifact*; SDKs and
  schemas **MUST** be regenerated from the proto rather than hand-edited.
- `A2A-Version` service parameter **MUST** be sent per request; if omitted, agents **MUST** interpret
  it as `0.3`. Unsupported → `VersionNotSupportedError`. Agents **CAN** expose several versions
  concurrently.
- Deprecation: renamed fields keep the old name until at least the next major release; migration
  guidance **MUST** be published for breaking changes.

**Two v1.0 breaking changes from 0.3.x that matter to a mapping layer:**

1. **The `kind` discriminator was removed from `Part`** and from streaming-event JSON. The JSON
   member name is now the discriminator.
2. `AgentCard.supportsExtendedAgentCard` moved into `AgentCapabilities.extendedAgentCard`.

## A.2 Bindings — a server need not implement all three

Three standard bindings — **JSON-RPC 2.0**, **gRPC**, **HTTP+JSON/REST** — plus explicitly permitted
custom bindings.

> Agents **MUST** declare all supported protocols in their AgentCard. Clients **MAY** choose any
> protocol declared by the agent.

But where more than one *is* offered they **MUST** provide identical functionality, consistent
behaviour, the same error handling, and equivalent authentication.

| Operation | JSON-RPC / gRPC method | REST |
|---|---|---|
| Send message | `SendMessage` | `POST /message:send` |
| Send streaming message | `SendStreamingMessage` | `POST /message:stream` |
| Get task | `GetTask` | `GET /tasks/{id}` |
| List tasks | `ListTasks` | `GET /tasks` |
| Cancel task | `CancelTask` | `POST /tasks/{id}:cancel` |
| Subscribe to task | `SubscribeToTask` | `POST /tasks/{id}:subscribe` (SSE) |
| Push config CRUD | `Create/Get/List/DeleteTaskPushNotificationConfig` | `POST/GET/GET/DELETE /tasks/{id}/pushNotificationConfigs[/{configId}]` |
| Extended card | `GetExtendedAgentCard` | `GET /extendedAgentCard` |

JSON serialization **MUST** use camelCase; enums serialize as full SCREAMING_SNAKE names
(`TASK_STATE_INPUT_REQUIRED`, `ROLE_USER`). REST **SHOULD** use `Content-Type: application/a2a+json`.
Service parameters (`A2A-Version`, `A2A-Extensions`) travel as HTTP headers or gRPC metadata; all
spec-defined names are prefixed `a2a-`.

## A.3 Agent Card

`GET /.well-known/agent-card.json` — an **IANA-registered well-known URI** (Permanent). Servers
**MUST** publish one.

Required fields: `name`, `description`, `supportedInterfaces` (ordered; first = preferred),
`version`, `capabilities`, `defaultInputModes`, `defaultOutputModes`, `skills`. Optional:
`provider`, `documentationUrl`, `securitySchemes`, `securityRequirements`, `signatures`, `iconUrl`.

- **`AgentInterface`** = `{url, protocolBinding ("JSONRPC" | "GRPC" | "HTTP+JSON" | a URI),
  protocolVersion, tenant?}`. If `tenant` is set, clients **MUST** echo it in every request.
- **`AgentSkill`** = `{id, name, description, tags (all required), examples?, inputModes?,
  outputModes?, securityRequirements?}`. Note what is absent: no instructions body, no bundled files.
- **`AgentCapabilities`** = `{streaming?, pushNotifications?, extensions[], extendedAgentCard?}`.
- **`SecurityScheme`** is a `oneof` over apiKey / httpAuth / oauth2 / openIdConnect / **mtls**,
  modelled on OpenAPI 3.2. OAuth flows: `authorizationCode` (with `pkceRequired`),
  `clientCredentials`, `deviceCode`; `implicit` and `password` are **deprecated**.
- **Signing**: JWS (RFC 7515) over an RFC 8785 canonicalization, with the `signatures` field itself
  excluded from the signed payload. `AgentCardSignature = {protected, signature, header?}`. Clients
  **SHOULD** verify at least one signature before trusting a card.
- **Caching**: endpoints **SHOULD** send `Cache-Control` and `ETag`; clients **SHOULD** honour RFC
  9111 and use conditional requests.
- **Extended card**: gated on `capabilities.extendedAgentCard`; **MUST** require authentication using
  a scheme from the *public* card.

## A.4 Objects

- **`Task`** = `{id, contextId?, status, artifacts[]?, history[]?, metadata?}`.
- **`TaskStatus`** = `{state, message?, timestamp?}`.
- **`Message`** = `{messageId, contextId?, taskId?, role, parts[] (≥1), metadata?, extensions[]?,
  referenceTaskIds[]?}`. `Role` ∈ `ROLE_UNSPECIFIED | ROLE_USER | ROLE_AGENT`.
- **`Part`** — a `oneof` of exactly one of **`text`** (string), **`raw`** (bytes, base64 in JSON),
  **`url`** (string), **`data`** (arbitrary JSON) — **no `kind` field in v1.0** — plus common
  `metadata?`, `filename?`, `mediaType?`.
- **`Artifact`** = `{artifactId, name?, description?, parts[] (≥1), metadata?, extensions[]?}`.
- **`TaskStatusUpdateEvent`** = `{taskId, contextId, status, metadata?}`.
- **`TaskArtifactUpdateEvent`** = `{taskId, contextId, artifact, append?, lastChunk?, metadata?}` —
  `append` is what makes chunked artifact streaming possible.
- **`StreamResponse`** = `oneof {task | message | statusUpdate | artifactUpdate}`.
- **`TaskPushNotificationConfig`** = `{tenant?, id, taskId, url, token?, authentication?}`;
  **`AuthenticationInfo`** = `{scheme, credentials?}`.

## A.5 Lifecycle

`TASK_STATE_` × `UNSPECIFIED | SUBMITTED | WORKING | COMPLETED | FAILED | CANCELED |
INPUT_REQUIRED | REJECTED | AUTH_REQUIRED`. Terminal: COMPLETED, FAILED, CANCELED, REJECTED.
Interrupted (non-terminal): INPUT_REQUIRED, AUTH_REQUIRED.

- A terminal task **MUST NOT** accept further messages (`UnsupportedOperationError`);
  `SubscribeToTask` on a terminal task likewise; streams **MUST** close on terminal state; tasks are
  immutable once terminal and refinement starts a **new task under the same `contextId`**.
- `SendMessageConfiguration.returnImmediately` (default `false`): blocking mode **MUST** wait for a
  terminal *or interrupted* state.
- **`AUTH_REQUIRED`** is A2A's mid-task authorization delegation. The agent **MUST** track it as a
  task, transition state, and include an explanatory `TaskStatus.message`. Chains are permitted — a
  client that is itself an agent **MAY** put its own task into `AUTH_REQUIRED`. And critically:

  > Agents **MUST NOT** treat the `TASK_STATE_AUTH_REQUIRED` state transition, by itself, as
  > authorization for any particular operation.

## A.6 Streaming — ordering is a MUST, delivery is not

- `SendStreamingMessage` and `SubscribeToTask`, both gated on `capabilities.streaming`.
- A `Task`-shaped response **MUST** begin with the `Task` object; `SubscribeToTask`'s first event
  **MUST** be the current `Task` snapshot (closing the `GetTask`-then-subscribe race).
- SSE for JSON-RPC and REST; native server streaming for gRPC. **Webhook deliveries are always
  HTTP+JSON regardless of binding.**

> All implementations **MUST** deliver events in the order they were generated. Events **MUST NOT**
> be reordered during transmission, regardless of protocol binding.

Multiple concurrent streams for one task are permitted; events **MUST** be broadcast identically and
in the same order to every stream, and closing one **MUST NOT** affect others.

**There is no `Last-Event-ID`-style cursor resume.** Reconnection means a fresh `SubscribeToTask`,
which re-delivers the current `Task` snapshot. And the spec is blunt about what that costs:

> Clients using streaming to retrieve task updates **MAY** not receive all status update messages if
> the client is disconnected and then reconnects. Messages **MUST NOT** be considered a reliable
> delivery mechanism for critical information.

So `Task`/`Artifact` state — fetchable via `GetTask` — is the durable source of truth; `Message`
content attached to status events is best-effort.

## A.7 Push notifications

Webhook `POST` carrying a **`StreamResponse`**. Agent **MUST** include credentials per
`AuthenticationInfo`; **MUST** attempt delivery at least once; **MAY** retry with backoff;
**SHOULD** use a 10–30 s timeout; **SHOULD** validate webhook URLs against SSRF (reject private,
loopback, link-local). Receiver **MUST** return 2xx, **SHOULD** process idempotently (duplicates
occur), **SHOULD** verify the task id, **MUST** validate authenticity. Config **MUST** persist until
task completion or explicit deletion; delete **MUST** be idempotent.

## A.8 Security

A2A deliberately invents no auth scheme — *"identity information is handled at the protocol layer,
not within A2A semantics."* Production **MUST** use HTTPS/TLS. Servers **MUST** authenticate every
request. Two clauses worth adopting verbatim:

> Authorization checks **MUST** occur before any data access that could leak resource existence.
> Servers **MUST NOT** reveal existence of resources outside the caller's authorized scope, and
> **SHOULD NOT** distinguish "not found" from "not authorized" in error responses.

## A.9 Extensions

`AgentCapabilities.extensions[] = AgentExtension{uri, description, required, params?}`; clients opt
in per request via the `A2A-Extensions` service parameter, and per-object via `extensions[]` with
payload in `metadata[<extension-uri>]`. Version lives in the URI and **a new URI MUST be created for
breaking changes**. A `required: true` extension the client does not support → the agent **MUST**
return `ExtensionSupportRequiredError`, and **MUST NOT** silently fall back. **SDKs must disable
extensions by default**, and extension support is **not required for base conformance**.

Governance: `experimental-ext-{name}` → TSC vote → `ext-{name}`, namespace
`https://a2a-protocol.org/extensions/{name}/v{n}`. Anyone may publish outside that framework.

## A.10 Ecosystem and conformance

Official SDKs: Python, Go, Java, JS/TS, C#/.NET, Rust. **No official C or C++ SDK.** A community
C++20 SDK exists — `MisterVVP/a2a-cpp` — targeting v1.0.0 with client/server, all three transports,
and a TCK conformance workflow; unaffiliated with official governance.

**There is an official TCK: `a2aproject/a2a-tck`.**

```bash
./run_tck.py --sut-host http://localhost:9999
./run_tck.py --sut-host http://localhost:9999 --transport grpc|jsonrpc|http_json
./run_tck.py --sut-host http://localhost:9999 --level must|should|may
```

It fetches the Agent Card, derives which transports to test from `supportedInterfaces`, tags tests
by **RFC 2119 level** (MUST failures are hard; SHOULD failures are `xfail`; MAY skipped unless the
capability is declared), and writes `compatibility.json`, `compatibility.html`, `tck_report.html`,
and `junitreport.xml`. A companion `a2a-inspector` provides interactive debugging.

---

# Part B — AG-UI

## B.1 Maturity — the load-bearing finding

**AG-UI has no formal specification document.** Its normative content is the TypeScript SDK's Zod
schemas (`@ag-ui/core` `events.ts`/`types.ts`), a mirrored protobuf definition, and prose docs.
There is no clause-numbered RFC-2119 text to conform *to*.

- **Pre-1.0**: `@ag-ui/core` at `0.0.57`, Python `ag-ui-protocol` at `0.1.19` (checked 2026-07-31).
  The `THINKING_*` events are marked *"Will be removed in 1.0.0"* — even the deprecation cleanup
  targets a 1.0 that has not shipped.
- **MIT licence** (A2A is Apache-2.0).
- **Informal, vendor-led governance**: born from CopilotKit's partnership with LangGraph and CrewAI;
  no `GOVERNANCE.md`, no `MAINTAINERS.md`, no foundation, no TSC. Closest thing is a public
  bi-weekly working-group call.
- **No conformance suite, TCK, or validator** exists that can be pointed at an independent server.

This is a materially different risk profile from MCP and A2A, and RFC 013 must say so.

## B.2 Event types (exact)

Every event inherits `BaseEvent`: `type`, `timestamp?`, `rawEvent?`.

**Lifecycle** — `RUN_STARTED` `{threadId, runId, parentRunId?, input?}` · `RUN_FINISHED`
`{threadId, runId, result?, outcome?}` where `outcome` is `{type:"success"}` or
`{type:"interrupt", interrupts: Interrupt[]}` (**an omitted `outcome` means a legacy producer and is
treated as normal completion**) · `RUN_ERROR` `{message, code?}` · `STEP_STARTED` `{stepName}` ·
`STEP_FINISHED` `{stepName}`.

A run **begins** with `RUN_STARTED` and **ends** with exactly one of `RUN_FINISHED` / `RUN_ERROR`.

**Text** — `TEXT_MESSAGE_START` `{messageId, role, name?}` · `TEXT_MESSAGE_CONTENT`
`{messageId, delta}` · `TEXT_MESSAGE_END` `{messageId}` · `TEXT_MESSAGE_CHUNK` (auto-expanding
convenience form).

**Tool call** — `TOOL_CALL_START` `{toolCallId, toolCallName, parentMessageId?}` · `TOOL_CALL_ARGS`
`{toolCallId, delta}` · `TOOL_CALL_END` `{toolCallId}` · `TOOL_CALL_RESULT`
`{messageId, toolCallId, content, role?}` · `TOOL_CALL_CHUNK`.

**State** — `STATE_SNAPSHOT` `{snapshot}` · `STATE_DELTA` `{delta}` (**RFC 6902 JSON Patch**) ·
`MESSAGES_SNAPSHOT` `{messages}`. Snapshots are all-or-nothing per role: a snapshot containing any
message of a role is authoritative for that role, and omitted entries are deleted client-side.

**Activity** — `ACTIVITY_SNAPSHOT` `{messageId, activityType, content, replace?}` · `ACTIVITY_DELTA`
`{messageId, activityType, patch}` (JSON Patch).

**Reasoning** — `REASONING_START` · `REASONING_MESSAGE_START` · `REASONING_MESSAGE_CONTENT` ·
`REASONING_MESSAGE_END` · `REASONING_MESSAGE_CHUNK` · `REASONING_END` ·
`REASONING_ENCRYPTED_VALUE` `{subtype: "tool-call"|"message", entityId, encryptedValue}` — an opaque
blob the client stores and forwards without decrypting.

**Special** — `RAW` `{event, source?}` · `CUSTOM` `{name, value}`.

**Deprecated, removal at 1.0.0** — `THINKING_START`, `THINKING_END`,
`THINKING_TEXT_MESSAGE_{START,CONTENT,END}` → the `REASONING_*` equivalents.

**Draft** — `MetaEvent` `{metaType, payload}`.

## B.3 Transport

Transport-agnostic by design: the core abstraction is `run(input: RunAgentInput) →
Observable<BaseEvent>`. The reference `HttpAgent` POSTs a `RunAgentInput` and consumes a stream, in
one of two encodings:

1. **SSE** (default): `text/event-stream`, one `data: <JSON BaseEvent>` frame per event.
2. **Binary protobuf**: negotiated by `Accept: application/vnd.ag-ui.event+proto`, framed as a
   **4-byte big-endian length prefix followed by the protobuf-encoded event**. Servers that cannot
   produce it fall back to SSE.

Ordering is stated as design guidance (*"events should be processed in the order received"*), **not
as a MUST** — unlike A2A. Resumability is a *declared capability*
(`AgentCapabilities.transport.resumable`, "resuming interrupted streams via sequence numbers") whose
wire protocol **is not specified**. The specified resync path is a fresh snapshot.

## B.4 Interrupts — a third shape for human-in-the-loop

AG-UI does **not** pause a run. A run that needs input **ends**:

`RUN_FINISHED { outcome: { type: "interrupt", interrupts: [...] } }`, where
`Interrupt = {id, reason, message?, toolCallId?, responseSchema?, expiresAt?, metadata?}` and
`reason` ∈ `tool_call` | `input_required` | `confirmation`, plus a `<framework>:<name>` extension
namespace (`core:` reserved).

Resumption is a **new run** whose `RunAgentInput.resume[]` carries
`{interruptId, status: "resolved"|"cancelled", payload?}`. Rules: same `threadId`; every open
interrupt **must** be covered (no partial resumes); a new input on a thread with pending interrupts
that omits `resume` **must** produce `RunError`; resumes must be idempotent; stale resumes (past
`expiresAt`) **must** be rejected with `RunError`. For tool-bound interrupts the agent does **not**
re-emit `TOOL_CALL_START/ARGS/END` — it emits `TOOL_CALL_RESULT` against the original `toolCallId`.
"Approve with edits" is conventionally `{approved: boolean, editedArgs?: object}` in
`responseSchema`, with `editedArgs` a **full replacement, not a merge**.

Before the interrupt-bearing `RUN_FINISHED`, the agent must emit whatever `STATE_SNAPSHOT` /
`MESSAGES_SNAPSHOT` a resume would need.

`RUN_ERROR` is the **sole** error event.

## B.5 Agent contract

`RunAgentInput` = `{threadId, runId, parentRunId?, state, messages, tools, context,
forwardedProps, resume?}`. `tools` carries **client-provided tools only**. `Message` is a union over
roles `developer | system | assistant | user | tool | activity | reasoning`, with multimodal user
content (`text`/`image`/`audio`/`video`/`document`) sourced as `{type:"data"|"url", value, mimeType}`.

Capability discovery is an optional `getCapabilities()` **method**, not a well-known URL, returning
categories `identity`, `transport`, `tools`, `output`, `state`, `multiAgent`, `reasoning`,
`multimodal`, `execution`, `humanInTheLoop`, `custom`. Stated principle: **discovery only, no
negotiation**; an absent field means "not declared", not "false". No caching or signing story.

## B.6 Ecosystem

Core SDKs: TypeScript and Python. Community: Go, Java, Kotlin, Dart, Rust supported; .NET and others
in progress. **A community C++ SDK lives inside the official monorepo** at `sdks/community/c++` —
CMake, with `agent/http_agent`, `core/event` + `event_verifier`, `apply/apply` (JSON Patch),
`stream/sse_parser`, and a mock server for tests. Directly useful as a reference for a C++23
implementation.

Adopters verified: **AWS Bedrock AgentCore** (with its own AG-UI runtime contract page) and
**Microsoft Agent Framework**, both listed 1st-party; plus Google ADK, Mastra, Pydantic AI, Agno,
LlamaIndex, AG2, LangGraph, CrewAI.

Validation tooling: `verifyEvents`, a client-side RxJS middleware enforcing stream invariants, and
the AG-UI Dojo reference app with Playwright e2e tests. Neither is a portable black-box suite.

## B.7 Positioning

AG-UI frames a three-layer stack — AG-UI for agent↔user, MCP for agent↔tools, A2A for agent↔agent —
explicitly complementary, with "handshakes" letting AG-UI front for MCP- and A2A-backed agents. The
docs also explicitly disambiguate AG-UI from the similarly-named **A2UI**, a generative-UI content
spec unrelated to Agent2Agent.

---

## Cross-cutting finding: human-in-the-loop has three incompatible shapes

| Protocol | Shape |
|---|---|
| **MCP** `2026-07-28` | Server returns `InputRequiredResult`; client **retries the original request** with `inputResponses` and **a different JSON-RPC id** |
| **A2A** v1.0 | Task **stays alive** in `INPUT_REQUIRED`/`AUTH_REQUIRED`; client sends a new message on the **same `taskId`** |
| **AG-UI** | Run **ends** with an interrupt outcome; client starts a **new run** carrying `resume[]` |

One conceptual event — "the agent needs something from a human" — with a retry, a continuation, and a
restart. Any engine that speaks all three needs one internal representation that projects to all
three without losing the correlation identity each requires (`requestState`, `taskId`, `interruptId`).
This sharpens OQ-4.

---

**Sources.** A2A: `a2a-protocol.org/latest/specification/`; `github.com/a2aproject/A2A`
(`docs/specification.md`, `specification/a2a.proto`, `docs/announcing-1.0.md`,
`docs/whats-new-v1.md`, `docs/topics/*`, `docs/sdk/index.md`); `a2aproject/a2a-tck`;
`a2aproject/a2a-inspector`. AG-UI: `docs.ag-ui.com`; `github.com/ag-ui-protocol/ag-ui`
(`sdks/typescript/packages/core/src/{events,types,capabilities}.ts`, `packages/proto/src/proto/*`,
`packages/encoder/src/*`, `packages/client/src/verify/verify.ts`, `sdks/python/ag_ui/core/*`,
`docs/concepts/*`, `LICENSE`, `sdks/community/c++/`).
