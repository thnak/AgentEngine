# Research record — MCP ecosystem: registry, SDKs, C/C++ landscape, conformance tooling

**Compiled:** 2026-07-31 · **Protocol revision in force:** `2026-07-28` · **Status:** dated snapshot

Companion to [`2026-standards-landscape.md`](2026-standards-landscape.md), which covers the
protocol *content*. This record covers the ecosystem *around* it: where servers are published, what
SDKs exist, what the C/C++ situation actually is, and — most usefully — what we can run against our
own implementation to turn RFC 011's conformance claim into a measured number.

---

## 1. The official MCP Registry

**Still in preview**, not GA: every registry doc page carries *"The MCP Registry is currently in
preview. Breaking changes or data resets may occur before general availability."*

- Base URL `https://registry.modelcontextprotocol.io`, OpenAPI at `/openapi.yaml` (`info.version:
  1.0.0`), interactive docs at `/docs`. Implementation: `modelcontextprotocol/registry` (Go, ~7.1k ★).
- Four distinct terms in the FAQ: **MCP Registry API** (any API implementing the OpenAPI spec) ·
  **Official MCP Registry API** (the REST API at that URL, a *superset*) · **MCP registry** (any
  third-party service providing one) · **Official MCP Registry** (the service).

### Endpoints

`/v0.1/` is current (`/v0/` also exists).

| Group | Endpoints |
|---|---|
| Discovery (unauthenticated) | `GET /v0.1/servers` (`cursor`, `limit`, `updated_since`, `search`, `version`, `include_deleted`) · `GET /v0.1/servers/{serverName}/versions` · `GET /v0.1/servers/{serverName}/versions/{version}` (`latest` is special-cased) |
| Auth token exchange | `POST /v0.1/auth/{github-at, github-oidc, dns, http, oidc}` |
| Publishing (authenticated) | `POST /v0.1/publish` · `PUT /v0.1/servers/{serverName}/versions/{version}` · `PATCH .../status` · `PATCH .../versions/{version}/status` |
| Utility | `POST /v0.1/validate` · `GET /v0.1/health` · `/ping` · `/version` |

Path parameters **MUST** be URL-encoded (`io.modelcontextprotocol/everything` →
`io.modelcontextprotocol%2Feverything`). Pagination is cursor-based, cursor format
`<serverName>:<version>`; responses are `{"servers":[…],"metadata":{"count":N,"nextCursor":"…"}}`
with each element `{"server":{…},"_meta":{…}}` and registry-managed fields under
`_meta["io.modelcontextprotocol.registry/official"]` (`status`, `statusChangedAt`, `publishedAt`,
`updatedAt`, `isLatest`). Incremental sync via `?updated_since=<RFC 3339>`.

### `server.json`

Schema version **`2025-12-11`** (`https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json`),
**JSON Schema draft-07**.

- Root: `$schema`, **`name`** (req, `^[a-zA-Z0-9.-]+/[a-zA-Z0-9._-]+$`, 3–200), **`description`**
  (req, **1–100 chars**), `title`, **`version`** (req, ≤255, ranges prohibited), `repository`,
  `packages[]`, `remotes[]`, `icons[]`, `websiteUrl`, `_meta`.
- `Package`: **`registryType`** (`npm`|`pypi`|`nuget`|`oci`|`mcpb`), **`identifier`**, `version`,
  `registryBaseUrl`, **`transport`**, `runtimeHint`, `runtimeArguments[]`, `packageArguments[]`,
  `environmentVariables[]`, `fileSha256` (`^[a-f0-9]{64}$`).
- `Input`: `description`, `isRequired`, `format` (`string`|`number`|`boolean`|`filepath`),
  `isSecret`, `default`, `choices[]`, `placeholder`, `value` (with `{curly_braces}` substitution).
  `KeyValueInput` adds `name`; `Argument` = `PositionalArgument` | `NamedArgument`.
