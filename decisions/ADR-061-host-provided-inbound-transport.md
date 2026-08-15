# ADR-061 — Host-provided inbound transport: who authenticates an inbound MCP/A2A/AG-UI request?

**Status:** Design, second iteration (2026-08-15) — red-teamed once, not yet proven or judged.
Supersedes ADR-022 in effect (the reactor question is moot if no first-party listener is ever built)
and re-scopes ADR-021.

Two independent adversarial passes returned 27 findings (§7), four of which were **live security
defects in shipped M7 code** rather than design problems — latent only because no inbound transport
existed to reach them, which is precisely what this ADR proposes to add. Those four are now fixed and
proven (§7a, `tests/test_task_principal_binding.cpp`), except for two halves that need spec
amendments this ADR owes.

§3/§4 are the first design iteration, superseded by **§8** and kept as record. §8 reframes the
boundary as four contracts rather than one, withdraws Design C, introduces
`RequestAuthority`/`AuthorityRef`, and replaces the claims table.

**§8 was then red-teamed in turn (§9) and did not survive.** Its recommended design (F + G) is
defeated: G launders a host-chosen identity into `verified_by_engine` (S1), and F's central premise
rests on a **fabricated citation** — 020 §3a specifies no identity contract, and
`make_embedded_principal` appears in no spec file at all (S2). The core device is unsound as
specified: `AuthorityRef` is a guessable plain aggregate reinventing ADR-005 Design B (S3), and
`live()` is checked at event boundaries while effects run unbounded on detached threads (S4). A second
pass added 18 more (§9h), including that **the design space itself was wrong**: 011 §7's stdio server
role needs no socket, TLS, HTTP framing, or fixture host, and yields an honestly engine-attributable
conformance run — an option neither §3 nor §8 considered (T0).

33 findings against §8. **§10 is the third iteration**, and its substantive move is not a better
answer to §8's question but the finding that §8's question was gating more than it needed to:
`conformance client --command` **spawns our binary**, so 011 §10 G2 needs no listener, no host, no
fixture, and no attribution apparatus — it is fully engine-attributable today, and the M7 Phase G
audit was wrong to list it as listener-blocked. §10 therefore tiers the work, commits only to Tier 1
(MCP client role over stdio), and defers all 33 findings to Tier 3 where they actually bite.

Two measured/verified results survive and are reusable: claim 6 at 104 bytes against a 192-byte
ceiling (§8.1), and §10.0's corrections to two of §9's own findings.

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

## 3. Competing designs — FIRST ITERATION, superseded by §8

**Superseded by §8 (2026-08-15), kept because the reasoning that was wrong is part of the record**
(decisions/README.md). §7's red-team found this framing under-specified: it treats the re-scope as a
single question ("who establishes the `Principal`") when the host/engine boundary is four contracts,
and it defines a request type with no response type at all (R14). Designs A-D below are still the
honest starting point and §8 is written as a delta against them, not a replacement that pretends they
never existed.

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

## 4. Falsifiable claims — FIRST ITERATION, superseded by §8b

**Superseded by §8b.** §7e records which of these were disproven or shown insufficient as written
(claims 3, 4, 5, 8, 9, 10) and why. Kept for the same reason §3 is.

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
thing enforcing these boundaries.** ADR-061 is the work that removes that protection. They are
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

## 8. Second design iteration (2026-08-15)

### 8.0 The reframe the red-team forced

§3 asked one question. §7 shows the host/engine boundary is **four contracts**, and answering only
the first leaves the other three unassigned — which is how a design that looked complete could omit
the response type entirely (R14):

1. **Request admission** — credentials in, an authenticated identity out.
2. **Response emission** — every status and header a conformance clause names (401 + `WWW-Authenticate`,
   403 on bad `Origin`, 405, 202, content negotiation), plus MCP's request-scoped notification channel.
3. **Stream lifecycle** — open, backpressure, multi-subscriber fan-out, and **termination when the
   authority behind the stream stops being valid**.
4. **Authority lifetime** — per-request, *owned*, revocable, and small enough to cross Quark's
   192-byte actor boundary.

Iteration 1 specified (1) partially and nothing else. Iteration 2 specifies all four, because §7
demonstrates they are not separable: R15 (mid-stream revocation), R16 (borrowed-pointer UAF), R17
(the 192-byte ceiling) and R27 (three more construction-time authorities) are all the same missing
object seen from four directions.

### 8.1 The load-bearing device: `RequestAuthority` + `AuthorityRef`

One structure answers R7, R15, R16, R17 and R27 together. This is the substantive change in
iteration 2; the design choices in §8.2 are then choices about *how it is populated*.

```cpp
// Owned, reference-counted, revocable. Never crosses the actor boundary by value.
class RequestAuthority {
public:
    Principal      const& principal()    const noexcept;
    CapabilitySet  const& capabilities() const noexcept;   // OWNED, not borrowed
    ApprovalDecider const& approve()     const noexcept;
    // R15: the liveness question a long-lived stream must be able to re-ask.
    [[nodiscard]] bool live(std::chrono::system_clock::time_point now) const noexcept;
private:
    Principal            principal_;      // private-constructed, carries provenance (§8.1a)
    CapabilitySet        capabilities_;
    ApprovalDecider      approve_;
    std::chrono::system_clock::time_point expiry_;  // from the verified credential's own `exp`
    std::atomic<bool>    revoked_;
    friend class AuthorityTable;                     // the only producer
};

// 16 bytes. This — not a Principal — is what crosses `StartRun`.
struct AuthorityRef { std::uint64_t slot; std::uint64_t generation; };
```

- **R16 (use-after-free)** — `EffectContext` holds `std::shared_ptr<const RequestAuthority>` instead
  of a borrowed `CapabilitySet const*`. `background_task()` already copies `EffectContext` into a
  detached thread on the documented "the host owns it and must outlive the session" contract
  (`core/tool_pipeline.hpp:355-360`); that contract holds only while authority is per-connection, and
  a refcounted owner makes it hold unconditionally. This is the difference between per-request
  authority being safe and being a dangling read.
- **R17 (192-byte ceiling)** — `quark::Ask<StartRun, AgentResponse>` measured 208 bytes with a full
  `optional<Principal>` embedded, against `MessagePool::kMaxPayload` of 192, and Quark may not be
  patched in-tree. A 16-byte `AuthorityRef` fits with room to spare, and §2's blocked plan to grow
  `Principal` with `claims`/`issuer`/`expiry` is replaced by putting those on the authority record
  the ref resolves to. The two-string `SessionCaller` stand-in can then retire.

  **Measured, not asserted (2026-08-15, MSVC x64, `/std:c++latest`)** — §8b claim 6 run during the
  design phase rather than deferred, because the whole device collapses if the number is wrong:

  | Shape | `sizeof` |
  |---|---|
  | `quark::detail::MessagePool::kMaxPayload` | **192** |
  | `Ask<StartRun, AgentResponse>` — today, two-string `SessionCaller` | 160 |
  | `Ask<StartRunWithPrincipal, AgentResponse>` — control, full `optional<Principal>` | **208** ✗ |
  | `Ask<StartRunWithRef, AgentResponse>` — proposed, 16-byte `AuthorityRef` | **104** ✓ |

  Two things worth recording. The control reproduces **exactly** the 208 bytes
  `core/agent_session.hpp:92-97` cites from its own earlier measurement, which independently
  confirms that comment rather than taking it on trust — the reason `SessionCaller` exists at all.
  And the proposed shape is not merely under the ceiling, it is **56 bytes smaller than what crosses
  the boundary today** (`Principal` alone is 112 bytes; `SessionCaller` is 64), so replacing the
  stand-in with a real authority handle *reduces* message-pool pressure while carrying strictly more
  identity. Claim 6 is therefore CORRECT with an 88-byte margin, not merely satisfied.
- **R27 (three more construction-time authorities)** — `held_`, `approve_`, and the principal all
  resolve from one per-request record, so there is one thing to make per-request rather than three to
  remember.
- **R15 (mid-stream revocation)** — a stream holds the `shared_ptr` and re-checks `live()` at each
  emission boundary, terminating with a distinct terminal event when it goes false.

**Honest note on R15 and ADR-009.** ADR-009 explicitly *rejected* a whole-set epoch counter on
`CapabilitySet` (`ADR-009:66-77`), and this is adjacent enough that it must not be waved through:
what is proposed here is **not** that. ADR-009's question was whether an already-issued in-process
capability handle can be invalidated mid-call; this is a liveness flag on a per-*request* authority
record, re-read at stream-emission boundaries only. The mechanisms differ in granularity and in what
they promise. That said, "adjacent but different" is exactly the kind of claim that should be
checked by someone who did not write it — **this needs ADR-009's own re-examination before the prove
phase treats it as settled**, and it is listed as such in §8c.

#### 8.1a `Principal` gets private construction and provenance (R7)

R7's finding is that Design D typed the *request* while the laundering happens on the `Principal` one
line later: it is a plain aggregate with public `id`/`tenant_id`, default construction, and copy
assignment (`trust/principal.hpp:26-46`), so provenance evaporates the instant either arm returns.
007 §3 property 4 already requires private construction for *capabilities*; identity gets the same
discipline:

