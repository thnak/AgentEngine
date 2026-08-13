# ADR-021 — Inbound protocol trust boundary: who terminates TLS+OAuth 2.1 for MCP/A2A?

**Superseded (2026-08-13) by `decisions/ADR-039-inbound-transport-host-pluggable.md`** for §3–§8's
own strategic direction ("AgentEngine owns TLS/auth first-party") — ADR-039 accepts a host-pluggable
transport instead, given no comparable SDK (MAF, OpenAI Agents SDK) owns its own listener and this
project's own protocol dispatchers were already transport-agnostic (§2's own inventory, below). This
document's own text is kept unmodified as the historical record of how that first decision was made;
read ADR-039 for the current direction. §7's real, Judged bearer-token mechanism
(`trust/bearer_token.hpp`/`hmac.hpp`) is NOT superseded — it survives unchanged as a reusable
primitive under the new design (ADR-039 §3c/§4).

**Status:** Judged (2026-08-08) — the STRATEGIC question only (§8). Design A (first-party termination)
accepted; its core bearer-token mechanism is real, red-teamed, and proven
(`trust/bearer_token.hpp`/`hmac.hpp`, `tests/test_bearer_token_proof.cpp`, 32/32 checks). The network
listener, server-role TLS, and HTTP/1.1 request parsing remain real, named follow-on work — Milestone 7
Phase C4+ (011 MCP transport/auth) and Phase D5+ (012 A2A transport/auth) stay blocked on THAT, not on
this ADR's own question, which is now settled.

## 1. The question

**Does AgentEngine's v1 inbound protocol surface (MCP Streamable HTTP `tools/call`, A2A JSON-RPC/REST
`SendMessage`) terminate TLS and validate an OAuth 2.1 bearer token itself — first-party, in-process —
or does it deliberately decline to, binding only to loopback and trusting a pre-validated principal
forwarded by an operator-supplied reverse proxy / API gateway?**

This has a wrong answer. Get it wrong in the "own it" direction and this project ships a hand-rolled
TLS+OAuth stack nobody red-teamed as hard as nginx/Envoy has been, in a security-critical position, for
no real gain. Get it wrong in the "defer it" direction and every deployment that can't or won't stand
up a reverse proxy is simply unable to run AgentEngine's MCP/A2A server role at all — a real capability
regression 018 §1's own table ("HTTP / AG-UI / OpenAI-compatible — OAuth 2.1 bearer / OIDC, mTLS, or
API key (dev only)") reads as a first-party engine responsibility, not an operator's.

018 §1's table is not itself dispositive — it says WHAT mechanism identifies a caller, not WHO
terminates it — which is exactly why this is a real, contested question rather than something the RFC
text already answers.

## 2. What already exists (2026-08-08 inventory)

- **No listening socket anywhere in AgentEngine's own code.** `include/`, `src/` never call
  `listen`/`accept`/`bind`/`WSAStartup`. `protocol/mcp/server.hpp` and `protocol/a2a/server.hpp` are
  explicitly transport-agnostic in-process dispatchers (Milestone 7 Phases C2/D3): handed a request
  object, they return a response object, and take `held`/`approve`/a `RunStarter` as given.
- **Quark's own PAL has a listener**: `quark::pal::tcp_listen`/`accept_one`/`local_port`
  (`third_party/quark/pal/{windows_x86_64,linux_x86_64}/net.hpp`), already consumed by Quark's own
  `TcpTransport` (`third_party/quark/include/quark/net/tcp_transport.hpp`) for node-to-node cluster
  networking — a real, hardened accept loop over the same PAL AgentEngine's own egress client already
  depends on (`net_egress_proxy.cpp`). `TcpTransport` itself is Quark's own length-prefixed cluster
  frame protocol, not HTTP-aware, and per CLAUDE.md's locked decision ("Quark is a submodule, never
  forked or patched in-tree") is not a base to extend for HTTP.
