# ADR-039 — Inbound transport: host-pluggable protocol handlers, not a first-party listener

**Status:** Proposed (2026-08-13). Designed, red-teamed (independent adversarial pass, real code
citations), and partially proven (the one small, closable mechanism the red-team found missing —
`trust::principal_from_bearer_claims()` — is real, tested code); awaiting the project owner's
explicit "Judged" sign-off per this project's governance (`decisions/README.md`; `OpenQuestions.md`
OQ-11's resolution that the project owner is the ADR judge).

**Supersedes:** `decisions/ADR-021-inbound-protocol-trust-boundary.md` §3–§8 (its Design A/B/C
decision and the "AgentEngine owns TLS/auth termination first-party" strategic direction) and
`decisions/ADR-022-inbound-listener-reactor-model.md` in full (its entire question — which reactor a
first-party listener shares — is moot once there is no first-party listener, and doubly moot since
ADR-037 deleted every Quark type its designs were built from). Neither document is deleted — both
keep their full text as the historical record of how this project reasoned before this reversal; each
gets a pointer to this ADR at its own top.

## 1. Why this is being revisited, not merely revised

Two independent things changed since ADR-021/022 were Judged (2026-08-08):

1. **ADR-037 removed Quark entirely.** ADR-022's whole question — "does the listener share
   `TcpTransport::event_loop()`'s reactor, or run standalone" — presupposed types
   (`quark::pal::IoContext`, `TcpTransport`) that no longer exist anywhere in this codebase. There is
   no reactor left to choose between. Separately, ADR-021 §2's `quark::pal::tcp_listen`/`accept_one`
   citation is now `agentengine::pal::tcp_listen`/`accept_one` (`include/agentengine/pal/net.hpp`,
   ADR-037's vendored socket PAL) — a standalone listener is now, if anything, *easier* to build than
   ADR-022 assumed, since it needs zero Quark dependency at all. This alone would justify a documentation
   pass; it does not by itself justify reopening the strategic question.
2. **A real precedent gap, not previously checked.** Comparable agent SDKs — Microsoft Agent
   Framework (MAF, .NET) and the OpenAI Agents SDK (Python) — do not implement their own TLS/HTTP
   listener at all. MAF's `MapMcp()` mounts protocol handlers onto the host application's own Kestrel;
   the OpenAI Agents SDK does the same onto uvicorn/Starlette. Neither SDK reimplements a socket/TLS
   stack. ADR-021 never asked whether AgentEngine needs to either — both of its competing designs
   (§3, Design A and Design B) assumed AgentEngine builds its own listener, differing only in whether
   it also terminates TLS/OAuth itself or defers that to a reverse proxy sitting in front of that
   self-built listener.

**And a fact ADR-021 §2 already recorded but never acted on: this codebase's own protocol dispatchers
were already built exactly like MAF's/OpenAI's host-mounted handlers, not like a listener.** Confirmed
directly against current code, not memory:

- `McpServer::dispatch(JsonRpcRequest const&) -> JsonRpcResponse` (`include/agentengine/protocol/mcp/server.hpp:148`)
  is a pure in-process function — object in, object out, zero socket/TLS awareness.
- `A2aServer`'s own file-top comment (`include/agentengine/protocol/a2a/server.hpp:6`): *"Transport-agnostic,
  like `protocol/mcp/server.hpp`: no JSON-RPC/REST envelope here... and no actor-messaging plumbing
  either."*
- `agui/sse.hpp`'s own file-top comment (`include/agentengine/protocol/agui/sse.hpp:7-12`): *"There is
  no HTTP listener anywhere in this codebase yet... nothing here opens a socket or sets a
  `Content-Type` header; a future transport sub-phase writes these bytes onto whatever connection it
  owns."*

ADR-021 built its Design A/B choice on top of this fact instead of following it to its conclusion. The
question this ADR actually answers: given the dispatch layer already assumes nothing about transport,
should AgentEngine's *v1 scope* include building one anyway (a listener + TLS server + HTTP/1.1
parser, ADR-021 Design A), or should "the host supplies the transport" be the permanent, structural
answer, matching every comparable SDK?

## 2. Design considered and rejected: AgentEngine owns a first-party listener (ADR-021 Design A)

Still technically buildable — `agentengine::pal` (ADR-037) plus the already-vendored mbedTLS
(`AGENTENGINE_WITH_HTTPS`, ADR-013) give this project everything Design A needed, now with less
Quark-shaped scaffolding than ADR-021 assumed. Rejected anyway, on three independent grounds:

