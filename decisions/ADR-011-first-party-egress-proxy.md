# ADR-011 — What should the first-party host-mediated egress proxy be, so that `NetOut`'s allowlist, byte cap, and method restrictions are real controls a guest cannot route around, not assertions in prose?

**Resolves:** `008-Sandbox-and-Isolation.md` §10 Q3's already-RFC-resolved shape ("a real first-party
default, and a seam a deployer may replace") — this ADR is what makes that real, mechanically, for
M2's actual consumer: `wasm`'s `http-request` host import (`src/backends/wasm/wasm_backend.cpp`),
currently a stub that traps `"http-request: not implemented in M2's minimal host"`. Scoped per
[docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md](../docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md)
Phase F task F1.

**Why this needed the full cycle, not an ordinary task:** F1's own sizing ("XL, security-critical:
host-mediated egress for every profile depends on it being right") and 008 §4's own rule that egress
is *always* host-mediated — this is the one component every profile's `NetOut` enforcement collapses
onto. It is also the first genuinely new networking code in this codebase (confirmed by investigation:
no HTTP/TLS/socket library is vendored anywhere in AgentEngine; `native-jail` denies all networking
outright on both platforms with no partial path to extend).

## 1. The question

`cap::NetOut{host_allowlist, byte_cap, method_restrictions}` (007 §3, ADR-009) is already a real,
parameterized, attenuation-checked capability type — `CapabilitySet`/`BoundCapability` already make
*holding* it or not a structural fact. What's missing is what happens once a guest holding a bound
`NetOut` actually calls `http-request` (`wit/ae-tool.wit` — note the guest supplies only `method`,
`path`, `headers`, `body`; **host, port, and scheme come entirely from the capability grant, never
from the guest** — 006 design choice already stated in the WIT file's own capability-interface
banner: "the guest supplies only the part of a call the host must still validate per-call... never
the grant's own parameters"). The question: what does the host actually do with that call so that
008 §7's named abuse cases (SSRF to link-local/metadata endpoints, DNS-rebinding around an egress
allowlist) are genuinely contained, not merely declared contained.

## 2. Background this design must respect

- **007 §3 property 5:** "`NetOut` without an allowlist is not a capability, it is a hole" —
  parameterized, not boolean.
- **008 §7's abuse-case list**, verbatim: "DNS-rebinding around an egress allowlist · SSRF to
  link-local metadata endpoints (`169.254.169.254` and friends — blocked by default in every
  profile)."
- **008 §10 Q3's resolution text**, which already commits this ADR to a specific mechanism class:
  "host/port/scheme allowlist enforcement, blocking RFC 1918/link-local/metadata ranges, re-resolving
  and re-checking addresses post-connect against DNS rebinding," framed as "ordinary host-side
  socket/DNS logic" that does *not* need a vetted crypto library — that reasoning is about the
  allowlist/DNS mechanism, not license to roll a TLS stack (§3's scope cut makes this explicit).
- **008 §2a's seam pattern** (`SandboxBackend` — a concept, first-party defaults ship, a deployer's
  own conforming type works unmodified): Q3's resolution explicitly says the egress proxy "stays a
  seam underneath (the same `SandboxBackend`-is-a-concept pattern §2a already uses)".
- **No HTTP/TLS/socket library is vendored anywhere in this repository** (confirmed by direct
  investigation — no cpp-httplib/curl/asio, no concrete `ChatClient` implementation either). Quark's
  `secure_transport.hpp`/mbedTLS adapter exists but is built for node-to-node cluster mTLS identity,
  not general outbound HTTPS with ordinary CA validation — not reusable here without its own design
  work.
- **`third_party/quark/pal/*/net.hpp`** already provides a proven, cross-platform (epoll/WSAPoll),
  IPv4-only, non-blocking raw-socket primitive set (`tcp_connect`/`connect_result`/`send_some`/
  `recv_some`/`ensure_winsock` on Windows) — "reuse where it already exists rather than reinventing"
  (ADR-009 §2's own stated principle). No DNS resolution exists in the PAL (cluster peers are
  configured by raw address, never hostname) — that part is genuinely new.
