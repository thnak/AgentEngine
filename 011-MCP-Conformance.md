# 011 — MCP Conformance

**Status:** Draft · **Protocol revision:** MCP **`2026-07-28`** · **Depends on:** 003, 006, 007, 016, 018 · **Gate:** §10

> **Revision note.** `2026-07-28` is the largest revision since MCP launched and it changed the
> protocol's *shape*, not just its surface. A client written against `2025-11-25` is not a client
> for this revision with a few patches. This RFC is written against the new shape from the start.
> Wire details cited here come from the
> [official changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog); the
> field-by-field conformance tables in §3–§6 are being completed from the specification proper.

## Goal

Be a correct MCP **client** (consuming servers as tools/resources/prompts) and a correct MCP
**server** (exposing AgentEngine agents, tools, and skills), at a named revision, with an executable
conformance suite as the gate (**I7**).

## 1. Why this is written stateless-first

The revision removed the `initialize` / `notifications/initialized` handshake, the protocol-level
session, and the `Mcp-Session-Id` header. Every request is self-contained, carrying its protocol
version and client capabilities in `_meta`. That is not a detail — it means:

- **No connection state machine.** A client is a request builder plus a cache, which maps cleanly
  onto Quark's stateless-worker pools (Quark 025) rather than a per-server connection actor.
- **No session affinity**, so horizontal scaling and load balancing are free on both sides.
- **Reconnect is not a resumption problem.** SSE resumability and message redelivery were removed
  (`Last-Event-ID` and event ids are gone); a broken response stream loses the in-flight request and
  the client **MUST** re-issue it as a new request with a new request id.

**Design consequence:** every MCP call is idempotent-by-construction from our side or carries an
idempotency key (019 §3), because "re-issue as a new request" is the specified recovery path and a
non-idempotent tool re-issued blindly is a double effect.

## 2. Per-request `_meta`

Every outbound request carries:

| Key | Content |
|---|---|
| `io.modelcontextprotocol/protocolVersion` | The revision we speak |
| `io.modelcontextprotocol/clientCapabilities` | Our declared capabilities, including `extensions` |
| `io.modelcontextprotocol/clientInfo` | Client identity (SHOULD) |
| `io.modelcontextprotocol/logLevel` | Per-request log level; a server **MUST NOT** emit `notifications/message` for a request that omitted it |
| `traceparent` / `tracestate` / `baggage` | OpenTelemetry context propagation (SEP-414) — the cross-process trace link required by 016 §2 |

Servers **SHOULD** identify themselves in each result's `_meta`
(`io.modelcontextprotocol/serverInfo`); as a server we do.

**`server/discover`** is required of servers: it advertises supported protocol versions,
capabilities, and identity. As a client we **MAY** call it for up-front version selection and use it
as a backward-compatibility probe on STDIO; as a server we **MUST** implement it. Version mismatch
returns `UnsupportedProtocolVersionError`.

## 3. Consuming servers (client role)

### 3.1 Tools

- Discovered via `tools/list`, mapped into the tool plane (006 §2) as a distinct source.
- **Schemas are JSON Schema 2020-12** with any keywords permitted, `$ref` resolution requirements,
  and composition-keyword resource bounds. We validate arguments against them **strictly** (006 §3
  step 2 — reject, never coerce), and enforce our own bounds on `$ref` depth and schema size,
  because a schema is attacker-controlled input when the server is not first-party.
- `structuredContent` may be any JSON value; results map to content parts (003).
- **Caching**: `tools/list`, `prompts/list`, `resources/list`, `resources/read`, and
  `resources/templates/list` results carry `ttlMs` and `cacheScope` (`public`/`private`). We honour
  both — `private` results are never shared across principals — and combine them with `listChanged`
  notifications rather than polling.
- Servers **SHOULD** return tools in deterministic order for prompt-cache stability; we preserve
  server order and never re-sort, so their cache-friendliness is not undone by us.

### 3.2 Resources and prompts

- Resources and resource templates are read through the same capability gate as any other effect;
  a resource read is **tainted external content** (003 §2) and is delimited when it enters a prompt
  (017 §3).
- Prompts are imported as instruction fragments, subject to the same taint rules — a server-supplied
  prompt is not a trusted instruction.

### 3.3 Change notifications

