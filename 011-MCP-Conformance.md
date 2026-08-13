# 011 — MCP Conformance

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Protocol revision:** MCP **`2026-07-28`** · **Depends on:** 003, 006, 007, 016, 018 · **Gate:** §10

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
  onto `agentengine::rt::ThreadPool` rather than a per-server connection actor (historical: originally
  described as mapping onto Quark's stateless-worker pools — ADR-037 removed Quark; the exact
  current MCP-server threading shape should be re-verified against `rt::ThreadPool`'s implementation
  rather than assumed identical).
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
- **Schemas are JSON Schema 2020-12** (the default dialect when `$schema` is absent). We must support
  at least 2020-12 and **MUST** handle an unsupported dialect with an explicit error rather than a
  best-effort parse. `inputSchema.type` **MUST** be `"object"`; `outputSchema` carries no such
  constraint and `structuredContent` may be **any** JSON value.
- **`$ref` is a hardening obligation, not a parsing convenience.** We **MUST NOT** automatically
  dereference `$ref` values resolving to a network URI. Any opt-in fetch mode **MUST** be disabled by
  default and enforce a host allowlist, reject loopback/link-local/private addresses, apply timeouts
  and size limits, and log what it dereferenced. A schema with an unresolved external `$ref` is
  **rejected**, never treated as permissive. (Conformance scenario: `json-schema-ref-deref`.)
- **Composition-keyword bounds are ours to set and enforce.** The spec explicitly licenses hard
  limits — maximum schema depth, a cap on total subschemas, a per-validation time budget — because
  `anyOf`/`oneOf`/`allOf`/`if`/`$defs` from a third-party server is a DoS vector against our
  validator. A server's schema is attacker-controlled input.
- Arguments are validated **strictly** (006 §3 step 2 — reject, never coerce).
- **Tool annotations exist and carry a `Hint` suffix**: `readOnlyHint` (default false),
  `destructiveHint` (**default true**), `idempotentHint` (default false), `openWorldHint` (default
  true). Two cautions we encode: these names are normative **only in the schema**, not the prose
  page, so we generate from the schema; and clients **MUST** treat annotations as **untrusted** unless
  the server is trusted. We map them to advisory metadata only — an annotation never relaxes a
  capability check or an approval requirement (**I3**). They are under active revision by an interest
  group with four Draft SEPs in flight, so our type is forward-extensible by construction.
- **`x-mcp-header` is a mandatory client-side surface** on Streamable HTTP, and it is easy to miss.
  We validate every constraint (non-empty, HTTP token syntax, no control characters, case-insensitive
  uniqueness, primitives only — **`number` is not permitted** — and static reachability through
  `properties` chains only, never through `items`, composition, conditionals, or `$ref`), and a
  violation means **excluding that tool from the result of `tools/list`**, not merely warning.
- Tool names are `A-Za-z0-9_-.`, 1–128 chars, case-sensitive. **`serverInfo.name` is not guaranteed
  unique and must not be used for disambiguation** — our namespacing (006 §6) uses the binding id.