- Transports: `{type:"stdio"}` | `{type:"streamable-http", url, headers[]}` | `{type:"sse", …}`.
  SSE is deprecated — publish an `"sse"` remote only for existing clients. Remote URLs support
  templating (`https://{tenant_id}.example.com/mcp`).
- `_meta`: only `io.modelcontextprotocol.registry/publisher-provided` is preserved; all other keys
  discarded; **4 KB** limit.

Schema history includes two breaking changes (`2025-09-29` removed registry-managed fields;
`2025-09-16` renamed ~13 fields to camelCase).

### Namespaces and ownership proof

| `registryType` | Allowed registry | Ownership proof |
|---|---|---|
| `npm` | registry.npmjs.org | `mcpName` in `package.json` matches `server.json` `name` |
| `pypi` | pypi.org | `mcp-name: $SERVER_NAME` in README |
| `nuget` | api.nuget.org | `mcp-name: $SERVER_NAME` in README |
| `oci` | docker.io, ghcr.io, `*.pkg.dev`, `*.azurecr.io`, mcr.microsoft.com | `LABEL io.modelcontextprotocol.server.name=` |
| `mcpb` | GitHub/GitLab releases | URL must contain `"mcp"`; `fileSha256` required — **the registry does not validate it; clients do** |

Namespaces: GitHub auth → `io.github.{user,org}/*`; domain auth → reverse-DNS of a controlled
domain, proven by DNS TXT on the apex (`v=MCPv1; k=ed25519; p=${PUBLIC_KEY}`) or
`https://example.com/.well-known/mcp-registry-auth`. Both support Google KMS (Ed25519) and Azure Key
Vault (ECDSA P-384) signing. Versions are **immutable and unique**; ranges prohibited.

### Moderation — the part that matters for our trust model

> *"We only remove illegal content, malware, spam, and completely broken servers."*
> Not removed: low-quality/buggy servers, **servers with security vulnerabilities**, duplicates,
> adult content.
> *"The MCP Registry does not make guarantees about moderation, and consumers should assume
> minimal-to-no moderation."*

Removal sets `status:"deleted"` but metadata remains API-accessible. Registry data is CC0 1.0.

The expected discovery chain is **Official Registry → aggregators/subregistries → host
applications**; aggregators should poll `GET /v0.1/servers` about once per hour and persist locally,
because *"The MCP Registry does not provide uptime or data durability guarantees."* Host
applications are explicitly **not** meant to consume it directly.

**Consequence for us (007 §7, 011 §8):** registry presence is *not* a trust signal and must never be
treated as one. Our digest-pinning, capability-grant, and re-approval-on-change controls are doing
all the work. If we ever consume the registry, we do it as an aggregator (poll + persist), never
live in the request path.

## 2. SDKs

| SDK | Tier | Latest release | Implements `2026-07-28`? |
|---|---|---|---|
| TypeScript | 1 | `@modelcontextprotocol/server@2.0.0` (07-27) | **Yes** |
| Python | 1 | `v2.0.0` (07-28) | **Yes** |
| C# | 1 | `v2.0.0` (07-28) | **Yes** |
| Go | 1 | `v1.7.0` (07-28) | **Yes** |
| Rust | 2 | `rmcp-v3.0.1` (07-29) | **Yes** |
| Java | 2 | `v2.0.0` (06-11) | No — tracks `2025-11-25` |
| Swift · Ruby · PHP · Kotlin | 3 | various | No |

All four Tier 1 SDKs shipped within a day of the revision. The near-universal major-version bumps
(`2.0.0`, `3.0.0`) are a good proxy for how disruptive this revision is — which is the justification
for RFC 011 being written stateless-first rather than as a migration.

## 3. C and C++ — there is no official SDK