The HTTP GET endpoint and `resources/subscribe`/`unsubscribe` were replaced by
**`subscriptions/listen`**: one long-lived POST-response stream to which a client opts in for
specific notification types (`toolsListChanged`, `promptsListChanged`, `resourcesListChanged`,
`resourceSubscriptions`), with the server acknowledging and tagging notifications with
`io.modelcontextprotocol/subscriptionId`.

Request-scoped notifications (`notifications/progress`, `notifications/message`) continue to flow on
the response stream of the request they belong to — **not** on the `subscriptions/listen` stream.
Our progress projection (013 §3) follows that split exactly.

### 3.4 Multi Round-Trip Requests (MRTR)

MRTR replaces server-initiated requests (`roots/list`, `sampling/createMessage`,
`elicitation/create`). A server returns an `InputRequiredResult` (`resultType: "input_required"`)
whose `inputRequests` carries what it needs; the client responds with `inputResponses` **on a retry
of the original request**. All results now carry a required `resultType`; results from
earlier-revision servers that omit it **MUST** be treated as `"complete"`.

**This is a first-class control-flow shape in our client, not a callback.** It maps onto the
unified `InputRequired` run state (001 §2) — the same mechanism as tool approval, workflow request
ports, and A2A `INPUT_REQUIRED`. A server needing to correlate an elicitation across retries encodes
its own identifier in `requestState`; the `notifications/elicitation/complete` notification and the
`elicitationId` field are gone.

**Security note:** an MRTR input request is a server asking our host for something. It is subject to
the same authority rules as everything else — a server cannot obtain data or authority through
`inputRequests` that the principal does not hold (007 I3).

### 3.5 Deprecated features we do not adopt

**Roots, Sampling, and Logging are deprecated** (12-month minimum window). New implementations
should not add support, and we do not:

| Deprecated | Our position |
|---|---|
| Roots | Pass directories/files as tool parameters, resource URIs, or server configuration — which is also the only form compatible with our mount-based capability model (007 §3) |
| Sampling | We integrate provider APIs directly (004). A server driving inference through our credentials is a confused-deputy shape we would not want even if it were current |
| Logging | Server logs to stderr (stdio) or OpenTelemetry (016) |
| HTTP+SSE transport | Streamable HTTP only |
| `includeContext: "thisServer"/"allServers"` | Omit or `"none"` |
| OAuth Dynamic Client Registration | Client ID Metadata Documents (§6) |

We must still **tolerate** peers that use deprecated features during their window; tolerating is not
adopting.

### 3.6 Extensions

`extensions` on client/server capabilities negotiates optional behaviour beyond the core. Tasks
moved out of core into the official **`io.modelcontextprotocol/tasks`** extension, redesigned around
polling (`tasks/get`) plus `tasks/update` for client-to-server input, with `tasks/list` removed and
servers permitted to return task handles unsolicited.

That extension is the natural mapping for our long-running/`Suspended` runs (019 §2) — and it is one
of the four competing shapes OQ-4 needs to unify.

## 4. Exposing AgentEngine (server role)

- **Tools** are generated from tool metadata (006 §1) — one schema source, so an MCP listing cannot
  drift from the tool's actual contract. Deterministic ordering; `ttlMs`/`cacheScope` set honestly.
- **Agents as tools**: an agent is exposed as a tool whose invocation starts a run. Long-running
  runs use the tasks extension; `InputRequired` surfaces as an MRTR `InputRequiredResult`.
- **Skills** (009 §8) are exposed through whichever mechanism the ecosystem standardizes; the
  mapping is pending the skills research (§9).
- **We implement `server/discover`**, and we do **not** implement deprecated features at all.
- **Authorization** per §6; every inbound request establishes a principal (018 §1) and executes with
  that principal's capability set, never the host's.

## 5. Error handling

The revision partitions the JSON-RPC server-error range: `-32000..-32019` implementation-defined
(existing SDK usage grandfathered), `-32020..-32099` reserved for the specification. Codes
introduced in this revision were renumbered accordingly — `HeaderMismatch` `-32020`,
`MissingRequiredClientCapability` `-32021`, `UnsupportedProtocolVersion` `-32022` — and
resource-not-found moved from `-32002` to `-32602` (Invalid Params). We map these onto our failure
classification (001 §6) and never flatten them into a string.

## 6. Authorization

As a client:

- Authorization servers **SHOULD** include `iss` per RFC 9207, and we **MUST** validate a present
  `iss` against the recorded issuer before redeeming an authorization code.