- **`isError` vs JSON-RPC error is a semantic split we honour on both sides**: protocol errors
  (unknown tool, malformed request) are JSON-RPC errors; execution errors — including **input
  validation failures** — are results with `isError: true`, which we surface to the model for
  self-correction (001 §6's "a tool failure is not a run failure").
- **Caching** is a correctness surface, not an optimization. `server/discover`, `tools/list`,
  `prompts/list`, `resources/list`, `resources/templates/list`, and `resources/read` carry `ttlMs`
  and `cacheScope`. Our obligations:
  - **Cache key = method + the parameters that affect the result.** Never serve a cached response to
    a request whose method or parameters differ.
  - **Never cache an MRTR retry result** — anything carrying `inputResponses` or `requestState`.
  - `cacheScope: "private"` **MUST NOT** cross authorization contexts; a different token is a
    different cache. `"public"` may be shared, and the spec warns that this holds *even for results
    from authenticated endpoints* — so as a **server** we apply per-primitive access control and
    never rely on `cacheScope` for authorization.
  - `ttlMs` absent or negative → treat as `0`. TTL is **not** a polling interval; if we poll at all,
    we apply jitter and backoff.
  - All pages of one list request share a `cacheScope`; a relevant `listChanged` notification
    **invalidates** a still-fresh cached entry.
- **Pagination**: cursors are opaque and **an empty string is a valid cursor**, not end-of-results —
  a classic off-by-one that silently truncates a tool list. Page size is server-determined. An
  invalidated cursor means discarding cached pages and restarting; there is no cross-page consistency
  guarantee.
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
ports, and A2A `INPUT_REQUIRED`. As a client, our own `interaction_id` (001 §2, resolving OQ-4) is
what we encode into `requestState`, HMAC/AEAD-protected per §8a below; a retry decodes it, looks up
the `Interaction`, and resolves it. A peer server needing to correlate its own elicitation across
retries encodes its own identifier in the same field, on its own side of the boundary; the
`notifications/elicitation/complete` notification and the `elicitationId` field are gone.

**Security note:** an MRTR input request is a server asking our host for something. It is subject to
the same authority rules as everything else — a server cannot obtain data or authority through
`inputRequests` that the principal does not hold (**I3**).

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

**Extensions are disabled by default and require explicit opt-in** — we advertise only the extensions
we actually implement in `clientCapabilities`/our server's own capabilities (§2), never a blanket
acceptance of whatever a peer offers, and an unrequested extension is never assumed enabled on either
side of the connection.

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

### 8a. Spec-mandated obligations we implement as conformance clauses

Lifted from the specification's own security material, restated as checkable requirements
([detail](docs/research/2026-mcp-protocol-detail.md) §13):

- **Token passthrough is forbidden.** We **MUST NOT** accept a token not issued for us, **MUST**
  validate audience per RFC 8707 (401 on failure), and **MUST NOT** forward a received token
  upstream. Outbound calls use credentials issued to this engine, with delegation as `on_behalf_of`
  (007 §2). As a client we send the `resource` parameter on both authorization and token requests.
- **State handles are not authentication.** Server-side, we **MUST NOT** treat possession of a
  server-minted handle (or a task id) as authenticating anyone; handles are high-entropy, expiring,
  and bound server-side as `<user_id>:<handle>` where **the user id comes from the verified token,
  never from the client**.
- **`requestState` is attacker-controlled input.** Where it influences authorization or business
  logic we integrity-protect it (HMAC/AEAD) and reject what fails verification; single-use, where
  required, is enforced server-side rather than assumed.
- **SSRF defence on every discovery fetch**: HTTPS in production, and block private, loopback,
  link-local, and **cloud metadata** ranges. The spec's own warning — *"avoid implementing IP
  validation manually"* — is why this routes through the same host-mediated egress as everything
  else (008 §4) rather than a bespoke parser.
- **Origin validation and localhost binding** for our HTTP surface (403 on invalid `Origin`).
  Conformance scenario: `dns-rebinding`.
- **Local stdio servers get consent showing the exact command, untruncated**, and run in a sandbox
  with minimal privileges — which is simply 007 §6's T2 tier applied to a subprocess.
- **Elicitation discipline**: never form-mode for credentials; never pre-fetch or auto-open a URL;
  always show the full URL; never transmit credentials obtained via URL elicitation back to a client.
- **Icons are untrusted bytes**: safe schemes only, fetched without credentials, content type
  detected by magic bytes with the declared MIME type treated as advisory.

**We import the spec's stdio boundary verbatim into our threat model:** *"The SDK's stdio transport
is not a sandbox."* A local MCP server is code we spawned with our privileges — which is exactly why
008's `Exec` capability and profile selection govern it, and why we do not pretend the transport
provides isolation it does not.

### 8b. Two negative findings — what we specify as local policy

Verified against the specification: **there is no section on tool poisoning or rug pulls, and neither
term appears in it. Prompt injection likewise has no named section.** The nearest normative
statements are that annotations and behaviour descriptions are untrusted, that clients **SHOULD**
show tool inputs before calling, and that results **SHOULD** be validated before reaching the model.

Structurally the rug-pull surface is wide open: a server may change its tool set at any moment via
`notifications/tools/list_changed`, or simply by letting `ttlMs` expire — and **nothing in the spec
requires a client to re-confirm consent when a tool definition changes.**

**Therefore our digest-pinning and re-approval control (§8), and the injection defences in 017, are
local policy, and this RFC labels them as such.** They are not conformance, we do not claim them as
conformance, and a peer that lacks them is not non-conformant. Being explicit about that line is the
difference between a security posture and a marketing one.

## 9. The registry, and why it is not a trust signal

The **official MCP Registry** (`registry.modelcontextprotocol.io`) is **still in preview** — its own
documentation warns of breaking changes and data resets before GA. Its publication model is sound
(namespace ownership proven by npm/PyPI/NuGet metadata, OCI labels, DNS TXT records, or a
`.well-known/mcp-registry-auth` document; immutable versions; `server.json` schema `2025-12-11`), and
we can validate our own `server.json` against it via `POST /v0.1/validate`.

Its **moderation policy is deliberately minimal**, and this is the load-bearing fact:

> *"We only remove illegal content, malware, spam, and completely broken servers."* Explicitly **not**
> removed: low-quality servers, **servers with security vulnerabilities**, duplicates.
> *"Consumers should assume minimal-to-no moderation."*

**Therefore: registry presence is never a trust signal in this engine, and no code path may treat it
as one.** Every control in §8 — digest pinning, per-server capability grants, re-approval on change,
taint — carries the entire weight. Registry metadata may inform a human's approval decision; it may
never substitute for one.

The registry's own guidance is that host applications should **not** consume it live: aggregators
poll `GET /v0.1/servers` roughly hourly and persist locally, because the registry offers no uptime
or durability guarantee. If we ever consume it, we do so as an aggregator, never in a request path.

Details and sources: [`docs/research/2026-mcp-ecosystem.md`](docs/research/2026-mcp-ecosystem.md).

## 10. Conformance tooling — the gate is executable

There is an **official conformance suite**, `@modelcontextprotocol/conformance`, that tests both
roles and validates every message against the spec's `schema.json`:

```bash
conformance server --url http://localhost:3000/mcp --suite all
conformance client --command "./agentengine-mcp-client" --spec-version 2026-07-28 --suite all
```

Its scenario list maps almost one-to-one onto the hard parts of this RFC — server: `stateless`,
`input-required-result`, `caching`, `http-standard-headers`, `json-schema-2020-12`, `tools`,
`resources`, `prompts`, `lifecycle`, `dns-rebinding`, `tasks/`, `negative-mrtr`; client:
`mrtr-client`, `request-metadata`, `json-schema-ref-deref`, `http-custom-headers`, `auth/`.

**Baseline discipline:** the suite supports an expected-failures baseline where an unbaselined
failure exits non-zero *and a baselined-but-now-passing check also exits non-zero*, so the baseline
cannot rot silently. That is exactly the property a conformance gate needs, and we adopt it as-is
rather than writing our own harness.

Two governance points from **SEP-2484** (Final) are adopted verbatim as project policy:

> *"Where a test and the spec disagree, the spec is authoritative and the test is a bug."*

and — the sentence that makes **I7** measurable —

> *"The conformance suite itself is not restricted to official SDKs. Any implementation … may run it
> and report a compliance percentage."*

**We publish that percentage, per role, per suite, pinned to a conformance release.** A claim of MCP
support in this project means a number from this tool, not a paragraph.

The **Inspector** is a debugging aid, not a validator, and is not part of any gate.

**Ecosystem context:** no C or C++ implementation currently claims `2026-07-28` conformance (the
closest is one project's "in progress"), while all four Tier 1 SDKs shipped support within a day of
the revision. AgentEngine would be among the first `2026-07-28`-native C++ implementations — which
is a reason to lean harder on the official suite, not less.

### Promotion gate

- **G1** — `conformance server` passes at `2026-07-28` on Windows and Linux, with a published
  percentage and a baseline containing only justified entries; zero deprecated features implemented.
- **G2** — `conformance client` passes across `core`, `extensions`, `backcompat`, and `auth` suites,
  with a published percentage.
- **G3 (statelessness)** — a proxy round-robining our requests across N server replicas produces
  identical results to a single replica (the property the removed session was hiding).
- **G4 (re-issue safety)** — a stream broken mid-request, re-issued per the spec's recovery path,
  produces exactly one external effect under fault injection, proven by an external counter.
- **G5 (cache correctness)** — `ttlMs`/`cacheScope` honoured; a `private` result is never served
  across principals (canary test).
- **G6 (rug-pull)** — a server that mutates a tool's description or schema between calls is detected
  and blocked pending re-approval.
- **G7 (authorization)** — negative suite: wrong audience, missing/mismatched `iss`, credentials
  reused across authorization servers, and token passthrough are each rejected.
- **G8 (trace)** — `traceparent` propagation produces one connected trace across the boundary
  (016 §7 G2).
- **G9 (registry)** — our published `server.json` validates against schema `2025-12-11` and
  `POST /v0.1/validate`; and a test proves no code path treats registry presence as trust.

## 11. Skills over MCP — we specify nothing

**There is no skill primitive in MCP at `2026-07-28` and no official extension for skills**; the
string does not appear in the normative schema. A Skills Over MCP Working Group exists, and its
current direction — **SEP-2640, an unmerged Draft** proposing `io.modelcontextprotocol/skills` with
`skills/list` and `skills/get` — **has already replaced its own earlier design once**. The roadmap
places skills under "On the Horizon".

We therefore implement no skills-over-MCP conformance, and instead keep extension negotiation (§3.6)
general enough that the extension slots in as configuration if it lands. Our skill format is
`SKILL.md` / agentskills.io (009 §8), which is a real open standard adopted by more than one vendor
and is independent of any transport.

One clause from that Draft is adopted now on its merits regardless of the SEP's fate:

> Hosts **MUST NOT** treat a digest match as a security boundary — digests are unsigned and
> server-supplied.

## 12. Migration and interoperability

- **Era is a property of the server, not of a request.** We cache the determination per server
  process (stdio) or origin (HTTP) and re-probe when a cached assumption fails.
- **stdio probe**: `DiscoverResult` → modern; a *recognized modern* JSON-RPC error → modern with a
  version mismatch, and we **do not** fall back to `initialize`; anything else or a timeout → legacy.
  The fallback **MUST NOT** be keyed to one error code, because legacy servers answer unknown
  pre-`initialize` requests with implementation-defined errors or not at all.
- **HTTP probe**: inspect the body before falling back on a `400` — modern servers also return `400`
  for `UnsupportedProtocolVersionError`, `MissingRequiredClientCapabilityError`, and header-validation
  failures.
- **Legacy clients cannot fall forward.** As a server we speak only the modern era, and per the
  spec's courtesy clause we name our supported versions in the error we return to an `initialize`
  request — that error may be the only diagnostic a legacy client can show its user.

## 13. Open questions

- ~~**Q1** — Which revisions to support simultaneously. Two constants and two suites (CONVENTIONS), or
  `2026-07-28` only and require peers to upgrade? The deprecation window makes the second defensible
  and unfriendly.~~ **Resolved — this was mostly already answered in §12, just not marked closed
  (2026-08-04):** **as a server, `2026-07-28`-only** (§12 already states this explicitly: "as a
  server we speak only the modern era"). **As a client, legacy-server tolerance stays best-effort**
  (§12's era-detection probing), never a second gated conformance suite — reusing 021 §1's tier
  vocabulary one layer up (Supported/CI-verified/Best-effort/Unsupported, applied to a protocol
  revision instead of a platform): `2026-07-28` is Supported (gated by §10's G1/G2), tolerating a
  legacy server we happen to detect is Best-effort (builds/runs, not CI-gated). This avoids the
  two-constants-two-suites maintenance burden 024 §2 would otherwise commit us to for a revision
  population that's shrinking under its own 12-month deprecation window (024 §2), while still being a
  reasonable citizen during the transition rather than refusing legacy peers outright.
- ~~**Q2** — The tasks extension versus A2A tasks versus our `Suspended` state.~~ **Resolved
  (OQ-4, 019 §8 Q2):** the tasks extension maps onto `Backgroundable`/`StandingEffect` (006 §6b) for
  a single long tool call; a whole-run pause is carried via `interaction_id` in `requestState`, the
  same mechanism §3.4's MRTR mapping already uses. A2A tasks needed no mapping — `Task ← Run` (012
  §1) already is one.
- ~~**Q3** — Whether to publish our tools with `outputSchema` and enforce it on results (006 Q1).~~
  **Resolved, split by whose schema it is (2026-08-04):** **as a server**, yes to both — publish
  `outputSchema` for our tools (§4 already generates listings from one schema source, so this is
  free) and enforce our own output against it strictly, the same "reject, never coerce" discipline
  §3.1 already applies to inputs — it's our own contract, and failing it is our own bug to catch at
  the boundary, not a peer-compatibility risk. **As a client**, validating a *third-party* server's
  `structuredContent` against *its own* claimed `outputSchema` is the harder case the "may break real
  servers" caution was actually about: a mismatch there is degraded to an annotated marker surfaced
  to the model alongside the raw content (the same "surfaced to the model for self-correction"
  treatment §3.1 already gives `isError` results), never a hard client-side rejection — schema
  conformance was never a trust signal to begin with (§8's tool results are tainted either way, per
  017), only a shape-correctness one, so a mismatch is worth surfacing, not worth breaking
  interoperability with an imperfect but functioning peer over. Same resolution written into 006
  §9 Q1.
- ~~**Q4** — MCP server versus A2A peer: two spellings for exposing an agent (012 Q4). Guidance
  needed.~~ **Resolved, publish both by default; they serve different consumer populations, not one
  redundant choice (2026-08-04):** 002 §7 already guarantees both are generated from one metadata
  table with no drift risk, so publishing both costs nothing extra and there's no real "pick one"
  tension once that's true. What actually differs is who each protocol serves: an MCP tool listing is
  for the (currently much larger) population of LLM-tool-calling clients that want to invoke a
  bounded operation like a function; an A2A Agent Card is for peer orchestrators that want to delegate
  an open-ended goal and manage a task lifecycle — progress, artifacts, multi-turn negotiation (012
  §5's "inter-organization / inter-process seam"). The remaining judgment call is authoring, not
  architecture: an agent whose actual behavior is bounded and function-like should be *described*
  that way even where both surfaces are technically available, and an agent that's genuinely
  autonomous/long-running should be described accordingly — over- or under-selling an agent's
  interaction style in its skills description is a documentation quality issue, not a protocol-
  exposure restriction. Enabling either surface stays an ordinary per-agent 020 §4 configuration
  choice. Same resolution written into 012 §9 Q4.
- ~~**Q5** — Whether to implement a client-side MCP **proxy/aggregator** so many servers appear as one
  tool surface, which is convenient and concentrates trust badly.~~ **Resolved, the convenience
  already exists without the hazard; the hazard is a different, narrower thing we don't do
  (2026-08-04):** the union-with-attribution pattern this project already has (005 §5's
  `ContextContribution.tools`, unioned into one per-run tool table while still "carr[ying] the
  provider's identity in the audit record, same as a plugin's") is exactly what a convenience
  aggregator would need — a unified, browsable tool surface across N MCP servers for **our own
  agents**, without collapsing each server's independent capability grant, digest pinning, or audit
  attribution (§8's "least privilege per server" stays intact underneath the union). That isn't
  really "a proxy" in the trust-concentrating sense the question worried about, and it needs no new
  mechanism — 006's tool-table union already does this. What we explicitly do **not** do: re-publish
  that aggregate as a single external-facing MCP server to **third parties** — that would be the real
  hazard (a third-party client trusting "us" without knowing it's actually N independently-trust-
  tiered backends, so a compromise of any one appears to originate from us). The line is who the
  client is, not whether aggregation happens.
