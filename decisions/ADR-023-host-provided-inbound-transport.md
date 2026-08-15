# ADR-023 — Host-provided inbound transport: who authenticates an inbound MCP/A2A/AG-UI request?

**Status:** Red-teamed (2026-08-15), not yet proven or judged. Supersedes ADR-022 in effect (the
reactor question is moot if no first-party listener is ever built) and re-scopes ADR-021. Two
independent adversarial passes returned 27 findings (§7), four of which are **live security defects
in shipped M7 code** rather than design problems — currently latent only because no inbound transport
exists to reach them, which is precisely what this ADR proposes to add.

## 0. What changed, and why this ADR exists

ADR-021 asked whether AgentEngine terminates TLS+OAuth itself or defers to a reverse proxy, and
accepted Design A (first-party termination). ADR-022 then chose the reactor model for the listener
that Design A implies. **Both assumed AgentEngine ships a listening socket.**

Project-owner direction (2026-08-15) removes that assumption: **AgentEngine will not implement
HTTP networking at all.** The engine exposes a protocol-handler API; consumer code owns the socket,
TLS, HTTP framing, and routing — the shape Microsoft Agent Framework takes on .NET, where the
framework supplies protocol handlers and the application's own web host supplies the server.

This is not a small re-scope. It moves the **trust boundary** — the one thing ADR-021 existed to
place — from a socket AgentEngine controls to an API call AgentEngine merely receives. Where that
boundary lands is this ADR's question, and the RFCs do not already answer it: 018 §1's inbound-identity
table has *both* a "host-supplied principal; the host is trusted" row (Embedded / in-process) and
three rows that mandate real token validation (HTTP/AG-UI, A2A, MCP). A host-fronted HTTP request is
simultaneously arriving over HTTP and being handed to us by a host. Which row governs is undecided,
and picking wrong is expensive in both directions.

## 1. The question

**When a consumer-supplied host hands AgentEngine an already-parsed inbound protocol request, does
the engine derive the request's `Principal` by validating credentials itself, or does it accept a
`Principal` the host asserts?**

This has a wrong answer in both directions.

Get it wrong in the "trust the host" direction and 011 §8a's own MUST — *"the user id comes from the
verified token, **never from the client**"* — becomes something AgentEngine structurally cannot
enforce and merely documents. Every adopter who writes a thin HTTP adapter (the exact shape this
decision encourages) becomes the sole thing standing between an attacker-controlled header and a
cross-tenant identity forgery, with the engine unable to detect it. 018 §7 G4 already classes
cross-tenant identity leakage as a release-blocking defect class.

Get it wrong in the "engine authenticates everything" direction and AgentEngine is unusable inside
the applications it is trying to embed in. A host that has already authenticated its user — an
existing service with OIDC, session cookies, mTLS, or an API gateway in front — would have to
**re-mint an AgentEngine-format token per request** to tell the engine who the caller is, because
ADR-021 proved exactly one scheme (self-issued HMAC-SHA256 with `exp`/`aud`/`iss`) and explicitly did
*not* build OAuth 2.1 validation against a live external Authorization Server. Making a correctly-
authenticated host jump through a token-minting hoop to express a fact it already knows is the kind
of friction that gets bypassed, and a bypassed control protects nothing.

## 2. What already exists (2026-08-15 inventory)

- **Both server dispatchers are already transport-agnostic**, by deliberate design, and both already
  name this ADR's gap in their own headers:
  - `protocol/mcp/server.hpp:1-30` — *"NO real transport yet... this is a pure, transport-agnostic
    REQUEST DISPATCHER: hand it a `JsonRpcRequest`, get a `JsonRpcResponse` back, exactly the shape a
    later transport adapter wires bytes in and out of"*, and *"Authorization/principal establishment
    (011 §4)... is TRANSPORT work — a real inbound connection is what carries the credentials a
    principal is established FROM."*
  - `protocol/a2a/server.hpp:138` — *"there is no principal/authorization boundary in this
    transport-agnostic dispatcher yet."*
- **A load-bearing structural problem this ADR must fix regardless of which design wins.**
  `McpServer` takes its authority at **construction**, not per request:
  `McpServer(ToolTable const&, CapabilitySet const& held, ApprovalDecider, ...)` with
  `CapabilitySet const& held_` as a member (`server.hpp:142`, `:371`), consumed by
  `invoke_tool(table_, held_, ...)` (`:246`, `:286`). One `McpServer` therefore serves every inbound
  request under **one** capability set. That is per-*server*, effectively per-*connection*, authority
  — and ADR-021 §8 carries forward, as a binding constraint on any future implementation,
  *"per-request — never per-connection — token validation."* Whatever this ADR decides, `dispatch()`
  must take the request's own principal/authority as a parameter. This is not a detail; it is the
  difference between the constraint holding and being quietly violated on the first real host.
- **`Principal` has no credential-bearing fields yet.** `trust/principal.hpp:18-24` states plainly
  that 007 §2's `claims`/`issuer`/`expiry` are *"token-bearing-surface concepts (HTTP bearer/OIDC,
  018 §1) that need 013 (M7) to have an inbound surface at all — named deferred, not silently
  dropped."* This ADR is that surface arriving; the deferral's condition is now met.
- **`make_embedded_principal()` already exists** (`principal.hpp:49`) and is explicitly 018 §1's
  *"Embedded / in-process — Host-supplied principal; the host is trusted (007 §1)"* row. A
  host-asserted principal is therefore **not** a new concept in this codebase — the question is
  whether an HTTP-fronted host gets to use that row.
- **ADR-021's cryptographic mechanism is real and proven**: `trust/bearer_token.hpp` /
  `bearer_token.cpp` (mint/verify, algorithm pinned by construction, `aud`/`iss` from caller config
  never token content, `exp`-pruned `ReplayGuard`), `trust/hmac.hpp` (shared, previously-audited
  HMAC-SHA256 + constant-time compare), `tests/test_bearer_token_proof.cpp`, 32/32 checks. ~~Critically,
  **it operates on token bytes and needs no socket** — it survives this re-scope entirely intact.~~
  **Struck, and corrected by §7 (R5, R6): both halves of that sentence are wrong.** It does not
  operate on bytes — `BearerToken` holds already-parsed claims, no `Authorization` header decoder
  exists anywhere in the tree, and the type has zero consumers outside its own test. And it does not
  survive intact — its unconditional `ReplayGuard` makes a valid token single-use, which is
  incompatible with the per-request validation ADR-021 §8 itself makes binding. This was the design
  phase's most consequential factual error and is left visible rather than edited away.
