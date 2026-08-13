// Implements tls_client.hpp. See decisions/ADR-013-https-egress-tls-client.md.

#include "agentengine/sandbox/tls_client.hpp"

#if defined(_WIN32)
// select()/FD_SET/FD_ZERO already pulled in transitively by pal/windows_x86_64/net.hpp.
#else
#include <sys/select.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <string>

namespace agentengine::sandbox {

// Generated at CMake configure time from the vendored CA bundle (cmake/ca_bundle_embed.cpp.in).
extern char const* const kVendoredCaBundlePem;

namespace {

constexpr int kIoTimeoutMs = 10'000;

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

std::string mbedtls_error_string(int code) {
    char buf[256];
    mbedtls_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

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
    if (!wait_ready(*fd, /*for_write=*/true, kIoTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::send_some(*fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) {
        if (r.error() == agentengine::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* fd = static_cast<agentengine::pal::fd_t*>(ctx);
    if (!wait_ready(*fd, /*for_write=*/false, kIoTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::recv_some(*fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) {
        if (r.error() == agentengine::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return static_cast<int>(*r);
}

}  // namespace

struct TlsClientSession::Impl {
    agentengine::pal::fd_t fd{};
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca_chain;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;

    Impl() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca_chain);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
    }
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca_chain);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

TlsClientSession::TlsClientSession(Impl* impl) noexcept : impl_(impl) {}
TlsClientSession::TlsClientSession(TlsClientSession&&) noexcept = default;
TlsClientSession& TlsClientSession::operator=(TlsClientSession&&) noexcept = default;
TlsClientSession::~TlsClientSession() = default;

result<TlsClientSession> TlsClientSession::handshake(agentengine::pal::fd_t fd, std::string_view hostname,
                                                       std::string_view ca_bundle_pem_override) {
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;

    char const* const pers = "agentengine-tls-client";
    if (mbedtls_ctr_drbg_seed(&impl->drbg, mbedtls_entropy_func, &impl->entropy,
                               reinterpret_cast<unsigned char const*>(pers), std::strlen(pers)) != 0) {
        return std::unexpected(
            error{failure_class::fatal, "TLS RNG seed failed", "net.tls_setup_failed"});
    }

    // The vendored CA bundle, embedded at compile time (never read from a run-time path) -- system-
    // CA-style validation against a pinned root set this project owns rotating, not whatever the
    // build/run machine happens to trust (ADR-013 section 3). `ca_bundle_pem_override` (non-empty
    // only from a test) substitutes a synthetic root instead -- see this function's own declaration
    // comment. Copied into an owned, guaranteed-NUL-terminated std::string regardless of source:
    // mbedtls_x509_crt_parse's PEM path requires the buffer's declared length to include a
    // terminating NUL, which an arbitrary string_view is not guaranteed to have even when its
    // underlying storage happens to (only std::string::data() carries that guarantee, since C++11).
    std::string const ca_bundle_pem(ca_bundle_pem_override.empty()
                                         ? std::string_view(kVendoredCaBundlePem)
                                         : ca_bundle_pem_override);
    int const parsed = mbedtls_x509_crt_parse(&impl->ca_chain,
                                               reinterpret_cast<unsigned char const*>(ca_bundle_pem.c_str()),
                                               ca_bundle_pem.size() + 1);
    if (parsed < 0) {
        return std::unexpected(error{failure_class::fatal,
                                      "vendored CA bundle failed to parse: " + mbedtls_error_string(parsed),
                                      "net.tls_setup_failed"});
    }

    if (mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return std::unexpected(
            error{failure_class::fatal, "TLS config defaults failed", "net.tls_setup_failed"});
    }
    // Ordinary outbound HTTPS to an arbitrary agent-declared host -- REQUIRED verification against
    // the vendored CA chain, never optional/logged-only (unlike a debugging-only relaxed mode some
    // HTTP clients ship). No client certificate is configured: this is a client connecting to
    // third-party servers, not the node-to-node mTLS SecureTransport already handles elsewhere.
    mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&impl->conf, &impl->ca_chain, nullptr);
    mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->drbg);
    // TLS 1.2 floor (mbedTLS 3.6's own default maximum is TLS 1.3; nothing here raises it further,
    // only refuses to negotiate downward past 1.2) -- ADR-013 section 3's minimum-security baseline.
    mbedtls_ssl_conf_min_tls_version(&impl->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    if (mbedtls_ssl_setup(&impl->ssl, &impl->conf) != 0) {
        return std::unexpected(error{failure_class::fatal, "TLS session setup failed", "net.tls_setup_failed"});
    }
    // Real hostname verification (RFC 6125) against the ORIGINAL target hostname -- never nulled
    // out the way SecureTransport's cluster-mTLS path deliberately nulls it (mbedtls_handshake.hpp:
    // "the client's peer authentication is NodeId/ClusterId bound... deliberately opt out of
    // mbedTLS's hostname-vs-SAN check"). This is the one line whose ABSENCE would silently turn
    // "verified" into "any cert this CA chain ever issued to anyone" -- ADR-013's red-team R-C2
    // exists specifically to prove this line is doing real work.
    std::string const host_owned(hostname);
    if (mbedtls_ssl_set_hostname(&impl->ssl, host_owned.c_str()) != 0) {
        return std::unexpected(error{failure_class::fatal, "TLS hostname configuration failed", "net.tls_setup_failed"});
    }
    mbedtls_ssl_set_bio(&impl->ssl, &impl->fd, bio_send, bio_recv, nullptr);

    for (;;) {
        int const ret = mbedtls_ssl_handshake(&impl->ssl);
        if (ret == 0) break;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            std::uint32_t const flags = mbedtls_ssl_get_verify_result(&impl->ssl);
            char flag_buf[256];
            mbedtls_x509_crt_verify_info(flag_buf, sizeof(flag_buf), "", flags);
            return std::unexpected(error{failure_class::policy,
                                          "TLS certificate verification failed: " + std::string(flag_buf),
                                          "net.tls_certificate_rejected"});
        }
        return std::unexpected(error{failure_class::policy,
                                      "TLS handshake failed: " + mbedtls_error_string(ret),
                                      "net.tls_handshake_failed"});
    }

    return TlsClientSession(impl.release());
}

result<std::size_t> TlsClientSession::send(std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int const n = mbedtls_ssl_write(&impl_->ssl, reinterpret_cast<unsigned char const*>(data.data() + sent),
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
        int const n = mbedtls_ssl_read(&impl_->ssl, reinterpret_cast<unsigned char*>(buffer), buffer_size);
        if (n >= 0) return static_cast<std::size_t>(n);  // 0 == clean TLS close_notify
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return std::size_t{0};
        return std::unexpected(
            error{failure_class::transient, "TLS read failed: " + mbedtls_error_string(n), "net.connect_failed"});
    }
}

}  // namespace agentengine::sandbox