```cpp
enum class principal_provenance : std::uint8_t {
    anonymous,          // no credential presented; 018 §1's "Anonymous is a principal, not a bypass"
    verified_by_engine, // derived from a credential this engine itself validated
    asserted_by_host,   // an in-process host said so (§8.2's Design F confines where this is reachable)
};
```

`Principal` becomes privately constructed, with the existing named factories in `trust/` as the only
producers, and `provenance` is non-defaultable. Any surface under an 011 §8a MUST refuses a principal
whose provenance is not `verified_by_engine`. The value carries its own origin, so no downstream API
can be fooled by one that looks right.

This also closes R13's residue structurally: an anonymous principal is a distinct provenance rather
than "the one with `id == "anonymous"`", and a default-constructed `Principal` stops existing.

### 8.2 Competing designs, second iteration

**Design C is withdrawn.** R11 removed its only remaining justification. In-process, a signed
assertion is futile (a host that can call the API can read the signing key from its own memory);
out-of-process, 007 §1's "the host process is trusted" does not transfer to a peer process reached
over IPC, and the scheme has no key-rotation story (`BearerSecretKey` is a bare 32-byte struct with
no key id or acceptance window, so 018 §3's "rotation without restart" is unreachable) and no
attribution value (symmetric HMAC — the engine can mint what the sidecar mints). Withdrawn rather
than carried as a weak third option.

#### Design E — One API, provenance on the type

A single `authenticate()` family over one request type; the trusting and verifying paths both produce
a `Principal`, distinguished by provenance (§8.1a) rather than by the request type. MUST-bearing
surfaces refuse `asserted_by_host`.

Steelman: one code path, one place to audit, and provenance travels with the value rather than with
the call site — which is the half of D that R7 showed actually matters.

Cost: an in-process embedded host and a remote HTTP caller flow through the same entry point, so
"can this deployment assert an identity?" is a runtime property of the value rather than a
structural property of the deployment. It is checkable, but it is convention-shaped where §8.1a's
own precedent (ADR-009, ADR-022) prefers construction-shaped.

#### Design F — Two disjoint APIs; the protocol surface has no trusting arm at all

The insight R7 points at but iteration 1 missed: **the trusting posture already has its own API, and
it is not this one.** 020 §3a is explicit — for an embedded in-process host, *"No protocol surface
(011/012/013 §2) sits between host and engine here: the host is in the same process as the `Run`, so
it consumes 013 §1's event stream directly, in its native struct form"*, via
`ask_stream<RunEvent>(StartRun{...})`. That contract is already specified and already has
`make_embedded_principal()` behind it.

So: the protocol-endpoint API accepts **credentials only**. There is no `TrustedHostRequest`, no
asserted-principal field, and therefore no laundering path — not because a check forbids it but
because the type does not exist. An embedded host that wants to assert identity uses 020 §3a's
in-process contract, which is a different API returning a different thing.

Steelman: this is the construction-level guarantee ADR-022 said this project prefers, achieved by
*removing* surface rather than adding a gate. It also stops D's ergonomic hazard at the root — R7's
"the unsafe path is strictly cheaper to write" cannot happen when the unsafe path is not writable
through this API at all. And it matches 018 §1's table honestly: the Embedded row and the HTTP/MCP/A2A
rows are different rows *because they are different surfaces*, which is what iteration 1 blurred.

Cost: a host that terminates its own auth (OIDC, session cookie, mTLS at its edge) and wants to front
the *protocol* surface must express identity as a credential the engine validates. That is exactly
R5's friction, and Design G is the answer to it rather than a reason to reject F.

#### Design G — Engine-issued session credential (the bridge for already-authenticated hosts)

The friction §1 warned about is real: making a correctly-authenticated host re-mint a token per HTTP
request is what gets a control bypassed. G removes it without giving the protocol surface a trusting
arm.

The host authenticates its user however it already does, then performs **one** identity exchange over
the in-process seam (Design F's 020 §3a contract, where the host is legitimately trusted), receiving
an **engine-minted, engine-audienced, short-lived credential** bound to that session. Every subsequent
protocol request carries that credential and is validated normally, with no special case.

Why this is materially better than per-request assertion:

- **The trust surface shrinks from every request to one call.** A compromised in-process host can
  still exchange for an identity it should not have — that is unavoidable and 007 §1 already says so
  — but it does it through one narrow, auditable entry point rather than on every request, and each
  exchange is a recordable event under 007 §8. Blast radius and detectability both improve.
- **R8 (audience) dissolves for this path**: the credential's audience is the engine's own, minted by
  the engine, never derived from anything the host supplies.
- **R10 (token passthrough) dissolves**: the engine never holds the user's upstream credential at
  all. The host's OIDC token stays in the host. 018 §1's "no token passthrough" MUST becomes
  structurally satisfied instead of conventionally observed.
- **R5 (single-use tokens) is answerable**: an engine-minted session credential is explicitly a
  reusable access token with a short `exp`, and one-shot `jti` semantics are reserved for genuine
  one-shot assertions (§8.4).

Cost: a second credential type and its lifecycle (issue, renew, revoke), and the exchange seam is a
new privileged entry point that needs its own negative corpus.

**F and G compose; E is the alternative.** The recommendation going into the prove phase is **F as
the boundary shape, G as the bridge**, with E kept as the comparison that must be beaten on evidence
rather than dismissed.

### 8.3 The other three contracts, specified

#### Response emission (R14)

```cpp
struct OutboundResponse {
    std::uint16_t status;                 // engine-determined, always
    HeaderList    headers;                // incl. WWW-Authenticate, Content-Type
    std::vector<std::byte> body;
};
```

The rule: **every status and header a conformance clause names is determined by engine code.** The
host writes bytes and chooses nothing. This is what makes §2a's "Origin validation stays in-engine"
true rather than half-true — the check *and* its 403 both belong to the engine.

For MCP's request-scoped notification channel (011 §3.3: progress notifications *"flow on the
response stream of the request they belong to"*), a single response value is insufficient — that is
why `McpProgressProjector` currently has nowhere to send its output. So a handler returns either a
complete `OutboundResponse` or a `ResponseStream`:

```cpp
using ResponseStream = ae::stream<ResponseChunk>;   // core/stream.hpp, already credit-controlled
using HandlerResult  = std::variant<OutboundResponse, ResponseStream>;
```

#### Stream lifecycle (R15, R22, R23)

- **Termination on authority loss** — §8.1's `live()` check at each emission boundary.
- **R22, the discarded push outcome** — `emit_run_event_for()` currently does
  `(void)run_event_producer_.push(...)` (`core/agent_session.hpp:849-855`), silently ignoring
  `Terminated` and emitting into a dead ring forever. The outcome must be handled and end the run's
  emission. With R15 this is also the concrete mechanism by which a disconnected client's run stops
  consuming authority it no longer has a reader for.
- **R22, the pinned worker** — `push()` blocks losslessly (`core/stream.hpp:84-89`) inside a
  `quark::task<>` on a Quark worker, so a host that stops draining stalls a scheduler thread. The
  boundary needs a bounded-block variant with a deadline, so a slow or hostile HTTP client is not a
  lever on the shared scheduler.
- **R23, multi-subscriber** — `enable_event_stream()` overwrites a single producer member
  (`core/agent_session.hpp:528-533`), so a second subscriber silently kills the first, while 012 §2.3
  makes identical ordered broadcast to every stream a MUST. 013 §7 Q2 already resolved *how* (ordinary
  AgentEngine-owned ordered fan-out); it simply is not built, and a host-owned transport makes N
  connections to one run the normal case rather than an edge case.

#### Endpoint identity: audience and admin separation (R8, R21)

Both findings need the same missing thing — the engine has no idea *where* a request arrived — and
one construct answers both:

```cpp
enum class endpoint_surface : std::uint8_t { public_api, admin };

// An INDEX into operator-supplied configuration, not a value the host invents.
struct EndpointId { std::uint32_t index; };
```

The operator configures N endpoints at engine construction, each with its own audience and surface
kind. The host declares which endpoint a request arrived on; the engine looks up the audience *from
its own configuration*. **The host selects among operator-approved endpoints; it never supplies an
audience value.** That is what keeps R8's audience validation from becoming a tautology while still
letting an engine serve several logical resources.

The same construct makes 020 §4 engine-enforceable again (R21): the engine refuses admin methods on
an endpoint whose surface is `public_api`. Without it, "the admin API is never on the same listener as
the public surfaces" degrades to advice a host may ignore.

#### One dispatch source (R9)

The engine derives the operation from **exactly one** source — the JSON-RPC body — and **rejects**,
never silently prefers, any request whose transport-level hints (`target`, `Mcp-Method`, `Mcp-Name`)
disagree with it. This closes the in-process analogue of request smuggling: a host that routes
`/mcp/readonly` on `Mcp-Method: tools/list` while the engine executes a `tools/call` from the body.

#### Ownership at the boundary (R24)

`InboundRequest`'s fields become owning, or the body is copied once at the boundary and parsed
exclusively from the copy. Two reasons, and the second is the one that generalises: a `span` into
host memory makes the digest/parse seam unverifiable by construction, and any view reaching a
background task, a stream, or an audit record dangles the moment the host's request scope ends —
after a call that looked synchronous.

### 8.4 Credential handling, corrected (R5, R6, R10, R12)

- **A decoder must exist and be fuzzed (R6).** There is no `Authorization: Bearer <string>` parser
  anywhere; `BearerToken` holds already-parsed claims and has zero consumers outside its own test.
  The highest-risk step in the chain — attacker-controlled bytes with attacker-controlled `u32`
  length prefixes — is unbuilt. It gets a libFuzzer harness on ADR-015's established pattern.
- **Replay semantics split by credential kind (R5).** A reusable **access token** (what 018 §1's HTTP
  row means) is valid for many requests within its `exp`; `ReplayGuard` must not burn it. A
  **one-shot assertion** keeps `jti`-single-use. Conflating them is what made ADR-021's proven
  mechanism incompatible with ADR-021 §8's own per-request constraint. Stated plainly rather than
  papered over: a bearer token is replayable by anyone who captures it, which is inherent to bearer
  credentials and is why transport confidentiality is a hard requirement, not why a replay guard
  should be bolted onto the access-token path.
- **The credential is unreachable from dispatch (R10).** `Authorization` is extracted at the boundary
  into a move-only, unprintable `Credential` consumed by the authenticator; the `HeaderView` handed
  onward does not contain it. Design A's structural weakness — being the only design that hands the
  engine a live upstream-usable credential in the same process as every outbound client — is fixed by
  making passthrough not compile rather than not happen. Under Design G it does not arise at all.
- **Verification is O(1) amortized (R12).** `ReplayGuard::check_and_record` walks and prunes the whole
  map under one global mutex on every call. Now that it applies only to assertions the pressure is
  lower, but the structure is still wrong and is measured at N≈10⁵ rather than N≈0.

### 8.5 Conformance honesty and the gate apparatus (R19, R20, R21)

§6's fixture-host plan survives, with three corrections §7 makes unavoidable:

1. **Two numbers, not one.** 011 §10's *"A claim of MCP support means a number from this tool"*
   cannot survive unamended when the engine no longer implements localhost binding, the Origin 403,
   OAuth resource-server framing, admin-listener separation, or any connection limit. The project's
   own precedent is exact and already adopted elsewhere: 012 §9 Q1 resolved gRPC with *"We do not
   claim gRPC conformance ourselves... honestly outside what our own TCK run can prove (021 §1's 'no
   claim' discipline)."* Applied here: publish an **engine-attributable conformance number** plus a
   **numbered, normative host-obligation list**, and state that a deployment's conformance is the
   former conditional on discharging the latter.
2. **Scenarios are attributed.** Each conformance scenario is labelled engine-determined or
   fixture-determined, and only the former counts toward the published number. The check that this is
   honest: mutate the fixture (bind `0.0.0.0`, drop the Origin 403) and the number must not move.
3. **The fixture's status is decided, not asserted.** "Not shipped surface" has no mechanism behind it
   — `agentengine_core` is an INTERFACE target with no `install()` rules, so adopters consume this
   repo as source and will copy whatever the fixture does. Either it is a supported reference host
   with the obligations that implies, or the obligation list is normative enough that copying the
   fixture is unnecessary. 020 §7 G2 forces the question anyway, since with the Standalone row void
   the fixture is the only thing that can play "standalone server."

### 8.6 Spec amendments this iteration commits to

§2a's table, extended with what §7 added. Each is an edit the decision phase owes, not a maybe:

| Spec | Change |
|---|---|
| 011 §7 | Engine implements Streamable HTTP *semantics* over a host-provided transport |
| 011 §8a | Origin validation **and its 403** stay in-engine; localhost binding becomes host obligation #1 |
| 011 §10 | Split into engine-attributable number + normative host-obligation list (§8.5) |
| 011 §10 G3 | Statelessness claimed only for the core suite, or the task store is externalized (R18) |
| 012 §1/§5 | `task_id` IS `run_id` — must be decoupled so the public handle is not the deterministic internal one (R1's unfixed half) |
| 012 §8 G1 | Same two-number split as 011 §10 |
| 013 §6 G2 | Restate as an engine-side property at the stream seam + a host obligation (R22) |
| 019 §3 | `IdempotencyKey` gains a principal/tenant dimension; digest becomes cryptographic where it keys a security decision (R4's unfixed half) |
| 020 §3 | Standalone row becomes "engine + host adapter", or the table drops to four shapes (R21) |
| 020 §4 | Admin separation re-expressed as `EndpointId::surface`, engine-enforced (§8.3) |
| 020 §8 Q2 | **Reopened** — both premises of its "in-process, same binary" resolution are destroyed by this ADR (R21) |
| 007 §1 | No change needed under Design F/G, *because* Design C is withdrawn — worth recording as a reason the withdrawal was cheap |

### 8b. Falsifiable claims, second iteration

Replaces §4. Claims marked **(carried)** survive from iteration 1 unchanged; the rest are new or
repaired. Every security claim's disproving experiment includes a positive control, per
decisions/README.md.

| # | Target | Claim | Disproving experiment |
|---|---|---|---|
| 1 | F | No code path through the protocol-endpoint API produces a `Principal` whose `id`/`tenant_id` was chosen by the caller rather than derived from a credential this engine verified | Negative suite over every channel: `X-Principal`-shaped headers, `target` path segments, `TransportFacts.sni`, MCP `requestState`, an A2A task id used as a lookup key. Plus a `try_compile()` gate that no asserted-principal type is constructible through this API at all |
| 2 | all | **Positive control**: a valid credential yields the correct principal and a *successful* `tools/call` | Green path returns a real result — the path can succeed, so its rejections mean something |
| 3 | §8.1a | `Principal` is constructible only by `trust/` factories, and provenance is non-defaultable | `try_compile()`: `Principal p{"admin","t"};` and `p.id = "admin";` both fail outside `trust/`; no factory yields a default provenance |
| 4 | §8.1a | A surface under an 011 §8a MUST refuses a principal whose provenance is not `verified_by_engine` | Attempt `tools/call` on a protocol endpoint with an `asserted_by_host` principal; must be refused |
| 5 | §8.1 | **Repairs claim 3.** Authority is per-request AND memory-safe under overlap: a backgrounded call and an open stream started under request A keep exactly A's authority after A's frame returns and request B has run under a different principal | Overlapping (not sequential) requests under ASan/TSan. The old claim passed against a dangling implementation because two sequential requests never overlap |
| 6 | §8.1 | `AuthorityRef` crosses the actor boundary within budget | `static_assert(sizeof(quark::Ask<StartRun, AgentResponse>) <= quark::detail::MessagePool::kMaxPayload)`. **CORRECT — measured in the design phase (§8.1): 104 vs 192, an 88-byte margin, with the 208-byte control reproducing the codebase's own cited figure** |
| 7 | §8.1/R15 | A stream open when its authority is revoked or expires emits no further event after a bounded interval, and terminates with a distinct terminal event | Open a stream under a credential with `exp = now + 1s`; assert termination and no event N+1. **Currently unwritable — there is no revoke** |
| 8 | R23 | Two concurrent subscribers to one run receive identical events in identical order, and closing one does not affect the other (012 §2.3) | Two subscribers, one run; currently disproved by construction |
| 9 | R22 | A `push()` returning `Terminated` ends the run's emission and is observable; and no host behaviour at the stream seam causes unbounded engine-owned memory growth or occupies a Quark worker beyond a bounded interval | Non-draining host fixture; measure engine memory and worker occupancy. Disproved today by the `(void)push` |
| 10 | R14 | Every conformance-relevant response condition (401/403/405/202, `WWW-Authenticate`, `Content-Type`) is fully determined by engine code | Find one conformance scenario whose pass/fail depends on a host-chosen status. Plus: a `tools/call` emitting progress produces N notification frames and one result frame through one engine-returned carrier |
| 11 | R8/§8.3 | `expected_aud` is operator configuration indexed by `EndpointId`, never a value read from the request; an engine serving N mounts requires N configured audiences | Two endpoints, distinct audiences; a token for endpoint 1 presented at endpoint 2 is rejected. If the API cannot express this, disproven by inspection |
| 12 | R21/§8.3 | An admin method is unreachable on an endpoint whose surface is `public_api` | Dispatch every admin method against a public endpoint; all refused |
| 13 | R9 | Dispatch derives the operation from one source, and any disagreement between `target`/`Mcp-Method`/`Mcp-Name` and the body is a **rejection**, never a silent preference | Differential suite pairing each transport hint with a conflicting body |
| 14 | R5 | The **same** valid unexpired access token authenticates N ≥ 2 successive requests, each yielding the same principal | Present one token twice. Fails today — `check_and_record` burns it on first use |
| 15 | R6 | A wire-format decoder exists in-engine, is fuzzed against truncated / oversized / length-prefix-overflow input, and nothing outside `trust/` constructs `BearerTokenClaims` | libFuzzer harness (ADR-015's pattern) + a grep gate |
| 16 | R10 | The `Authorization` value is unreachable from dispatch: passthrough does not compile | `try_compile()` negative, plus 018 §7 G2's canary-secret scan over an outbound-capture fixture |
| 17 | R12 | Verification latency is O(1) amortized in outstanding un-expired credentials | p99 at N = 0 vs N = 10⁵, ≥8 concurrent verifier threads. **Repairs claim 9**, which measured N ≈ 0 |
| 18 | R24 | The bytes verified and the bytes parsed are the same object, and nothing derived from `InboundRequest` outlives the call unless owned | Hostile host fixture that mutates its buffer after `authenticate()` returns and scribbles/frees on return, under ASan, with a background task and a stream in flight |
| 19 | R25 | **Splits claim 5.** (a) `TransportFacts` never influences identity or capability derivation; (b) its non-identity uses are exactly enumerated; (c) `tls_terminated_by_host == false` on a credential-bearing request is a rejection in production configuration | (a) adversarially varied facts, identical authorization outcome; (c) present a credential with the flag false, assert rejection |
| 20 | R1/R6-family | Every server-minted handle is bound to its establishing principal, and a cross-principal read is indistinguishable from not-found | **Already proven** — `tests/test_task_principal_binding.cpp`, 19 checks, harness teeth verified |
| 21 | R13 | An anonymous principal's `tenant_id` is never taken from the request | Send unauthenticated requests asserting a victim tenant through every channel; derived tenant unchanged |
| 22 | R26 | Every audit record carries `{principal.id, tenant_id, on_behalf_of}`, and 007 §9 G5's reconciliation re-runs with two concurrent principals | **Partly proven** — the fields exist; the two-principal reconciliation is not yet run |
| 23 | G | The identity-exchange seam is the only path to an `asserted_by_host` principal, it is recorded as an auditable event, and the issued credential's audience is engine-minted | Attempt to reach an asserted principal by any other route; assert an audit record per exchange |
| 24 | R19/R20 | Every scenario counted toward the published conformance number is engine-determined | Mutate the fixture (bind `0.0.0.0`, drop the Origin 403); the number must not move |

### 8c. What iteration 2 does NOT resolve

Named so the prove phase starts from an honest list rather than discovering these a third time:

- **ADR-009 re-examination (§8.1)** — whether `RequestAuthority::live()` is genuinely a different
  mechanism from the epoch counter ADR-009 rejected, checked by someone who did not write §8.1.
- **R18 / 011 §10 G3** — whether the task store is externalized behind a seam or statelessness is
  claimed only for the core suite. §8.6 lists both options because the choice is a real trade, not an
  oversight.
- **R11's residue** — Design C's withdrawal means 007 §1 needs no amendment *today*. If an
  out-of-process trusted host is ever wanted, that amendment comes back and this ADR does not
  pre-authorize it.
- **The remote-agent-as-tool binding** and **binary protobuf framing**, both named absent since M7
  Phase D4/E3 and untouched here.
- **Whether Design F's cost is acceptable to real adopters** — G is a designed answer, not a measured
  one. Claim 23 tests that it works, not that anybody prefers it.

## 9. Second red-team round — against §8

An independent adversarial pass (fresh context, targeted at §8 only, explicitly told not to
re-report R1-R27) returned 15 findings. A second pass on the remaining contracts is recorded in §9g.
Every code-level finding below was **re-verified by hand** before being recorded.

**Verdict up front: §8's recommended design (F + G) is defeated, and one of its supporting citations
was fabricated.** This section does not soften either.

### 9a. The recommendation collapses

**S1 (CONCEPT-FLAW) — Design G launders a host-chosen identity into `verified_by_engine`, and §8b
claims 4 and 23 cannot both be true.** The attack is three steps: a host authenticates nobody, calls
G's exchange seam asking for `sub = "alice", tenant = "acme"`, and receives an engine-minted,
engine-audienced credential. It presents that credential on the protocol endpoint. The engine
validates **its own** signature, succeeds, and derives a `Principal` — which by §8.1a's own
definition ("derived from a credential this engine itself validated") is `verified_by_engine`. So the
MUST-bearing surface accepts an identity whose `id`/`tenant_id` the host chose freely, and claim 1 is
false through the design's own recommended path.

The other branch is no better: if G's credential instead yields `asserted_by_host`, claim 4 refuses
it at every 011 §8a surface and G does nothing about the friction it exists to remove. **`verified_by_engine`
does not mean what §8.1a's text promises — it means *the bytes verified*, never *the identity
verified*.** And claim 23 ("the exchange seam is the only path to an `asserted_by_host` principal")
is backwards: G's output is a credential, and credentials are the `verified_by_engine` producer.

This is R5's critique of Design A reproduced exactly — *"the host would then need the signing key, at
which point it can mint any `sub`/`tenant_id` and Design A is Design B with extra steps."* G does not
need the signing key because the engine mints on the host's word instead. Same outcome, one
indirection. **Design G as specified is Design B with a ceremony.**

**S2 (CONCEPT-FLAW) — Design F's central premise is not what 020 §3a says, and the citation was
fabricated.** §8.2 states that 020 §3a's contract *"is already specified and already has
`make_embedded_principal()` behind it."* Verified by grep: `make_embedded_principal` appears in **no
spec file at all** — the only occurrences in the repo are this ADR itself and one milestone
breakdown. And 020 §3a (lines 53-118) contains **zero** occurrences of "principal", "identity", or
"auth": it specifies bring-up, run start and drain, threading ownership, secondary observers, and ABI
scope, and says nothing whatever about identity. The nearest thing 020 says is §3b's *opposite* rule
— "A webhook trigger authenticates before `StartRun` is ever called."

I supplied the missing half of that contract and attributed it to 020. That is the same class of
error as §2's bearer-token claim, against the same standing rule (CLAUDE.md: *"Do not assert what a
protocol does from memory"*), and it is load-bearing rather than incidental: F's entire advantage over
E was that the trusting API *already exists elsewhere*.

**And the disjointness does not exist structurally either.** Both surfaces funnel into
`quark::Ask<StartRun, AgentResponse>` on the same `AgentSession` actor, and §8.1 puts `AuthorityRef`
*on `StartRun`* — so the two "disjoint" APIs share the identity-carrying message type, and Quark
messages carry no origin. Worse, the place a host actually asserts identity today is
`AgentSession::initialize(session_id, Principal, ...)` (`core/agent_session.hpp:555-560`) plus
`set_capabilities()` (`:587`) — ordinary public methods on the same class the protocol dispatcher
drives. **F's load-bearing sentence — "not because a check forbids it but because the type does not
exist" — is false.** The trusting type exists, is linked into the same process, and is one method
call away from any adapter an adopter writes. F is Design E with the check relocated, which is
exactly what E was docked for.

### 9b. The core device

**S3 (CONCEPT-FLAW) — `AuthorityRef` is a forgeable, guessable plain aggregate, and it is ADR-005
Design B rebuilt without citation.** `struct AuthorityRef { std::uint64_t slot; std::uint64_t
generation; };` has public members, aggregate initialization, and no private construction — **the
exact shape §8.1a condemns `Principal` for.** Any code reaching the resolution seam writes
`AuthorityRef{0, 1}` and receives an authority containing somebody's `Principal`, `CapabilitySet`,
*and* `ApprovalDecider`. Forging a `Principal` bought an identity; forging an `AuthorityRef` buys
identity plus capabilities plus the approval decider — strictly more than the thing §8.1a hardened.

And it is trivially guessable: dense small integers, first authority at slot 0 generation 1. §7a's own
R3 remediation replaced two `mt19937_64` generators with a CSPRNG *precisely because* "a predictable
server-minted handle is exactly the hazard 011 §8a's own text calls out" — **this ADR introduces a
100%-predictable handle for a more sensitive object than a task id, in the same document that fixed
the weaker case.**

Separately: opaque reference + host-side registry + immediate revocation **is ADR-005 §3 Design B
verbatim**, including its accepted conclusion that capabilities needing immediate revocation should
use it. §8.1 re-derives it with a weaker handle (dense integers vs 128 random bits), no lookup
contract, and none of ADR-005 §9's named residuals. `CapabilityRegistry` should either be the
mechanism or be explicitly rejected.

**I2 exposure**: an `AuthorityRef` is a *name*, not a handle — converting it to authority requires
reaching a table. Either the table is a singleton (textbook ambient authority; I2 dead) or it is
passed explicitly everywhere the ref goes, including into `background_task()`'s detached thread — in
which case R16's lifetime argument reappears one level up, unaddressed. §8.1 never names the table's
owner, lifetime, thread-safety, slot-recycling policy, or wraparound behavior.

**S4 (CONCEPT-FLAW) — `live()` is checked where effects do not happen, and the `shared_ptr` makes
this worse rather than better.** Emission boundaries are *events*, not *effects*. Verified against
the code: `background_task()` runs step 8 as one blocking `tool->invoke(request.arguments, ctx)` on a
detached `std::thread` (`core/tool_pipeline.hpp:443-449`) with no emission boundary and no join
handle — and the function's own comment states *"006 §6b names no cancellation mechanism for IN-FLIGHT
native `invoke()` work"* (`:368-370`). An authority revoked at t=0 does not stop that thread at
t+10min. **There is no bound on post-revocation effects, and the design has no place to put one.**

The refcount actively hurts: under the old borrowed pointer a revoked authority would at least have
produced a sanitizer-findable crash. Refcounting converts a *detectable* use-after-free into an
*undetectable* use-after-revocation — the thread holds the authority alive and keeps using it,
forever, correctly typed.

**The ADR-009 re-examination §8c asked for, answered — and it found something sharper than
re-litigation.** On the narrow question the "adjacent but different" claim survives: ADR-009 rejected
the epoch counter for insufficiency at per-call granularity, and `live()` is a coarser flag for a
different question. But `revoked_` is **structurally disconnected from the mechanism ADR-009
accepted**: `capabilities()` returns `CapabilitySet const&`, `bind()` mints a `BoundCapability` over
a fresh `InvocationTicket`, and `revoke()` flips *that ticket* — nothing connects
`RequestAuthority::revoked_` to any ticket. Flipping it leaves every already-bound handle live, which
is precisely the failure ADR-009 rejected Design B for, reproduced with the epoch moving and the
handle passing anyway. The correct verdict is not "different, therefore fine" but **"different in the
direction that matters: `live()` revokes nothing the effect path consults."** `live()` is also merely
advisory — `capabilities()` has no liveness precondition and nothing forces a caller to ask, which is
convention-shaped enforcement in the one place §8.2's own steelman insists on construction-shaped.

**S5 (CONCEPT-FLAW) — revocation has no authorization model and evaporates at the next request.**
Nothing says who may revoke, or whether the request is checked against the target's principal or
tenant. Combined with S3's guessable ref, an authenticated low-privilege caller enumerates slots and
revokes every other principal's in-flight authority — a total availability attack, cross-tenant,
costing one integer per victim. And because authority is per-*request*, the next request re-presents
the same unexpired credential and gets a **fresh** record with `revoked_ == false`: "revoked" means
"this one request's record is dead", not "this principal's authority is withdrawn." Making it stick
needs a revocation list keyed by credential — ADR-005 Design B again. For G's minted credential
specifically, **ADR-005 §7/§8 already proved revocation is unavailable**: *"nothing about possessing
a `SecretKey` lets a host 'unmint' a token already handed out"* (claim B3, verdict CORRECT). §8.2's
cost line for G lists "issue, renew, revoke" as if revoke were a design task; it is not available for
that credential shape.

**S6 (CONCEPT-FLAW) — `AuthorityRef` does not survive a restart, and resolves to the *wrong*
principal.** §8.1 moved identity onto an in-process record keyed by dense slot+generation counters
that restart from zero. A run suspended under 019 and resumed in a new process holds
`AuthorityRef{3, 1}`, which **resolves successfully — to a different principal's authority.** Silent
cross-tenant misattribution with a non-erroring lookup. A 128-bit random ref would make this a clean
not-found. Compounding it, §8.1 put `Principal` + claims/issuer/expiry + `ApprovalDecider` (a
`std::function`, inherently unserializable) onto the record — and `AgentSessionRecord` already cannot
serialize a `CapabilitySet` (its own comment, `core/agent_session.hpp:222-233`). **The durability gap
got strictly bigger and §8.6 lists no 019 amendment for it.**

**S7 (CONCEPT-FLAW) — per-request authority collides with the admission rule that is actually
built.** `AgentSession::handle()` sets `effect_context_.principal = principal_` — the session's
*owning* principal, fixed at `initialize()` — unconditionally, after the admission check
(`core/agent_session.hpp:310`). So the identity reaching every effect, every outbound
`ChatClient::chat()`, and every audit record is the session owner's, **not the requester's**. Adding
an `AuthorityRef` to `StartRun` without deleting that line gives one run two identities and lets the
wrong one win, silently, since both are well-formed. §8.1's claim to answer R27 was stated against
`held_`/`approve_`/`EffectContext` and never reaches `AgentSession::principal_` — the fourth
construction-time authority, and the one that actually wins.

Sharper still: admission degrades to **exact id+tenant match** for `SessionCaller` by documented
design. Under a host-fronted surface, N callers against one conversation session is the normal shape,
and every caller who is not the session owner is denied. The design must choose between one session
per request (destroying the conversation continuity A2A `contextId` and MCP sessions exist for) and
relaxing 018 §2. §8 raises neither.

**S8 (IMPL→CONCEPT) — `live(now)` puts an unrecorded wall-clock read on the hottest path, against
I5.** Someone must produce `now` per emitted event. `run_id` is minted from a counter specifically
because *"an unrecorded wall-clock read here would be exactly the kind of untracked nondeterminism I5
forbids"* (`core/agent_session.hpp:290-293`). Clock access is also a capability (`cap::Clock`), so an
engine-internal `system_clock::now()` per event is unmediated clock authority. A replayed run would
terminate at a different event index, breaking 019's rewind-then-reexecute. The clocks also disagree:
`expiry_` would be `system_clock` while `EffectContext::deadline` is `steady_clock` — and
`system_clock` is settable, so an NTP step backward extends every authority's life and forward kills
every live stream at once.

### 9c. The identity model

**S9 (CONCEPT-FLAW) — provenance is unforgeable in C++ and forgeable in storage.** Private
construction guards call sites, not bytes. Verified: `restore_from_record()` rebuilds identity with
**aggregate initialization** — `principal_ = Principal{rec.principal_id, rec.principal_tenant_id};`
(`core/agent_session.hpp:784`). Under §8.1a that line does not compile, and every way of making it
compile is a hole: persist provenance and anyone who can write the store mints `verified_by_engine`;
don't persist it and the restore path must fabricate one — `verified_by_engine` is a forgery factory,
while `anonymous` means **a 019-suspended run resumed after restart can no longer use any surface
under an 011 §8a MUST**, i.e. authenticated long-running work cannot survive a checkpoint. §8.1a's
"the value carries its own origin" holds only for values that never leave the process, and this one
already does.

**S10 (CONCEPT-FLAW) — `derive_on_behalf_of` has no valid provenance value.** It is a `trust/`
factory taking a caller-chosen `derived_id`, and none of the three enum values describes a delegated
identity. Inherit the parent's → anyone holding a `verified_by_engine` principal mints one with an
arbitrary id (`derive_on_behalf_of(alice, "admin")`), so claims 1 and 4 fall through a factory claim 3
explicitly blesses. Assign `verified_by_engine` → same hole, plus a provenance-*upgrade* primitive
reachable from an `asserted_by_host` parent. Assign `asserted_by_host` → every sub-agent and delegated
call is refused at every MUST surface, breaking 007 §2 and 012 §4 outright. This is live code, not
hypothetical: sessions are initialized with derived principals today.

### 9d. Internal inconsistencies

**S11 — §8.2 withdrew Design C for a defect Design G then adopts.** C's withdrawal cites "no
key-rotation story (`BearerSecretKey` is a bare 32-byte struct with no key id or acceptance window)".
Verified: zero occurrences of `kid`/`key_id`/acceptance/rotation in `trust/bearer_token.hpp`. G mints
engine-issued credentials with the same and only primitive, so it **inherits the identical defect** —
rotating the signing key invalidates every outstanding session credential at once, breaking in-flight
calls, which is exactly what 018 §3 forbids. A design that kills C on grounds X and adopts G with
property X has not evaluated G. §8.6 lists no 018 §3 amendment.

**S12 — §8.4's replay split has nowhere to carry the discriminator.** `BearerTokenClaims` has no kind
field. Put the kind *in the token* and the verifier reads a selector from the credential to decide how
the credential is checked — structurally the algorithm-confusion class, against a rule
`bearer_token.hpp` itself declares BINDING. Put it in *endpoint config* and replay semantics depend on
where a credential is presented, so a captured one-shot assertion replayed at an access-token endpoint
is never checked. **And claim 14 is a control-removal claim with no paired negative**: the most likely
implementation (delete the one `check_and_record` call site) passes claim 14 with flying colors while
silently deleting ADR-021's fourth proven finding.

**S13 — the compensating control §8.4 offers is a boolean the host sets.** §8.4 justifies making
access tokens replayable on the grounds that "transport confidentiality is a hard requirement" — but
the engine has no socket, so its only signal is `TransportFacts::tls_terminated_by_host`, a bool
supplied by the same party that supplies the credential. That is R8's audience tautology reappearing
for confidentiality, in a section whose purpose was fixing R8-adjacent problems. Claim 19(c) is also
qualified "in production configuration" — defeated by deployment posture, in a document arguing
construction beats configuration.

### 9e. Claim insufficiencies

**S14 — claims 5 and 6 pass against implementations where the device is decorative.** Claim 5 is
satisfied by an implementation where the `shared_ptr` rides along in `EffectContext` for audit while
every capability check still consults the server-wide `held_` — because the pipeline authorizes from
*separate parameters* (`invoke_tool(table_, held_, call, ctx, approve_)`), not from `EffectContext`.
It tests lifetime, not provenance of the authorization decision — the same miss §7e recorded for old
claim 3. Claim 6's `static_assert` measures the query type while the source note it answers says 208
was "already over budget **before `Responder<R>`'s own overhead**" — so it can pass while the real
pooled message overflows. *(This qualifies the measurement recorded in §8.1: the 104-byte figure is
real and the margin is large, but the assert as written must be against the actual pooled payload.)*

### 9f. What held up

Recorded so the next iteration does not over-correct:

- **§8.0's reframe** (four contracts, not one question) survived both lenses.
- **§8.3's `EndpointId`-as-index** is the right shape and claim 11 tests it properly — making the host
  *select among* operator-approved endpoints rather than supply a value is a genuine answer to R8/R21.
- **The `live()`-vs-ADR-009 narrow question** comes back clean: it is not the mechanism ADR-009
  rejected. The problem is the opposite one (S4).
- **Claim 6's measurement** stands at 104 vs 192 with the control reproducing the codebase's own
  208-byte figure; only the assert's framing needs repair (S14).

### 9g. Status

**§8 is not ready for a prove phase.** Its recommended design is defeated (S1, S2), its central device
is unsound as specified (S3-S8), and its identity model has a storage-layer hole (S9) and an
unassignable case (S10). A third design iteration is required, and it should start from these
constraints rather than from §8's designs:

1. **Anything the host names, the host controls.** Both G's exchange and B's assertion reduce to the
   same thing; the only real question is whether that is acceptable *and labelled*, not whether
   ceremony disguises it.
2. **A handle is a credential.** If `AuthorityRef` survives at all it needs CSPRNG entropy, private
   construction, and an owner — or `CapabilityRegistry` (ADR-005 Design B) is used directly.
3. **Revocation must reach the effect path or not be claimed.** Given the detached-thread reality,
   the honest options are binding revocation to `InvocationTicket`, or stating that in-flight native
   effects are un-revocable and credential lifetime is the only bound.
4. **Identity must survive a checkpoint.** Provenance that cannot be serialized safely is provenance
   that ends at the first suspend.

### 9h. Second pass — contracts, claims table, spec amendments (18 further findings)

The second independent pass, on the lens §9a-§9f did not cover. Verified by hand as above. It found
the option §8 never considered, so that goes first.

**T0 — the cheapest option on the board was omitted from the design space entirely.** 011 §7's
transport clause has a second sentence this ADR never engages: *"**stdio** remains for local servers,
with `server/discover` usable as the backward-compatibility probe"* (`011-MCP-Conformance.md:227-228`,
verified). **An stdio server role needs no socket, no TLS, no HTTP framing, no fixture host, and no
chunked-encoding parser** — the artifact R19 says the fixture must contain and which exists nowhere in
this tree. It is the one inbound transport the project-owner direction does not forbid the engine from
owning outright, and owning it makes a genuinely *engine-attributable* conformance run possible for a
real subset **without any of §8.5's attribution apparatus at all**. §8.6 rewrites 011 §7 without
noticing that half of it survives the re-scope untouched. This is not a repair to §8; it is a design
option that should have been in §3 and §8 and was in neither.

**T1 (CONCEPT-FLAW) — claim 8 contradicts the very resolution §8.3 cites as its authority.** §8.3 says
013 §7 Q2 "already resolved *how*" and drops the load-bearing half. Verified: Q2 resolved specifically
on **`EvictAfter<N>` — bounded buffer, then evict with an explicit gap signal — and states "`Block` is
not required for this need"** (`013:290-295`), because A2A's ordering MUST applies only to *currently
attached* subscribers and §2.4 disclaims gap-free delivery on reconnect. Under `EvictAfter<N>` two
subscribers draining at different rates receive **different event sets by design**. Claim 8 asserts
"identical events in identical order" — **falsified by the resolved design it is meant to test.**
Claim 9 compounds it by demanding lossless + ordered + bounded memory + bounded blocking
simultaneously against an adversarial consumer; those four cannot hold together, and the only escape
is dropping with a gap signal, which claim 8 forbids. 020 §7 G5 demands a third, incompatible policy
(unbounded block), and is not among the twelve amendments.

**T2 (CONCEPT-FLAW) — claim 23 is falsified by a fourth principal source that is already
spec-mandated and gated.** 020 §3b: *"the trigger config names the principal it runs as, resolved at
admission (018) like any other run"* (`020:139-141`, verified), gated by 020 §7 G6. An
operator-configured principal is neither `verified_by_engine`, nor `anonymous`, nor reachable through
G's exchange — **§8.1a's enum has no member for it**, and §8.1a's rule would refuse every triggered
run. Worse, 020 §3b exists precisely to stop what this ADR does: *"verification (018) gates the call
rather than being left for each deployment to reinvent — or skip"* (`020:142-144`). Neither 020 §3b,
020 §7 G6, nor 019 §2's external-event wake row appears in §8.6, §8.5's obligation list, or §8c.

**T3 — `AuthorityRef` findings independently reproduced.** The second pass reached S3's conclusion by
its own route, adding that `AuthorityTable` is *never specified anywhere in §8* — not its lifetime,
lookup, thread-safety, slot recycling, or wraparound — and that a per-request insert plus per-effect
lookup under one mutex is R12's lock relocated to a hotter path, which claim 17 does not measure.

**T4 (CONCEPT-FLAW) — §8.6's 019 §3 amendment has two horns and both break a proven gate.** Digest the
whole `Principal` and a resumed run reconstructs a *different* key (the record round-trips only
id/tenant and §8.1a adds non-defaultable provenance), failing 019 §7 G1/G2/G6 and 011 §10 G4. Digest
only `{id, tenant_id}` and provenance-distinct principals share a journal key, reopening R4 along the
axis this ADR introduced. **And the amendment fixes the wrong half**: `IdempotencyKey::to_string()` is
a raw colon concatenation (`core/tool_pipeline.hpp:155-158`), so adding attacker-influenceable string
fields is a delimiter-injection collision — `{tenant:"a:b", id:"c"}` and `{tenant:"a", id:"b:c"}`
collide. Upgrading the digest does nothing about the concatenation. §7d recorded that ADR-021's
length-prefixed encoding is exactly what made this class structurally impossible; the lesson is not
applied one file over. **§8b has no durability claim at all.**

**T5 (CONCEPT-FLAW) — the 012 §1/§5 decoupling is four times larger than the amendment states.**
`task_id`-IS-`run_id` is load-bearing in 012 §2.3's push-notification dedup (`012:83-86`), 012 §8 G4
(`012:229`), **012 §5a's OQ-4 resolution** (`012:196-198` — decoupling reopens a *resolved* open
question), and 013 §2.2 (`013:169-170`). None is in §8.6. Also: 011 §8a requires handles be
"high-entropy, **expiring**, and bound" — **expiry appears nowhere in the twelve amendments or in
§8b**, and claim 20 is marked "already proven" while covering only the binding third of a three-part
MUST.

**T6 (CONCEPT-FLAW) — §8.3's `ResponseStream` arm carries no status and no headers, disproving claim
10 by inspection of §8.3's own type.** `OutboundResponse` has `{status, headers, body}`;
`ResponseStream` has neither — so on the streaming arm the host must invent the status line and
`Content-Type: text/event-stream`, which claim 10 names explicitly as engine-determined. And HTTP
commits status before body, so an expiry detected mid-stream cannot be expressed as the 401 the engine
"determines"; neither 011 nor 012 has any trailer clause.

**T7 (CONCEPT-FLAW) — §8.3 enumerates response obligations that do not exist in the specs and omits
the ones that do.** Grepped: `202` and `405` have **zero occurrences** as status codes in 011 or 012;
content negotiation / `Accept` / `Content-Type` likewise zero. Meanwhile the real MUSTs are absent
from §8.3 and claim 10 — `Mcp-Method`/`Mcp-Name` on POST (`011:225-227`), `x-mcp-header` violation
excluding the tool from `tools/list` (`011:81-85`), `serverInfo` in each result's `_meta`
(`011:47-48`), no `notifications/message` without `logLevel` (`011:44`), `A2A-Version` +
`VersionNotSupportedError` (`012:15-19`), `A2A-Extensions` (`012:166-169`), RFC 9111 card caching
(`012:161-162`). **I enumerated from generic HTTP knowledge rather than from the specs** — the same
error class as S2, and claim 10's "find one counterexample" phrasing means an implementation getting
every real header MUST wrong scores green.

**T8 (CONCEPT-FLAW) — `EndpointId` is an asserted host fact that is authorization-relevant, so Design
F has a trusting arm after all; it is just not called a principal.** It is a public aggregate whose
`EndpointId{}` is index 0. Its containment depends on audiences being distinct per endpoint, which
nothing requires — two mounts of one logical resource, one public and one admin, is a plausible
config where a lying index is entirely uncontained. It also creates a second, unsynchronized copy of
the routing table with no drift detection, and it carries no origin allowlist, so §8.6's "Origin
validation and its 403 stay in-engine" has **no input**. Claims 11/12 test behaviour given a *correct*
index; neither tests what stops a wrong one.

**T9 — the single-dispatch-source rule is undefined for `target`, and silent on absence, which is the
bypass.** No clause maps a path to a JSON-RPC method and none can, since the host chose the mount.
More importantly, 011 §7 makes the headers **mandatory**, so a rule keyed on *disagreement* is
satisfied by a host that routes on the hints then strips them — the cheapest adapter to write, and
R9's attack restored in full.

**T10 — `ae::stream<ResponseChunk>` is the wrong carrier, and §8.3's own R22 fix needs a primitive
that cannot be added without patching Quark.** The consumer is poll-only with no readiness signal
(`stream.hpp:139-144`; the wake word wakes only the producer), so a host driving an idle
`subscriptions/listen` stream busy-polls — `stream.hpp:38-40` concedes the contract. The bounded-block
deadline §8.3 states as a parameter requires either a Quark change (forbidden by a locked decision) or
abandoning Quark's proven lost-wakeup ordering. The response arena has no owner in `HandlerResult`,
and §8.3's R24 ownership fix is inbound-only. Plus one heap allocation per SSE frame.

**T11 — §8.5's obligation list has no closure criterion, and its two-number split is weaker than the
mechanism 011 §10 already adopts.** 011 §10 commits to the official suite's expected-failures baseline
where *"a baselined-but-now-passing check also exits non-zero, so the baseline cannot rot silently…
we adopt it as-is rather than writing our own harness"* (`011:336-339`). §8.5 proposes exactly a
bespoke harness with a hand-maintained label per scenario and an **author-chosen denominator**.
Claim 24's control is vacuous both ways: dropping the Origin 403 cannot be a fixture mutation (§8.6
puts it in-engine), and binding `0.0.0.0` only moves a number that by the labelling never counted
`dns-rebinding`. It is also negative-only — **ADR-015's precedent, which §7a applied correctly, demands
the converse arm: neuter an engine-side check and the number MUST drop.**

**T12 — claim 7 passes against a stream that never terminates.** Liveness re-checked *only at emission
boundaries* means an idle stream is never checked, so an implementation that never terminates one
satisfies "no event N+1" **vacuously**. 011 §3.3's `subscriptions/listen` is exactly that: a
long-lived, idle-by-nature channel, unbuilt, unmodelled by §8, absent from §8.6 — and the longest-lived
authority holder in the protocol.

**T13 — §8.6's 013 §6 G2 row is a weakening presented as a restatement.** G2 is end-to-end to *the
provider read*; §8.3's deadline deliberately severs that chain, after which the engine must drop
(contradicting 012 §2.3) or buffer (contradicting 013 §1). 013 §2.2 names the unrecoverable case: a
push abandoned between a snapshot and its interrupt-bearing `RUN_FINISHED` is *"unrecoverable, because
the run is over"* (`013:143-145`).

**T14 — `McpProgressProjector`, which §8.3 promotes onto the inbound path, has unbounded
attacker-keyed state.** Verified: `progress_by_token_[p.call_id]` on a `std::unordered_map` with **no
erase anywhere** (`protocol/mcp/progress.hpp:59-66`), keyed on `call_id` = the **caller-chosen**
JSON-RPC request id. A caller supplying a fresh long id per request grows engine memory without bound,
upstream of any I8 budget. It also emits a progress notification for *every* delta with no opt-in
check, while 011 §2 sets the opposite discipline for the sibling channel. Same shape as §7a: a latent
defect this ADR would make live.

**T15 — §8b violates its own stated positive-control rule in seven rows** (1, 4, 12, 13, 16, 21, 24).
Claim 12 passes against an engine that refuses admin methods everywhere; claim 13 passes against one
that rejects every request; claim 16's canary scan proves nothing without a planted-secret arm. **This
is the exact defect class §7e found in the table this one replaced, at roughly one row in four.**

**T16 — two of the twelve amendments contradict each other.** Row 11 reopens 020 §8 Q2 *because* 020
§4's separate-listener premise is destroyed; row 10 claims 020 §4 is "re-expressed as
`EndpointId::surface`, engine-enforced." Both cannot hold — and row 10 is a weakening anyway, since
020 §4's property is *"never on the same listener"* while `EndpointId::surface` only refuses admin
methods on requests the host *labels* public. Row 9 omits 020 §7 G2's verbatim "all five". Row 4
conflates 011 §10 G3 with the suite's `stateless` scenario, and omits that 020 §5's `session_id`
placement already offers a third option. Rows 11 and 12 are not edits at all, so the real count is
ten. **And §8c does not list 020 §8 Q2** — the ADR commits to reopening a resolved decision and then
does not track it as open.

### 9i. Consolidated status

Across two rounds, **33 findings against §8**. The recommended design is defeated (S1, S2), the core
device is unsound (S3-S8, T3), the identity model has a storage hole (S9), an unassignable delegation
case (S10) and a missing fourth principal source (T2), the claims table reproduces the defect class it
was written to fix (T15), the amendment set is internally contradictory (T16), and the response
contract disproves its own claim by inspection (T6).

Two findings are worth more than the rest combined, in opposite directions:

- **T0 (stdio)** — the design space was wrong, not just the design. An engine-owned stdio server needs
  none of the apparatus §8.5 invents, and yields an honestly engine-attributable conformance number.
- **S2 / T7** — twice now I have asserted what a spec says rather than reading it (020 §3a's identity
  contract; the 011/012 response obligations). Both were load-bearing. The rule in CLAUDE.md exists
  for exactly this, and the third iteration should treat every spec citation as requiring a grep
  before it is written, not after it is challenged.

Constraints for a third iteration, superseding §9g's four:

1. **Start from stdio.** Establish what a real, engine-owned, fully-attributable conformance run looks
   like before designing the host-fronted HTTP case, so the HTTP design is a delta against something
   proven rather than the whole surface at once.
2. **Anything the host names, the host controls** — `EndpointId` included (T8). Label it, do not
   disguise it.
3. **Enumerate the principal sources by construction**, not behaviourally: credential-verified,
   host-asserted, operator-configured (T2), delegated (S10), restored-from-checkpoint (S9). A census
   over `trust/`'s factory set, not an "attempt another route" test.
4. **Pick one stream policy per seam and name it** — 013 §7 Q2 already chose `EvictAfter<N>`; claims 8
   and 9 must be rewritten to that, not against it (T1).
5. **Every spec citation is grepped before it is written.**
6. **Every claim gets its positive control**, and security claims get the ADR-015 teeth arm (T11, T15).

## 10. Third design iteration (2026-08-15)

Written under §9i's six constraints. Every spec citation below was grepped before it was written
(constraint 5), and that immediately produced two corrections to §9's own findings — recorded first,
because building on an unchecked red-team finding would repeat the error the constraint exists to
prevent.

### 10.0 Corrections to §9's findings

**T0 overclaimed, and the overclaim is load-bearing.** T0 asserted that an engine-owned stdio server
"yields an honestly engine-attributable conformance run." Checked: 011 §10's documented invocation is
`conformance server --url http://localhost:3000/mcp` (`011-MCP-Conformance.md:327`), and this
project's own dated research tabulates that command against **"our HTTP server"**
(`docs/research/2026-mcp-ecosystem.md:220`). **There is no evidence anywhere in this repo that
`conformance server` can drive a stdio server.** T0's *insight* stands — stdio is engine-ownable,
needs no socket/TLS/HTTP framing/fixture, and was absent from both §3's and §8's option sets. T0's
*conformance claim* is unverified and probably false for the server suite as documented. Treated
below as a real product surface whose gate status is an open research item, not as a gate.

**T7 is partly wrong, and the correction adds an obligation rather than removing one.** T7 reported
`405` as having "zero occurrences" — true of 011 and 012, which is what it grepped, but the
obligation exists in the detail source those files cite: *"Legacy traffic: GET/DELETE → `405`;
`Mcp-Session-Id` → ignore, never mint or echo; `Last-Event-ID` → ignore, streams are not resumable"*
(`docs/research/2026-mcp-protocol-detail.md:260-261`). So `405` is real and must be in the response
contract. `202` I could not find in any spec or research file; that half of T7 stands. T7's direction
— that §8.3's list came from generic HTTP rather than from the specs — is correct regardless.

### 10.1 The reordering: attributable-first, and the anchor is the *client* role

Constraint 1 said start from stdio. Grepping the tooling shows something better one step over.
The two invocations are asymmetric (`011:327-328`):

```
conformance server --url http://localhost:3000/mcp --suite all
conformance client --command "./agentengine-mcp-client" --spec-version 2026-07-28 --suite all
```

**The client suite spawns our binary.** It needs no listener, no host adapter, no fixture, no HTTP,
and none of §8.5's attribution apparatus — the harness drives us, so every pass or failure is
engine-attributable by construction. And the seam already exists: `McpClient` takes a
`RequestSender = std::function<JsonRpcResponse(JsonRpcRequest const&)>`
(`protocol/mcp/client.hpp:48`, real since Phase C3), so the only new work is a stdio transport behind
that callable plus a small executable — and the project already builds executables under `tools/`
(`CMakeLists.txt:61`).

**This closes 011 §10 G2, which the M7 Phase G audit lists as BLOCKED.** That audit attributed the
block to "no real HTTP transport/listener" *and* "no official conformance tool integration"; the
first half is wrong for G2 specifically. G2 never needed a listener.

So the design is tiered, in dependency order, and iteration 3 commits only to Tier 1's design:

| Tier | Scope | Host contract needed | Gate |
|---|---|---|---|
| **1** | **MCP client role over stdio** | **None** | **011 §10 G2 — fully attributable** |
| 2 | MCP server role over stdio | None (engine owns the transport) | G1 status is an open research item (§10.0) |
| 3 | Server role, host-fronted HTTP | The whole §8 problem | G1/G3/G4/G8/G9, attribution-split per §8.5 |

Tier 3 is where all 33 findings live. Tiering does not answer them — it stops them blocking work that
does not depend on them, and makes the eventual HTTP design a delta against two working tiers instead
of the entire surface at once. **This is the substantive change in iteration 3**: not a better answer
to §8's question, but the observation that §8's question was gating strictly more than it needed to.

### 10.2 Principal provenance, enumerated by construction (constraint 3)

§8.1a's three-value enum was wrong by omission, and the omissions are what S1, S10, T2 and S9 each
found from different directions. The real set, each verified against the source that produces it:

| Provenance | Producer | Verified at |
|---|---|---|
| `anonymous` | no credential presented | `trust/principal.hpp:70` |
| `credential_verified` | engine validated a credential it did not mint on request | ADR-021's mechanism |
| `host_asserted` | an in-process host said so | `trust/principal.hpp:49` |
| `operator_configured` | a trigger config names the principal it runs as | `020:139-141`, gated by 020 §7 G6 |
| `derived` | `derive_on_behalf_of` | `trust/principal.hpp:90` |
| `restored` | rebuilt from a checkpoint | `core/agent_session.hpp:784` |

**The rule that replaces §8.1a's**, and the one that kills S1: *provenance describes the origin of the
**identity**, never the last hop that carried it.* A credential the engine mints during an exchange
therefore **carries the provenance of the identity it was minted for**, so presenting it back yields
`host_asserted` — not `credential_verified`. That is the whole of S1's attack, closed by making
provenance a property that survives the round trip rather than one that resets at each verification.

Admissibility is then **per-surface and per-source, declared by the operator** — not a single
"must be `credential_verified`" test. That is what makes `operator_configured` (T2) and `derived`
(S10) expressible at all, and it is the honest shape: 020 §3b's triggered run and a delegated
sub-agent are both legitimate and neither presents a credential.

`restored` (S9) is the case that must fail closed: since `AgentSessionRecord` round-trips only
id/tenant (`core/agent_session.hpp:776-784`), a restored principal cannot claim any stronger
provenance than the record proves, and the operator declares which surfaces `restored` may reach.
This makes S9's dilemma a stated policy instead of a fabrication or a silent downgrade.

### 10.3 Withdrawals

- **Design G as specified is withdrawn** (S1). Its *shape* survives only under §10.2's carried-provenance
  rule, and is deferred to Tier 3 where it is actually needed.
- **`AuthorityRef` as specified is withdrawn** (S3, T3). If a per-request authority handle is needed at
  Tier 3, it either uses ADR-005's already-judged `CapabilityRegistry` or justifies not doing so; it
  does not get re-derived with dense guessable integers. §8.1's measured 104-byte result (§8.1) remains
  valid and reusable for whatever handle replaces it.
- **The absolute "engine determines every status" rule is withdrawn** (T6, T11). Replaced by: *the
  engine determines every status it can reach; the statuses it structurally cannot reach are
  enumerated, and that enumeration seeds §8.5's host-obligation list.* `405` (§10.0) joins the
  reachable set.
- **Claims 8 and 9 are withdrawn** (T1). 013 §7 Q2 chose `EvictAfter<N>` and states *"`Block` is not
  required for this need"* (`013:290-295`); claims written against that resolution replace claims
  written against its opposite. Per-seam policy naming is deferred to Tier 3 with the seam list from
  T1 as its starting point.

### 10.4 Tier 1, specified — CORRECTED by primary-source research

§10.4 originally specified Tier 1 as "a stdio transport behind `McpClient`'s `RequestSender`" and
flagged the harness's pipe direction as the one thing it did not know, requiring research before any
code. **That research was done and it falsified the transport choice**
(`docs/research/2026-08-15-mcp-conformance-harness.md`, primary source fetched from the tool's own
repository). The original text is replaced rather than annotated, because it described work that
would have been wasted.

**What the harness actually does.** It starts a test server for the scenario, spawns the client under
test as a subprocess, **appends the server's URL as an argument**, and sets `MCP_CONFORMANCE_SCENARIO`
in the environment. The client then connects **outbound over HTTP** to that URL. It does not speak MCP
over its own stdin/stdout. And the suite is **URL-based only for both roles — no stdio transport
anywhere.**

**Tier 1 gets cheaper, because the structural claim was about the absence of a listener, not about
stdio.** §10.1's actual claim — no listener, no host adapter, no fixture, no attribution apparatus —
is unaffected, and AgentEngine already owns a proven outbound HTTP path, so Tier 1 needs **no new
transport at all**:

| Piece | Status |
|---|---|
| Outbound plain-HTTP exchange (ADR-011, proven) | `sandbox/net_egress_proxy.hpp` |
| Not gated on `AGENTENGINE_WITH_HTTPS` — the plaintext branch "has no such gate" | `sandbox/provider_http_client.hpp:27-31` |
| The seam to wire it into | `protocol/mcp/client.hpp:48` |
| Executable-target precedent | `CMakeLists.txt:61` |

So Tier 1 is: `tools/agentengine_mcp_client.cpp`, reading the URL from argv and the scenario from the
environment, wiring `perform_http_exchange` into a `RequestSender`, driven by `conformance client`.

**One real obstacle, which doubles as a positive control.** The harness's URL is loopback, and
`is_blocked_address()` blocks 127.0.0.0/8 by design as ADR-011's anti-SSRF control
(`sandbox/net_egress_proxy.hpp:65-68`). Reaching it needs an **explicit** egress address policy — which
already exists and is already judged (ADR-016, whose own G2 gate is "the provider path reaches a
private/loopback address"). A Tier 1 run therefore proves both that the policy permits the intended
destination and, with the policy removed, that the SSRF block is real.

**§10.0's open question is now closed, and the answer removes Tier 2's gate.** `conformance server` is
URL-only, so **011 §10 G1 has no stdio escape hatch**: it genuinely requires an HTTP endpoint, i.e.
Tier 3. Stdio remains a real product surface (011 §7: *"stdio remains for local servers"*) but yields
**no conformance gate**, so Tier 2 is deprioritised to product work. T0's insight was right — stdio
was absent from the design space — and T0's conformance claim is now definitively dead. The M7 Phase G
audit's characterisation of **G1** as listener-blocked is correct; only its treatment of **G2** was
wrong.

### 10b. Falsifiable claims — Tier 1 only

Deliberately scoped to Tier 1. Writing Tier 3 claims now would repeat §8b's error of specifying
against an unbuilt surface. Every row carries a positive control, and every security-relevant row
carries an ADR-015 teeth arm (constraint 6).

| # | Claim | Disproving experiment | Positive control / teeth |
|---|---|---|---|
| 1 | `conformance client` runs against our binary and produces a published percentage per suite (`core`, `extensions`, `backcompat`, `auth`), pinned to a conformance release | Run it; a non-zero exit outside the justified baseline disproves | Control: the harness reports at least one PASS, proving it is really exercising us rather than failing at spawn |
| 2 | Every check the percentage counts is engine-attributable — no fixture in the loop | Inspect the invocation: the harness starts its own server and spawns our binary | **Teeth:** break one `McpClient` behaviour (e.g. `isError` surfacing, `client.hpp`) and the number must drop by ≥1 |
| 3 | The binary consumes the harness's contract correctly: server URL from the appended argv, scenario from `MCP_CONFORMANCE_SCENARIO` | Supply neither; the binary must fail loudly rather than default to a guessed endpoint | Control: with both supplied, a session completes |
| 4 | Reaching the harness requires an **explicit** egress address policy; loopback is not reachable by default | Run with the ADR-016 policy removed; the run must fail on an address-policy error, not silently succeed | **Teeth + control in one:** this is ADR-011's SSRF block proving it is real, and ADR-016's policy proving it is sufficient |
| 5 | The transport is a `RequestSender` and nothing else — no ambient state, no second path into `McpClient` | Inspection/`try_compile()`: `McpClient`'s only inbound seam remains `client.hpp:48` | Control: the real transport satisfies it |
| 6 | No credential or secret reaches stdout/stderr over a full suite run (018 §4) | Capture both streams; scan for a planted canary | **Teeth:** plant the canary in a header value and confirm the same scan trips, per ADR-015's precedent |

### 10c. What iteration 3 does not resolve

- **All 33 findings against §8** remain open for Tier 3. They are deferred, not answered — and the
  research in §10.4 confirms Tier 3 is the *only* path to 011 §10 G1, so they must eventually be
  answered rather than tiered around.
- ~~**Tier 2's gate status**~~ **Closed** by `docs/research/2026-08-15-mcp-conformance-harness.md`:
  the suite is URL-only, so a stdio server role yields no gate. Tier 2 is product work, deprioritised.
- ~~**Tier 1's pipe direction**~~ **Closed** by the same research — and it falsified §10.4's original
  stdio design, which is why it was the first task rather than an implementation detail.
- **§10.2's admissibility policy** needs a declaration surface the operator writes; that is 007 §5's
  rule language, which this project has repeatedly named as deferred and which iteration 3 does not
  build either.

## 11. Prove phase

*(not yet run — Tier 1's claims in §10b are what it runs against first)*

## 12. Decision

*(not yet judged — Tier 1 must be proven first; Tier 3 needs the 33 open findings answered)*