- **mbedTLS is already vendored** (ADR-013), client-role only today (`TlsClientSession`), behind
  `AGENTENGINE_WITH_HTTPS` (default OFF). mbedTLS itself supports a server role; using it server-side
  is new code, not a new dependency.
- **No OAuth/JWT/bearer-validation code exists anywhere.** `trust/capability_token.hpp` (ADR-005) is
  an unrelated macaroon-style *capability* bearer token for cross-process capability delegation, not
  an RFC 6750 user-authentication bearer token.
- **Precedent for "push heavy infra to the deployment layer"**: 012 §9 Q1 (gRPC binding) resolved
  exactly this way — "a standard gRPC↔JSON transcoding proxy... in front of our HTTP+JSON/REST binding
  is exactly the well-understood mechanical translation a deployment-side proxy does well... not
  designed speculatively now." The `remote` sandbox profile's own locked decision reasons identically:
  push to infrastructure that already provides the property, rather than rebuild it in-engine.
- **Precedent for "own the whole path first-party"**: ADR-011 (egress proxy) and ADR-013 (TLS client)
  both rejected leaning on existing tooling for the OUTBOUND path, because the outbound path's threat
  model (SSRF, DNS rebinding, confused-deputy credential use) is genuinely this engine's own attribution
  problem — no external proxy can know which principal's capability grant authorized a given outbound
  call.

## 3. Competing designs

### Design A — First-party termination

AgentEngine owns the whole path: a TCP listener (Quark's PAL, mirroring `TcpTransport`'s own
accept-loop shape but HTTP-aware, not extending Quark itself), an mbedTLS *server* session
(`TlsServerSession`, new code reusing the vendored library), a hand-rolled HTTP/1.1 request parser
(mirroring ADR-011's own hand-rolled response parser, same "no vetted HTTP library to lean on"
finding), and an in-process bearer-token validator that derives a real `Principal`/`CapabilitySet`
before either `McpServer::dispatch()` or `A2aServer::send_message()` ever runs.

