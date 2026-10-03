// Implements mbedtls_client_config.hpp -- decisions/ADR-013-https-egress-tls-client.md's client configuration,
// moved here verbatim from src/sandbox/tls_client.cpp (2026-10-03, ADR-237 §6.3) so the blocking client and
// rt::TlsStream share one copy. The comments on each step are the original ones.

#include "backends/tls_mbedtls/mbedtls_client_config.hpp"

#include <mbedtls/error.h>

#include <cstdint>
#include <expected>

namespace agentengine::sandbox {
// Generated at CMake configure time from the vendored CA bundle (cmake/ca_bundle_embed.cpp.in).
extern char const* const kVendoredCaBundlePem;
}  // namespace agentengine::sandbox

namespace agentengine::detail::mbedtls_client {

Context::Context() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca_chain);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
}

Context::~Context() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&ca_chain);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
}

std::string error_string(int code) {
    char buf[256];
    mbedtls_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

result<void> configure(Context& ctx, std::string_view hostname, std::string_view ca_bundle_pem_override,
                       std::string_view personalization) {
    if (mbedtls_ctr_drbg_seed(&ctx.drbg, mbedtls_entropy_func, &ctx.entropy,
                              reinterpret_cast<unsigned char const*>(personalization.data()),
                              personalization.size()) != 0) {
        return std::unexpected(error{failure_class::fatal, "TLS RNG seed failed", "net.tls_setup_failed"});
    }

    // The vendored CA bundle, embedded at compile time (never read from a run-time path) -- system-
    // CA-style validation against a pinned root set this project owns rotating, not whatever the
    // build/run machine happens to trust (ADR-013 section 3). `ca_bundle_pem_override` (non-empty
    // only from a test) substitutes a synthetic root instead -- see this function's own declaration
    // comment. Copied into an owned, guaranteed-NUL-terminated std::string regardless of source:
    // mbedtls_x509_crt_parse's PEM path requires the buffer's declared length to include a
    // terminating NUL, which an arbitrary string_view is not guaranteed to have even when its
    // underlying storage happens to (only std::string::data() carries that guarantee, since C++11).
    std::string const ca_bundle_pem(ca_bundle_pem_override.empty() ? std::string_view(sandbox::kVendoredCaBundlePem)
                                                                   : ca_bundle_pem_override);
    int const parsed = mbedtls_x509_crt_parse(&ctx.ca_chain,
                                              reinterpret_cast<unsigned char const*>(ca_bundle_pem.c_str()),
                                              ca_bundle_pem.size() + 1);
    if (parsed < 0) {
        return std::unexpected(error{failure_class::fatal,
                                     "vendored CA bundle failed to parse: " + error_string(parsed),
                                     "net.tls_setup_failed"});
    }

    if (mbedtls_ssl_config_defaults(&ctx.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return std::unexpected(error{failure_class::fatal, "TLS config defaults failed", "net.tls_setup_failed"});
    }
    // Ordinary outbound HTTPS to an arbitrary agent-declared host -- REQUIRED verification against
    // the vendored CA chain, never optional/logged-only (unlike a debugging-only relaxed mode some
    // HTTP clients ship). No client certificate is configured: this is a client connecting to
    // third-party servers, not the node-to-node mTLS SecureTransport already handles elsewhere.
    mbedtls_ssl_conf_authmode(&ctx.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&ctx.conf, &ctx.ca_chain, nullptr);
    mbedtls_ssl_conf_rng(&ctx.conf, mbedtls_ctr_drbg_random, &ctx.drbg);
    // TLS 1.2 floor (mbedTLS 3.6's own default maximum is TLS 1.3; nothing here raises it further,
    // only refuses to negotiate downward past 1.2) -- ADR-013 section 3's minimum-security baseline.
    mbedtls_ssl_conf_min_tls_version(&ctx.conf, MBEDTLS_SSL_VERSION_TLS1_2);

    if (mbedtls_ssl_setup(&ctx.ssl, &ctx.conf) != 0) {
        return std::unexpected(error{failure_class::fatal, "TLS session setup failed", "net.tls_setup_failed"});
    }
    // Real hostname verification (RFC 6125) against the ORIGINAL target hostname -- never nulled
    // out the way SecureTransport's cluster-mTLS path deliberately nulls it (mbedtls_handshake.hpp:
    // "the client's peer authentication is NodeId/ClusterId bound... deliberately opt out of
    // mbedTLS's hostname-vs-SAN check"). This is the one line whose ABSENCE would silently turn
    // "verified" into "any cert this CA chain ever issued to anyone" -- ADR-013's red-team R-C2
    // exists specifically to prove this line is doing real work.
    std::string const host_owned(hostname);
    if (mbedtls_ssl_set_hostname(&ctx.ssl, host_owned.c_str()) != 0) {
        return std::unexpected(
            error{failure_class::fatal, "TLS hostname configuration failed", "net.tls_setup_failed"});
    }
    return {};
}

error handshake_error(mbedtls_ssl_context const& ssl, int ret) {
    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        std::uint32_t const flags = mbedtls_ssl_get_verify_result(&ssl);
        char                flag_buf[256];
        mbedtls_x509_crt_verify_info(flag_buf, sizeof(flag_buf), "", flags);
        return error{failure_class::policy, "TLS certificate verification failed: " + std::string(flag_buf),
                     "net.tls_certificate_rejected"};
    }
    return error{failure_class::policy, "TLS handshake failed: " + error_string(ret), "net.tls_handshake_failed"};
}

}  // namespace agentengine::detail::mbedtls_client
