# MCP conformance harness — how it actually drives an implementation under test

**Date:** 2026-08-15 · **Researched for:** ADR-023 §10 (third design iteration), Tier 1
· **Status:** primary-source verified

## Why this was researched

ADR-023 §10.4 committed Tier 1 to "a stdio transport behind `McpClient`'s `RequestSender`", and named
the harness's pipe direction as the one thing it did **not** know — *"not established by anything in
this repo"* — flagging it as a dated-research item before any code. This is that item. The answer
falsifies §10.4's transport choice and confirms its structural claim, and it also settles ADR-023
§10.0's open question about stdio server-role testing.

## What the harness actually does

Fetched from the tool's own repository (`github.com/modelcontextprotocol/conformance`, 2026-08-15).

### Client role — the harness spawns your binary and hands it a URL

> *"The framework appends `<server-url>` as an argument to your command and sets the
> `MCP_CONFORMANCE_SCENARIO` environment variable to the scenario name."*

```
npx @modelcontextprotocol/conformance client --command "<client-command>" --scenario <scenario-name>
```

So the sequence is: the harness starts a **test server** for the scenario, spawns the client under
test as a subprocess, appends the server's URL to the command line, and sets
`MCP_CONFORMANCE_SCENARIO` in the environment. **The client under test then connects outbound to that
URL.** It does *not* speak MCP over its own stdin/stdout to the harness.

### Server role — the harness connects to an already-running server

> *"Connecting to the running server as an MCP client"*

```
npx @modelcontextprotocol/conformance server --url <url>
```

The harness acts as an MCP client making requests to a URL the operator supplies.

### Transport support

**HTTP/URL-based only, for both roles.** The documentation mentions only HTTP URLs; server examples
use `http://localhost:3000/mcp`; **no stdio transport is discussed anywhere in the README.**

## Consequences for AgentEngine

### 1. ADR-023 §10.4's stdio-client design is wrong and is withdrawn

A client under test needs an **outbound HTTP client**, not a stdio transport. The stdio framing
obligations §10.4 enumerated (newline-delimited, no non-MCP stdout, exit on stdin close) are real
protocol obligations for a stdio *server* (`docs/research/2026-mcp-protocol-detail.md:268-271`) but
are not what the client conformance suite exercises.

### 2. The Tier 1 structural claim survives, and gets cheaper

ADR-023 §10.1's actual claim — that the client role needs **no listener, no host adapter, no fixture,
and no attribution apparatus**, because the harness drives us — is unaffected and now stronger:
AgentEngine already owns a proven outbound HTTP path, so Tier 1 needs no new transport at all.

| Piece | Status | Where |
|---|---|---|
| Outbound plain-HTTP exchange | Real, proven (ADR-011) | `sandbox/net_egress_proxy.hpp` |
| Not gated on `AGENTENGINE_WITH_HTTPS` | Confirmed — the plaintext branch "has no such gate" | `sandbox/provider_http_client.hpp:27-31` |
| The seam to wire it into | `RequestSender = std::function<JsonRpcResponse(JsonRpcRequest const&)>` | `protocol/mcp/client.hpp:48` |
| An executable target precedent | `add_executable(agentengine_policy_reachability tools/…)` | `CMakeLists.txt:61` |

### 3. One real obstacle: our own SSRF defence blocks the harness

The harness's URL is loopback (`http://localhost:3000/mcp`), and `is_blocked_address()` blocks
loopback (127.0.0.0/8) by design as ADR-011's anti-SSRF control
(`sandbox/net_egress_proxy.hpp:65-68`). Reaching the harness therefore requires an **explicit**
egress address policy — which already exists and is already judged: ADR-016 established the opt-in
policy for exactly this class of legitimate loopback destination (its own §G2 gate is
*"the provider path reaches a private/loopback address"*, and it records that an explicit policy
*"is no longer needed merely to reach loopback"*, `ADR-016:128`, `:141`).

This is a **positive control opportunity, not just an obstacle**: a Tier 1 run proves both that the
policy permits the intended destination and — with the policy removed — that the SSRF block is real.

### 4. 011 §10 G1 has no stdio escape hatch

ADR-023 §10.0 left open whether `conformance server` could drive stdio, and treated Tier 2's gate
status as unresolved. It is now resolved: **it cannot.** The server suite is URL-based only, so
011 §10 G1 genuinely requires an HTTP endpoint — either a first-party listener (ADR-021/ADR-022, which
the project-owner direction rules out) or a host adapter (ADR-023 Tier 3). There is no cheaper path,
and the M7 Phase G audit's characterisation of G1 as listener-blocked is correct.

Stdio remains a real *product* surface — 011 §7: *"**stdio** remains for local servers"* — but it
yields **no conformance gate**, so ADR-023 §9h's T0 finding is now fully resolved: its insight
(stdio was absent from the design space) was right, its conformance claim was wrong, and the
conformance half is dead.

## Net effect on ADR-023's tiering

| Tier | Before this research | After |
|---|---|---|
| 1 — MCP client role | stdio transport, closes G2 | **Outbound HTTP (already built), closes G2** — cheaper than assumed |
| 2 — stdio server role | gate status unknown | **No gate.** Product surface only; deprioritised |
| 3 — host-fronted HTTP | needed for G1/G3/G4/G8/G9 | **Unchanged, and now confirmed as the only path to G1** |