- **`Principal → CapabilitySet` derivation does not exist.** ADR-021 §7 named it a residual
  ("007 §5's policy engine, already named elsewhere in this project as deferred pending a real rule
  language"). It is still absent, and every design below terminates in it.
- **`quark::pal::tcp_listen`/`accept_one` is available and already used as loopback test servers** in
  10+ tests (`test_https_egress.cpp:203`, `test_provider_http_client.cpp`, and others). Relevant not
  as shipped surface but because the promotion gates need *some* socket (§6 below).
- **013 §4 already names in-process as a first-class transport**: *"In-process — for embedded hosts —
  no serialization on the local path"* (`013:230`). The declarative surfaces (AG-UI SSE framing,
  Phase E3) are byte-producing and transport-independent already.

### 2a. Where the RFCs conflict with the new direction

Named now so the judge phase has an explicit edit list rather than discovering these later:

| Spec | Text | Conflict |
|---|---|---|
| 011 §7 | "**Streamable HTTP** is the transport" | Engine implements the *semantics*; the host carries the bytes |
| 011 §8a | "**Origin validation and localhost binding** for our HTTP surface (403 on invalid `Origin`)" | Origin validation is header-based and stays in-engine; **localhost binding is a socket property the engine no longer owns** and must become a stated host obligation |
| 011 §10 G1/G2 | `conformance server --url http://localhost:3000/mcp` | Gate needs a URL; a host must exist to run it against (§6) |
| 012 §8 G1 | "`a2a-tck` passes **against our server**" | Same |
| 020 §3 | "**Standalone server** — one process exposing the protocol surfaces over HTTP" | AgentEngine alone no longer satisfies this row |
| 020 §4 | "The admin API is **never on the same listener** as the public surfaces by default" | Becomes a host obligation the engine cannot enforce |

**Precedent that this direction is consistent with the project, not a reversal:** 012 §9 Q1 resolved
the gRPC binding to a deployment-side proxy on exactly this reasoning; the `remote` sandbox profile's
locked decision reasons identically ("push to infrastructure that already provides the property");
and CONVENTIONS' heavy-dependency rule already argues against vendoring an HTTP server.

## 3. Competing designs

All four share the same transport-fact carrier — the host's observations, which are **evidence, never
authority**:

```cpp
struct TransportFacts {
    std::string_view peer_address;          // audit + rebinding heuristics, never authorization
    bool             tls_terminated_by_host;
    std::string_view sni;
};

struct InboundRequest {
    HttpMethod                  method;
    std::string_view            target;     // path + query
    HeaderView                  headers;    // engine reads Origin / Authorization / Mcp-* itself
    std::span<const std::byte>  body;
    TransportFacts              transport;
    // ...and, per design, EITHER nothing more (A), a Principal (B), or a signed assertion (C).
};
```

### Design A — Credential forwarding (host is pure transport)

The host passes the raw `Authorization` header through untouched. The engine calls
`verify_bearer_token()` (ADR-021, unchanged) with route-pinned `expected_aud`/`expected_iss`, derives
`Principal` from the verified claims, and only then dispatches. `make_embedded_principal()` is
**unreachable** from this path. 011 §8a's MUST holds by construction: the engine never reads an
identity the host chose.

Steelman: this is the only design where the engine's own conformance claim is self-supporting. It
also degrades gracefully — a host that does nothing but move bytes is a *correct* host, so the
easiest thing to build is also the safe thing, which is the property that actually determines what
adopters ship.

Cost, stated honestly: a host that already authenticated must re-express that as an AgentEngine
bearer token. For a host with its own OIDC/session/mTLS edge this is real, recurring friction, and
the engine has no external-AS validator to offer instead (ADR-021 §3 scoped that out explicitly).

### Design B — Asserted principal (host inside the TCB)

`InboundRequest` carries a `Principal` the host constructed; the engine trusts it, exactly as
`make_embedded_principal()` already does. This extends 018 §1's Embedded row to cover host-fronted
HTTP.

Steelman: it is honest about where the security boundary *actually* is. An in-process host shares
AgentEngine's address space — it can already reach any capability set, forge any struct, and patch
any check. Pretending the engine can defend against its own linker-mate is theatre, and theatre that
costs adopters real friction. This is also, per the project owner's own framing, what the .NET
ecosystem does: the web host authenticates, the framework consumes an established identity.

