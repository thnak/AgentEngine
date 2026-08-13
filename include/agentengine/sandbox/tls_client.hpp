#pragma once
// Implements decisions/ADR-013-https-egress-tls-client.md: an ordinary outbound TLS client (system-
// CA-style validation against a vendored root bundle, real hostname verification) wrapping mbedTLS
// -- distinct from Quark's SecureTransport, built for node-identity cluster mTLS (ADR-011 section 3
// Design C's own rejection reasoning: SecureTransport deliberately opts OUT of hostname verification
// and trusts a private cluster CA chain, the opposite of what an arbitrary agent-declared host
// needs). Only compiled when AGENTENGINE_WITH_HTTPS is ON (root CMakeLists.txt); net_egress_proxy.cpp
// guards its own include of this header the same way.
//
// TLS wraps the TRANSPORT of an already-decided connection -- it does not re-decide who to connect
// to. `handshake()` takes an already-connected, already-verified-address socket
// (ADR-011's resolve-once-connect-to-verified-literal mechanism, `resolve_and_validate`/
// `VerifiedEndpoint`, already ran before this type exists) plus the ORIGINAL hostname (never re-
// resolved here) to verify the peer certificate against -- exactly RFC 6125's hostname-verification
// target, kept separate from the numeric address TCP actually connected to.

// <atomic> before pal/net.hpp: third_party/quark/pal/linux_x86_64/net.hpp uses std::atomic<bool>
// without including <atomic> itself (a Quark header, never patched in-tree per CLAUDE.md -- this
// project's own include order works around it instead, the same fix <memory>'s own transitive
// pull-in of <bits/shared_ptr_atomic.h> would otherwise accidentally mask or unmask depending on
// unrelated include order elsewhere).
#include <atomic>

#include <cstddef>
#include <memory>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/pal/net.hpp"

namespace agentengine::sandbox {

// Non-copyable, movable. Owns the mbedTLS session state for exactly one TLS-wrapped connection;
// `send`/`recv` block internally (matching net_egress_proxy.cpp's own `send_all`/read-loop posture
// for plain HTTP) rather than returning would-block to the caller -- the caller's read loop looks
// identical whether it is reading through this or a raw socket.
class TlsClientSession {
public:
    // Performs the full TLS handshake before returning: system-CA-style validation against the
    // vendored root bundle (cmake/ca_bundle_embed.cpp.in, compiled in, never read from a run-time
    // path), TLS 1.2 floor, real hostname verification against `hostname` (never disabled the way
    // SecureTransport's cluster-mTLS path disables it). `fd` must already be a connected, blocking-
    // mode-irrelevant socket (this type performs its own readiness waits internally); ownership of
    // `fd` is NOT taken -- the caller (net_egress_proxy.cpp's `FdGuard`) still owns closing it.
    //
    // `ca_bundle_pem_override`: empty (the default) trusts the real vendored bundle. A test may pass
    // a synthetic, test-generated root instead, to prove certificate-chain validation against
    // deterministic, offline-generated certificates rather than needing real, publicly-trusted ones
    // (impossible to test hostname-mismatch/expired/untrusted-root rejection against deterministically
    // otherwise). A testability seam, not a security bypass, matching ADR-011's own injectable-
    // resolver precedent (net_egress_proxy.hpp's `HostEgressProxy::resolver`) -- production code
    // (net_egress_proxy.cpp's own call site) never passes a non-empty override.
    [[nodiscard]] static result<TlsClientSession> handshake(agentengine::pal::fd_t fd, std::string_view hostname,
                                                              std::string_view ca_bundle_pem_override = {});

    TlsClientSession(TlsClientSession&&) noexcept;
    TlsClientSession& operator=(TlsClientSession&&) noexcept;
    TlsClientSession(TlsClientSession const&) = delete;
    TlsClientSession& operator=(TlsClientSession const&) = delete;
    ~TlsClientSession();

    // Blocks (via the same wait-then-syscall posture net_egress_proxy.cpp's own `send_all` uses)
    // until every byte of `data` is sent or a hard error occurs. Returns `data.size()` on success --
    // never a short count silently swallowed, matching net_egress_proxy.cpp's own all-or-error
    // posture for the plain-HTTP path.
    [[nodiscard]] result<std::size_t> send(std::string_view data);

    // One TLS-record's worth of decrypted application data per call (mbedTLS's own read-loop
    // internals may block/retry multiple times to assemble a record -- that retrying happens inside
    // this call, never surfaced as a would-block the caller has to loop on itself). Returns 0 on a
    // clean TLS close (`close_notify`), matching a plain socket's "peer closed" convention the
    // caller's existing read loop already checks for.
    [[nodiscard]] result<std::size_t> recv(char* buffer, std::size_t buffer_size);

private:
    struct Impl;
    explicit TlsClientSession(Impl* impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agentengine::sandbox
