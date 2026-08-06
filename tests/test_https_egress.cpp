// Proves decisions/ADR-013-https-egress-tls-client.md's falsifiable claims -- only built when
// AGENTENGINE_WITH_HTTPS is ON (tests/CMakeLists.txt). Structure follows
// tests/test_net_egress_proxy.cpp's own ADR-011 precedent: a real loopback TLS server, no live
// internet dependency, deterministic in-process-generated certificates rather than real, publicly-
// trusted ones (impossible to test hostname-mismatch/expired/untrusted-root rejection
// deterministically against real CA-issued certs).
//
//   C1 (positive control): a leaf certificate, signed by a trusted-for-this-test root and matching
//      the requested hostname, is accepted -- the whole TLS handshake, HTTP request, and response
//      round-trip succeeds end to end.
//   C2: a leaf certificate signed by a root NOT in the client's trust store is rejected.
//   C3: a leaf certificate valid for the wrong hostname (but signed by a trusted root) is rejected --
//      this is the one property whose ABSENCE would silently turn "verified" into "any cert this CA
//      chain ever issued to anyone" (tls_client.cpp's own comment on this exact line).
//   C4: an expired leaf certificate (notAfter in the past) is rejected.
//
// (ADR-013's own claim that `ca_bundle_pem_override` is a testability seam, never used from
// production code, is verified by direct inspection of net_egress_proxy.cpp's one call site --
// grepping a compiled test binary's own source dependency would be more fragile than just reading
// it, so that claim is not re-proven here.)

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

// <atomic> before pal/net.hpp: see tls_client.hpp's own comment on this exact ordering requirement
// (third_party/quark/pal/linux_x86_64/net.hpp uses std::atomic<bool> without including <atomic>).
#include <atomic>

#include "pal/net.hpp"

#if defined(_WIN32)
// select()/FD_SET/FD_ZERO already pulled in transitively by pal/windows_x86_64/net.hpp.
#else
#include <sys/select.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "agentengine/sandbox/tls_client.hpp"

using agentengine::sandbox::TlsClientSession;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

// -- test-only certificate generation (in-memory, no filesystem/CLI dependency, deterministic) ----

struct GeneratedKeyCert {
    std::string cert_pem;
    std::string key_pem;
};

class TestCertAuthority {
public:
    TestCertAuthority() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-https-test-ca";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                               reinterpret_cast<unsigned char const*>(pers), std::strlen(pers));
    }
    ~TestCertAuthority() {
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TestCertAuthority(TestCertAuthority const&) = delete;

    // Fresh ECDSA P-256 keypair. Caller owns the returned context (mbedtls_pk_free it).
    mbedtls_pk_context generate_key() {
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg_);
        return pk;
    }

    // `issuer_key`/`issuer_name` describe who signs this cert: pass `subject_key`/`subject_name`
    // (the same key/name) for a self-signed root. `sans`, if non-empty, becomes the certificate's
    // Subject Alternative Name dNSName entries -- what TlsClientSession's hostname verification
    // actually checks against (mbedTLS 3.6.x has no CN fallback for hostname verification; a leaf
    // with no matching SAN entry is correctly rejected regardless of its CN).
    GeneratedKeyCert issue(mbedtls_pk_context* subject_key, std::string_view subject_name,
                           mbedtls_pk_context* issuer_key, std::string_view issuer_name,
                           std::string_view not_before, std::string_view not_after, bool is_ca,
                           std::string_view san_hostname = {}) {
        mbedtls_x509write_cert ctx;
        mbedtls_x509write_crt_init(&ctx);
        mbedtls_x509write_crt_set_version(&ctx, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&ctx, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&ctx, subject_key);
        mbedtls_x509write_crt_set_issuer_key(&ctx, issuer_key);

        std::string const subject_dn = "CN=" + std::string(subject_name);
        std::string const issuer_dn = "CN=" + std::string(issuer_name);
        mbedtls_x509write_crt_set_subject_name(&ctx, subject_dn.c_str());
        mbedtls_x509write_crt_set_issuer_name(&ctx, issuer_dn.c_str());

        unsigned char const serial = static_cast<unsigned char>(next_serial_++);
        mbedtls_x509write_crt_set_serial_raw(&ctx, const_cast<unsigned char*>(&serial), 1);

        std::string const nb(not_before), na(not_after);
        mbedtls_x509write_crt_set_validity(&ctx, nb.c_str(), na.c_str());
        mbedtls_x509write_crt_set_basic_constraints(&ctx, is_ca ? 1 : 0, is_ca ? -1 : 0);

        std::string const san_owned(san_hostname);
        mbedtls_x509_san_list san{};
        if (!san_owned.empty()) {
            san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
            san.node.san.unstructured_name.p =
                reinterpret_cast<unsigned char*>(const_cast<char*>(san_owned.data()));
            san.node.san.unstructured_name.len = san_owned.size();
            san.next = nullptr;
            mbedtls_x509write_crt_set_subject_alternative_name(&ctx, &san);
        }

        unsigned char cert_buf[4096];
        int const cert_len = mbedtls_x509write_crt_pem(&ctx, cert_buf, sizeof(cert_buf),
                                                         mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_x509write_crt_free(&ctx);

        unsigned char key_buf[4096];
        int const key_len = mbedtls_pk_write_key_pem(subject_key, key_buf, sizeof(key_buf));

        GeneratedKeyCert out;
        if (cert_len >= 0) out.cert_pem.assign(reinterpret_cast<char*>(cert_buf));
        if (key_len >= 0) out.key_pem.assign(reinterpret_cast<char*>(key_buf));
        return out;
    }

private:
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context drbg_{};
    int next_serial_ = 1;
};

