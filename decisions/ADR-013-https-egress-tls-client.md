# ADR-013 — How does the egress proxy get a vetted TLS client for `https://`, and how is it vendored, given 008 §10 Q3 draws the "needs a vetted crypto library" line at TLS specifically?

## 1. The question

`decisions/ADR-011-first-party-egress-proxy.md` shipped `HostEgressProxy` scoped to plain HTTP only,
naming the reason explicitly (§9): "no TLS/HTTP library is vendored anywhere in this repo," and
Quark's existing `SecureTransport`/mbedTLS adapter is built for node-to-node cluster mTLS identity —
deliberately opts out of hostname verification and trusts a private cluster CA chain, the opposite of
what fetching an arbitrary agent-declared host needs. `008-Sandbox-and-Isolation.md` §10 Q3 itself
draws a sharp line between the two halves of an egress proxy: the allowlist/DNS-rebinding mechanism
"doesn't need a vetted third-party crypto library... so there's no equivalent 'don't roll your own'
argument," but TLS is different — hand-rolling it would be exactly the unaudited-crypto risk this
project avoids elsewhere. A follow-up ADR was named as the right place to take HTTPS on once a TLS
client exists.

The question this ADR answers, stated so it has a wrong answer: **where does the egress proxy's TLS
client come from, and how is the dependency it needs vendored** — reusing Quark's existing mbedTLS
integration path, vendoring mbedTLS independently with this project's own pinning discipline, or
using each platform's native TLS stack (Windows SChannel, Linux OpenSSL) instead of a vendored
cross-platform library at all?

## 2. Background this design must respect

- **`HostEgressProxy`'s existing shape** (ADR-011): `fetch()` composes single-target-grant gate ->
  scheme gate -> CRLF gate -> method-restriction gate -> `resolve_and_validate` (resolve-once-
  connect-to-verified-IPv4-literal) -> `perform_http_exchange` (raw HTTP/1.1 over `quark::pal`'s
  socket primitives, byte-cap enforced mid-stream). None of this needed to change: TLS wraps the
  *transport* of an already-decided connection, it does not re-decide who to connect to — the same
  resolve-once-connect-to-verified-literal mechanism that closes DNS-rebinding for plain HTTP applies
  identically underneath a TLS-wrapped connection.
- **WIT needs zero changes.** `wit/ae-tool.wit`'s `http-request-data` record supplies only
  `method`/`path`/`headers`/`body` — host/port/scheme come entirely from the capability grant, never
  the guest. `007`'s `cap::NetOut` doesn't validate scheme either (`subsumes_payload` only compares
  `host_allowlist`/`byte_cap`/`method_restrictions` as opaque data) — scheme legality has always been
  `net_egress_proxy.cpp`'s own runtime concern. HTTPS support is purely a host-side mechanism change.
- **Quark's `SecureTransport`/mbedTLS adapter, confirmed unsuitable directly** (re-verified for this
  ADR, not just cited from ADR-011): `mbedtls_handshake.hpp` deliberately calls
  `mbedtls_ssl_set_hostname(&ssl_, nullptr)` on the client side with a comment explaining why —
  "the client's peer authentication is NodeId/ClusterId bound from the leaf cert's CN... deliberately
  opt out of mbedTLS's hostname-vs-SAN check." Trust is a private cluster CA chain
  (`IdentityMaterial`/`TrustStore`), never the system/ordinary CA trust model. Reusing this class for
  ordinary outbound HTTPS would mean either monkey-patching a hostname check back in (fragile,
  fighting the type's own design) or building a second, parallel mbedTLS wrapper anyway — at which
  point it is not reuse.
- **Quark's mbedTLS *vendoring* mechanism, independent of `SecureTransport` the class**:
  `third_party/quark/cmake/QuarkSecurityAdapters.cmake` exposes a `quark::security_mbedtls`
  INTERFACE target (`QUARK_WITH_MBEDTLS`, off by default) wrapping raw mbedTLS — usable in principle
  without touching `SecureTransport` at all. But its pinning discipline is materially weaker than
  this project's own established precedent (see the wasmtime/CPython comparison below): `vcpkg.json`
  declares `"mbedtls": {"version>=": "3.6.2"}`, a **floor**, resolved against a vcpkg registry
  baseline commit — no archive checksum, no exact version, and (via its primary `find_package`
  path) it depends on a vcpkg toolchain file this project does not otherwise require at all, or (via
  its fallback path) whatever mbedTLS happens to be installed on the build machine.
- **CONVENTIONS.md tier 2**: "Seam backends... may take a heavy dependency (wasmtime, an HTTP client,
  a JSON library, **a TLS stack**), one dependency per backend, behind a CMake option, never linked
  into a build that does not select that backend." TLS is explicitly named as an example.
