# Research record — MCP `2026-07-28` normative detail

**Compiled:** 2026-07-31 · **Status:** dated snapshot · **Feeds:** RFC 011

Field-level dossier behind RFC 011. Modal verbs are preserved from the source because they are the
conformance obligations. Where the specification and a SEP disagree, both are recorded.

---

## 1. Method inventory

| Method | Paginated | Cacheable | May return `InputRequiredResult` |
|---|---|---|---|
| `server/discover` | no | **yes** | no |
| `tools/list` | yes | **yes** | no |
| `tools/call` | no | no | **yes** |
| `resources/list` · `resources/templates/list` | yes | **yes** | no |
| `resources/read` | no | **yes** | **yes** |
| `prompts/list` | yes | **yes** | no |
| `prompts/get` | no | no | **yes** |
| `completion/complete` | no | no | no |
| `subscriptions/listen` | no | no | no |

Notifications: `notifications/{cancelled, progress, message, resources/list_changed,
resources/updated, prompts/list_changed, tools/list_changed, subscriptions/acknowledged}`.

**Gone:** `initialize`, `notifications/initialized`, `ping`, `logging/setLevel`,
`resources/subscribe`, `resources/unsubscribe`, top-level `roots/list`,
`notifications/roots/list_changed`.

## 2. Result envelope — flat, not nested

```ts
interface Result          { _meta?: ResultMetaObject; resultType: ResultType; [k: string]: unknown }
interface CacheableResult extends Result { ttlMs: number; cacheScope: "public" | "private" }
interface ListToolsResult extends PaginatedResult, CacheableResult { tools: Tool[] }
```

`resultType`, `ttlMs`, `cacheScope`, `nextCursor` all sit **flat on the result object**.

- An unrecognized `resultType` **MUST** be considered invalid; an **absent** one **MUST** be treated
  as `"complete"` (older servers).
- `GetPromptResult` and `CallToolResult` extend `Result` only — **not cacheable**.

## 3. Caching (`/server/utilities/caching`)

- Servers **MUST** include caching hints on `resultType: "complete"` results of the six cacheable
  operations. Interim `"input_required"` results are not cacheable.
- **Cache key = method + the request parameters that affect the result.** Clients **MUST NOT** serve
  a cached response for a request whose method or parameters differ.
- **Results from an MRTR retry — requests carrying `inputResponses` or `requestState` — MUST NOT be
  cached.**
- `ttlMs`: servers **MUST** provide `>= 0`; `0` **SHOULD** be treated as immediately stale; absent
  **SHOULD** be treated as `0`; negative **SHOULD** be ignored and treated as `0`.
- Clients **SHOULD NOT** treat TTL as a polling interval; implementations that poll **MUST** apply
  jitter and backoff.
- `"public"`: any client, shared gateway, or caching proxy **MAY** store and serve to any user.
  `"private"`: **MUST NOT** be shared across authorization contexts (a different access token
  requires a different cache).
- Pagination: **servers MUST apply the same `cacheScope` to all pages of a given list request.**
- A relevant notification **invalidates** a fresh cached response.
- Security: servers **MUST** apply per-primitive access controls and **MUST NOT** rely on
  `cacheScope` alone to prevent unauthorized access.

## 4. Pagination

- Opaque cursors. Clients **MUST NOT** parse or modify them, and **MUST NOT** treat an **empty
  string as end-of-results** — an empty string is a valid cursor.
- Page size is server-determined; clients **MUST NOT** assume a fixed size.
- Invalid cursor **SHOULD** yield `-32602`. If a cursor becomes invalid the client **SHOULD** discard
  all cached pages and re-fetch from the beginning. **No cross-page consistency guarantee.**

## 5. The list-set invariant (repeated verbatim on all three list pages)

> The set **MAY** be empty and **MAY** change over time … but **MUST NOT** vary per-connection or as
> a side effect of other requests on the connection. The set **MAY** vary by the authorization
> presented on the request … since credentials are per-request input, not connection state.

Tools additionally: servers **SHOULD** return tools in deterministic order, to enable client caching
and improve LLM prompt-cache hit rates.