// -- a loopback TLS server, one handshake per connection, then a fixed HTTP response ---------------

bool wait_ready(quark::pal::fd_t fd, bool for_write, int timeout_ms) {
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

struct BioCtx {
    quark::pal::fd_t fd;
};
int bio_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, true, 5000)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = quark::pal::send_some(c->fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, false, 5000)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = quark::pal::recv_some(c->fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(*r);
}

class TlsTestServer {
public:
    TlsTestServer(GeneratedKeyCert const& kc) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-https-test-server";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                               reinterpret_cast<unsigned char const*>(pers), std::strlen(pers));
        mbedtls_x509_crt_parse(&cert_, reinterpret_cast<unsigned char const*>(kc.cert_pem.c_str()),
                                kc.cert_pem.size() + 1);
        mbedtls_pk_parse_key(&key_, reinterpret_cast<unsigned char const*>(kc.key_pem.c_str()),
                              kc.key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg_);

        auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *quark::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~TlsTestServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) quark::pal::close_fd(listen_fd_);
        mbedtls_pk_free(&key_);
        mbedtls_x509_crt_free(&cert_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TlsTestServer(TlsTestServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int accept_count() const { return accept_count_.load(); }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = quark::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            accept_count_.fetch_add(1);
            serve_one(*a);
            quark::pal::close_fd(*a);
        }
    }

    void serve_one(quark::pal::fd_t fd) {
        BioCtx ctx{fd};
        mbedtls_ssl_config conf;
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            return;
        }
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_own_cert(&conf, &cert_, &key_);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            return;
        }
        mbedtls_ssl_set_bio(&ssl, &ctx, bio_send, bio_recv, nullptr);

        int ret;
        for (;;) {
            ret = mbedtls_ssl_handshake(&ssl);
            if (ret == 0) break;
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            return;  // handshake failed (expected for the negative-control tests) -- nothing to serve
        }

        // Drain whatever the client sent, then write a fixed 200 OK -- these tests only need to
        // prove the handshake outcome, not exercise real HTTP semantics over the TLS session.
        char drain[512];
        for (int i = 0; i < 20; ++i) {
            int const n = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(drain), sizeof(drain));
            if (n <= 0) break;
        }
        char const* const body = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
        std::size_t sent = 0;
        std::size_t const total = std::strlen(body);
        while (sent < total) {
            int const n = mbedtls_ssl_write(&ssl, reinterpret_cast<unsigned char const*>(body + sent), total - sent);
            if (n <= 0) {
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }

    mbedtls_x509_crt cert_;
    mbedtls_pk_context key_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    bool ok_ = false;
    quark::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> accept_count_{0};
    std::jthread thread_;
};

}  // namespace

