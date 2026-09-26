// Milestone 5 Phase C (docs/planning/milestone-5-providers-identity-secrets-breakdown.md):
// sandbox/provider_http_client.hpp -- the host-INITIATED HTTPS client 004 §3's real ChatClient
// backends (Phase D/E) will sit on. Two claims, neither covered by test_net_egress_proxy.cpp/
// test_https_egress.cpp (which prove the guest-mediated HostEgressProxy path and raw TLS-handshake
// certificate validation respectively, not this entry point):
//
//   P1: a request round-trips against a real (test, loopback) TLS server with NO cap::NetOut grant
//       anywhere in the call -- proving this is genuinely a separate, host-initiated path, not
//       HostEgressProxy wearing a new name.
//   P2 (004 §1 / Phase C2): a cancellation requested mid-response, via std::stop_token, aborts the
//       call within a bounded time -- well under how long the (deliberately stalling) test server
//       would otherwise take to finish -- rather than blocking until the response completes or the
//       underlying I/O timeout fires.
//
// Certificate generation follows test_https_egress.cpp's own established pattern (in-memory,
// deterministic, no filesystem/CLI dependency) -- a second, independent copy rather than a shared
// header, matching that file's own precedent of not sharing with test_net_egress_proxy.cpp either.
// Only P1's happy path is needed here (certificate REJECTION is already exhaustively proven there),
// so this file's TestCertAuthority is trimmed to just what issuing one valid leaf needs.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>

#include "agentengine/pal/net.hpp"

#if defined(_WIN32)
#else
#include <sys/select.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stop_token>
#include <string>
#include <thread>

#include "agentengine/sandbox/provider_http_client.hpp"

using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::perform_provider_https_exchange;

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

struct GeneratedKeyCert {
    std::string cert_pem;
    std::string key_pem;
};

class TestCertAuthority {
public:
    TestCertAuthority() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-provider-http-test-ca";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                               reinterpret_cast<unsigned char const*>(pers), std::strlen(pers));
    }
    ~TestCertAuthority() {
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TestCertAuthority(TestCertAuthority const&) = delete;

    mbedtls_pk_context generate_key() {
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg_);
        return pk;
    }

    GeneratedKeyCert issue_self_signed_leaf(mbedtls_pk_context* key, std::string_view name) {
        mbedtls_x509write_cert ctx;
        mbedtls_x509write_crt_init(&ctx);
        mbedtls_x509write_crt_set_version(&ctx, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&ctx, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&ctx, key);
        mbedtls_x509write_crt_set_issuer_key(&ctx, key);

        std::string const dn = "CN=" + std::string(name);
        mbedtls_x509write_crt_set_subject_name(&ctx, dn.c_str());
        mbedtls_x509write_crt_set_issuer_name(&ctx, dn.c_str());

        unsigned char const serial = 1;
        mbedtls_x509write_crt_set_serial_raw(&ctx, const_cast<unsigned char*>(&serial), 1);
        mbedtls_x509write_crt_set_validity(&ctx, "20240101000000", "20991231235959");
        mbedtls_x509write_crt_set_basic_constraints(&ctx, 0, 0);

        std::string const name_owned(name);
        mbedtls_x509_san_list san{};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p =
            reinterpret_cast<unsigned char*>(const_cast<char*>(name_owned.data()));
        san.node.san.unstructured_name.len = name_owned.size();
        san.next = nullptr;
        mbedtls_x509write_crt_set_subject_alternative_name(&ctx, &san);

        unsigned char cert_buf[4096];
        int const cert_len = mbedtls_x509write_crt_pem(&ctx, cert_buf, sizeof(cert_buf),
                                                         mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_x509write_crt_free(&ctx);

        unsigned char key_buf[4096];
        int const key_len = mbedtls_pk_write_key_pem(key, key_buf, sizeof(key_buf));

        GeneratedKeyCert out;
        if (cert_len >= 0) out.cert_pem.assign(reinterpret_cast<char*>(cert_buf));
        if (key_len >= 0) out.key_pem.assign(reinterpret_cast<char*>(key_buf));
        return out;
    }

private:
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context drbg_{};
};

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

struct BioCtx {
    agentengine::pal::fd_t fd;
};
// Short, not test_https_egress.cpp's 5000ms: that file's own drain loop (below) can afford a long
// per-read timeout because it only ever runs once per test case. This file's P2 case deliberately
// interleaves the server's writes with real wall-clock sleeps (simulating a slow provider) that a
// cancellation test needs to measure precisely against -- a long BIO timeout here would make the
// drain loop's own "wait for more client data that isn't coming" step dominate the measured time
// instead of the thing actually under test.
constexpr int kBioTimeoutMs = 200;