## 6. Tools

```ts
interface Tool extends BaseMetadata, Icons {
  description?: string;
  inputSchema: { $schema?: string; type: "object"; [k: string]: unknown };  // required
  outputSchema?: { $schema?: string; [k: string]: unknown };
  annotations?: ToolAnnotations;
  _meta?: MetaObject;
}
```

**Names (SEP-986, all SHOULD):** 1–128 chars, case-sensitive, only `A-Za-z0-9_-.`; no spaces or
other specials; unique within a server. Aggregators **SHOULD** disambiguate by prefixing — and
**the server `name` from `serverInfo` is not guaranteed unique and SHOULD NOT be relied on** for it.

**Schemas (SEP-2106/1613):** default dialect JSON Schema **2020-12** when `$schema` absent;
implementations **MUST** support at least 2020-12 and **MUST** handle unsupported dialects
gracefully with an error. `inputSchema.type` **MUST** be `"object"`. `outputSchema` has no such
constraint, and `structuredContent` may be **any** JSON value.

**`$ref` — directly checkable, verbatim:**

> Implementations **MUST NOT** automatically dereference `$ref` values that resolve to a network
> URI. Implementations **MAY** offer an opt-in mode … but it **MUST** be disabled by default and
> **SHOULD** enforce an allowlist of hosts or at minimum reject loopback, link-local, and private
> network addresses, apply timeouts and size limits, and log dereferenced URIs. Schemas that fail to
> validate due to an unresolved external `$ref` **SHOULD** be rejected rather than silently treated
> as permissive.

**Composition bounds — verbatim:**

> Composition keywords (`anyOf`, `oneOf`, `allOf`, `if`/`then`/`else`) and `$defs` … **SHOULD** apply
> reasonable bounds, such as a maximum schema depth, a cap on the total number of subschemas, or a
> per-validation time budget, to prevent a malicious schema from acting as a Denial-of-Service
> vector against the validator.

**Annotations still exist, and the names carry a `Hint` suffix:**

```ts
interface ToolAnnotations {
  title?: string;
  readOnlyHint?: boolean;     // default false
  destructiveHint?: boolean;  // default true
  idempotentHint?: boolean;   // default false
  openWorldHint?: boolean;    // default true
}
```

Three cautions: (1) **these field names are normative only in `schema.ts`** — the prose page says
only "properties describing tool behavior", so cite the schema; (2) clients **MUST** consider tool
annotations untrusted unless from a trusted server; (3) a Tool Annotations Interest Group and four
Draft SEPs (1862, 1913, 1984, 2417) are actively reworking this set — **keep the type
forward-extensible**. `Tool.execution.taskSupport` from `2025-11-25` is **removed**.

**Content blocks:** `TextContent` · `ImageContent` · `AudioContent` · `ResourceLink` ·
`EmbeddedResource`. Shared `Annotations`: `audience?: ("user"|"assistant")[]`, `priority?: 0.0–1.0`,
`lastModified?` (ISO 8601).

**`isError` (SEP-1303) — the split:** *protocol* errors (unknown tool, malformed request, server
error) → JSON-RPC `error`. *Tool execution* errors (API failure, **input validation failure**,
business-logic failure) → `result` with `isError: true`. Clients **SHOULD** provide tool execution
errors to the model to enable self-correction; protocol errors are less recoverable.

**`x-mcp-header`** (on an input-schema property; value becomes `Mcp-Param-{name}`): **MUST NOT** be
empty; **MUST** match HTTP token syntax; **MUST NOT** contain control characters; **MUST** be
case-insensitively unique within the schema; **MUST** apply only to **integer, string, boolean**
(*not* `number`); **MUST** only be on properties statically reachable through `properties` chains —
never through `items`, `oneOf`/`anyOf`/`allOf`/`not`, `if`/`then`/`else`, or `$ref`. HTTP clients
**MUST reject tool definitions violating these constraints, and rejection means excluding the tool
from the result of `tools/list`.** Servers **SHOULD NOT** mark sensitive parameters this way —
header values are visible to intermediaries.