Confirmed three ways (the `sdk.md` list of ten, a GitHub org SDK search, and full enumeration of the
org's 42 repos). `modelcontextprotocol/{cpp,c,cxx}-sdk` all 404.

> Common misreading: the SDK table's C# entry renders with a `square-c` icon and lives at
> `csharp-sdk`. That is C-sharp, not C.

~35 community C/C++ libraries exist, and **star count is actively misleading** — the two
highest-star C++ options are the most protocol-stale:

| Repo | ★ | Revision targeted | Licence | Note |
|---|---|---|---|---|
| `hkr04/cpp-mcp` | 311 | **`2025-03-26`** | MIT | Most-starred and most stale. Do not port |
| `GopherSecurity/gopher-mcp` | 134 | `2025-11-25`; `2026-07-28` *in progress* | Apache-2.0 | C++14, libevent+OpenSSL, ships a C API |
| `Qihoo360/TinyMCP` | 105 | `2024-11-05` | MIT | stdio only |
| `Neumann-Labs/mcp-cpp` | 61 | `2025-11-25` | ⚠️ **GPL-3.0 in LICENSE despite an Apache-2.0 README badge** | Avoid unless GPL is acceptable |
| `caomengxuan666/cxxmcp` | 17 | `2025-11-25` in source, conformance-tested against "latest" | MIT | C++17, **no runtime deps beyond stdlib** |
| `vparla/mcp-cpp` | 9 | `2025-11-25` | MIT | Pure C++20, CMake |

**No C or C++ implementation claims `2026-07-28` conformance.** The closest is `gopher-mcp`'s "in
progress". `cxxmcp` publishes conformance evidence (server 109/1, client 448/0 against "latest"; the
single failure is `ServerRejectsMissingMethodHeader` — the `Mcp-Method` header, a `2026-07-28`-only
requirement, which incidentally confirms "latest" means `2026-07-28`). Self-reported, not reproduced.

**Consequence:** AgentEngine would be among the first `2026-07-28`-native C++ implementations, and
plausibly the first to claim full conformance. If we borrow, the defensible references are `cxxmcp`
(MIT, dependency-light, conformance-evidenced) and `vparla/mcp-cpp` (MIT, pure C++20).

Search hygiene: results are polluted by Microchip **MCP2515/MCP23017/MCP4725** drivers, Minecraft
PE, a UART protocol reusing the acronym (`emMCP`), and an MQTT tunnel (`esp-mcp-over-mqtt`). At
least six distinct repos are named `mcp-cpp`.

## 4. Conformance tooling — this is our gate

### `modelcontextprotocol/conformance`

npm `@modelcontextprotocol/conformance` (`0.1.16`, MIT, bin `conformance`), TypeScript.

```bash
conformance server --url http://localhost:3000/mcp \
    [--scenario …] [--suite active|all|draft|pending] [--expected-failures …] [--verbose]

conformance client --command "./my-client" \
    [--scenario …] [--suite all|core|extensions|backcompat|auth|metadata|draft|sep-835] \
    [--spec-version 2025-11-25|2026-07-28|draft] [--force] [--timeout 30000]

conformance list [--server]
```

Client mode starts a test server, runs our binary, captures the wire, and validates — injecting
`MCP_CONFORMANCE_SCENARIO`, `MCP_CONFORMANCE_CONTEXT`, `MCP_CONFORMANCE_PROTOCOL_VERSION`.

**Server scenarios:** `caching`, `dns-rebinding`, `elicitation-defaults`, `elicitation-enums`,
`http-standard-headers`, `input-required-result`, `json-schema-2020-12`, `lifecycle`, `prompts`,
`resources`, `sse-multiple-streams`, `sse-polling`, `stateless`, `tools`, plus `tasks/` and
`negative`/`negative-mrtr`.

**Client scenarios:** `draft-result-fields`, `elicitation-defaults`, `http-base`,
`http-custom-headers`, `http-standard-headers`, `initialize`, `json-schema-ref-deref`,
`mrtr-client`, `request-metadata`, `sse-retry`, `tools_call`, plus `auth/`.