1. **Unprecedented among every comparable SDK surveyed.** Neither MAF nor the OpenAI Agents SDK
   accepts the cost this ADR is about to name; both push it to the host's already-hardened web
   framework. AgentEngine building one anyway is a real, unforced scope expansion with no peer
   precedent, not an industry-standard expectation this project is behind on.
2. **ADR-021 §5's own worry, unresolved by anything since.** *"This project ships a hand-rolled
   TLS+OAuth stack nobody red-teamed as hard as nginx/Envoy has been, in a security-critical position,
   for no real gain."* Nothing in the 2026-08-08 → 2026-08-13 gap changed this calculus — if anything,
   ADR-037's own scope-discipline precedent (removing a whole runtime rather than carrying dead weight)
   argues the same instinct applies here: don't build infrastructure a host almost always already has.
3. **Made strictly less necessary by §1's fact, not more.** ADR-021 treated "own TLS+auth" and "own
   the listener" as one coupled decision. They are not: the dispatch layer already doesn't care who
   parsed the HTTP request or terminated TLS. Choosing "first-party listener" was choosing to build
   the *one* part of this whole path that every comparable SDK deliberately does NOT build, on top of
   a dispatch layer that never needed it built.

Not rejected: ADR-021's real, Judged bearer-token mechanism (`trust/bearer_token.hpp`/`hmac.hpp`) —
see §4.

## 3. The accepted design: host-pluggable transport

**AgentEngine does not build a first-party network listener, TLS server, or HTTP/1.1 parser as core
product code, for v1 or any currently-planned milestone.** Instead:

### 3a. A transport-agnostic request/response boundary (named target shape, not yet plumbed — see §5 finding 1)

A minimal type pair — conceptually `InboundTransportRequest{method, path, headers, body, Principal
principal}` / `InboundTransportResponse{status, headers, body}` — that any host-supplied transport
(the embedding application's own web framework, a bespoke listener, or a thin adapter in front of a
reverse proxy) constructs and hands to `McpServer::dispatch()` / `A2aServer` / the AG-UI SSE framing
functions. **This is not built yet.** The red-team's first finding (§5) is that today's real
`dispatch()`/`send_message()` signatures do not take a per-call `Principal`/`CapabilitySet` at all —
`McpServer` binds its `CapabilitySet const&` once, at construction (`server.hpp:140-144`). Wiring
`InboundTransportRequest` through to these entry points is real, named follow-on implementation work,
not something this ADR's own text should claim is already true.

### 3b. Session-scoped, not per-call, Principal binding — with an explicit lifetime contract