**Tool security:** servers **MUST** validate inputs, implement access controls, rate limit, sanitize
outputs. Clients **SHOULD** confirm sensitive operations, **show tool inputs to the user before
calling the server to avoid exfiltration**, validate results before passing to the LLM, follow the
`$ref` rules, apply timeouts, and log usage. There **SHOULD** always be a human in the loop able to
deny an invocation.

## 7. Resources and prompts

`Resource`: `uri`, `name` required; `title`, `description`, `mimeType`, `size`, `annotations`,
`icons`, `_meta` optional. `ResourceTemplate`: `uriTemplate` (RFC 6570) + `name` required.

**Errors (SEP-2164):** non-existent resource **MUST** return **`-32602`**; internal errors
**SHOULD** be `-32603`; clients **SHOULD** still accept `-32002` from older peers. **Servers MUST
NOT return an empty `contents` array for a non-existent resource** — it is ambiguous.

**Security:** servers **MUST** validate all resource URIs; binary data **MUST** be properly encoded;
**servers MUST sanitize file paths to prevent directory traversal when serving `file://`**.

`Prompt`: `name` required; `arguments?: PromptArgument[]`. `prompts/get` → `{description?,
messages: PromptMessage[]}`, **not cacheable**. Implementations **MUST** carefully validate all
prompt inputs and outputs to prevent injection attacks or unauthorized resource access.

## 8. `listChanged` is now gated

Servers **SHOULD** send list-changed notifications only to clients that opened a
`subscriptions/listen` stream requesting that type. Every notification on that stream **MUST** carry
`_meta["io.modelcontextprotocol/subscriptionId"]`, whose value is the JSON-RPC `id` of the
`subscriptions/listen` request. Unconditionally: **"The server MUST NOT send notification types the
client has not explicitly requested."**

`SubscriptionFilter`: `{toolsListChanged?, promptsListChanged?, resourcesListChanged?,
resourceSubscriptions?: string[]}`. The server **MUST** send `notifications/subscriptions/acknowledged`
first, reflecting the subset it agreed to honour. On stdio, after a reconnect the client **MUST**
re-send `subscriptions/listen` — no subscription state survives.

## 9. MRTR field detail

```ts
interface InputRequiredResult extends Result { inputRequests?: InputRequests; requestState?: string }
type InputRequests  = { [serverAssignedId: string]: InputRequest }
type InputResponses = { [serverAssignedId: string]: InputResponse }
```

`InputRequest` values carry `{method, params?}` and **no `id`, no `jsonrpc`** — they are not
JSON-RPC messages.

- Only `prompts/get`, `resources/read`, `tools/call` may return `InputRequiredResult`; servers
  **MUST NOT** send it on any other request.
- Servers **MUST** include at least one of `inputRequests` or `requestState`.
- Servers **MUST NOT** send an `inputRequest` type the client has not declared support for.
- Servers **MUST NOT** assume the client will fulfil or retry.
- Clients **MUST NOT** inspect, parse, or modify `requestState`; if absent in the result, the client
  **MUST NOT** include one in the retry.
- **The JSON-RPC `id` MUST differ between the original request and the retry.**
- `inputRequests`/`requestState` affect **only** that retry and **MUST NOT** be reused on any
  parallel request.
- If the client under-answers, the server **SHOULD** respond with a new `InputRequiredResult` rather
  than an error.

## 10. Stateless-core detail

Five clauses from `/basic/index`: servers **MUST NOT** rely on prior requests over the same
connection for context; **SHOULD** handle requests from multiple tasks/threads/conversations;
**SHOULD NOT** require connection reuse for related operations; clients **SHOULD NOT** use a
conversation as the lifetime boundary for a stdio process; cross-request state **MUST** be referenced
by an explicit identifier passed on each request. *"An open connection, such as a STDIO process, is
not a conversation or session."*

`_meta` request keys: `io.modelcontextprotocol/protocolVersion` (**required**),
`.../clientCapabilities` (**required**), `.../clientInfo` (**SHOULD** send — note the spec page marks
it optional while SEP-2575 marks it required; the spec page governs), `.../logLevel`,
`progressToken`, `traceparent`/`tracestate`/`baggage`. Response: `.../serverInfo` **SHOULD** appear
in every result's `_meta`.