Cost: 011 §8a's MUST becomes undischargeable by the engine. Worse, the *conformance* claim built on
it (011 §10's published percentage) would be reporting on a control the engine no longer implements.

### Design C — Signed assertion (host asserts, engine verifies)

The host asserts a `Principal` **and** signs the assertion — over `{principal, method, target,
body-digest, nonce, exp}` — with a key provisioned out of band, reusing `trust/hmac.hpp`. The engine
verifies before accepting.

This is ADR-021's rejected Design B (signed forwarded-principal header) resurrected — and the
resurrection is not idle. ADR-021 killed it on **HTTP request smuggling as a principal-forgery
primitive**: a smuggled request desynchronizing proxy and engine framing over a *pooled backend
connection* inherits another caller's trusted header. That attack **cannot exist here**: an
in-process host hands over one already-parsed request object, so there is no shared connection, no
pipelining, and no framing ambiguity to desynchronize. The specific finding that sank Design B in
ADR-021 does not transfer to the in-process case, which is why this is a new question rather than a
re-litigation.

Cost, and the likely fatal one: against an **in-process** host the signature proves nothing — a host
that can call the API can also read the signing key out of its own process memory. Design C's crypto
is only load-bearing when the host is **out-of-process** (020 §3's Sidecar row, local IPC).

### Design D — Typed dual boundary

A and B both exist, distinguished by **type**, not configuration:

```cpp
Principal authenticate(TrustedHostRequest const&);   // asserted; host is in the TCB
result<Principal> authenticate(UntrustedTransportRequest const&);  // engine verifies credentials
```

A deployment cannot slide from one posture to the other by flipping a config flag, because the two
postures are different types with different call sites and different return types (the trusting one
cannot fail; the verifying one is fallible and must be handled). Design C's signed assertion is what
an out-of-process `TrustedHost` uses to earn its `TrustedHostRequest` across the IPC boundary.

Steelman: this is this project's own established idiom — ADR-009 chose parameterized capability kinds
over an epoch counter, and ADR-022 chose Design B specifically because *"this project has an
established preference for 'unforgeable/impossible by construction' over 'correct by convention'"*.
It also matches the RFC as written: 018 §1 genuinely has two rows with two mechanisms, and modelling
one API over both is what erases the distinction the table is making.

Cost: two entry points is more surface, and the failure mode is a host that reaches for the trusting
type because it compiles with less ceremony — which is Design B's hazard wearing a type.

## 4. Falsifiable claims

| # | Design | Claim | Disproving experiment |
|---|---|---|---|
| 1 | A, D | A host CANNOT inject an identity: no code path from an `UntrustedTransportRequest` reaches a `Principal` whose `id`/`tenant_id` was chosen by the host rather than derived from verified claims | Negative suite: host supplies `X-Principal`-shaped headers, a well-formed but unsigned identity claim, and a valid token for tenant X alongside a header naming tenant Y — the derived principal must be tenant X's or a rejection, never Y's |
| 2 | A, D | The **positive control**: a genuinely valid token yields the correct principal and a *successful* dispatch — the path can succeed, so its rejections mean something | Valid token → `tools/call` returns a real result (022 §5's mandatory positive control) |
| 3 | all | Authority is **per-request**, not per-server: two requests bearing two different principals against the SAME server object execute under two different capability sets | Construct one `McpServer`, dispatch two requests with different principals, assert the second cannot invoke a tool the first could (this fails against today's `held_` member — it is the §2 structural defect, stated as a test) |
| 4 | all | A request bearing NO credential is admitted only as `make_anonymous_principal()`, never fails open to a wider set (018 §1's "Anonymous is a principal, not a bypass") | Omit `Authorization` entirely; assert the derived principal is anonymous and that a capability-requiring tool is refused |
| 5 | all | `TransportFacts` is inert for authorization: mutating `peer_address`/`sni`/`tls_terminated_by_host` arbitrarily changes no authorization outcome | Same request, adversarially varied transport facts, identical authorization result each time |
| 6 | C | A tampered assertion (any field of `{principal, method, target, body-digest, nonce, exp}` mutated independently) is rejected | Per-field mutation suite, each with the correct `failure_class` |
| 7 | C | The signed assertion binds the REQUEST, not just the identity — an assertion captured from request 1 cannot authorize request 2 | Replay a valid assertion against a different `target`/body; must reject |
| 8 | D | The two postures cannot be confused at a call site: passing a `TrustedHostRequest` where credentials are required (or vice versa) fails to **compile** | `try_compile()` gate, matching the 007 §9 G1/G2 and ADR-012 precedent |
| 9 | A, B | Cost: engine-side validation per request is not a hot-path regression versus the asserted path (Design A's claimed cost, measured rather than asserted) | p50/p99 over N requests for both paths on the same harness |
| 10 | all | The credential value is never printable: a token/assertion cannot reach a log, error string, or telemetry payload by construction (018 §4) | Attempt to format/serialize the carrier type; must not compile or must emit `***` |

## 5. Red-team brief (next phase)

Hardest where each design claims to be safe:

- **Design A**: is the `Origin` check genuinely in-engine, or does it silently depend on a host
  obligation (011 §8a pairs it with localhost binding, which the engine has definitively lost)? Can a
  host smuggle an identity through a channel the design does not model as a credential —
  `TransportFacts`, a header the engine reads for a *different* purpose, a `target` path segment, MCP
  `requestState` (011 §8a already calls it attacker-controlled), or an A2A task id used as a lookup
  key? Does route-pinned `expected_aud` survive contact with a host that mounts the engine at an
  arbitrary path it, not the engine, chose?
- **Design B**: does trusting the host *only* widen 007 §1's stated threat model (a malicious
  operator is an explicit non-goal) or does it also cover a **compromised** host, which is not the
  same thing — the exact distinction ADR-021 §5 drew for its own Design B? What does 011 §10's
  published conformance percentage honestly say about a MUST the engine no longer implements?
- **Design C**: beyond the in-process futility already conceded — nonce/replay state across a
  restarted or horizontally-scaled engine (ADR-021's `ReplayGuard` single-instance limitation
  transfers directly); is the body-digest computed over the same bytes the dispatcher later parses,
  or is there a re-read/TOCTOU seam between verification and use?
- **Design D**: what stops the trusting type from becoming the default by ergonomics? Is there a
  path where a `TrustedHostRequest` is *constructed from* an `UntrustedTransportRequest` — the
  laundering move that would collapse D into B?
- **All**: `McpServer`'s construction-time `held_` (§2) — is fixing it to per-request sufficient, or
  does the same per-connection-authority defect exist in `A2aServer`, the AG-UI projection, and the
  streaming path, where a long-lived SSE stream outlives the request that authorized it? **A stream
  is the sharpest case: what revokes an in-flight SSE stream when the principal's authority is
  revoked or its token expires mid-stream?** No claim above covers it.
- **All**: DoS shape moves to the host (good) but resource *accounting* does not — what stops a host
  from opening unbounded concurrent runs, and is that I8 (budgets are enforced) or the host's problem?

## 6. The gate consequence, stated up front

**This decision does not remove the need for a socket; it moves it out of the shipped engine.**
011 §10 G1/G2 run `conformance server --url http://localhost:3000/mcp`, and 012 §8 G1 runs `a2a-tck`
against a served endpoint. Both need a real listening URL.

The proposal is a **test-fixture host** under `tests/` over `quark::pal::tcp_listen` — the primitive
already used as a loopback server in 10+ existing tests — explicitly marked as a fixture and not
shipped surface, so no adopter mistakes it for a supported deployment. This is far cheaper than a
production listener and commits the project to nothing, but it must be built for M7's exit criterion
to be reachable at all, and saying so now is the honest accounting the milestone's Phase G audit
already asked for.

## 7. Red-team findings

Two independent adversarial passes (fresh context, no exposure to this document's author's
reasoning) ran against §4's claims and §5's brief, with disjoint lenses: one on the trust boundary
and its cryptography, one on codebase integration and conformance honesty. They returned 27 findings
between them and overlapped on three, independently, from different directions — the overlaps are
marked. §4's claims table covers 3 of the 27.

Every code-level finding reproduced below was **re-verified by hand against the source** before being
recorded here; the citations are this author's own confirmation, not the passes' unchecked report.

### 7a. Shipped defects, independent of this ADR — and the reason they are latent

Four findings are not about the proposed designs at all. They are live defects in code committed
during M7 Phases C and D, and they are severe.

**R1 (both passes, independently) — A2A task ids are structured and guessable, and `GetTask` has no
principal check. Unauthenticated cross-principal read of full conversation history.**
`A2aServer::send_message` sets `t.id = outcome->run_id` (`protocol/a2a/server.hpp:127`), and
`run_id` is minted as `session_id_ + ":run:" + std::to_string(run_counter_)`
(`core/agent_session.hpp:319`) — so A2A task ids are literally `"<session-id>:run:1"`, `":run:2"`, …
`get_task()` and `cancel_task()` take a bare `std::string const& task_id` and do a plain
`tasks_.find()` with no principal parameter at all (`:141-149`, `:155-166`). `Task::history` carries
both the caller's inbound message and the agent's full response (`:131-132`). Anyone who knows or
guesses a session id enumerates every run and reads both sides of the conversation. 011 §8a's MUST is
explicit and directly contradicted: *"we MUST NOT treat possession of a server-minted handle (or a
task id) as authenticating anyone; handles are high-entropy, expiring, and bound server-side as
`<user_id>:<handle>`."* None of the three properties holds. This is 018 §7 G4's release-blocking
cross-tenant-leak class.

**R2 — every inbound A2A message bypasses 018 §2 admission.** `StartRun::caller` is
`std::optional<SessionCaller> caller = std::nullopt` (`core/agent_session.hpp:127`), and the handler
reads `if (m.query.caller.has_value() && !principal_admitted_for(...))` (`:280`) — `nullopt` skips
the check entirely, as the code's own comment states (`:274`). `A2aServer::send_message` constructs
`starter_(StartRun{std::move(input)})` (`protocol/a2a/server.hpp:102`) — **`caller` unset**. The
default was a deliberate, documented back-compat choice for ~44 pre-existing test call sites
(`:120-127`), which is defensible for tests and is a fail-open the moment a protocol surface uses it.
The failure shape is an omission that compiles — precisely what a host adapter will reproduce.

**R3 — MCP's task store has the same missing principal check.** `handle_tasks_get` /
`handle_tasks_cancel` (`protocol/mcp/server.hpp:315-363`) are `tasks_.find(task_id)` with no
principal, returning the full `ToolResult` content. Ids here are at least unstructured — but they come
from `std::mt19937_64` seeded from one `random_device` draw (`:120-127`), which is not a CSPRNG and is
state-recoverable from its own output. `src/trust/bearer_token.cpp:63` already reaches a real CSPRNG.

**R4 — `IdempotencyKey` has no identity dimension.** It is `{run_id, turn_index, call_index,
argument_digest}` (`core/tool_pipeline.hpp:146-166`), and `McpServer` default-constructs
`EffectContext ctx` (`protocol/mcp/server.hpp:245`, `:284`), leaving `run_id` empty. Every inbound MCP
`tools/call` with identical arguments therefore derives the identical key `":0:0:<digest>"` across
every caller and every tenant, so a journal deduping on it can serve principal B a result computed for
principal A. The digest is 64-bit FNV-1a (`:136-144`) over attacker-supplied JSON — collisions are
constructible by hand.

**Why these are latent today, and why that matters here.** Nothing outside the process can reach any
of them, because there is no inbound transport. **The absence of a listener is currently the only
thing enforcing these boundaries.** ADR-023 is the work that removes that protection. They are
therefore not "pre-existing issues to file separately" — they are prerequisites of whatever this ADR
decides, and no design below is safe to ship ahead of them.

**Status of the R1-R4 remediation (2026-08-15), split by whether a spec change is required.**

Fixed, built, and proven by `tests/test_task_principal_binding.cpp` (19 checks, every negative case
paired with its own positive control per decisions/README.md's rule for security claims):

- **R1/R3 binding** — both task stores now record the establishing principal and check it on every
  lookup; a non-owner receives the byte-identical response an unknown id produces, compared against
  the *real* unknown-id response in the test rather than a hardcoded string so the two cannot drift
  apart silently. Ownership is checked *before* task state, so a stranger cannot learn a task exists
  from a state-specific error. Cross-tenant id collision is not ownership (018 §6), and two
  empty-id principals are not the same principal (R13).
- **R2** — `A2aServer::send_message` now requires a `SessionCaller` and passes it, so 018 §2
  admission actually runs on the A2A path. Deliberately no defaulted overload: a default would
  reintroduce the very fail-open being closed.
- **R3 entropy** — `trust/secure_random.hpp`, a header-only system CSPRNG (BCrypt / `getrandom`),
  now backs both generators, replacing the two independently-written `mt19937_64` copies. Header-only
  because routing it through the `if(WIN32)`-gated `src/trust/` would have made the MCP and A2A
  server surfaces Windows-only. Fails closed: a handle that cannot be generated securely is not
  generated.
- **R4/R26 identity half** — `EffectContext::principal` is populated on both MCP dispatch paths (it
  was default-constructed, so every inbound call ran as an empty `Principal{}`), and
  `ToolInvocationAudit` now carries `{principal_id, principal_tenant_id, principal_on_behalf_of}`,
  which 007 §8 requires and it did not have.

**Harness teeth verified**, per ADR-015's precedent of proving a detector by reintroducing the bug it
detects: neutering the ownership predicate turns 5 checks red while every positive control stays
green — confirming the failures are ownership decisions rather than a handler broken for everyone.

Deliberately NOT fixed here, because each requires a spec change this ADR must decide rather than a
drive-by edit:

- **R1's id enumerability.** 012 §1 and §5 assert `task_id` **is** `run_id` at spec level ("`task_id`
  already **is** `run_id` (§1)", 012 §5, relied on again at §7), and `run_id` must stay deterministic
  per 001 §7/I5 — so decoupling the public handle from the internal run identity is a 012 amendment.
  With binding in place, enumerability is defense-in-depth rather than the control, but it is still
  real and still owed.
- **R4's `IdempotencyKey` shape.** 019 §3 specifies the tuple as exactly
  `{run_id, turn_index, call_index, argument_digest}`. Adding a tenant/principal dimension — the only
  fix that preserves determinism *and* prevents cross-principal collision when `run_id` is empty — is
  a 019 amendment. The FNV-1a digest (`core/tool_pipeline.hpp`) is also non-cryptographic by
  deliberate choice ("019 names no collision-resistance requirement, only determinism"), which stops
  being defensible once the key is security-relevant across principals.
- **R16/R27's per-request authority.** `McpServer::held_` and `approve_` remain construction-time.
  Making them per-request requires per-request *owned* authority, because
  `EffectContext::capabilities` is a borrowed pointer whose safety argument holds only while
  authority is per-connection, and `background_task()` copies that context into a detached thread.
  `dispatch()` now takes a per-request `Principal`, which is what the binding fix needed; it
  deliberately does not pretend to have fixed authority selection.

### 7b. Findings that break the designs as scoped

**R5 (CONCEPT-FLAW, Design A and D's untrusted arm) — the proven bearer mechanism is incompatible
with ADR-021 §8's own "per-request, never per-connection" binding constraint. A bearer token is
single-use.** `verify_bearer_token()` calls `replay_guard.check_and_record(jti, exp, now)`
unconditionally on every otherwise-successful verification (`src/trust/bearer_token.cpp:130-134`), and
`check_and_record` returns `false` for any `jti` seen inside its `exp` window (`:92-95`). Under
per-request validation **a client's access token is burned on its first request**; request 2 fails
`bearer_token.replayed`. That is not RFC 6750 access-token behaviour — `jti`-single-use belongs to
one-shot assertions, not to the credential 018 §1 names. The two escapes are both bad: mint a fresh
token per HTTP request (the exact friction §1 says gets Design A bypassed — and the host would then
need the signing key, at which point it can mint any `sub`/`tenant_id` and **Design A is Design B with
extra steps**), or disable the replay guard and discard one of ADR-021's four proven findings.

**This directly disproves a claim in this ADR's own §2.** That section asserted the mechanism
*"survives this re-scope entirely intact."* It does not. ADR-021 never had a request loop to run it
in, and the re-scope is what makes the incompatibility visible.

**R6 (CONCEPT-FLAW, A and D) — there is no decoder, and the inventory overstated what exists.** §2
also said the mechanism *"operates on token bytes and needs no socket."* It does not operate on bytes:
`BearerToken` holds an already-parsed `BearerTokenClaims` (`trust/bearer_token.hpp:65-68`), and
`encode_claims()` is mint-side only (`src/trust/bearer_token.cpp:43-57`). Confirmed by grep: the only
files mentioning `BearerToken` anywhere in the tree are its own header, its own `.cpp`, and
`tests/test_bearer_token_proof.cpp` — **zero production consumers.** No `Authorization: Bearer <string>`
parser exists. The single highest-risk step in the chain — attacker-controlled bytes with
attacker-controlled `u32` length prefixes (`put_str`, `:38-41`) decoded into typed claims — is unbuilt
and was asserted as done.

**R7 (CONCEPT-FLAW, Design D) — D types the request; the laundering happens on the `Principal`, one
line later.** Both §3 signatures return the same type, and `Principal` is a plain aggregate with
public `std::string id` / `tenant_id`, a defaulted `operator==`, default construction and copy
assignment, and **no private construction** (`trust/principal.hpp:26-46`) — unlike 007 §3 property 4,
which requires exactly that discipline for capabilities. So the instant either arm returns, provenance
is gone: no downstream API can distinguish a token-derived principal from `Principal{"admin","t"}`
written by hand. **Claim 8's `try_compile()` gate passes while the design is broken**, because it
proves the two *request* types don't interconvert and says nothing about the value both produce — which
is the value 011 §8a's MUST is actually about. The asymmetry is worse than §3 conceded: the trusting
arm returns `Principal` (infallible), the verifying arm `result<Principal>` (must be handled), so
**D makes the unsafe path strictly cheaper to write.** The ADR-022 "unforgeable by construction"
precedent §3 invokes is claimed, not achieved.

**R8 (CONCEPT-FLAW, A and D) — `expected_aud` cannot be route-pinned by an engine that does not own
the route.** `trust/bearer_token.hpp:24-26` states the invariant: expected audience comes from static
per-route server configuration, *"never derived from the token or the surrounding request. A verifier
that instead read the expected audience from request content would be validating a claim against
itself."* Under this ADR the engine has no route — the host chose the mount path. Exactly two sources
remain, and both break something: static per-engine config collapses N logical resources onto one
audience (degrading RFC 8707's per-resource confused-deputy control, which 018 §1 calls "not
optional", to per-process), or the host supplies it per request — in which case the same party
supplies the token and the expected audience, audience validation is a tautology, and A collapses
toward B through a parameter §3 does not model as a credential. There is no third option: deriving it
from `target` is what the header explicitly forbids.

**R9 (CONCEPT-FLAW, B/C/D) — request smuggling's mechanism is gone; its effect is intact.** §3
justified resurrecting Design C by arguing that an already-parsed request object has "no framing
ambiguity to desynchronize." That conflates the primitive with the property it breaks. The property
ADR-021 §6 actually named is *identity bound to something the engine and the frontend interpret
differently* — and in-process, the desync simply moves from framing to **interpretation**. The host
routes and authorizes on `target` and the `Mcp-Method`/`Mcp-Name` headers (011 §7 makes these
MUST-carry, so an adapter reads them); `McpServer::dispatch()` branches **exclusively on `req.method`
parsed from the JSON-RPC body** (`protocol/mcp/server.hpp:148-156`), with the tool name from
`params["name"]` (`:206`). A host exposing `/mcp/readonly` to anonymous callers — the obvious shape
once the host owns routing — is bypassed by POSTing `{"method":"tools/call","params":{"name":"fs_write"}}`
with `Mcp-Method: tools/list`. Host says read-only; engine executes the write. This removes C's stated
reason for existing.

**R10 (CONCEPT-FLAW, A) — Design A is the only design that hands the engine a live upstream-usable
credential, which inverts §4's implicit A-is-safer framing.** 018 §1 and 011 §8a both make
no-token-passthrough a MUST. Design A puts the raw `Authorization` header into a printable
`HeaderView` reachable from the whole dispatch call tree, in the same process as
`protocol/mcp/client.hpp`, `protocol/a2a/client.hpp`, and every provider `ChatClient`. There is no
type-level barrier — 018 §7 G1 demands passthrough attempts be *rejected*, but with a plain string
view there is nothing to reject, only a convention to keep. **Design B never gives the engine the
token at all and is therefore structurally better than Design A on this particular MUST.** Neither
§3's steelmen nor §4 mentions this axis.

**R11 (CONCEPT-FLAW, C and D's out-of-process arm) — "the host is trusted" is about the host
*process*, not a peer process.** 007 §1 trusts *"the host process, its configuration, and first-party
native code."* For an in-process host that is tautological, and §3's Design B steelman is right about
it. But §3 extends the trusting type across an IPC boundary to 020 §3's Sidecar row — and a separate
process reached over IPC is not "the host process"; its compromise is a distinct, unmodelled event,
exactly the distinction ADR-021 §5 drew for its own proxy. Design D as written widens 007 §1's threat
model by reusing a sentence written about a different entity. Compounding it, the crypto cannot carry
the weight: `trust/hmac.hpp` is a shared-secret HMAC, so the engine can mint assertions
indistinguishable from the sidecar's (no non-repudiation, no attribution value for I4), and
`BearerSecretKey` is a bare 32-byte struct (`bearer_token.hpp:50-52`) with no key-id, no
multi-key acceptance window, and therefore no way to satisfy 018 §3's *"rotation without restart…
in-flight calls are not broken."*

**R12 (IMPLEMENTATION-HAZARD, A/C/D, on the one hot path the host cannot shield) —
`ReplayGuard::check_and_record` walks and prunes the entire map on every call, under a single global
mutex** (`src/trust/bearer_token.cpp:86-97`). Verification is O(N) in outstanding un-expired
credentials, fully serialized. In Design C, where the host mints a fresh signed assertion per request,
N grows with request rate × assertion lifetime, so the auth path goes quadratic and single-threaded on
well-formed traffic the host has no reason to rate-limit. §5 waved DoS off as "moves to the host"; this
part does not — it is engine-side, upstream of any I8 budget. Claim 9 measures p50/p99 on a clean
harness where N ≈ 0 and would report green. (Correctly ordered, though: the replay check runs *after*
signature/exp/aud/iss, so unsigned junk cannot pollute the map.)

**R13 (CONCEPT-FLAW/IMPL, all) — anonymous fail-open has two more variants beyond R2.**
`McpServer` default-constructs `EffectContext ctx` (`protocol/mcp/server.hpp:245`, `:284`), yielding
`Principal{}` with `id == ""` — **not** `make_anonymous_principal()`'s `id == "anonymous"`
(`trust/principal.hpp:70-76`). `principal_admitted_for` (`:112-116`) then admits any empty-id caller to
any empty-id-owned session, and in a single-tenant deployment (`:27`: "empty for single-tenant") the
tenant guard is vacuous — so the distinguishability property `make_anonymous_principal`'s own comment
claims (`:67-69`) is defeated by the default constructor sitting next to it. Separately,
`make_anonymous_principal(std::string tenant_id = {})` takes a tenant, and for an unauthenticated
request the only available source is the host or the request — letting an attacker select which
tenant's quota, memory scope, and sandbox (018 §6) an anonymous request lands in, without forging any
identity. Claim 4 tests neither.

### 7c. Findings that break the proposal's scope

**R14 (CONCEPT-FLAW, all) — the ADR defines a request type and no response type, so half the
conformance surface is unassigned.** §3 specifies `InboundRequest`/`TransportFacts` and no reply
carrier. What the engine produces today is `JsonRpcResponse` (`protocol/mcp/json_rpc.hpp:70`) — no HTTP
status, no headers — and `result<Task>`. So every obligation expressed as a status or response header
becomes the host's to invent: 401 + `WWW-Authenticate` (011 §6), **403 on invalid `Origin`** (011 §8a),
405, 202, content negotiation. §2a's table claims Origin validation "stays in-engine"; the *check* can,
the **403 cannot** — the engine computes a verdict it has no way to express. Worse, MCP's
request-scoped notification channel (011 §3.3: progress notifications *"flow on the response stream of
the request they belong to"*) has no slot at all — `McpProgressProjector` was built in Phase E4
(`protocol/mcp/progress.hpp:52-63`) and under this design **its output has nowhere to go**, since
`dispatch()` returns exactly one response. This is the interface question the re-scope actually poses,
and the ADR asked only the authentication one.

**R15 (CONCEPT-FLAW, all) — mid-stream revocation is not a manageable residual; the accepted
capability model structurally cannot express it, and fixing it reopens ADR-009.** ADR-009 accepted
per-invocation `bind()`/`revoke()` and **explicitly rejected the whole-set epoch counter**
(`decisions/ADR-009-capability-set-enforcement-mechanism.md:66-77`) — the one mechanism that revokes
authority already handed out, on exactly the macro events a token expiry belongs to. `CapabilitySet`
has no revoke, no epoch, no expiry. `Principal` has no `expiry`, and even the plan to add one (§2)
would be discarded at the conversion: `verify_bearer_token()` returns `BearerTokenClaims` carrying
`exp` (`bearer_token.hpp:61`), and nothing retains it. A run reads `effect_context_.principal` once at
`StartRun` (`core/agent_session.hpp:310`) and never re-checks. `ae::stream` does carry a stop signal
(`core/stream.hpp:132`, `:153-156`) but it flows consumer→producer only, for the consumer going away —
and `AgentSession` never reads it. **Honest statement: a stream authorized at t=0 executes under
authority no code path in this engine can withdraw before the run ends, by a prior judged decision.**

**R16 (promoted to CONCEPT-FLAW, all) — per-request authority converts a documented-safe borrowed
pointer into a use-after-free, and §2 under-scoped the fix by roughly the whole effect path.**
`EffectContext::capabilities` is a raw `CapabilitySet const*`, *"borrowed; never owned here"*
(`core/effect_context.hpp:18`); `AgentSession` holds `capabilities_` with the comment *"the host that
grants it owns it and must outlive the session"* (`core/agent_session.hpp:587-588`, `:318`); and
`background_task()` **copies `EffectContext` into a detached `std::thread`** resting its safety on that
same contract (`core/tool_pipeline.hpp:355-360`, `:424`). That argument holds only because authority is
per-connection today. Make it per-request and the natural implementation is a function-local value,
which dangles the moment `dispatch()` returns — while a detached background task (which also captures
`this`, `protocol/mcp/server.hpp:285-301`) and any open stream are still using it. **Claim 3 as written
passes cleanly against an implementation that dangles**, because two sequential requests never overlap.
Answering §5's question directly: no, a `CapabilitySet const&` member is not safe under per-request
authority, and neither is `EffectContext::capabilities` as a raw pointer.

**R17 (CONCEPT-FLAW, all) — §2's plan to enrich `Principal` is blocked by a locked external
constraint.** `StartRun` crosses Quark's fixed message pool: `MessagePool::kMaxPayload` is **192
bytes**, and `quark::Ask<StartRun, AgentResponse>` was **measured at 208 bytes with a full
`std::optional<Principal>` embedded — already over budget** before `Responder<R>`'s overhead
(`core/agent_session.hpp:92-97`). That is exactly why the two-string `SessionCaller` stand-in exists.
Quark is a submodule that CLAUDE.md forbids forking or patching in-tree, so the ceiling is not
negotiable. §2 proposed making `Principal` *bigger* (adding 007 §2's `claims`/`issuer`/`expiry`); that
plan is unbuildable at the actor boundary as stated. Consequences: 012 §4's *"an inbound request
carrying `on_behalf_of` produces a derived principal with attenuated authority"* is structurally
unreachable at the run boundary, since `SessionCaller` cannot express delegation by construction
(`:99-101`); and `AgentSessionRecord` round-trips only `principal_id`/`principal_tenant_id`, rebuilding
a restored principal with default `kind` (`anonymous`) and no delegation info (`:776-784`) — so a run
suspended for hours under 019 and resumed comes back with an unexpirable principal.

**R18 (CONCEPT-FLAW, all) — 011 §10 G3 (statelessness) is not merely blocked; the host-owned
transport actively falsifies it.** Both task stores are in-process members of one server object
(`protocol/mcp/server.hpp:375`, `protocol/a2a/server.hpp:179`), and `A2aServer::context_id_` is fixed
at construction (`:95-96`). Under this ADR the host owns routing and load balancing and the engine
cannot see it, so an adopter round-robining two engine instances — the normal HTTP deployment, and the
one G3 exists to test — gets `tasks/create` on replica 1 and `tasks/get` on replica 2 → spurious
"unknown taskId", undetectably.

**R19 (CONCEPT-FLAW) — the test-fixture host proves self-consistency, and 020 §7 G2 forces it to
become shipped surface.** Four problems with §6: (i) a percentage measured against a fixture written
by the engine's authors is a property of *engine + fixture*, and several named scenarios test the
fixture, not the engine — `dns-rebinding` most clearly, since 011 §8a pairs Origin validation with
localhost binding, which §2a concedes the engine has lost; (ii) **the fixture must contain the exact
request parser whose defects ADR-021 used to kill this ADR's Design C ancestor**, including chunked
transfer-encoding, which exists nowhere in this codebase — the shipped HTTP code is *"Content-Length-
framed only — no chunked transfer-encoding support, a documented cut"* and parses *responses*
(`sandbox/net_egress_proxy.hpp:106`); (iii) "not shipped surface" has no mechanism behind it —
`agentengine_core` is an INTERFACE target with no `install()` rules, so adopters consume this repo as
source and will copy the fixture; (iv) 020 §7 G2 ("the same agent runs unchanged in all five hosting
shapes") has nothing else that can play "standalone server," so the fixture is a shipped host with the
word "fixture" on it.

**R20 (CONCEPT-FLAW) — the conformance percentage becomes a claim about an artifact adopters will
not deploy, and the project already has the honest precedent it is not applying.** 011 §10 asserts
*"A claim of MCP support in this project means a number from this tool, not a paragraph."* Under this
ADR the engine no longer implements localhost binding, the Origin 403, OAuth resource-server 401
framing, 020 §4's admin-listener separation, or any connection/DoS limit — and the delta between
fixture and real adopter host is exactly the security-relevant clauses. The defensible shape already
exists in-repo: 012 §9 Q1 resolved gRPC with *"We do not claim gRPC conformance ourselves — an
operator fronting us with a transcoding proxy owns that interoperability claim, honestly outside what
our own TCK run can prove (021 §1's 'no claim' discipline)."* Applied here that means publishing a
**semantics-conformance number plus a numbered, normative host-obligation list**, not one number
called MCP conformance.

**R21 (CONCEPT-FLAW) — 020 §3's "the same binary serves all five" does not survive, and its collapse
silently reopens a resolved question §2a did not list.** 020 §8 Q2 (should the admin API exist
in-process at all) was resolved *in-process, same binary* on two stated premises: that §3's
"same binary serves all five" is a strong existing commitment, and that §4's separate-listener rule
already delivers the security property. **This ADR destroys the first and, by §2a's own admission,
converts the second into an unenforceable host obligation — both legs of a resolved decision are
gone.** Under a host-owned listener nothing prevents an adopter mounting `/admin` on the public
listener. The Cluster row has the matching problem: 020 §5 places sessions by HRW on `session_id`, and
the host now chooses which node a request lands on with no visibility into placement.

**R22 (IMPL + CONCEPT-FLAW) — backpressure: the engine can offer it and cannot claim it, and a
non-draining host pins a Quark worker thread.** `stream_producer::push()` blocks losslessly until the
ring has credit (`core/stream.hpp:84-89`) — the mechanism is real. But
`AgentSession::emit_run_event_for()` **discards the push outcome**:
`(void)run_event_producer_.push(std::move(ev));` (`core/agent_session.hpp:849-855`), so a `Terminated`
is silently ignored and the run keeps emitting into a dead ring — which, with R15, is the concrete
mechanism by which a disconnected client's run continues under unrevoked authority. And because
`push()` blocks inside a `quark::task<>` on a Quark worker, **a host that stops draining stalls a
scheduler thread**, making any slow HTTP client a lever on the shared scheduler. Conversely the
*easiest* host implementation — drain into a `std::deque` and let the web framework write it out —
defeats backpressure entirely, and the engine cannot detect it. So 013 §6 G2's "host memory stays flat"
measures the fixture and says nothing about an adopter.

**R23 (IMPL + CONCEPT-FLAW) — one producer per session: a second subscriber silently kills the
first.** `enable_event_stream()` overwrites the single `run_event_producer_` member
(`core/agent_session.hpp:528-533`), so two concurrent subscribers to one run are impossible and the
second call tears down the first consumer's ring — while 012 §2.3 makes it a MUST that *"events MUST
be broadcast identically and in the same order to every stream, and closing one MUST NOT affect the
others."* 013 §7 Q2 resolved this and it is not built; `A2aServer` has no `SubscribeToTask` at all.
This matters *more* under a host-owned transport, where N connections to one run is the normal case
and the failure is silent.

**R24 (IMPL, severe) — `InboundRequest` is entirely non-owning views, so §5's TOCTOU question is
answered structurally: the engine cannot know.** Every field in §3 is a view or a span into host-owned
memory. Design C verifies a digest at t₀ and the dispatcher parses at t₁, and the host — or its own
buffer recycling — may mutate in between, undetectably; the only sound fix is that the engine copies
the body before digesting and parses exclusively from the copy, stated as API contract, because "the
host promises not to mutate it" is precisely the category of unenforceable host obligation §2a already
conceded for localhost binding. Independently, any view reaching a background task, a stream, a
`Principal.id`, or an audit record is a dangling read once the host's request scope ends — and the host
has no reason to suspect it, having made a synchronous-looking call.

**R25 (CONCEPT-FLAW) — claim 5 contradicts §3's own prose, and both resolutions forfeit something.**
§3 documents `peer_address` as *"audit + rebinding heuristics, never authorization"*, but a rebinding
heuristic **is** an authorization outcome. If `TransportFacts` is genuinely inert (claim 5 holds), then
`tls_terminated_by_host` is decorative and **the engine cannot refuse a credential presented over
cleartext** — it has no other signal, having lost the socket, so 011 §8a's production-HTTPS
requirement becomes documentation. If either field does influence an outcome, claim 5 is false and
these are host-asserted authorization inputs sitting in the struct labelled "evidence, never
authority." `sni` is the sharpest case: a multi-tenant host mapping SNI → tenant makes it a host-chosen
tenant selector.

**R26 (IMPL, all) — identity is absent from every downstream key even once `authenticate()` is
correct.** `ToolInvocationAudit` (`core/tool_pipeline.hpp:204-212`) has no principal and no tenant,
while 007 §8 requires principal *and* delegation chain on every record; `call_id` is the
**caller-chosen** JSON-RPC request id (`protocol/mcp/server.hpp:243`, `:365-368`), so two principals can
emit identical audit keys at will. With R4, I4's attributability claim does not survive a second
principal.

**R27 (IMPL, all) — §5 asked whether fixing `held_` is sufficient; it is not.** Three more
construction-time authorities share the flaw and were unnamed: `ApprovalDecider approve_`
(`protocol/mcp/server.hpp:372` — 006 §4 binds approval to the exact call and 007 §5 makes policy a
function of the principal, so a decider fixed at construction cannot know who is asking); the
default-constructed `EffectContext` (R4/R13); and `A2aServer::context_id_` (`protocol/a2a/server.hpp:95-96`,
`:177`), stamped onto every task and message, so one server cannot serve two principals without merging
their conversation grouping. `RunEventProjector::thread_id_` mirrors it (`protocol/agui/projection.hpp:46-48`).

### 7d. Additional notes returned

- **No `Origin` handling exists anywhere in the tree** — a grep over `include/` and `src/` returns only
  `MemoryOrigin` (`core/memory.hpp:30`). §2a claimed Origin validation "stays in-engine"; the accurate
  statement is that 011 §8a's `dns-rebinding` scenario has **zero implementation on either half** today.
- **§0's framing is factually wrong about this codebase and inflates its own cost side.** The engine
  already ships a proven TLS client (ADR-013), an HTTP exchange with byte-capped reads and stop-token
  cancellation, resolve-once anti-rebinding (ADR-011 C6), and an egress address policy (ADR-016). What
  is missing is *inbound framing*, not "HTTP networking." Since the request parser must be written
  somewhere regardless (R19), the real choice is "in `include/` under ADR discipline, or in `tests/`
  under fixture discipline" — and stating it that way changes the calculus.
- **§2's citation of 013 §4's in-process transport as precedent is a category error.**
  `InboundRequest` carries `HeaderView` and a byte span — a fully serialized HTTP request with the
  socket removed. It gets neither 013 §4's in-process benefit nor a real HTTP surface's completeness,
  and 020 §3a already defines the genuine in-process contract (`ask_stream<RunEvent>`, direct structs),
  which is a different API.
- **`TransportFacts`' `string_view` members** are non-owning on a struct consumed by paths that outlive
  the call; claim 5 proves inertness for authorization, not safety of retention for audit, which I4
  requires.
- **What held up, recorded so the judge phase does not over-correct:** `verify_bearer_token`'s check
  ordering is right (signature → exp → aud/iss → replay, `bearer_token.cpp:113-134`), so unsigned
  traffic cannot pollute the replay map; the length-prefixed canonical encoding has no
  delimiter-injection ambiguity and verification re-encodes rather than trusting a supplied encoding,
  making canonicalization mismatch structurally impossible; `constant_time_equal` reuse is real; and
  algorithm-confusion-by-construction survives inspection. The problems are in the layer this ADR
  proposes, **except R5**, where the primitive's replay semantics are wrong for the role assigned it.

### 7e. Status of §4's claims table after this pass

Covered: 3 of 27 findings. Disproven or shown insufficient as written: **claim 3** (passes against a
dangling implementation, R16; covers only `held_`, R27), **claim 4** (tests neither admission nor the
anonymous tenant, R13), **claim 5** (self-contradicting with §3's prose, R25), **claim 8** (passes
while Design D is broken, R7), **claim 9** (measures N ≈ 0, R12), **claim 10** (right mechanism, wrong
scope, R10). The table needs roughly a dozen new claims before any design here is provable; those are
enumerated per-finding above rather than restated, and writing them is the first task of the prove
phase, not a judged outcome.

## 8. Prove phase

*(not yet run)*

## 9. Decision

*(not yet judged)*