The original draft of this design (this ADR's own first pass, before red-team) proposed binding a
`Principal` per individual dispatch call. **The red-team found this both unnecessary and dangerous**:
MCP is itself a session-oriented protocol (one authenticated connection, many `tools/call` requests
against it), and the naive implementation of "per-call Principal" — constructing a fresh `McpServer`
per HTTP request — collides with a real, already-documented constraint: `handle_tools_call_as_task`
(`server.hpp:270-313`) captures `this` non-owning in a completion lambda that fires later from a
**detached background thread** (`server.hpp:266-269`'s own comment: *"this `McpServer` must outlive
every task it starts"*). A per-request server destructs the moment `dispatch()` returns; a
backgrounded (`tasks/call`) completion then fires into freed memory. This is ADR-021 §6's
request-smuggling-as-forgery hazard reborn as an object-lifetime bug — same root cause (identity/state
bound to something shorter-lived than the work it authorized), different surface.

**Corrected design**: one dispatcher instance (`McpServer`/`A2aServer`) per *authenticated session*,
constructed by the host immediately after IT establishes who the caller is (however it chooses — its
own OAuth middleware, mTLS at its own reverse proxy, or by calling AgentEngine's own
`trust::verify_bearer_token()` directly if it wants to reuse that already-Judged mechanism), held via
shared ownership (e.g. `std::shared_ptr<McpServer>` captured by the host's own connection object) for
at least as long as the session's underlying connection lives — which, for a real SSE/streaming
session, is naturally at least as long as any task it started (the client needs the connection to
observe that task's outcome anyway). **This is a hard requirement on every host-pluggable transport
adapter, not implementer judgment** — matching this project's stated preference for
"unforgeable/impossible by construction over correct by convention" (ADR-022 §7's own precedent)
wherever construction-level enforcement is actually reachable; here it is not fully reachable in C++
without a real ownership-tracking type, so it is recorded as a **binding contract**, the same category
ADR-021 §6 used for its own "algorithm/key pinning is permanent" finding.

### 3c. The Principal bridge — real code, this pass

The red-team's fourth finding: nothing bridges a verified `trust::BearerTokenClaims` to a `Principal`
— `Principal` (`trust/principal.hpp:26-46`) is a plain, all-public-field aggregate, so every host
adapter wanting to reuse AgentEngine's own bearer-token mechanism would otherwise hand-roll its own
claims→Principal mapping, inconsistently, with no guardrail against e.g. a host accidentally
constructing an over-privileged `Principal` instead of deriving one from real verified claims.

**Built and proven this pass**: `trust::principal_from_bearer_claims(BearerTokenClaims const&,
principal_kind = principal_kind::service)` (`include/agentengine/trust/bearer_token.hpp`). Takes
*only* a successful `verify_bearer_token()` return value — there is no call path into it that skips
signature/`exp`/`aud`/`iss`/replay verification. `kind` is a caller-supplied parameter (route/endpoint
configuration), never read from the token itself, for the same reason `expected_aud`/`expected_iss`
are caller-supplied to `verify_bearer_token()`: a bearer token asserts *who*, never *what kind of
caller*, and letting token content assert `kind` would let anything mintable off one key claim a
stronger kind than the issuing host intended for that subject. Proven in
`tests/test_principal_from_bearer_claims.cpp` (9 checks, all passing): `sub`/`tenant_id` map correctly
(P1), the `kind` override is honored and never silently ignored (P2), no claims field beyond
`sub`/`tenant_id`/the explicit `kind` leaks into the resulting `Principal` (P3, a regression guard
against a future `Principal` field addition silently starting to leak token-internal claims), and
distinct subjects never collide (P4).

### 3d. A2A's own tenant-scoping gap — named, not closed this pass

The red-team's third finding, independent of the transport question but exposed by it: `A2aServer`'s
own file comment (`include/agentengine/protocol/a2a/server.hpp:149-152`) already states plainly
*"there is no principal/authorization boundary in this transport-agnostic dispatcher yet"* —
`get_task`/`cancel_task` key purely on `task_id` (high-entropy, `generate_task_id()`,
`a2a/server.hpp:94-101`, so not guessable — but nothing here checks the caller's `Principal` against
the task's owner even when a real, legitimate `Principal` is available). In a single-tenant embedded
deployment this is inert (018 §1's "Embedded/in-process" row already trusts the host completely). In a
host-pluggable, potentially multi-tenant deployment, this becomes a real gap the moment more than one
`Principal` can reach the same `A2aServer` instance. **Not closed here** — closing it needs
`principal_admitted_for()` (`trust/principal.hpp:112-116`, already real and proven for exactly this
"is this caller allowed to touch this owner's thing" shape) wired into `get_task`/`cancel_task`,
follow-on work scoped the same way ADR-021 §7 scoped out its own listener/TLS/parser work.

### 3e. Reference transport adapter, explicitly scoped as example, not hardened core

AgentEngine MAY ship one reference/example transport adapter — built on its own already-vendored
`agentengine::pal` sockets + mbedTLS (ADR-013/037), living under `examples/`, never
`include/agentengine/core` or `include/agentengine/protocol` — demonstrating the boundary actually
works end to end, matching how MAF ships a sample ASP.NET host rather than claiming Kestrel is MAF's
own code. This directly answers the one deployment-shape question the red-team's brief raised but did
not itself resolve: an operator who wants "just run a server, no separate host application" (018 §1's
"HTTP / AG-UI / OpenAI-compatible" row) is not left with nothing — they get a real, working reference
adapter, clearly labeled with its own narrower hardening scope, that they may point straight at their
deployment if they accept that scope, or replace with their own hardened transport if they don't.
Nothing about "host-pluggable" removes AgentEngine's ability to *also* ship one working example.

**Named residual the red-team flagged and this ADR does not resolve**: the moment Milestone 7's
conformance suites (`conformance server`/`conformance client`, `a2a-tck`, the AG-UI compatibility
suite) need SOME transport to run against, there is real pressure for "the example" to quietly become
"the de facto production path" — this project already found exactly this drift class once
(`decisions/ADR-026-milestone-status-doc-accuracy-and-drift-lint.md`, docs claiming more than code
delivers). Mitigation named, not yet built: M7's conformance suites must be written to run against
*any* conforming `InboundTransportRequest`/`InboundTransportResponse` adapter, not hardcoded to the
reference one, so a future hardened production adapter can swap in without re-litigating what "passes
M7" means — a real design constraint on the conformance-test harness itself, follow-on work.

## 4. What survives from ADR-021 unchanged

`trust/bearer_token.hpp`/`trust/hmac.hpp` (ADR-021's own real, Judged prove-phase deliverable) are
**not** rejected or replaced — they survive as exactly what §3b/§3c describe: a reusable primitive a
host adapter *may* call to authenticate a caller and derive a `Principal`, not as something
AgentEngine's own dispatch layer invokes internally (which it never did — `McpServer`/`A2aServer` have
never read an `Authorization` header themselves). ADR-021 §6's two binding constraints (algorithm/key
pinning is permanent; identity must be bound to a per-request signed value, never a connection alone)
both carry forward, restated in §3b's session-scoping contract as the concrete shape they take once
there is no first-party listener for them to apply to.

## 5. Red-team findings (independent adversarial pass, fresh context)

Reported verdict-by-verdict; findings 1–4 are addressed above (§3a design correction, §3b design
correction + binding contract, §3c real closable code, §3d named residual):

1. **FATAL** — the per-call `Principal` this ADR's own first draft sketched does not exist on the real
   `dispatch()`/`send_message()` signatures; closed by design correction (§3a/§3b), not by pretending
   the plumbing already exists.
2. **FATAL** — the naive fix for #1 ("construct a fresh dispatcher per request") creates a concrete
   use-after-free against `McpServer`'s own documented backgrounded-task lifetime contract; closed by
   the session-scoped, shared-ownership binding contract (§3b).
3. **MUST-FIX** — `A2aServer` has zero principal/tenant scoping today; named residual (§3d), not closed
   this pass.
4. **MUST-FIX** — no bridge from verified claims to `Principal` existed, so every host adapter would
   hand-roll one inconsistently; closed with real, proven code (§3c).
5. Direct answer to this ADR's own pre-red-team question ("does per-request `Principal` structurally
   prevent ADR-021's smuggling-as-forgery class?"): **not yet** — the mechanism wasn't wired in at all
   (finding 1), so the question was moot rather than answered; §3b's session-scoped contract is the
   actual, corrected answer, and closing it fully still depends on §3a's follow-on plumbing work
   existing for real.
6. **RESIDUAL** — the example-vs-hardened-adapter boundary is real but not self-enforcing; named in
   §3e with a concrete mitigation direction (transport-agnostic conformance harness), not yet built.
7. **CONFIRMED-CLEAN** — `Principal`'s own delegation/tenant logic (`derive_on_behalf_of`,
   `principal_admitted_for`, `kMaxDelegationDepth`) is fail-closed and attenuation-safe as a value
   type, independent of every wiring gap above; nothing in this ADR's design touches or weakens it.

## 6. Files changed this pass

- `include/agentengine/trust/bearer_token.hpp` — `principal_from_bearer_claims()` (§3c), plus the new
  `#include "agentengine/trust/principal.hpp"`.
- `tests/test_principal_from_bearer_claims.cpp` (new, 9 checks) — this ADR's §3c evidence.
- `tests/CMakeLists.txt` — registers the new test target (same `WIN32`-gated block as
  `test_bearer_token_proof`, since both transitively depend on `trust/hmac.hpp`'s current
  BCrypt-backed implementation).

Verified: the new test passes in isolation (9/9), and the two sibling tests in the same build block
(`test_bearer_token_proof`, `test_capability_token_proof`) were rebuilt and re-run to confirm the new
`#include` in `bearer_token.hpp` introduces no regression — both still pass in full.

## 7. What this ADR does NOT decide, left as follow-on work

- Threading `InboundTransportRequest`/`InboundTransportResponse` through `McpServer::dispatch()` /
  `A2aServer` / the AG-UI SSE path for real (§3a) — the single largest remaining piece.
- `A2aServer`'s tenant-scoping fix (§3d).
- The reference transport adapter itself (§3e) — not built this pass; scoped and named only.
- The transport-agnostic conformance-harness constraint (§3e's mitigation for the example-vs-hardened
  drift risk) — a real design requirement on Milestone 7's own test infrastructure, not yet designed
  in detail.
- Server-role TLS and HTTP/1.1 request parsing, if and when the reference adapter is actually built —
  ADR-021 §7's own residuals, unaffected in kind by this ADR, just now scoped to an `examples/`
  deliverable instead of core product code.