- A request missing a required field is malformed: **`-32602`**, and on HTTP **`400 Bad Request`**.
- Missing client capability → `MissingRequiredClientCapabilityError` **`-32021`** with
  `data.requiredCapabilities`.
- Error codes: `UnsupportedProtocolVersionError` **`-32022`** (`data: {supported, requested}`),
  `HeaderMismatchError` **`-32020`**. Retired and **MUST NOT** be emitted: `-32002`, `-32042`.
- Identity fields are self-reported and **not verified**; implementations **SHOULD NOT** use them for
  behaviour or security decisions.
- `_meta` key prefixes whose second label is `modelcontextprotocol` or `mcp` are **reserved**.

`DiscoverResult extends CacheableResult { supportedVersions: string[]; capabilities:
ServerCapabilities; instructions?: string }` — `serverInfo` lives in `_meta`, not top-level.

**Progress:** `progressToken` **MUST** be unique across active requests; **the `progress` value MUST
increase with each notification**; notifications **MUST** stop after completion and **MUST** only
reference tokens from an active request.

## 11. Transport detail

Required headers (SEP-2243): `MCP-Protocol-Version` on all POSTs; `Mcp-Method` on all requests;
`Mcp-Name` on `tools/call`, `resources/read`, `prompts/get` (from `params.name` or `params.uri`);
`Mcp-Param-{Name}` per `x-mcp-header`. Header/body mismatch → `400` + `-32020`. **Unknown method →
`404 Not Found` + `-32601`** (this is what distinguishes a modern server from a legacy one). Header
*names* are case-insensitive; header *values* are case-sensitive.

Non-ASCII or whitespace-bearing values use the sentinel encoding `=?base64?{...}?=` — lowercase,
exact; **clients MUST also encode plain-ASCII values that happen to match the sentinel pattern.**

Streamable HTTP: `Accept` **MUST** list both `application/json` and `text/event-stream`; the POST
body **MUST** be a single JSON-RPC request or notification and **the client MUST NOT send
responses**; notification → `202 Accepted` with no body; **the server MUST NOT send independent
JSON-RPC requests on the stream**; keep-alive is an SSE comment line that clients must ignore.
Legacy traffic: GET/DELETE → `405`; `Mcp-Session-Id` → ignore, never mint or echo; `Last-Event-ID` →
ignore, streams are not resumable.

**Cancellation:** on HTTP, closing the stream *is* the signal and the server **MUST** treat a client
disconnect as cancellation — no `notifications/cancelled` expected. On stdio the client **MUST** send
it. Servers **MUST NOT** send further messages for a cancelled request, and **MUST NOT** send
`notifications/cancelled` for any purpose other than tearing down a `subscriptions/listen` stream.

**stdio:** newline-delimited, no embedded newlines; server **MUST NOT** write non-MCP output to
stdout; **the server MUST NOT write JSON-RPC requests to stdout**; stderr is free-form and clients
**SHOULD NOT** treat stderr output as indicating an error. Servers **SHOULD** exit when stdin closes
— *"the primary graceful-shutdown signal and the only portable one"*.

## 12. Extensions and the tasks extension

Identifier form `{vendor-prefix}/{extension-name}`. **Extensions are always disabled by default and
require explicit opt-in.** On non-support, the supporting party **MUST** either revert to core
behaviour or reject with an appropriate error. Process (SEP-2133): Extensions Track SEPs need
RFC-2119 language, an associated working group, and **at least one reference implementation in an
official SDK before review**. Breaking change includes **adding a new required field**. There is **no
formal extension registry** — two index pages only.

Official extensions: `io.modelcontextprotocol/{tasks, ui, oauth-client-credentials,
enterprise-managed-authorization}`.

**Tasks:** `tasks/get`, `tasks/update`, `tasks/cancel`, `notifications/tasks`. Status ∈ `working |
input_required | completed | cancelled | failed`. Supported request type **today: `tools/call` only**.