That list maps almost one-to-one onto RFC 011's hard parts: `stateless`, `input-required-result`,
`mrtr-client`, `http-standard-headers`, `caching`, `json-schema-ref-deref`.

Every scenario includes **wire-schema validation** against the spec's `schema.json`. Results land in
`results/<scenario>-<timestamp>/`. An **expected-failures baseline** makes it usable as an
incremental CI gate:

- baselined failure → exit 0
- unbaselined failure → exit 1 (regression)
- **baselined but now passing → exit 1** (stale entry — the baseline cannot rot silently)

A composite GitHub Action exists (`uses: modelcontextprotocol/conformance@v0.1.11`). Omitting
`--spec-version` infers applicability per scenario; an out-of-window version skips with exit 0
unless `--force`.

### SEP-2484 — conformance gates spec Finality

Status **Final**. A Standards Track SEP changing observable behaviour cannot reach Final without a
merged conformance scenario plus a `sep-NNNN.yaml` traceability file mapping **every
MUST/MUST NOT/SHOULD/SHOULD NOT** to a check (or an `excluded:` justification with an issue link).
SHOULD-level checks report as **warnings**, not failures. Governance points worth adopting verbatim:

> *"Where a test and the spec disagree, the spec is authoritative and the test is a bug."*

Tier assessments run against a **pinned conformance release**, not tip-of-main. And critically:

> *"The conformance suite itself is not restricted to official SDKs. Any implementation (official
> SDK, community SDK, or custom deployment) may run it and report a compliance percentage."*

That is explicit permission to publish a conformance percentage for AgentEngine — which is exactly
what invariant **I7** demands.

### Inspector

`@modelcontextprotocol/inspector@2.0.0` (MIT, Node ≥ 22.19.0, ~10.5k ★), with web UI, `--cli`, and
`--tui` modes. **Scoping caveat: it is an interactive debugging tool, not a conformance validator** —
the docs frame it as verifying basic connectivity and capability negotiation. Whether `2.0.0`
supports `2026-07-28` is **not stated**.

### What we can run against our engine today

| Tool | Target | Output |
|---|---|---|
| `conformance server --url …` | our HTTP server | pass/fail per check, baselineable |
| `conformance client --command …` | our client binary | pass/fail per check |
| conformance GitHub Action | CI | exit code |
| `inspector --cli` | our server | interactive only |
| `POST /v0.1/validate` | our `server.json` | schema + registry rules |
| draft-07 validator vs `server.schema.json` | our `server.json` | schema only |
| JSON Schema validator vs `schema/2026-07-28/schema.json` | captured wire messages | message shape |

## 5. Not verified

Recorded so it is not mistaken for established fact:

- No published conformance scoreboard or tier-score table exists.
- Package-type discrepancy: the docs site lists five `registryType` values; the registry repo's
  requirements doc additionally lists `cargo`/crates.io and Quay.io. Which the running service
  accepts is unverified.
- Whether Inspector `2.0.0` supports `2026-07-28`.
- Total server count in the registry.
- `cxxmcp` and `cMCP` conformance results are self-reported, not independently reproduced.
- The protocol revision targeted by many low-star C/C++ repos (their repos do not state it).

**Sources:** `registry.modelcontextprotocol.io/openapi.yaml` and `/v0.1/servers`;
`static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json`;
`modelcontextprotocol.io/registry/*` (about, quickstart, package-types, remote-servers,
authentication, versioning, registry-aggregators, faq, moderation-policy, github-actions, ToS);
SEP-1730 (SDK tiering) and SEP-2484 (conformance gating); the SDK and Inspector-v2 working-group
pages; `modelcontextprotocol/{registry,conformance,inspector}` including the client/server scenario
trees; npm registry metadata; and the GitHub REST API across the ten official SDK repos and ~36
C/C++ candidate repos.
