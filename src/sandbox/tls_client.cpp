// Implements tls_client.hpp. See decisions/ADR-013-https-egress-tls-client.md.

#include "agentengine/sandbox/tls_client.hpp"
#include "agentengine/sandbox/io_timeout.hpp"

#if defined(_WIN32)
// select()/FD_SET/FD_ZERO already pulled in transitively by pal/windows_x86_64/net.hpp.
#else
#include <sys/select.h>
#endif

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <string>
#include <utility>

// The client configuration (CA bundle, VERIFY_REQUIRED, TLS 1.2 floor, hostname) is shared with ADR-237's
// rt::TlsStream since 2026-10-03: moved there verbatim, behaviour unchanged.
#include "backends/tls_mbedtls/mbedtls_client_config.hpp"

namespace agentengine::sandbox {

namespace mbedtls_client = agentengine::detail::mbedtls_client;

namespace {

// Live-model evidence (repeated `net.connect_failed`/"TLS read failed: SSL - The operation timed
// out" failures across several tests/test_rt_agent_session_*_live_e2e.cpp runs, isolated by cross-
// checking the identical scenario through the plain Python `openai` SDK, which never reproduced the
// failure) traced this to 10s being far too short: `TlsClientSession::recv()`'s retry loop only
// continues past MBEDTLS_ERR_SSL_WANT_READ/WANT_WRITE -- a real MBEDTLS_ERR_SSL_TIMEOUT from
// wait_ready() below falls straight through as a hard error, and a reasoning-capable model can
// legitimately take well over 10s to produce its first response byte on a non-streaming completion
// (there is nothing to read until the model has finished "thinking"). Widened to something that
// comfortably covers that latency while still bounding a genuinely dead connection, not removed
// entirely -- this is one `wait_ready` poll's own timeout, not a whole-request budget.
// ADR-177: one shared, env-configurable value now -- see sandbox/io_timeout.hpp.
inline int kIoTimeoutMsFn() { return ::agentengine::sandbox::io_timeout_ms(); }

// Mirrors net_egress_proxy.cpp's own `wait_ready` exactly (same agentengine::pal primitive, same
// select()-based readiness wait) -- kept as an independent copy rather than a shared header, since
// each is a ~10-line, fully self-contained primitive over agentengine::pal and the two translation
// units have no other reason to depend on each other (CLAUDE.md: three similar lines beat a
// premature abstraction).
bool wait_ready(agentengine::pal::fd_t fd, bool for_write, int timeout_ms) {
    ::fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    ::timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int const nfds = static_cast<int>(fd) + 1;
    int const rc = for_write ? ::select(nfds, nullptr, &set, nullptr, &tv)
                              : ::select(nfds, &set, nullptr, nullptr, &tv);
    return rc > 0;
}

std::string mbedtls_error_string(int code) { return mbedtls_client::error_string(code); }

// mbedTLS's own BIO contract (ssl.h's mbedtls_ssl_send_t/mbedtls_ssl_recv_t): return the byte count
// on success, 0 on a clean peer close (recv only), or a negative MBEDTLS_ERR_* code. Reimplements
// mbedTLS's own reference `mbedtls_net_send`/`mbedtls_net_recv` (net_sockets.c) against
// agentengine::pal's primitives instead of calling POSIX/Winsock directly a second time -- the same
// primitive choice net_egress_proxy.cpp's own plain-HTTP path already made, kept consistent here
// rather than introducing a second raw-socket code path for the same job. `wait_ready` blocking
// inside these callbacks is why `TlsClientSession::send`/`recv` never surface WANT_READ/WANT_WRITE
// to their own callers -- a spurious would-block after `wait_ready` said ready is retried in the
// caller loop below, not treated as a real error.
int bio_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto* fd = static_cast<agentengine::pal::fd_t*>(ctx);
    if (!wait_ready(*fd, /*for_write=*/true, kIoTimeoutMsFn())) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::send_some(*fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) {
        if (r.error() == agentengine::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* fd = static_cast<agentengine::pal::fd_t*>(ctx);
    if (!wait_ready(*fd, /*for_write=*/false, kIoTimeoutMsFn())) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::recv_some(*fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) {
        if (r.error() == agentengine::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return static_cast<int>(*r);
}

}  // namespace

struct TlsClientSession::Impl {
    agentengine::pal::fd_t  fd{};
    mbedtls_client::Context ctx;  // entropy, DRBG, CA chain, config, session -- freed in that reverse order
};

TlsClientSession::TlsClientSession(Impl* impl) noexcept : impl_(impl) {}
TlsClientSession::TlsClientSession(TlsClientSession&&) noexcept = default;
TlsClientSession& TlsClientSession::operator=(TlsClientSession&&) noexcept = default;
TlsClientSession::~TlsClientSession() = default;

result<TlsClientSession> TlsClientSession::handshake(agentengine::pal::fd_t fd, std::string_view hostname,
                                                       std::string_view ca_bundle_pem_override) {
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;

    // Seed, vendored CA bundle (or a test's override), VERIFY_REQUIRED, TLS 1.2 floor, real hostname
    // verification -- see backends/tls_mbedtls/mbedtls_client_config.cpp, where these steps and their
    // original comments now live.
    if (auto configured = mbedtls_client::configure(impl->ctx, hostname, ca_bundle_pem_override,
                                                    "agentengine-tls-client");
        !configured) {
        return std::unexpected(std::move(configured.error()));
    }
    mbedtls_ssl_set_bio(&impl->ctx.ssl, &impl->fd, bio_send, bio_recv, nullptr);

    for (;;) {
        int const ret = mbedtls_ssl_handshake(&impl->ctx.ssl);
        if (ret == 0) break;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return std::unexpected(mbedtls_client::handshake_error(impl->ctx.ssl, ret));
    }

    return TlsClientSession(impl.release());
}

result<std::size_t> TlsClientSession::send(std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int const n = mbedtls_ssl_write(&impl_->ctx.ssl, reinterpret_cast<unsigned char const*>(data.data() + sent),
                                         data.size() - sent);
        if (n >= 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return std::unexpected(
            error{failure_class::transient, "TLS write failed: " + mbedtls_error_string(n), "net.connect_failed"});
    }
    return sent;
}

result<std::size_t> TlsClientSession::recv(char* buffer, std::size_t buffer_size) {
    for (;;) {
        int const n = mbedtls_ssl_read(&impl_->ctx.ssl, reinterpret_cast<unsigned char*>(buffer), buffer_size);
        if (n >= 0) return static_cast<std::size_t>(n);  // 0 == clean TLS close_notify
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return std::size_t{0};
        return std::unexpected(
            error{failure_class::transient, "TLS read failed: " + mbedtls_error_string(n), "net.connect_failed"});
    }
}

}  // namespace agentengine::sandbox