- A server **MUST NOT** return `CreateTaskResult` to a client that did not include the extension
  capability **on that request**, regardless of prior declarations.
- **A server MUST NOT return `CreateTaskResult` until the task is durably created** — until a
  `tasks/get` would resolve; in eventually-consistent environments it **MUST** wait for consistency.
- **`notifications/cancelled` MUST NOT be used for task cancellation.**
- **The `failed` status MUST NOT represent a tool result with `isError: true`** — that is `completed`
  with error details in `result`.
- Over Streamable HTTP, `Mcp-Name` **MUST** be set to `params.taskId`.
- Task IDs may act as bearer tokens for stored state: servers **MUST** generate them with sufficient
  entropy and **MUST** authorize every task request. There is deliberately no `tasks/list`, so a
  server cannot leak one caller's tasks to another.

## 13. Security clauses worth lifting verbatim

**Token passthrough:** *"MCP servers MUST NOT accept any tokens that were not explicitly issued for
the MCP server."* Servers **MUST** validate audience per RFC 8707 (failure → 401); **MUST** only
accept tokens valid for their own resources; **MUST NOT** accept or transit any other tokens; **MUST
NOT** pass a received token upstream. Clients **MUST NOT** send tokens other than those issued by the
server's AS, and **MUST** send the `resource` parameter (RFC 8707) on both authorization and token
requests, regardless of whether the AS supports it.

**Confused deputy:** proxies with static client IDs **MUST** obtain per-client user consent before
forwarding to a third-party AS; **MUST** maintain a per-user registry of approved `client_id`s;
consent UI **MUST** identify the client, scopes, and registered `redirect_uri`, carry CSRF
protection, and block framing; consent cookies **MUST** use `__Host-`, `Secure`, `HttpOnly`,
`SameSite=Lax`, be signed or server-side, and **bind to the specific `client_id`**; redirect URIs
**MUST** be validated by **exact string match**; `state` **MUST** be cryptographically random,
single-use, short-lived, and — emphatically — **the consent cookie/session carrying `state` MUST NOT
be set until after the user approves the consent screen**, or the consent screen is ineffective.

**State-handle hijacking** (the renamed session-hijacking section, because protocol sessions no
longer exist): servers implementing authorization **MUST** verify all inbound requests; **MUST NOT
treat possession of a state handle as authentication**; **SHOULD** use non-deterministic handles from
a secure RNG with expiry; **SHOULD** bind handles server-side to the authenticated user, keyed as
`<user_id>:<handle>` where **the user ID is derived from the verified token rather than supplied by
the client**. MRTR `requestState` is attacker-controlled input: if it influences authorization,
resource access, or business logic, servers **MUST** integrity-protect it (HMAC/AEAD) and **MUST**
reject state that fails verification; single-use, if required, **MUST** be enforced server-side.

**SSRF during OAuth discovery:** clients deployed to a server **MUST** consider SSRF and mitigate;
**SHOULD** require HTTPS in production, block `10/8`, `172.16/12`, `192.168/16`, `127/8`, `::1`,
**`169.254/16` including cloud metadata endpoints**, `fc00::/7`, `fe80::/10`; and — *"Avoid
implementing IP validation manually"*, because encoding tricks (octal, hex, IPv4-mapped IPv6) defeat
custom parsers.

**Local server compromise:** one-click local server configuration **MUST** implement consent showing
the **exact command without truncation**, identify it as executing code on the user's system, and
require explicit approval. Clients **SHOULD** sandbox local servers with minimal privileges.

**Origin / DNS rebinding:** servers **MUST** validate the `Origin` header and **MUST** respond `403`
when present and invalid; **SHOULD** bind to localhost rather than all interfaces when local.

**Icons:** treat metadata and bytes as untrusted; **MUST** reject unsafe URI schemes and redirects
(`javascript:`, `file:`, `ftp:`, `ws:`); **fetch without credentials**; treat the declared MIME type
as advisory and **detect content type via magic bytes, rejecting on mismatch**.