int bio_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, true, kBioTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::send_some(c->fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == agentengine::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, false, kBioTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::recv_some(c->fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == agentengine::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(*r);
}

// `slow`: false -> a normal, complete, immediate response (P1). true -> sends a header + a small
// body prefix, then sleeps well past both this test's own assertions AND its own timeout, simulating
// a stalled/slow provider (P2) -- the client is expected to have already given up and returned by
// the time this server would otherwise finish.
class TlsTestServer {
public:
    TlsTestServer(GeneratedKeyCert const& kc, bool slow) : slow_(slow) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-provider-http-test-server";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                               reinterpret_cast<unsigned char const*>(pers), std::strlen(pers));
        mbedtls_x509_crt_parse(&cert_, reinterpret_cast<unsigned char const*>(kc.cert_pem.c_str()),
                                kc.cert_pem.size() + 1);
        mbedtls_pk_parse_key(&key_, reinterpret_cast<unsigned char const*>(kc.key_pem.c_str()),
                              kc.key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg_);

        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~TlsTestServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
        mbedtls_pk_free(&key_);
        mbedtls_x509_crt_free(&cert_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    TlsTestServer(TlsTestServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a, st);
            agentengine::pal::close_fd(*a);
        }
    }

    void serve_one(agentengine::pal::fd_t fd, std::stop_token st) {
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

        for (;;) {
            int const ret = mbedtls_ssl_handshake(&ssl);
            if (ret == 0) break;
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            return;
        }

        char drain[512];
        for (int i = 0; i < 20; ++i) {
            int const n = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(drain), sizeof(drain));
            if (n <= 0) break;
        }

        auto write_all = [&](std::string_view text) {
            std::size_t sent = 0;
            while (sent < text.size()) {
                int const n = mbedtls_ssl_write(&ssl, reinterpret_cast<unsigned char const*>(text.data() + sent),
                                                 text.size() - sent);
                if (n <= 0) {
                    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
        };

        if (!slow_) {
            write_all("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello");
        } else {
            // Mimics a real SSE-streaming provider connection (periodic small chunks, never total
            // silence) rather than one write followed by a long sleep: the client's `recv()` unblocks
            // on every chunk, so the cancellation check at the top of the read loop
            // (net_egress_proxy.cpp's `check_not_cancelled`) actually gets exercised promptly instead
            // of sitting inside one multi-second blocking call the whole time. Advertises a body far
            // longer than ever actually sent; the client is expected to cancel out long before this
            // loop's own ~5s budget runs out.
            write_all("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1000000\r\n\r\n");
            for (int i = 0; i < 25 && !st.stop_requested(); ++i) {
                write_all("x");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));  // ~5s total
            }
        }
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }

    mbedtls_x509_crt cert_;
    mbedtls_pk_context key_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    bool slow_;
    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    // "localhost", not test_https_egress.cpp's "test.invalid": unlike TlsClientSession::handshake
    // (called directly there, given an already-connected loopback fd), perform_provider_https_exchange
    // does its own hostname RESOLUTION first (resolve_and_validate) -- exactly the real-world path a
    // ChatClient backend takes when constructed with "api.openai.com" -- so the test host must
    // actually resolve. "localhost" -> 127.0.0.1 is guaranteed offline (the OS hosts database, no
    // live DNS query) the way an RFC 6761 reserved name like "test.invalid" is not.
    TestCertAuthority ca;
    mbedtls_pk_context leaf_key = ca.generate_key();
    GeneratedKeyCert const leaf = ca.issue_self_signed_leaf(&leaf_key, "localhost");

    NetEgressRequest req;
    req.method = "GET";
    req.path = "/v1/chat/completions";
    req.headers.emplace_back("Authorization", "Bearer test-only-value");

    // Answers exactly the question DNS answers (the target's real IPv4 address) without touching
    // `is_blocked_address`'s own real enforcement -- the loopback test server's address IS loopback,
    // which `resolve_and_validate` correctly blocks in production (already exhaustively proven by
    // test_net_egress_proxy.cpp); this is `perform_provider_https_exchange`'s own injectable-resolver
    // testability seam (this file's own header comment), not a bypass of that defense.
    auto const fake_resolver =
        [](std::string_view, std::uint16_t port) -> agentengine::result<agentengine::sandbox::VerifiedEndpoint> {
        return agentengine::sandbox::VerifiedEndpoint{kLoopbackHostOrder, port};
    };

    // ---- P1: round-trips with NO cap::NetOut grant anywhere in the call --------------------------
    {
        TlsTestServer server(leaf, /*slow=*/false);
        check(server.ok(), "P1: test server started");
        if (server.ok()) {
            // Note: no CapabilitySet, no cap::NetOut, nothing capability-shaped passed anywhere --
            // the whole point of this being a SEPARATE entry point from HostEgressProxy::fetch.
            auto resp = perform_provider_https_exchange("localhost", server.port(), req, {}, std::nullopt,
                                                          fake_resolver, leaf.cert_pem);
            check(resp.has_value(), "P1: the call succeeds with no capability grant involved at all");
            if (resp.has_value()) {
                check(resp->status == 200, "P1: real status code round-trips");
                check(resp->body == "hello", "P1: real body round-trips");
            }
        }
    }

    // ---- P2 (Phase C2): cancellation mid-response aborts within a bounded time --------------------
    {
        TlsTestServer server(leaf, /*slow=*/true);
        check(server.ok(), "P2: slow test server started");
        if (server.ok()) {
            std::stop_source src;
            auto const started = std::chrono::steady_clock::now();

            agentengine::result<agentengine::sandbox::NetEgressResponse> outcome{
                agentengine::sandbox::NetEgressResponse{}};
            std::thread worker([&] {
                outcome = perform_provider_https_exchange("localhost", server.port(), req, src.get_token(),
                                                            std::nullopt, fake_resolver, leaf.cert_pem);
            });

            // Give the connection/handshake/first-partial-chunk enough time to land deterministically
            // on loopback, then request cancellation -- well before the server's ~5s stall ends.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            src.request_stop();
            worker.join();

            auto const elapsed = std::chrono::steady_clock::now() - started;
            check(elapsed < std::chrono::seconds(3),
                  "P2: the call returns well before the server's own ~5s stall ends -- cancellation "
                  "actually short-circuits the read loop, it doesn't just get lucky on a timeout");
            check(!outcome.has_value(), "P2: a cancelled call reports failure, not a truncated success");
            if (!outcome.has_value()) {
                check(outcome.error().code == "net.cancelled", "P2: specific diagnostic code");
            }
        }
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_provider_http_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_provider_http_client: %d FAILURE(S)\n", g_failures);
    return 1;
}