## Executed against the real tool (2026-08-15)

Everything above was then run, not just read. `node`/`npx` are available on the dev machine, so the
harness was executed directly. Six findings, all reproducible.

### 1. `latest` does not support `2026-07-28`; only a prerelease does

```
$ npx @modelcontextprotocol/conformance@latest list --spec-version 2026-07-28
Unknown spec version: 2026-07-28
Valid versions: 2025-03-26, 2025-06-18, 2025-11-25, draft, extension
```

`latest` is **0.1.16**. The revision this project pins is supported only by
**`0.2.0-alpha.11`** (dist-tag `alpha`, package modified 2026-08-07). This matters for 011 §10's own
requirement that a published percentage be *"pinned to a conformance release"*: today the only
version that can produce a `2026-07-28` number is a **prerelease**, and the RFC does not contemplate
that.

### 2. 011 §10's scenario lists do not match the tool

The RFC's client list is `mrtr-client`, `request-metadata`, `json-schema-ref-deref`,
`http-custom-headers`, `auth/`. The tool's actual `2026-07-28` client list adds `tools_call`,
`sep-2322-client-request-state`, `http-standard-headers`, `http-invalid-tool-headers`,
`json-schema-2020-12-preservation` — and differs on two points that matter:

- **`mrtr-client` does not exist** in the client scenario list for any spec version.
- **`json-schema-ref-deref` is actually `json-schema-ref-no-deref`** — a semantic inversion. The
  suite tests that a client does **not** dereference `$ref`. An implementer working from the RFC's
  name would build the opposite behaviour.

### 3. The client contract, empirically confirmed

```
Executing client: ...\agentengine_mcp_conformance_client.exe http://localhost:59577/mcp
```

The URL is appended as the final argument, exactly as the README documents. `tools_call` additionally
requires the client to really invoke the tool — a client that connects and exits reports
*"Tool was not called by client"*.

### 4. `_meta` was missing entirely — a real conformance defect the suite found

The suite's schema validator rejected our first request:

> `ListToolsRequest/params: must have required property '_meta'`

and the mock answered **HTTP 400**, which is precisely what the spec detail documents for a request
missing a required field (*"malformed: `-32602`, and on HTTP `400 Bad Request`"*, §detail:230).

011 §2 specifies this per-request `_meta` in full, and the engine did not implement it: Phase C1 built
the envelope with an explicit *"no `_meta` (011 §2)"* and Phase C3 never added it. **No outbound
request `McpClient` produced was schema-valid for this revision.** Fixed in
`protocol/mcp/client.hpp` (`with_request_meta`).

### 5. `clientCapabilities.extensions` is an object, not an array

011 §2 says only *"our declared capabilities, including `extensions`"* and gives no shape. An array
was rejected:

> `_meta/io.modelcontextprotocol~1clientCapabilities/extensions: must be object`

### 6. `MCP-Protocol-Version` is a required HTTP header that 011 §7 does not list

After the schema passed, the mock still returned 400:

> `{"code":-32020,"message":"Missing MCP-Protocol-Version header"}`

011 §7 names only `Mcp-Method` and `Mcp-Name` as the headers a POST **MUST** carry. The Streamable
HTTP transport also requires `MCP-Protocol-Version`, and the RFC omits it.

### First conformance numbers — engine-attributable, no fixture, no listener

Client role, spec `2026-07-28`, tool `0.2.0-alpha.11`, non-`auth` scenarios:

| Scenario | Checks |
|---|---|
| `tools_call` | **2/2** |
| `request-metadata` | **4/4** |
| `http-standard-headers` | **1/1** |
| `json-schema-ref-no-deref` | **1/1** |
| `http-invalid-tool-headers` | 10/11 |
| `json-schema-2020-12-preservation` | 2/3 |
| `sep-2322-client-request-state` | 2/5 |
| `http-custom-headers` | 0/5 |
| **Total** | **22/32 (69%)** |

Four scenarios pass completely. The `auth/*` scenarios (18 of them) were not run: they exercise
OAuth 2.1 flows against an authorization server, which ADR-021 §3 explicitly scoped out
("not a full OAuth 2.1 authorization-code/PKCE flow against a live external Authorization Server").

**This is the number ADR-023 §10.1 predicted was reachable without a listener, and it is.** Every
check is attributable to engine code: the harness runs its own server and spawns our binary, so there
is no fixture whose behaviour could be confused for ours.

## Sources

- [modelcontextprotocol/conformance](https://github.com/modelcontextprotocol/conformance) — primary,
  fetched 2026-08-15; both CLI invocations, the `<server-url>`-appended-argument and
  `MCP_CONFORMANCE_SCENARIO` mechanism, and the absence of any stdio transport.
- [modelcontextprotocol/modelcontextprotocol#1352](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/1352)
  and [#1841](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/1841) — corroborating
  ecosystem context that cross-transport conformance is an acknowledged gap, i.e. the HTTP-only scope
  is a current property of the tool rather than a misreading of it.
- [MCP transports, revision 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports)
  — the transport spec both roles are tested against.

## Caveat on durability

The tool is versioned and moving (011 §10 already requires the published percentage be *"pinned to a
conformance release"*). This document describes the harness as of **2026-08-15**; a later release
adding stdio support would reopen §10.0's question, and the pinned release recorded with any published
percentage is what makes that checkable rather than a surprise.