- Client credentials are **bound to the issuing authorization server**: keyed by issuer, never
  reused with a different AS, and re-registered when the AS changes.
- **Client ID Metadata Documents** are the preferred registration mechanism; DCR is deprecated and
  retained only for authorization servers that do not support CIMD, and when used **MUST** specify
  an appropriate `application_type` to avoid OIDC redirect-URI conflicts.
- **No token passthrough** (018 §1): a token received on an inbound request is never forwarded to an
  MCP server.

As a server: standard OAuth 2.1 resource-server behaviour with audience validation. A token minted
for another resource is rejected — this is the confused-deputy control.

## 7. Transport

**Streamable HTTP** is the transport. Requests **MUST** carry the standard MCP request headers
(`Mcp-Method`, `Mcp-Name`) on POST, with custom headers from tool parameters supported via
`x-mcp-header`. **stdio** remains for local servers, with `server/discover` usable as the
backward-compatibility probe.

**Local stdio servers are subprocesses**, and spawning one is an `Exec` capability (007 §3) with the
sandbox implications that follow: an MCP server on the local machine is third-party code (trust tier
T2) and is treated as such, not as an extension of the host.

## 8. Trust and supply chain

An MCP server is **T2 third-party code** (007 §6). Concretely:

- **Digest-pinned listings.** Tool names, descriptions, and schemas are hashed; a change requires
  re-approval rather than silent trust. This is the tool-poisoning / rug-pull control, and the
  revision's cacheable list results make it nearly free.
- **Least privilege per server**: each server binding carries its own capability grant; a server
  cannot reach capabilities granted to another.
- **Tool results are tainted** and delimited (017 §3).
- **Revocation is runtime**: unbinding a server cancels in-flight calls.

## 9. Pending research

Being completed from primary sources and folded in as normative tables:

- Field-by-field request/result shapes for tools, resources, resource templates, and prompts,
  including pagination, annotations/hints, and `isError` semantics.
- **Skills**: whether MCP specifies a skill primitive or an official extension, how that relates to
  `SKILL.md`-style packaging and to MAF's agent skills, and therefore whether 009 §8 needs an
  importer, a native mapping, or both.
- The official server registry, its API and publication model.
- Tier 1 SDK list, whether any C/C++ SDK exists, and the state of official conformance/inspector
  tooling we can run as the §10 gate.
- The specification's own security best-practices requirements (confused deputy, token passthrough,
  session hijacking) restated as MUST/SHOULD obligations we check.

## 10. Promotion gate

- **G1** — the official conformance/inspector tooling passes against our server at `2026-07-28`, on
  Windows and Linux, with zero deprecated features implemented.
- **G2** — our client interoperates with a corpus of real servers covering tools, resources,
  templates, prompts, MRTR, subscriptions, and the tasks extension.
- **G3 (statelessness)** — every request is independently serviceable: a proxy that round-robins our
  requests across N server replicas produces identical results to a single replica.
- **G4 (re-issue safety)** — a stream broken mid-request is re-issued and produces exactly one
  external effect, proven by an external counter under fault injection.
- **G5 (cache correctness)** — `ttlMs`/`cacheScope` honoured; a `private` result is never served
  across principals (canary test).
- **G6 (rug-pull)** — a server that mutates a tool's description or schema between calls is detected
  and blocked pending re-approval.
- **G7 (authorization)** — negative suite: wrong audience, missing/mismatched `iss`, credentials
  reused across authorization servers, and token passthrough are each rejected.
- **G8 (trace)** — `traceparent` propagation produces one connected trace across the boundary
  (016 §7 G2).

## 11. Open questions

- **Q1** — Which revisions to support simultaneously. Two constants and two suites (CONVENTIONS), or
  `2026-07-28` only and require peers to upgrade? The deprecation window makes the second defensible
  and unfriendly.
- **Q2** — The tasks extension versus A2A tasks versus our `Suspended` state (OQ-4).
- **Q3** — Whether to publish our tools with `outputSchema` and enforce it on results (006 Q1).
- **Q4** — MCP server versus A2A peer: two spellings for exposing an agent (012 Q4). Guidance needed.
- **Q5** — Whether to implement a client-side MCP **proxy/aggregator** so many servers appear as one
  tool surface, which is convenient and concentrates trust badly.