- **This project's own established vendoring precedent** (wasmtime, `AGENTENGINE_WITH_WASM`; CPython,
  `AGENTENGINE_BUILD_PYTHON_RUNNER`): `FetchContent_Declare` with an exact pinned version and a full
  `URL_HASH SHA256=...` checksum, hand-verified against a fresh download during the task that added
  it — no dependency on what the host machine happens to have installed, no floor-only version
  constraint.
- **No `Engine` type exists yet** (M2 scope) — nothing auto-discovers "which sandbox backends /
  which TLS options are available"; every vendored dependency in this codebase is opted into
  explicitly by a CMake flag the deploying developer sets, never inferred.

## 3. The competing designs

### Design A (accepted) — vendor mbedTLS directly, own pinning discipline, bundle a pinned CA root store

`AGENTENGINE_WITH_HTTPS` (root `CMakeLists.txt`, default OFF): `AGENTENGINE_VENDOR_MBEDTLS` (default
ON) fetches mbedTLS 3.6.7's official release tarball via `FetchContent_Declare` with an exact
`URL_HASH SHA256` (hand-verified against the project's own published `mbedtls-3.6.7-sha256sum.txt`
during this task), built as an ordinary CMake subproject (`ENABLE_TESTING`/`ENABLE_PROGRAMS`/
`MBEDTLS_FATAL_WARNINGS` forced off, matching `QUARK_BUILD_TESTS`/`QUARK_BUILD_BENCH`'s own
precedent for Quark). The 3.6.x LTS line, matching Quark's own floor, not the newer 4.x major (avoids
an unfamiliar API surface for a security-critical dependency). `AGENTENGINE_VENDOR_CA_BUNDLE`
(default ON) fetches curl.se's well-known, regularly-refreshed Mozilla-derived CA bundle, also with a
pinned `EXPECTED_HASH SHA256`, embedded into the binary at compile time (a generated `.cpp` via
`cmake/ca_bundle_embed.cpp.in`) rather than read from a run-time path.

