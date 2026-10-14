#pragma once
// Implements decisions/ADR-013-https-egress-tls-client.md's client configuration, shared by its two users:
// the blocking `sandbox::TlsClientSession` (src/sandbox/tls_client.cpp) and ADR-237 §6.3's asynchronous
// `rt::TlsStream` (tls_stream_mbedtls.cpp). Extracted verbatim from tls_client.cpp (2026-10-03) so the two
// cannot drift: the same vendored CA bundle, VERIFY_REQUIRED, the TLS 1.2 floor, real hostname verification,
// and the same error wording and codes. The blocking client's behaviour is unchanged by the extraction (its
// test, tests/sandbox/test_https_egress.cpp, is the check).
//
// Only compiled when AGENTENGINE_WITH_HTTPS is ON (root CMakeLists.txt, `agentengine_tls_common`). Private to
// src/: no public header names an mbedTLS type.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine::detail::mbedtls_client {

// Every mbedTLS object one client connection needs, initialised on construction and freed (in the blocking
// client's original order) on destruction. Not movable: `ssl` keeps pointers into `conf`, `conf` into the rest.
struct Context {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca_chain;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;

    Context();
    ~Context();
    Context(Context const&)            = delete;
    Context& operator=(Context const&) = delete;
};

[[nodiscard]] std::string error_string(int code);

// Seeds the DRBG with `personalization`, parses the CA bundle (the vendored, compiled-in one unless
// `ca_bundle_pem_override` is non-empty -- a test seam, never used by production call sites), configures a TLS
// client with VERIFY_REQUIRED and a TLS 1.2 floor, sets `ssl` up on it and sets the hostname to verify the peer
// certificate against (RFC 6125). The BIO is the caller's to set. Errors: `net.tls_setup_failed` (fatal).
[[nodiscard]] result<void> configure(Context& ctx, std::string_view hostname, std::string_view ca_bundle_pem_override,
                                     std::string_view personalization);

// The error for a failed `mbedtls_ssl_handshake` return (not WANT_READ/WANT_WRITE): a rejected certificate is
// `net.tls_certificate_rejected` with mbedTLS's verify-flag text, anything else `net.tls_handshake_failed`
// (both `policy`).
[[nodiscard]] error handshake_error(mbedtls_ssl_context const& ssl, int ret);

}  // namespace agentengine::detail::mbedtls_client