**Elicitation:** servers **MUST NOT** use form mode for passwords, API keys, tokens, or payment
credentials and **MUST** use URL mode; **MUST NOT** put sensitive user information in an elicitation
URL; **MUST NOT** provide a pre-authenticated URL; clients **MUST NOT** pre-fetch the URL or open it
without explicit consent, **MUST** show the full URL, and **MUST** open it so that neither the client
nor the LLM can inspect its content or the user's input. Servers **MUST** verify the identity of the
user who completes the flow is the same user who started it, and **MUST NOT** transmit credentials
obtained by URL elicitation to the client.

**Explicitly out of scope upstream — worth importing into our threat model verbatim:**

> the client spawns the server as a local subprocess and both might run with equivalent
> environment-level privilege … a malicious server already has arbitrary code execution by virtue of
> being run, and a malicious client already has full process control over the server it spawned …
> **The SDK's stdio transport is not a sandbox.**

## 14. Two important negative findings

**There is no section named "tool poisoning" or "rug pull" anywhere in the specification, and neither
term appears in it.** The nearest normative statements are that tool annotations and behaviour
descriptions **MUST**/should be considered untrusted unless from a trusted server, that clients
**SHOULD** show tool inputs to the user before calling, and that clients **SHOULD** validate tool
results. Structurally the rug-pull surface is open: a server may change its tool set at any time via
`notifications/tools/list_changed` or simply by letting `ttlMs` expire, and **nothing requires a
client to re-confirm consent when a tool definition changes.**

**Prompt injection likewise has no named section.** The nearest is the prompts page's
*"Implementations MUST carefully validate all prompt inputs and outputs to prevent injection attacks
or unauthorized access to resources"* and the tools page's *"Validate tool results before passing to
LLM"*. SEP-2577 cites prompt injection and exfiltration as part of the rationale for deprecating
Sampling.

**Consequence:** any rug-pull or injection defence we specify is **local policy**, correctly labelled
as such — not conformance.

## 15. Migration

| Client | Server | Outcome |
|---|---|---|
| Modern | Modern | Works |
| Modern | Legacy | **Fails** — a legacy server may even process an era-ambiguous method under legacy semantics |
| Dual-era | Modern / Legacy | Works |
| Legacy | Modern | **Fails — legacy clients have no fall-forward mechanism** |
| Legacy | Dual-era | Works |

stdio probe outcomes: `DiscoverResult` → modern; a recognized modern JSON-RPC error → modern but
version-mismatched (**do not fall back to `initialize`**); anything else or a timeout → legacy.
**The fallback MUST NOT be keyed to one specific error code.** On HTTP, a `400` must have its body
inspected first, because modern servers also use `400` for `UnsupportedProtocolVersionError`,
`MissingRequiredClientCapabilityError`, and header-validation failures.

Era is **a property of the server, not of a request**: clients **SHOULD** cache it for the server
process (stdio) or origin (HTTP) and re-probe if the cached assumption fails.

Deprecation policy mechanics: the twelve-month minimum runs **from the revision in which the feature
is first marked Deprecated**, not from SEP finality; removal becomes possible in the first revision
released on or after the window elapses; expedited security removal still requires **at least ninety
days**. **No features have yet been removed under this policy.**

---

**Sources:** `/specification/2026-07-28/` — `basic/index`, `basic/transports/streamable-http`,
`basic/transports/stdio`, `basic/patterns/{mrtr,subscriptions}`, `basic/authorization/*`,
`server/{index,discover,tools,resources,prompts}`, `server/utilities/{caching,pagination,completion,logging}`,
`client/*`, `/extensions/{overview,client-matrix}`, the `io.modelcontextprotocol/tasks` extension,
`/docs/2026-07-28/tutorials/security/security_best_practices`, `/community/feature-lifecycle`,
`/development/roadmap`, SEPs 986, 1303, 1613, 2106, 2133, 2164, 2243, 2322, 2484, 2549, 2575, 2577,
2596, 2640, 2663, and `schema/2026-07-28/schema.ts` in `modelcontextprotocol/modelcontextprotocol`.