int main() {
#if defined(_WIN32)
    quark::pal::ensure_winsock();
#endif

    TestCertAuthority ca;
    mbedtls_pk_context root_key = ca.generate_key();
    GeneratedKeyCert const root =
        ca.issue(&root_key, "ae-https-test-root", &root_key, "ae-https-test-root",
                 "20240101000000", "20991231235959", /*is_ca=*/true);

    mbedtls_pk_context valid_leaf_key = ca.generate_key();
    GeneratedKeyCert const valid_leaf =
        ca.issue(&valid_leaf_key, "test.invalid", &root_key, "ae-https-test-root", "20240101000000",
                 "20991231235959", /*is_ca=*/false, "test.invalid");

    mbedtls_pk_context wrong_host_leaf_key = ca.generate_key();
    GeneratedKeyCert const wrong_host_leaf =
        ca.issue(&wrong_host_leaf_key, "wrong.invalid", &root_key, "ae-https-test-root", "20240101000000",
                 "20991231235959", /*is_ca=*/false, "wrong.invalid");

    mbedtls_pk_context expired_leaf_key = ca.generate_key();
    GeneratedKeyCert const expired_leaf =
        ca.issue(&expired_leaf_key, "test.invalid", &root_key, "ae-https-test-root", "20200101000000",
                 "20200201000000", /*is_ca=*/false, "test.invalid");  // both in the past

    // A second, independent CA -- never added to the client's trust store, so its own leaf is
    // "untrusted root" regardless of hostname/validity correctness.
    TestCertAuthority other_ca;
    mbedtls_pk_context other_root_key = other_ca.generate_key();
    GeneratedKeyCert const other_root =
        other_ca.issue(&other_root_key, "ae-https-other-root", &other_root_key, "ae-https-other-root",
                        "20240101000000", "20991231235959", /*is_ca=*/true);
    mbedtls_pk_context untrusted_leaf_key = other_ca.generate_key();
    GeneratedKeyCert const untrusted_leaf =
        other_ca.issue(&untrusted_leaf_key, "test.invalid", &other_root_key, "ae-https-other-root",
                        "20240101000000", "20991231235959", /*is_ca=*/false, "test.invalid");

    // -- C1: positive control -- valid cert, trusted root, matching hostname --------------------
    {
        TlsTestServer server(valid_leaf);
        check(server.ok(), "C1: valid-leaf test server started");
        if (server.ok()) {
            auto connect_r = quark::pal::tcp_connect(kLoopbackHostOrder, server.port());
            check(connect_r.has_value(), "C1: connect() to the test server succeeds");
            if (connect_r) {
                auto session = TlsClientSession::handshake(*connect_r, "test.invalid", root.cert_pem);
                check(session.has_value(), "C1: handshake succeeds against a valid cert + trusted root + matching hostname");
                if (session) {
                    std::string const req = "GET / HTTP/1.1\r\nHost: test.invalid\r\nConnection: close\r\n\r\n";
                    auto s = session->send(req);
                    check(s.has_value() && *s == req.size(), "C1: request sent over the TLS session");
                    char buf[256]{};
                    auto r = session->recv(buf, sizeof(buf) - 1);
                    check(r.has_value() && *r > 0, "C1: response received over the TLS session");
                    if (r && *r > 0) {
                        std::string_view const resp(buf, *r);
                        check(resp.find("200 OK") != std::string_view::npos,
                              "C1: the real HTTP response text round-trips through TLS unchanged");
                    }
                }
                quark::pal::close_fd(*connect_r);
            }
        }
    }

    // -- C2: untrusted root -- rejected -----------------------------------------------------------
    {
        TlsTestServer server(untrusted_leaf);
        check(server.ok(), "C2: untrusted-root test server started");
        if (server.ok()) {
            auto connect_r = quark::pal::tcp_connect(kLoopbackHostOrder, server.port());
            if (connect_r) {
                auto session = TlsClientSession::handshake(*connect_r, "test.invalid", root.cert_pem);
                check(!session.has_value(), "C2: a cert signed by a root NOT in the trust store is rejected");
                if (!session) check(session.error().code == "net.tls_certificate_rejected", "C2: specific diagnostic code");
                quark::pal::close_fd(*connect_r);
            }
        }
    }

    // -- C3: hostname mismatch -- rejected ---------------------------------------------------------
    {
        TlsTestServer server(wrong_host_leaf);
        check(server.ok(), "C3: wrong-hostname test server started");
        if (server.ok()) {
            auto connect_r = quark::pal::tcp_connect(kLoopbackHostOrder, server.port());
            if (connect_r) {
                // The client asks for "test.invalid" -- the server's cert is only valid for
                // "wrong.invalid" (trusted root, valid dates, just the wrong name).
                auto session = TlsClientSession::handshake(*connect_r, "test.invalid", root.cert_pem);
                check(!session.has_value(),
                      "C3: a trusted, unexpired cert valid for a DIFFERENT hostname is still rejected");
                if (!session) check(session.error().code == "net.tls_certificate_rejected", "C3: specific diagnostic code");
                quark::pal::close_fd(*connect_r);
            }
        }
    }

    // -- C4: expired certificate -- rejected -------------------------------------------------------
    {
        TlsTestServer server(expired_leaf);
        check(server.ok(), "C4: expired-leaf test server started");
        if (server.ok()) {
            auto connect_r = quark::pal::tcp_connect(kLoopbackHostOrder, server.port());
            if (connect_r) {
                auto session = TlsClientSession::handshake(*connect_r, "test.invalid", root.cert_pem);
                check(!session.has_value(), "C4: an expired certificate is rejected");
                if (!session) check(session.error().code == "net.tls_certificate_rejected", "C4: specific diagnostic code");
                quark::pal::close_fd(*connect_r);
            }
        }
    }

    mbedtls_pk_free(&untrusted_leaf_key);
    mbedtls_pk_free(&other_root_key);
    mbedtls_pk_free(&expired_leaf_key);
    mbedtls_pk_free(&wrong_host_leaf_key);
    mbedtls_pk_free(&valid_leaf_key);
    mbedtls_pk_free(&root_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_https_egress: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_https_egress: %d FAILURE(S)\n", g_failures);
    return 1;
}