A new `src/sandbox/tls_client.cpp` / `include/agentengine/sandbox/tls_client.hpp` (`TlsClientSession`)
wraps an already-connected `quark::pal::fd_t` (never re-resolves, never re-decides which address to
talk to — ADR-011's resolve-once-connect-to-verified-literal mechanism already ran): required
verification (`MBEDTLS_SSL_VERIFY_REQUIRED`) against the vendored CA chain, **real** hostname
verification (`mbedtls_ssl_set_hostname`, never disabled), a TLS 1.2 floor, no client certificate. A
custom BIO (`bio_send`/`bio_recv`) routes mbedTLS's record layer through the exact same
`quark::pal::send_some`/`recv_some` primitives `net_egress_proxy.cpp`'s plain-HTTP path already uses,
rather than a second raw-socket code path. `net_egress_proxy.cpp` gains `perform_https_exchange`
(identical structure to `perform_http_exchange` — same request-building via a newly-shared
`build_raw_request`, same byte-cap-enforced read loop — only the transport differs) and the scheme
gate now accepts `https` only when `AGENTENGINE_WITH_HTTPS` is compiled in.

**Steelman.** Matches this project's own established, already-proven-out vendoring discipline exactly
(same `FetchContent` + exact `URL_HASH` shape as wasmtime/CPython) rather than inheriting a weaker one
from Quark's vcpkg-floor path. One portable code path across Windows and Linux — `net_egress_proxy.cpp`
stays platform-uniform, matching how it already only depends on `quark::pal`'s thin cross-platform
socket layer today. mbedTLS is a known quantity in this codebase already (Quark's own adapters), so
the API surface and its security properties are not a first encounter for this project.

### Design B (rejected, per explicit user direction) — platform-native TLS

Windows via WinHTTP/SChannel (a system component, zero new dependency, automatic OS trust store);
Linux via system OpenSSL (`find_package(OpenSSL)`, near-ubiquitous on any dev/CI image, automatic OS
trust store).

**Steelman.** No bundled CA file to maintain (008 §10 Q3's `Profile::Strict` resolution note about
`resolve_strict` notwithstanding, a stale bundled root set is a genuine, recurring maintenance
liability — see §9's residual risk on this exact point); leverages already-audited platform libraries
maintained by the OS vendor/distro rather than this project separately tracking mbedTLS's own CVEs;
zero new dependency at all on Windows.

**Rejected because:** introduces a genuine per-platform code split in a part of the codebase
(`net_egress_proxy`) that has been platform-uniform since ADR-011 (only `quark::pal`'s existing thin
abstraction differs per platform, never the calling code's own logic) — two TLS-handling code paths
to red-team, prove, and keep in sync instead of one. Decided by explicit user direction after the
trade-off was surfaced (`AskUserQuestion`, this session): "Vendor mbedTLS directly" over "Platform-
native TLS," prioritizing the one-portable-code-path property and this project's own pinning
discipline over avoiding a bundled CA file's maintenance cost.

### Design C (rejected, carried forward from ADR-011) — reuse Quark's `SecureTransport`

Already rejected in ADR-011 §3 Design C for cluster-mTLS-vs-ordinary-CA-validation reasons; this
ADR's own research (§2 above) re-confirmed the exact mechanism (the nulled-out `mbedtls_ssl_set_
hostname` call and its own comment explaining why) rather than merely re-citing the prior ADR's
conclusion. Not re-steelmanned; nothing changed since ADR-011 that would revisit this.

## 4. Falsifiable claims (Design A)

- **C1 (positive control, end to end).** A valid leaf certificate — signed by a trusted-for-the-test
  root, unexpired, valid for the exact requested hostname — is accepted; the full TLS handshake, an
  HTTP request, and its response round-trip correctly through the TLS session.
  *Disproof: the handshake fails, or response bytes are corrupted/truncated relative to what the
  server sent.*
- **C2 (untrusted root rejected).** A certificate signed by a root NOT in the client's trust store is
  rejected, regardless of hostname/validity correctness. *Disproof: the handshake succeeds.*
- **C3 (hostname mismatch rejected — the load-bearing security property).** A certificate that is
  otherwise fully valid (trusted root, unexpired) but issued for a DIFFERENT hostname than the one
  requested is rejected. *Disproof: the handshake succeeds — this is exactly the property whose
  absence would silently turn "verified" into "any certificate this CA chain ever issued to anyone,"
  matching Quark's own deliberately-disabled check for a different trust model.*
- **C4 (expired certificate rejected).** A certificate with `notAfter` in the past is rejected even
  with a trusted root and correct hostname. *Disproof: the handshake succeeds.*
- **C5 (no layering violation, no production bypass).** `agentengine::core`/`agentengine::net_egress_
  proxy` gain no dependency on backend-specific code; `TlsClientSession::handshake`'s
  `ca_bundle_pem_override` testability seam (mirroring ADR-011's own injectable-resolver precedent)
  is never invoked with a non-empty value from `net_egress_proxy.cpp`'s own production call site.
  *Disproof: the production call site passes a non-empty override, or the build breaks with
  `AGENTENGINE_WITH_HTTPS` off.*
- **C6 (default posture unaffected).** With `AGENTENGINE_WITH_HTTPS` off (the default), the entire
  project builds and every existing test passes unmodified in behavior — `https://` allowlist entries
  are still rejected with `net.scheme_unsupported`, exactly ADR-011's original claim, for a build with
  no TLS client vendored. *Disproof: any existing test's behavior changes, or the default build fails.*

## 5. The red-team attack

- **R-C3 (the actual attack).** This is the one property a red-team pass on a from-scratch TLS client
  integration must not take on faith: does `mbedtls_ssl_set_hostname` actually get called with the
  real target hostname, not a placeholder or a value derived from the wrong source (e.g. accidentally
  passing the resolved IP-literal string instead of the original hostname, which would make hostname
  verification meaningless against any cert issued for any name at that IP)? Verified directly: `net_
  egress_proxy.cpp`'s `perform_https_exchange` passes `host_header` — the SAME string used for the
  HTTP `Host:` header, itself the pre-resolution hostname from the capability grant's allowlist entry,
  never `VerifiedEndpoint`'s numeric address. C3's test (wrong-hostname leaf, trusted root, valid
  dates) is the executed proof, not just a code-reading argument.
- **R-C2/C4 (verification is actually REQUIRED, not logged-only).** `mbedtls_ssl_conf_authmode`
  configured with `MBEDTLS_SSL_VERIFY_REQUIRED`, never `MBEDTLS_SSL_VERIFY_OPTIONAL` (which would
  still perform the check but not fail the handshake on a bad result — a real, easy-to-introduce bug
  class this ADR's own code review specifically watched for, since `VERIFY_OPTIONAL` reads almost
  identically to `VERIFY_REQUIRED` in a diff). C2/C4's tests are the executed proof.
- **R-fallback.** Does anything in the new code path silently fall back to accepting an unverified
  connection if verification setup itself fails (e.g. the CA bundle fails to parse)? No: `mbedtls_
  x509_crt_parse` returning negative is treated as a hard `net.tls_setup_failed` error, never a "skip
  verification and continue" path — by construction, there is no code path from a setup failure to a
  completed handshake.
- **R-CN-fallback.** Does mbedTLS 3.6.x's own verification fall back to checking the certificate's CN
  when no SAN dNSName entry is present, in a way that could be exploited by a cert with a misleading
  CN and an absent/wrong SAN? Checked directly against the vendored 3.6.7 source
  (`library/x509_crt.c`) — no CN-fallback hostname-matching code found; this ADR's own test
  certificates all carry an explicit SAN dNSName entry rather than relying on any such fallback,
  matching the library's actual behavior rather than an assumption about it.
- **R-C5 (test seam is not a production bypass).** Grepped `net_egress_proxy.cpp`'s one call site
  directly (not merely asserted): `TlsClientSession::handshake(guard.fd, host_header)` — exactly two
  arguments, the third (`ca_bundle_pem_override`) never supplied. The seam exists only for
  `tests/test_https_egress.cpp`'s own deterministic, offline-generated certificate chains.

## 6. Executed evidence

**Vendoring.** mbedTLS 3.6.7's official release tarball downloaded and its SHA256 hand-verified
against the project's own published `mbedtls-3.6.7-sha256sum.txt` before pinning
(`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`). curl.se's CA bundle
(`cacert.pem`, "Certificate data from Mozilla as of: Thu Jul 16 03:12:01 2026 GMT" per its own header)
downloaded and hashed (`3ff344e30b9b1ed2971044eabb438a08f2e2245ddb5f8ab1a3ad8b63ab4eaf91`) before
pinning. Both verified to actually resolve and build: `cmake -S . -B build -DAGENTENGINE_WITH_HTTPS=ON`
succeeds on both Windows and a fresh Linux container, mbedTLS building as a real CMake subproject
(confirmed via `build/_deps/ae_vendored_mbedtls-src` populated with real source, not a stub).

**A genuine cross-platform bug found and fixed during this pass** (not a design flaw, an include-
order fragility): `third_party/quark/pal/linux_x86_64/net.hpp` uses `std::atomic<bool>` without
including `<atomic>` itself, relying on whatever else happens to have pulled it in first — true on
Windows (where `net_egress_proxy.cpp`'s own pre-existing include order happened to work) but not
guaranteed, and broke on Linux once `tls_client.hpp`'s own `<memory>` include (for `std::unique_ptr`)
sat in front of `pal/net.hpp` without `<atomic>` between them. Since Quark is never patched in-tree
(CLAUDE.md), the fix lives entirely on this project's own side: `tls_client.hpp` and `tests/test_
https_egress.cpp` both now include `<atomic>` explicitly before `pal/net.hpp`, documented as a
deliberate ordering requirement, not an incidental one.

**C1-C4 (`tests/test_https_egress.cpp`, only built when `AGENTENGINE_WITH_HTTPS` is ON).** A
test-only, in-memory X.509 generator (`mbedtls_x509write_crt_*`, no filesystem/CLI dependency,
deterministic ECDSA P-256 keys) produces: a self-signed root; a leaf matching hostname
`test.invalid` with a correct SAN and a long validity window (C1); a leaf signed by a SECOND,
untrusted root (C2); a leaf signed by the trusted root but with SAN `wrong.invalid` (C3); a leaf
signed by the trusted root, valid hostname, but with both `notBefore`/`notAfter` in the past (C4). A
loopback TLS test server (`TlsTestServer`, mbedTLS server-side handshake wrapping a real
`quark::pal` accept loop) presents each certificate in turn. All 15 individual checks pass:
```
  ok: C1: valid-leaf test server started
  ok: C1: connect() to the test server succeeds
  ok: C1: handshake succeeds against a valid cert + trusted root + matching hostname
  ok: C1: request sent over the TLS session
  ok: C1: response received over the TLS session
  ok: C1: the real HTTP response text round-trips through TLS unchanged
  ok: C2: untrusted-root test server started
  ok: C2: a cert signed by a root NOT in the trust store is rejected
  ok: C2: specific diagnostic code
  ok: C3: wrong-hostname test server started
  ok: C3: a trusted, unexpired cert valid for a DIFFERENT hostname is still rejected
  ok: C3: specific diagnostic code
  ok: C4: expired-leaf test server started
  ok: C4: an expired certificate is rejected
  ok: C4: specific diagnostic code
test_https_egress: ALL PASS
```
Reconfirmed clean over 5 consecutive standalone runs on both Windows and a fresh Linux container.

**C5.** Verified by direct source inspection (§5's R-C5), not by a compiled test — grepping a test
binary's own source dependency to prove a negative would be more fragile than reading the one call
site directly.

**C6.** A completely fresh, default-configuration build (`cmake -S . -B <dir>` with no
`AGENTENGINE_WITH_HTTPS` flag at all) on Windows: 32/32 tests passed, 0 failed — including the
previously-intermittent `test_native_jail_backend_windows` OOM-classification flake, which passed
clean on this particular run (confirming it is genuinely a timing flake, not a regression this task
introduced). A fresh Linux container's own default build: 23/23 passed, 1 expected skip
(`test_shell_runner_no_process_creation`, `llvm-nm` unavailable).

**With `AGENTENGINE_WITH_HTTPS=ON`, full-suite regression check.** Windows (`ctest -C Debug -j4`):
34/35 passed — the one failure is the same pre-existing, unrelated `test_native_jail_backend_windows`
Job-Object OOM-vs-timeout flake already documented against ADR-011/ADR-012 (reconfirmed as pre-
existing, not newly introduced, by C6's clean run above). Linux (Docker, gcc-14, fresh container):
24/24 passed, 1 expected skip. `tests/test_net_egress_proxy.cpp`'s own pre-existing C3 case (https
rejected before any network activity) was updated to be conditional: unchanged (still proves
pre-resolution rejection) when `AGENTENGINE_WITH_HTTPS` is off; when on, proves the scheme gate no
longer blocks https pre-resolution instead (the resolver is now called), since the full TLS-handshake
proof belongs in `test_https_egress.cpp`, not duplicated here.

**`tools/naming_lint.py`**: unsuppressed-finding count unchanged (still the same 8 pre-existing,
unrelated findings in files this task never touched). One incidental, pre-existing tooling gap
surfaced during this check, named here rather than silently worked around: `naming_lint.py`'s own
docstring says it expects "one file, one `namespace agentengine { }` block" — `sandbox/*.hpp`
(`sandbox.hpp`, `net_egress_proxy.hpp`, and now `tls_client.hpp`) all use the compound
`namespace agentengine::sandbox { }` spelling instead, which the script's brace-depth tracker does
not appear to recognize as "inside `agentengine`'s own scope" at all — every exported name in that
whole directory, not just this ADR's new ones, silently never gets checked. Pre-existing (confirmed:
`net_egress_proxy.hpp`'s own ADR-011-era types are equally unflagged, unsuppressed, and were never
audited against 027's vocabulary tables either), not introduced by this task, and out of this ADR's
own scope to fix — named as a residual (§9).

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — positive control end to end | **CORRECT** | `test_https_egress.cpp`, 6/6 C1 checks pass, reconfirmed over 5 runs on both platforms. |
| C2 — untrusted root rejected | **CORRECT** | 3/3 C2 checks pass; specific `net.tls_certificate_rejected` diagnostic confirmed. |
| C3 — hostname mismatch rejected | **CORRECT** | 3/3 C3 checks pass — the load-bearing security property, independently red-teamed (§5 R-C3) by tracing the actual argument passed, not just observing the test pass. |
| C4 — expired certificate rejected | **CORRECT** | 3/3 C4 checks pass. |
| C5 — no layering violation, no production bypass | **CORRECT** | Direct source inspection: exactly one call site, exactly two arguments. Full build succeeds with the option off. |
| C6 — default posture unaffected | **CORRECT** | Fresh default-config builds on both platforms, full suites pass (32/32 Windows, 23/23 Linux + 1 expected skip). |

## 8. The decision

**Accepted: Design A.** The egress proxy vendors mbedTLS directly (3.6.7, exact-pinned, checksummed,
this project's own established FetchContent discipline — not Quark's laxer vcpkg-floor path) behind
a new `AGENTENGINE_WITH_HTTPS` CMake option (default off, matching `AGENTENGINE_WITH_WASM`/
`AGENTENGINE_BUILD_PYTHON_RUNNER`'s own posture for this project's other tier-2 dependencies), plus a
separately-pinned CA root bundle embedded at compile time. `TlsClientSession` (`tls_client.hpp`/
`.cpp`) provides real system-CA-style validation and real hostname verification — the opposite of
Quark's `SecureTransport`, which remains untouched and unused here. `HostEgressProxy`'s scheme gate
now accepts `https` when this option is on; the default build's behavior is unchanged. Platform-
native TLS (Design B) was explicitly rejected per user direction after the trade-off was surfaced.

## 9. Residual risks and deferred gates

- **The bundled CA root store is now this project's own maintenance burden.** Unlike an OS trust
  store (auto-updated by the platform), a compiled-in `cacert.pem` snapshot (dated 2026-07-16 in this
  pin) will drift from reality over time as CAs rotate, get revoked, or new roots are added — this
  ADR does not build any rotation/refresh mechanism, only names the file and its provenance clearly
  enough that a future task can re-pin it. This was the concrete cost named when Design B (platform-
  native, auto-updated trust stores) was rejected in favor of Design A's one-portable-code-path
  property.
- **No OCSP/CRL revocation checking.** A certificate that is cryptographically valid but has been
  revoked by its issuer is currently accepted if it otherwise verifies — matching mbedTLS's own
  `MBEDTLS_SSL_PRESET_DEFAULT` posture (no revocation checking configured), not a deliberate
  weakening beyond that default.
- **No client certificates / mutual TLS.** This is a client connecting to arbitrary third-party
  servers on an agent's behalf, not the node-to-node mTLS `SecureTransport` already handles elsewhere
  — `mbedtls_ssl_conf_own_cert` is never called. If a future use case needs an agent to present a
  client certificate to a third-party HTTPS server, that is new, unbuilt scope.
- **TLS 1.3-specific behavior (0-RTT, session resumption/tickets, post-handshake auth) was not
  specifically exercised** — mbedTLS 3.6's own defaults apply; this ADR's tests exercise a single,
  fresh handshake per connection only, matching `perform_https_exchange`'s own `Connection: close`,
  one-request-per-connection posture (identical to the plain-HTTP path, ADR-011).
- **`naming_lint.py`'s `namespace agentengine::sandbox { }` blind spot** (§6) is real, pre-existing,
  and unfixed by this ADR — every exported name across `sandbox/*.hpp`, not only this task's new
  ones, is silently unaudited against 027's vocabulary tables. Named here as the place this was
  discovered, not claimed as fixed.
- **Only the WASM `http-request` import consumes this.** 009 §3a/§11 Q2's OCI-plugin-pull round trip
  (OQ-6, resolved 2026-08-03) already needed plain HTTPS for a structurally different call site (host-
  side plugin-pull machinery, not gated by a guest-held `cap::NetOut` at all) and used some ad hoc,
  non-vendored tool to prove it, predating `HostEgressProxy`'s own existence. Whether that call site
  should be migrated onto `TlsClientSession`/`HostEgressProxy` is a real, separate decision this ADR
  does not make.