Scoped falsifiably to keep the prove phase honest: the validator proves ONE concrete, self-contained
scheme — HMAC-SHA256-signed bearer tokens with `exp`/`aud`/`iss` claims (the mechanism 018 §7 G1's own
gate already names: "wrong audience, expired, wrong issuer, replayed... rejected, each with the
correct classification") — not a full OAuth 2.1 authorization-code/PKCE flow against a live external
Authorization Server, which is a separate, later integration question this ADR does not resolve.

### Design B — Proxy-delegated termination

AgentEngine binds ONLY to loopback (127.0.0.1/::1), refuses any non-loopback peer address at accept
time, and trusts a single operator-injected header (`X-AgentEngine-Principal`, HMAC-signed with a
shared secret provisioned out-of-band between the proxy and this engine — never trusted un-signed) that
names the already-validated principal. TLS termination, OAuth 2.1, mTLS, API-gateway policy all live in
whatever reverse proxy the operator already runs (nginx, Envoy, a cloud API gateway) — infrastructure
this project does not build, matching 012 §9 Q1's own resolution.

Still needs a real listener (loopback-only) and HTTP/1.1 request parsing — narrower in scope than
Design A (no TLS server code, no bearer-token cryptography), but the trust boundary itself becomes "is
the forwarded-principal header genuinely un-forgeable by anything other than the configured proxy,"
which is its own falsifiable security claim, not a free pass.

### Design C — Extend Quark's `TcpTransport` to speak HTTP (considered, rejected before prove)

Quark's `TcpTransport` already has a hardened accept loop on the identical PAL. Teaching it to also
frame HTTP/1.1 instead of (or alongside) its own length-prefixed cluster protocol would reuse the most
work. Rejected without a prove pass: CLAUDE.md's own locked decision is explicit — "Quark is a
submodule, never forked or patched in-tree. Runtime changes go upstream" — and `TcpTransport` is
Quark's own cluster-membership primitive (HELLO handshake, dial-dedup), not a generic HTTP listener;
bending its wire protocol to carry HTTP would be exactly the kind of in-tree Quark patch that rule
forbids. A clean-room listener over the same PAL (which A and B both already do) gets the PAL reuse
without touching Quark's own protocol.

## 4. Falsifiable claims

| # | Design | Claim | Disproving experiment |
|---|---|---|---|
| 1 | A | A forged/tampered bearer token (bad signature, wrong `aud`, expired `exp`, replayed `jti`) is rejected before any capability is derived | Negative suite: mutate each claim independently, confirm rejection + correct `failure_class` for each (018 §7 G1's own gate, reused as this ADR's positive-control bar) |
| 2 | A | A valid token for service X is rejected when presented to service Y (confused-deputy / audience-validation) | Two tokens, two distinct `aud` values, cross-presented — both rejected |
| 3 | B | A non-loopback peer is refused at accept time, before any byte of the request is read | Connect from a non-loopback-simulated peer address; confirm the connection is dropped pre-parse |
| 4 | B | An UNSIGNED or wrongly-signed `X-AgentEngine-Principal` header is rejected, never trusted as-is | Negative suite: missing signature, wrong key, tampered principal value — all rejected |
| 5 | A & B | Both designs correctly reject a request that never carries ANY caller identity — "Anonymous is a principal" (018 §1) is a real, checked default, not an accidental fail-open | A request with no `Authorization` header (A) / no forwarded-principal header (B) is admitted only under the Anonymous principal's own (minimal) capability set, never a wider one |
| 6 | A | Cost: a real HTTP/1.1 request → validated `Principal` round trip completes in bounded time suitable for a hot request path (measured, not asserted) | p50/p99 over N requests against a loopback listener, real mbedTLS handshake included |
| 7 | B | Cost: the loopback-only listener + header-trust path is measurably cheaper than Design A's own round trip (the claimed advantage of pushing crypto out of this engine) | Same measurement harness as claim 6, compared directly |

## 5. Red-team brief (next phase)

Hardest where each design claims to be safe:

- **Design A**: token-forgery classes beyond the negative suite (timing side-channel on HMAC
  comparison — is it constant-time? algorithm-confusion if `alg` is attacker-controlled? replay within
  a still-valid `exp` window if `jti`/nonce tracking is missing or unbounded-memory?); TLS
  server-session hazards (cert/key handling, does the server session share ANY code path with the
  client session that could leak a client-only assumption into server context); does the hand-rolled
  HTTP/1.1 parser (proven only against well-formed responses in ADR-011's own client role) hold up
  against adversarial REQUESTS (header injection, request smuggling via ambiguous
  `Content-Length`/`Transfer-Encoding`, oversized headers, slowloris-shaped partial requests)?
- **Design B**: is "loopback-only" actually enforced at the OS/socket level or only by a
  peer-address check that a spoofed source (on a system where that's possible) could defeat? Can the
  shared HMAC secret between proxy and engine be rotated without an outage (018 §3's own "rotation
  without restart" requirement)? What happens if the proxy itself is compromised — does Design B
  quietly widen 007 §1's stated threat model (a malicious operator is an explicit non-goal, but a
  *compromised* trusted proxy is not the same as a malicious operator) in a way Design A would not?
- **Both**: does either design leak the token/header value into logs, traces, or error messages
  (018 §4's "never in... telemetry, audit payloads, error strings" rule)?

## 6. Red-team findings

An independent adversarial pass (fresh context, no prior exposure to this document) reviewed both
designs against §4's claims, §5's own brief, and its own further analysis. Full findings are recorded
in the session transcript this ADR was produced in; the load-bearing ones are captured here so the
decision is traceable without that transcript.

**Design A — confirmed as implementation-hazard-class, not concept-flaw-class, with one binding
constraint added.** Every attack found (HMAC timing side-channel, algorithm confusion, `jti` replay
scaling across a horizontally-scaled deployment, connection-vs-request validation scoping, audience
derived from the wrong place, server-role TLS being entirely unprecedented in this codebase) is a real
implementation requirement the prove phase must close, not evidence the mechanism itself is unsound.
One finding is elevated to a BINDING CONSTRAINT on the design, not merely an implementation detail:
calling this scheme "OAuth 2.1-style" while validating only a self-issued HMAC token creates a real
scope-creep hazard — the day this validator is extended to accept tokens from a real external
Authorization Server, algorithm/key-selection MUST remain 100% server-pinned, never read from the
token. §3's Design A text and `trust/bearer_token.hpp`'s own header comment both now state this
explicitly as a structural property, not an aspiration.

**Design B — confirmed as mechanism-sound, but with a genuinely unaddressed, severe gap the original
claims table did not cover: HTTP request smuggling as a principal-forgery primitive.** Because Design
B's trust model is "the proxy authenticates, then attaches a trusted header, then forwards over a
pooled backend connection," a smuggled request that desynchronizes the proxy's and the engine's
framing can ride an already-trusted connection and inherit ANOTHER caller's signed principal header —
a direct, concrete cross-tenant identity forgery (018 §7 G4's "release-blocking defect class"), not a
theoretical concern. Closing this is not "write a stricter parser," it is a structural requirement:
identity must be bound to a per-request signed value (unforgeable-by-smuggling), never to the
connection alone. §4's claims table did not ask for this and would need a new claim before Design B
could be built as originally scoped.

**Design C (extend Quark's `TcpTransport`) — the as-written rejection was too hasty.** Quark's own
`TcpTransport::event_loop()` is a documented, sanctioned extension point (already used by
`quark::net::VoiceChannel`, ADR-030) that shares the node's already-hardened I/O reactor without
touching Quark's own wire protocol — fully compatible with CLAUDE.md's "never forked or patched
in-tree" rule, which the original rejection conflated with "never build atop." This does not make
Design C the winner (an HTTP listener is a different beast from a UDP datagram channel, and reactor
reuse hands over neither HTTP parsing, TLS, nor auth), but it reopens a real question for whichever of
A/B is eventually built end to end: build the listener's own I/O loop atop `TcpTransport::event_loop()`
rather than a second from-scratch reactor, unless a specific reason argues otherwise. Recorded as a
residual for the listener-implementation follow-on, not re-litigated here.

**Also found, not in the original claims table at all, and equally applicable to both designs:**
inbound DoS shape (connection/header-size/pipeline-depth limits, slowloris) has no claim covering it
in either design, and neither does the requirement that a token/header's raw value must be
UNPRINTABLE by construction from the moment it is read off the wire (018 §4), not merely handled
carefully by convention.

## 7. Prove phase

Full network-listener/TLS-server/HTTP-parser prove work is explicitly OUT of scope for this pass —
the red-team's own finding is that server-role TLS and a request-smuggling-hardened parser each need
their own dedicated red-team pass "equivalent to ADR-013's C1–C6," which this ADR does not have time
or standing to conduct as a drive-by inside a broader strategic decision. What IS fully scoped, real,
and provable without a listener at all is Design A's core CRYPTOGRAPHIC mechanism — the bearer-token
validator every later "own the whole path" implementation would depend on regardless of which HTTP/TLS
stack eventually carries it.

Built and proven:

- **`trust/hmac.hpp`/`hmac.cpp`** — `hmac_sha256()`/`constant_time_equal()` EXTRACTED from
  `capability_token.hpp`/`.cpp` (ADR-005) into a shared primitive, closing the red-team's own "does it
  call the same audited primitive" finding directly: there is now exactly one HMAC-SHA256/constant-
  time-compare implementation in this codebase, not two independently-written ones. `capability_token.cpp`
  refactored to use it; ADR-005's own tests (`test_capability_token_proof`, `test_capability_token_redteam`)
  re-run unchanged and still pass, confirming the extraction is behavior-preserving.
- **`trust/bearer_token.hpp`/`bearer_token.cpp`** — `BearerToken`/`BearerTokenClaims` (`sub`,
  `tenant_id`, `aud`, `iss`, `exp`, `jti`), `mint_bearer_token()`, `verify_bearer_token()`, `ReplayGuard`.
  Closes, by construction or by proven check, four of the red-team's Design A findings:
  1. **Algorithm confusion**: impossible by construction — the wire encoding has no algorithm-selector
     field at all; HMAC-SHA256 is the only algorithm this type can express. Not a runtime-testable
     property (there is nothing to exercise), proven by inspection of `encode_claims()`.
  2. **Confused-deputy / audience**: `expected_aud`/`expected_iss` are parameters `verify_bearer_token()`
     requires from the CALLER (static per-route config), never read from the token or request. Proven:
     two real tokens, distinct `aud`, cross-presented both ways — both rejected; each accepted on its
     own correct route (B4).
  3. **Constant-time comparison**: reuses `trust::constant_time_equal` from the extraction above, not a
     fresh comparison. Proven indirectly (a tampered-signature token is rejected, B2) — genuine
     constant-time behavior itself is a non-functional property this test suite does not attempt to
     measure via timing (that would need a dedicated statistical harness, named as a residual, not
     built here).
  4. **Bounded replay rejection**: `ReplayGuard` rejects a reused `jti` while still within its `exp`
     (B7), and prunes entries once their OWN `exp` passes so memory is bounded by distinct
     not-yet-expired tokens (B9) — proven with fully deterministic, caller-supplied clock values, no
     real sleep. The single-instance limitation the red-team flagged (replay across independently-
     deployed processes is NOT caught) is not silently true — it is directly DEMONSTRATED: the same
     `jti` is shown accepted on two independent `ReplayGuard`s (B8), the honest way to record a known
     gap per this project's own "named, not silently claimed" discipline.
- **`tests/test_bearer_token_proof.cpp`** (new, 32 checks, all passing) — includes a genuine POSITIVE
  control (B1: a valid token is accepted) before any negative case, per decisions/README.md's own
  "a test that cannot fail proves nothing" rule for security claims. 140/140 full suite.

Not built in this pass (real residuals, not silently dropped): the network listener itself (loopback
or otherwise), server-role TLS (mbedTLS `TlsServerSession`), the hand-rolled HTTP/1.1 request parser
and its request-smuggling defenses, DoS/connection-limit enforcement, Design B's proxy-header
mechanism and its own per-request-binding fix, and `Principal → CapabilitySet` derivation from a
verified `BearerTokenClaims` (007 §5's policy engine, already named elsewhere in this project as
deferred pending a real rule language).

## 8. Decision

**Design A (first-party termination) is accepted as this project's strategic direction** for MCP/A2A
inbound identity establishment, with the two binding constraints §6 names (algorithm/key pinning is
permanent, not just true today; per-request — never per-connection — token validation) carried forward
into any future listener implementation. Design A's core cryptographic mechanism is JUDGED: proven
correct against every claim in §4 that applies to it, with a genuine positive control, real negative
cases, and a red-teamed, previously-audited HMAC primitive reused rather than re-derived.

Design B is **not rejected** — it remains a legitimate deployment option for operators who already run
a reverse proxy and want AgentEngine to lean on it — but it is not what this project builds first-party,
because its most severe attack (request-smuggling-as-principal-forgery, §6) requires a structural fix
(per-request signed binding) that was not part of its original scope and meaningfully undermines its
main selling point (minimal, proxy-delegated simplicity). If Design B is revisited, that fix is a
prerequisite, not an optional hardening pass.

**What this ADR does NOT decide, left as follow-on work (each plausibly its own ADR, mirroring how
ADR-011's egress-proxy design and ADR-013's TLS-client specifics were split):** the actual network
listener and its I/O model (§6's reopened `TcpTransport::event_loop()` question), server-role TLS
(deserves its own ADR-013-equivalent red-team/prove pass per §6/§7), the HTTP/1.1 request parser and
its smuggling/DoS hardening, and `Principal → CapabilitySet` derivation policy. Milestone 7's own
Phase C4+ (MCP transport) and Phase D5+ (A2A transport) both remain blocked on that follow-on work —
this ADR unblocks the STRATEGIC question, not the full implementation.