- **CLAUDE.md machine safety / "no backend is permitted to weaken the contract by configuration"**
  (008 §2): a byte cap must actually bound host-side memory while reading, not just get checked after
  the fact once a response is fully buffered.
- [`docs/research/2026-08-05-ssrf-dns-rebinding-defense.md`](../docs/research/2026-08-05-ssrf-dns-rebinding-defense.md):
  allowlists alone don't close DNS-rebinding (the allowlist gates the *hostname*, not what it
  resolves to); resolve-once-connect-to-the-verified-literal-address closes the TOCTOU by
  construction; blocked-range checks must run on the resolved binary address (`inet_pton`/`getaddrinfo`
  output), never a string, to be immune to the whole decimal/octal/hex/IPv4-mapped-IPv6 encoding-bypass
  class.

## 3. The competing designs

### Design A (accepted) — `NetEgressBackend` concept + `HostEgressProxy`: resolve-once-connect-to-verified-IPv4-literal, plain HTTP only, no redirect-following

`include/agentengine/sandbox/net_egress_proxy.hpp` declares a `NetEgressBackend` concept (mirrors
`SandboxBackend`'s shape) and `HostEgressProxy`, its first-party default. `fetch(NetEgressRequest,
cap::NetOut const& granted)`:

1. **Single-target grant only.** `granted.host_allowlist` must have exactly one entry (`"host:port:
   scheme"`) — the WIT contract gives the guest no way to pick among several, so a multi-entry grant
   at this call site is ambiguous, and ambiguous is rejected, not resolved by picking `[0]` silently
   (that would be a real, if narrow, I2 gap the moment a multi-entry grant becomes real).
2. **Scheme gate.** Only `scheme == "http"` proceeds. `https` is rejected with a distinct, honest
   error — never silently served over plain HTTP, never silently accepted and TLS-skipped.
3. **CRLF/injection gate**, before any network activity: `method`, `path`, and every header
   name/value are rejected outright if they contain `\r` or `\n` — this proxy builds the raw HTTP/1.1
   request line and header block itself (no vetted HTTP library exists to lean on), so this is the
   only thing standing between a guest-controlled `path`/header value and request-splitting/header
   injection onto the wire.
4. **Method-restriction gate**, before any network activity: if `granted.method_restrictions` is
   non-empty, `req.method` (case-normalized) must be a member.
5. **`resolve_and_validate(host, port)`** — exactly one `getaddrinfo(..., AF_INET, ...)` call (an
   `inet_pton` fast path first, so a numeric-literal allowlist host never touches the resolver at
   all); every candidate is checked by `is_blocked_address` (a pure function over the resolved 32-bit
   address, never a string) against loopback, link-local (`169.254.0.0/16`, which contains the
   metadata address), RFC 1918 private ranges, CGNAT (`100.64.0.0/10`), multicast, reserved, and
   unspecified; the first non-blocked candidate is returned as a `VerifiedEndpoint{ip, port}` — a
   plain value type, not a hostname — or the call fails closed (`host_unresolvable` / all candidates
   `address_blocked`).
6. **`perform_http_exchange(VerifiedEndpoint, NetEgressRequest)`** connects (via `quark::pal`'s
   primitives — no raw socket API touched directly), sends the request, and reads the response with a
   response-size ceiling enforced **during** the read loop (`min(granted.byte_cap.value_or(∞), a
   16 MiB hard host-side ceiling that applies regardless of the declared cap)** — never buffered
   first and checked after. `VerifiedEndpoint` is the *only* thing this function can connect to; there
   is no parameter path by which it could re-resolve a hostname, so a second, attacker-timed
   resolution is not merely avoided by discipline — it has no expressible call.
7. **No redirect-following.** A 3xx response is returned to the caller exactly as received. Following
   a redirect would mean issuing a second request to a target the allowlist never actually checked
   (the `Location` header is remote-controlled) — a well-known SSRF sub-class. Not implementing
   redirect-following removes the class entirely rather than requiring a second, harder-to-verify
   re-validate-per-hop mechanism.

`resolve_and_validate`'s resolver is an injectable member of `HostEgressProxy` (defaults to the real
function) purely for testability: every network-reachable address inside this project's test
environment is itself loopback or RFC 1918 — exactly what production must always block — so proving
`fetch()`'s *post-resolution* composition (does it correctly call `perform_http_exchange` with
whatever `resolve_and_validate` decided) needs a way to supply an endpoint without asking production
code to trust an unblockable address. The injection point answers "what does this hostname resolve
to," the same question DNS answers; it does not skip `is_blocked_address`, which is proven exhaustively
on its own, as a pure function, with zero network dependency.

### Design B (rejected by design-level reasoning) — transparent redirect-following with per-hop re-validation

Follow `Location` redirects up to a small bound (e.g. 5), re-running the full allowlist/resolve/
validate pipeline on each hop.

**Why rejected without implementation:** it adds a second, harder-to-get-right code path (every hop
needs the identical CRLF/scheme/blocked-address gate the first request got, and an off-by-one or
early-return bug in a rarely-exercised loop is exactly the kind of thing that looks correct in a
casual read and isn't) to close a case that not implementing removes outright. §7's own abuse-case
list does not name "SSRF via redirect" as something this milestone must specifically handle beyond
"contain it" — and the simplest way to contain it is to never traverse it automatically. A caller
that legitimately needs to follow a redirect issues a new `http-request` call against the
`Location` target, which goes through the exact same, single, already-proven gate.

### Design C (rejected) — reuse Quark's `SecureTransport`/mbedTLS adapter as the HTTP(S) client

Quark's `secure_transport.hpp` exists and is vendored; reusing it would give TLS "for free."

**Why rejected:** it is built for node-to-node cluster mTLS — both peers hold node identities and
verify each other's certificate against the cluster's own trust material (021, ADR-040 in Quark).
Fetching an arbitrary agent-declared host (`api.search.example`) needs ordinary server-certificate
validation against the system CA trust store, a different trust model entirely; adapting
`SecureTransport` to that job is itself a real, separate design question (and CLAUDE.md's "Quark is a
submodule, never forked or patched in-tree" means any gap found while trying would have to go
upstream first). Out of scope for this ADR; named as the concrete follow-up once HTTPS is taken on.

## 4. Falsifiable claims (Design A)

| # | Claim | Disproven by |
|---|---|---|
| C1 | A `NetOut` grant whose `host_allowlist` does not have exactly one entry is rejected before any network activity. | A 0- or 2+-entry grant silently proceeding (e.g. using entry `[0]`). |
| C2 | `fetch()`'s post-resolution composition works: given a resolved, non-blocked endpoint, the real request is sent and a real response is parsed back correctly (status, headers, body). | The composed call failing or returning a malformed/empty response despite a working target. |
| C3 | A `scheme` other than `http` (in particular `https`) is rejected with a distinct error, never silently served over plain HTTP or silently accepted without TLS. | An `https` entry connecting in plaintext, or succeeding at all. |
| C4 | Every named blocked range (loopback, link-local incl. exactly `169.254.169.254`, the three RFC 1918 ranges, CGNAT, multicast, reserved, unspecified) is rejected — connect never attempted. | Any one range, tested individually, reaching a connect attempt. |
| C5 | The blocked-range check runs on the resolved binary address, never a string — a non-canonical numeric encoding (decimal/octal/hex) of a blocked address is rejected identically to its canonical form. | A decimal/octal/hex-encoded blocked address being treated as an unrecognized (allowed) hostname. |
| C6 | The address `perform_http_exchange` connects to is always the literal `VerifiedEndpoint` `resolve_and_validate` returned — structurally, no second hostname-based resolution is expressible between validation and connect. | Any call path in which a hostname (not a `VerifiedEndpoint`) reaches the connect step. |
| C7 | `method_restrictions`, non-empty, rejects a method not in the list; empty means unrestricted (matches `cap::NetOut`'s own documented default). | A restricted grant accepting a non-listed method, or an unrestricted grant rejecting any method. |
| C8 | A response body is never buffered past `min(granted.byte_cap, 16 MiB)` — the read loop aborts mid-stream, not after full buffering. | Host memory/buffer size exceeding the effective cap at any point during the read, even transiently. |
| C9 | `path`, `method`, and header names/values containing `\r` or `\n` are rejected before any byte reaches the socket. | A CRLF-bearing value reaching the wire, in the request line, a header, or splitting into a second request. |
| C10 | A 3xx response is returned to the caller unmodified; no second request is ever issued to a `Location` target. | Any evidence (a second connection, a second request on the wire) of the proxy following a redirect. |
| C11 | `NetEgressBackend` is a structural concept, not `HostEgressProxy`-only — a conforming third-party type satisfies it with no engine change. | `static_assert` failure, or any part of the call path requiring `HostEgressProxy` by name rather than the concept. |

## 5. The red-team attack

- **R-C4/C5 (the main attack):** a blocked-range check that gets the three RFC 1918 ranges right but
  is sloppy about the exact metadata address, or that canonicalizes-then-string-matches instead of
  comparing the resolved 32-bit integer directly, would pass a casual spot check. The test corpus
  exercises all seven range classes as **independent** cases (not one combined "internal-ish address"
  case) plus `169.254.169.254` by its exact value (not merely "some `169.254.0.0/16` address"), plus a
  decimal-encoded loopback literal (`2130706433`) as an allowlist host — proving the encoding-bypass
  class named in the research note doesn't apply, because `inet_pton`'s strict dotted-quad-only
  parsing (not the lenient legacy `inet_addr`) rejects the non-canonical form outright rather than
  needing to be defended against downstream.
- **R-C1 (ambiguous grant):** a `host_allowlist` with two entries pointing at *different* hosts is the
  sharper version of this attack — silently picking `[0]` would work today (only one entry is ever
  declared) and quietly become a real gap the day a multi-entry declaration exists, so this is tested
  now rather than left for whoever adds multi-entry grants to discover the hard way.
- **R-C8 (byte cap / host memory DoS):** a target that sends more than the cap in many small chunks,
  proven by asserting the proxy's own peak buffered size never exceeds the effective cap plus one read
  chunk — not merely that the *final returned* body respects the cap (which a "buffer everything, then
  truncate" implementation would also satisfy while still having transiently over-allocated). Also
  covers the **no-declared-cap** case: a target sending well past the 16 MiB hard ceiling with
  `granted.byte_cap = std::nullopt` must still be cut off — an absent guest-declared cap is not host-
  side "no protection at all."
- **R-C9 (the actual point of validating before any I/O):** `path` set to
  `"/x HTTP/1.1\r\nX-Injected: evil\r\n\r\nGET /y HTTP/1.1"` and, separately, a header value carrying
  embedded CRLF — both must be rejected at the gate, proven by a socket-level assertion that **zero
  bytes were ever written** for the rejected call (not merely that the malicious header didn't appear
  in the parsed response — a `perform_http_exchange` that "sanitizes" by stripping CRLF instead of
  refusing would still pass a weaker test that only inspects the response).
- **R-C10:** a local test server that returns a 302 with a `Location` pointing at a second local
  server; the test asserts the second server received **zero connections** — a redirect-following bug
  that only "mostly" follows (e.g. follows text-based redirects but the test only checks the header
  is preserved) would pass a check that only inspects the returned `NetEgressResponse`.
- **Path-boundary non-issue, noted for completeness:** unlike ADR-009's `path_prefix_covers` (which
  had a real boundary bug class to defend against), this design has no path-prefix matching at all —
  the allowlist is an exact `host:port:scheme` match, not a prefix — so that attack class does not
  apply here by construction, not by a defended check.

## 6. Executed evidence

**A real bug found and fixed during this pass, not anticipated in the design:** the first live-socket
test run against `perform_http_exchange` failed both the positive-pipeline and G2-positive-control
cases with `net.connect_failed: recv failed`. Root cause: `TestHttpServer` (the test double) wrote its
canned response and closed the accepted socket without ever reading the client's request bytes first —
closing a socket while inbound data is still unread in the kernel receive buffer sends a TCP RST
instead of a graceful FIN, which surfaced on the *client* side (this proxy's own `perform_http_exchange`)
as a spurious `ECONNRESET` mid-read of an otherwise-successful response. Fixed by draining the request
(reading until the `\r\n\r\n` header terminator, bounded) before writing the response, in the test
server only — `net_egress_proxy.cpp` itself was never the bug. Recorded here because it is exactly
the kind of finding this project's ADRs track even when it lands in test infrastructure, not product
code (ADR-010 §7.5's own precedent for this).

**Windows, MSVC 19.51.36252, Visual Studio 18 2026 generator, x64, Debug, `-j4`:** full repo build
including `-DAGENTENGINE_WITH_WASM=ON` (`cmake --build build --config Debug -j4`) — clean, zero
errors. Full `ctest -C Debug -j4`: **31/32 tests pass**, the one failure being
`test_native_jail_backend_windows`'s pre-existing, independently-tracked Job-Object memory-limit
timing flake (unrelated to this change — a different backend, `native-jail`, confirmed by re-running
it standalone, same failure signature as documented before this ADR). `test_net_egress_proxy`: **all
checks pass**, reconfirmed over **8 consecutive standalone runs** with zero flakes. `test_wasm_backend`
(updated for the real `cb_http_request` — see below): **all checks pass**.

**Linux, Docker `gcc:14`, g++ 14.4.0, Ninja, Debug, `-j4`** (the established M0-on substitute for a
real CI run — no git remote exists for GitHub Actions to run against): full source tree copied via
`tar` (excluding `build*`/`.git`), configured and built clean with `-DAGENTENGINE_WITH_WASM=ON`
(reusing the already-compiled `.wasm` fixture from the bind-mounted filesystem — WASM bytecode is
platform-independent, so no Rust/cargo-component toolchain install was needed). Full `ctest -j4`:
**23/23 run tests pass** (1 expected skip, `test_shell_runner_no_process_creation` — `llvm-nm` not
installed in this container, pre-existing and unrelated). `test_net_egress_proxy`: **all checks pass**,
reconfirmed over **10 consecutive standalone runs** with zero flakes.

**`test_wasm_backend.cpp` update, not a new bug in `net_egress_proxy`:** the pre-existing
`http-request/right-kind` probe asserted the wasmtime-level call itself would *fail* (`!result.has_
value()`), because under the old stub every gated callback trapped once its capability-kind check
passed. `cb_http_request` no longer traps for a policy rejection — it returns a real `Err(...)` value,
which the fixture's own `"fetch"` arm already handles without panicking (`Err(_) => text_result("fetch:
http-error returned")`, written during D5, before this ADR existed). The probe was rewritten to assert
the new, correct behavior: the call succeeds structurally (no trap) and the real gate is what rejected
an empty/default `cap::NetOut{}` (ADR-011 claim C1's ambiguous-grant case). `http-request/wrong-kind`
was unchanged — the capability-kind check still traps before `net_egress_proxy` is ever reached.

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 | **CORRECT** | `test_net_egress_proxy.cpp` zero/two-entry-allowlist blocks (resolver-call-count == 0); `test_wasm_backend.cpp`'s rewritten `http-request/right-kind` (empty `cap::NetOut{}` rejected end-to-end through the real WASM guest, not just the C++ unit test) |
| C2 | **CORRECT** | "fetch() succeeds end-to-end..." block, both platforms |
| C3 | **CORRECT** | https-rejected block (resolver-call-count == 0) |
| C4 | **CORRECT** | `is_blocked_address` block, all seven range classes plus the exact metadata address, individually |
| C5 | **CORRECT** | `inet_pton` strictness mechanism-check block (decimal/octal/hex all rejected at parse, not filtered downstream) |
| C6 | **CORRECT** (structural) | `VerifiedEndpoint` is the only type `perform_http_exchange` accepts — no hostname-accepting overload exists; reinforced by C2/positive-control both exercising the real call path end-to-end |
| C7 | **CORRECT** | method_restrictions disallowed/allowed/unrestricted blocks |
| C8 | **CORRECT** | small-declared-cap and no-declared-cap (hard 16 MiB ceiling) blocks, both aborting mid-stream with `net.byte_cap_exceeded` |
| C9 | **CORRECT** | CRLF-in-path/method/header-name/header-value block (resolver-call-count == 0 for all four) |
| C10 | **CORRECT** | redirect block — 302 returned as-is, target server's `accept_count() == 0` |
| C11 | **CORRECT** | `static_assert(NetEgressBackend<FakeBackend>)`, a namespace-scope type distinct from `HostEgressProxy` |

## 8. The decision

**Accepted.** Design A — the `NetEgressBackend` concept, `HostEgressProxy`'s resolve-once-connect-to-
verified-IPv4-literal mechanism, plain-HTTP-only scope, and no-redirect-following — is `NetOut`'s
enforcement mechanism for M2. It closes 008 §10 Q3's resolution into real, tested code: a first-party
default ships (`include/agentengine/sandbox/net_egress_proxy.hpp`, `src/sandbox/net_egress_proxy.cpp`),
and the seam underneath is a structural concept a deployer may satisfy with their own type. `wasm`'s
`http-request` host import (`src/backends/wasm/wasm_backend.cpp`'s `cb_http_request`) is wired to it
end-to-end — the first of this file's five gated I/O callbacks to go from stub to a real backing
effect, proven against the real compiled fixture, not a hand-crafted test double.

## 9. Residual risks and deferred gates

- **HTTPS/TLS — resolved.** `decisions/ADR-013-https-egress-tls-client.md` (M2 residual, post-Phase-F):
  mbedTLS vendored directly (exact-pinned, checksummed), `TlsClientSession` providing real system-CA-
  style validation and hostname verification. `https` allowlist entries are still rejected with
  `net.scheme_unsupported` in the default build (`AGENTENGINE_WITH_HTTPS` off) — never silently served
  over plain HTTP — and accepted for real once that option is on.
- **IPv6 is not implemented.** Matches the vendored PAL's own IPv4-only locator. An IPv6-only host
  fails closed with `net.host_unresolvable`, not a silently narrower blocklist.
- **`native-jail`'s interpreter-level `socket` mediation is out of scope.** 008 §1b's "socket proxies
  through the same host-mediated egress every other profile uses" still needs CPython's `sys.modules`/
  `sys.meta_path` machinery, which is M3's `PythonRunner` work (CLAUDE.md's locked decision 3) — not
  built here. `native-jail` continues to deny all networking outright (AppContainer/namespace denial,
  unchanged) until that milestone wires this proxy in as the mediation target.
- **No redirect-following, by design, not a gap** — see §3 Design B. A caller that needs to follow a
  redirect issues a new, separately-allowlist-checked request.
- ~~**Chunked transfer-encoding is not supported** on the response side...~~ **Fixed (2026-08-19).**
  The claim above was itself wrong, found while scoping M7's chunking-MCP-mock gap
  (`docs/planning/milestone-7-protocol-conformance-breakdown.md` H3): every request this client sends
  already carries `Connection: close`, so a `Transfer-Encoding: chunked`, no-`Content-Length` target
  does NOT fail to terminate — the read loop's existing read-until-peer-close path already collects
  the whole raw response correctly. The real bug was one level later: `parse_http_response()` handed
  that raw buffer straight to `resp.body`, chunk-size lines/extensions/terminators and all, a silently
  corrupted body, not the documented timeout. Fixed by dechunking for real (`dechunk_body()`,
  `net_egress_proxy.cpp`) whenever a response carries `Transfer-Encoding: chunked` — chunk extensions
  and trailer fields are parsed past and discarded, not surfaced; a malformed chunk-size or truncated
  chunk data fails closed with `net.protocol_error` rather than misparsing. No `Content-Length`-framed
  or read-until-close (non-chunked) response's behavior changes. New tests in
  `tests/test_net_egress_proxy.cpp`: a real chunked response (two data chunks, one with an extension,
  a discarded trailer) dechunks to the correct body; a malformed chunk-size fails closed with the
  documented error code, not a misparse. This is a bug fix against this ADR's own already-decided
  design (a `Content-Length`-or-read-until-close read loop, claim C8's byte-cap-during-the-loop
  discipline unchanged), not a new competing design — no separate ADR filed, per the ADR-062 removal
  precedent (`decisions/README.md`, "an ADR for everything is an ADR for nothing"). Applied only to
  `perform_http_exchange`/`perform_https_exchange` (the buffered, full-response functions) via a
  `dechunk_response_body_if_needed()` helper called AFTER `parse_http_response()`, never inside it —
  `parse_http_response()` is also called by `stream_response_body()` with only the header bytes (the
  streaming path relays the body incrementally and never buffers it here), and dechunking unconditionally
  inside `parse_http_response()` itself, tried first, dechunked an always-empty string on that path and
  broke every SSE test — caught by the full suite, not by `test_net_egress_proxy.cpp` alone, fixed
  before committing. That fix in turn surfaced a second, real conflict: `protocol/openai/chat_client.hpp`
  and `protocol/anthropic/chat_client.hpp` each already carried their OWN independent chunked-decode
  workaround on `chat()`'s non-streaming path (an older, narrower fix for the same real OpenRouter
  finding this ADR's own §9 originally undersold) — with this fix now dechunking at the transport layer
  first, that per-provider decode ran a second time on already-plain JSON and misparsed it as chunk
  framing. Retired both providers' redundant `decode_chunked_body`/`response_is_chunked`/
  `decoded_response_body` helpers (and their direct unit tests) rather than keeping two decode layers —
  `chat()` now trusts `resp->body` is already plain. Neither provider's STREAMING path
  (`chat_stream()`/`sandbox::perform_provider_streaming_exchange`) was touched; it uses the separate
  incremental `sandbox::ChunkedBodyDecoder`, which this fix deliberately does not reach. Full suite
  195/195 after both rounds of fixes, confirmed clean (not just the one target).
- **`method_restrictions`/`byte_cap` are the only two of `cap::NetOut`'s three parameters this ADR
  exercises against a live target in the WASM path** — the third, `host_allowlist`, is exercised
  structurally (C1) but every M2-era `NetOut<Host>` declaration tag only ever produces a single-entry
  allowlist (007/002's `to_capability` for `cap::decl::NetOut<Host>`), so the "ambiguous grant"
  rejection path (2+ entries) has no real caller yet — a real finding for whoever adds a
  multi-entry-capable declaration surface later, not a defect in this ADR's own scope.
- **`ResourceLimits::net_bytes`** (008 §2, `sandbox.hpp`) — **resolved**, M2 residual work post-Phase-F:
  `net_egress_proxy.hpp`'s `narrow_by_resource_limit()` reconciles it with this proxy's own `byte_cap`
  (the tighter of the two wins), wired into `wasm_backend.cpp`'s `cb_http_request` via
  `Instance::limits` (already captured at `create()` time, now also read here). Proven directly as a
  pure function (`tests/test_net_egress_proxy.cpp`'s C12: both-directions narrowing, no-grant-cap and
  no-resource-limit edge cases, `net_bytes == 0` treated as "no limit" not "zero bytes"), composed with
  the byte-cap enforcement C8 already proves rather than re-testing that through a live WASM
  round-trip. `ResourceLimits`' other six fields (`cpu_ms`, `pids`, `fds`, `disk_bytes`,
  `output_bytes`; `wall_ms`/`memory_bytes` were already wired via wasmtime's own store limiter/epoch
  deadline before this task) remain outside `wasm`'s `SandboxBackend::exec` — this task closed the one
  named gap ADR-011 itself flagged, not the general `ResourceLimits` integration.
