// Milestone 5 Phase J3 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, roadmap
// exit criterion; 004-Model-Provider-Plane.md §7 G2): "cancellation mid-stream releases the
// connection within a bounded time and leaves no orphaned socket or partial state (checked under
// ASan + a leak gate)."
//
// This file proves the REAL, narrower-than-G2's-full-text shape this milestone's actual code has --
// found by tracing the call path, not assumed complete:
//
//  (A) THE COMMON CASE, bounded trivially: `chat_stream()` (Phase D2/E2) performs one COMPLETE
//      blocking HTTP fetch before any item is ever pushed onto the ring (`perform_provider_https_
//      exchange` has no incremental read loop yet -- Phase C's own named gap). Against an ordinary,
//      fast-responding server, the fetch (and the underlying TLS connection) is ALREADY FINISHED --
//      socket already closed via the exchange's own normal completion -- by the time the consumer
//      can even observe the first item, let alone drop the stream. "No orphaned socket" holds here
//      by construction, proven with real timing (J3-R1/R2).
//
//  (B) THE GAP THIS FILE ORIGINALLY FOUND, NOW CLOSED BY ADR-017. As first written, J3-R3/R4/R5
//      asserted a FINDING rather than a property: `run_stream_worker` called
//      `perform_provider_https_exchange(..., /*stop_token=*/std::nullopt, ...)` at BOTH the OpenAI
//      (protocol/openai/chat_client.hpp) and Anthropic (protocol/anthropic/chat_client.hpp) call
//      sites, so a consumer dropping `stream<T>` cancelled the RING (core/stream.hpp) with no wiring
//      back into the underlying HTTP fetch. Against a slowly-responding server -- a realistic shape
//      for a large or throttled completion -- cancelling ~20ms in did not shorten the connection's
//      life at all: the detached worker read on until the fetch finished naturally, bounded only by
//      the coarse 10s `kIoTimeoutMs` stall detector.
//
//      ADR-017 wires it: `make_stream<T>` shares one `std::stop_source` between the two halves
//      (copies share stop-state, so no watcher thread and no polling), `stream<T>::cancel()` and
//      `~stream()` request stop, and each worker hands `stop_token()` to the fetch -- reaching Phase
//      C2's already-proven mid-flight bound (`test_provider_http_client.cpp`'s P2 case, ~440ms,
//      stop_token-driven), which was always real but had no caller threading a token through.
//
//      J3-R3/R4/R5 below are the SAME measurements, inverted, deliberately kept in the same currency
//      rather than replaced by a generic "it cancels" check: the original finding was that a
//      plausible cancellation story was false in a way only measurement exposed, so the fix has to be
//      shown the same way. They now assert the server's paced send is cut short. See their own notes
//      for which timestamps this harness genuinely can and cannot measure.
//
//  "Checked under ASan + a leak gate": this test's own behavioral claims (A)/(B) hold under the
// project's ordinary build; the ASan half of the gate is satisfied by CI's existing ASan job running
// the full test suite (this file included), not by a separate sanitizer-specific build invoked here.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if defined(_WIN32)
#else
#include <sys/select.h>
#endif

#include "agentengine/pal/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;

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
        char const* const pers = "ae-cancel-bound-test-ca";
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
    // Mutable per-call read timeout -- the handshake and the FIRST post-handshake read need the full
    // budget (a client on loopback can still take a moment to connect+write), but a server that reads
    // in a "drain up to N times" loop after the request has already fully arrived should not block
    // for the full budget just to confirm there is nothing more coming; TlsCannedServer::serve_one
    // lowers this after its first successful drain read.
    int recv_timeout_ms;
};
constexpr int kBioTimeoutMs = 2000;
constexpr int kDrainPollTimeoutMs = 100;

int bio_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, true, kBioTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::send_some(c->fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == agentengine::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, false, c->recv_timeout_ms)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = agentengine::pal::recv_some(c->fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == agentengine::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(*r);
}

// A server that drips its SSE body out in small delayed writes -- unlike test_openai_chat_client_
// live.cpp's TlsCannedServer (one immediate write), this one lets a test observe whether the
// CLIENT's cancellation shortened the connection's real lifetime, by recording whether its own write
// loop ran to full, uninterrupted completion (every write succeeded) or was cut short by the peer
// closing early (a write failure -- what a genuinely bounded cancellation would cause).
class SlowDripServer {
public:
    SlowDripServer(GeneratedKeyCert const& kc, int chunk_count, std::chrono::milliseconds inter_chunk_delay)
        : chunk_count_(chunk_count), inter_chunk_delay_(inter_chunk_delay) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-cancel-bound-test-server";
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
    ~SlowDripServer() {
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
    SlowDripServer(SlowDripServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    // True only once the write loop below sent every configured chunk successfully -- i.e. the
    // connection stayed alive and readable-by-the-peer for the WHOLE configured slow duration.
    [[nodiscard]] bool completed_fully() const { return completed_fully_.load(); }
    // How many of the configured chunks were actually written before the peer went away. The
    // meaningful teardown measurement in this harness -- see the J3-R4 note in main() for why
    // `finished_at()` cannot serve as a teardown TIMESTAMP (the server's own BIO timeout dominates it).
    [[nodiscard]] int chunks_sent() const { return chunks_sent_.load(); }
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> finished_at() const {
        std::lock_guard<std::mutex> lock(finished_mu_);
        return finished_at_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a);
            agentengine::pal::close_fd(*a);
        }
    }

    // Returns false the first time a write fails (peer gone) -- the honest signal that the
    // connection did NOT survive to the end of the configured slow send.
    bool write_all(mbedtls_ssl_context& ssl, std::string_view text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            int const n = mbedtls_ssl_write(&ssl, reinterpret_cast<unsigned char const*>(text.data() + sent),
                                             text.size() - sent);
            if (n <= 0) {
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    void serve_one(agentengine::pal::fd_t fd) {
        BioCtx ctx{fd, kBioTimeoutMs};
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

        bool ok_all = write_all(ssl, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                      "Transfer-Encoding: chunked\r\n\r\n");
        for (int i = 0; ok_all && i < chunk_count_; ++i) {
            std::string const sse_event = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
            char size_buf[32];
            std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", sse_event.size());
            ok_all = write_all(ssl, size_buf) && write_all(ssl, sse_event) && write_all(ssl, "\r\n");
            if (ok_all) {
                chunks_sent_.fetch_add(1);
                std::this_thread::sleep_for(inter_chunk_delay_);
            }
        }
        if (ok_all) ok_all = write_all(ssl, "0\r\n\r\n");

        {
            std::lock_guard<std::mutex> lock(finished_mu_);
            finished_at_ = std::chrono::steady_clock::now();
        }
        completed_fully_.store(ok_all);

        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }

    mbedtls_x509_crt cert_;
    mbedtls_pk_context key_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    int chunk_count_;
    std::chrono::milliseconds inter_chunk_delay_;
    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
    std::atomic<bool> completed_fully_{false};
    std::atomic<int> chunks_sent_{0};
    mutable std::mutex finished_mu_;
    std::optional<std::chrono::steady_clock::time_point> finished_at_;
};

class TlsCannedServer {
public:
    TlsCannedServer(GeneratedKeyCert const& kc, std::string raw_response)
        : raw_response_(std::move(raw_response)) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-cancel-bound-canned-server";
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
    ~TlsCannedServer() {
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
    TlsCannedServer(TlsCannedServer const&) = delete;

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
            serve_one(*a);
            agentengine::pal::close_fd(*a);
        }
    }

    void serve_one(agentengine::pal::fd_t fd) {
        BioCtx ctx{fd, kBioTimeoutMs};
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
            // The request arrived in this read -- any FURTHER read is just confirming there is
            // nothing more coming, which should not cost this file's own timing assertions the full
            // handshake-grade timeout (J3-R2 measures real wall-clock time for the "fast, common
            // case" leg; a 2s-per-request drain tax would make that measurement about this test
            // harness's own polling, not about chat_stream()'s real behavior).
            ctx.recv_timeout_ms = kDrainPollTimeoutMs;
        }

        std::size_t sent = 0;
        while (sent < raw_response_.size()) {
            int const n = mbedtls_ssl_write(&ssl, reinterpret_cast<unsigned char const*>(raw_response_.data() + sent),
                                             raw_response_.size() - sent);
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
    std::string raw_response_;
    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    req.messages.push_back(std::move(m));
    return req;
}

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    TestCertAuthority ca;
    mbedtls_pk_context leaf_key = ca.generate_key();
    GeneratedKeyCert const leaf = ca.issue_self_signed_leaf(&leaf_key, "localhost");

    auto const fake_resolver = [](std::string_view, std::uint16_t port) -> result<sandbox::VerifiedEndpoint> {
        return sandbox::VerifiedEndpoint{kLoopbackHostOrder, port};
    };

    InMemorySecretStore store;
    store.set("openai-api-key", "sk-test-value");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"openai-api-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    // ---- (A) the common case: a fast, single-shot SSE response -- already fully fetched and the
    // socket already closed by the time the consumer could possibly drop the stream. Bounded
    // trivially, measured to confirm it (sub-second), not just asserted from reading the code. -------
    {
        std::string const sse = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hi\"}}]}\n\n"
                                 "data: [DONE]\n\n";
        std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n";
        out += "Transfer-Encoding: chunked\r\n\r\n";
        char size_buf[32];
        std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", sse.size());
        out += size_buf;
        out += sse;
        out += "\r\n0\r\n\r\n";
        TlsCannedServer server(leaf, out);
        check(server.ok(), "J3: fast server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            auto const t0 = std::chrono::steady_clock::now();
            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            while (!s.done()) {
                while (s.next()) { /* drain */
                }
                if (!s.done()) std::this_thread::yield();
            }
            auto const elapsed = std::chrono::steady_clock::now() - t0;
            check(s.terminal() == stream_terminal::closed, "J3-R1: fast case reaches Closed");
            check(elapsed < std::chrono::seconds(2),
                  "J3-R2: the common case (fast provider, non-incremental fetch) completes end to end "
                  "in well under 2s on loopback -- the socket is opened, fully read, and closed within "
                  "one ordinary request/response cycle, not left dangling");
        }
    }

    // ---- (B) the case J3 found broken and ADR-017 fixed: a slowly-drip-fed response, cancelled -----
    // almost immediately. Until ADR-017, dropping the consumer cancelled the RING but left the
    // underlying fetch running (`run_stream_worker` passed `stop_token = std::nullopt` at both the
    // OpenAI and Anthropic call sites), so the connection lived out the server's full ~900ms pacing
    // and release time was bounded only by the 10s idle stall detector. `make_stream<T>` now shares
    // one `std::stop_source` between the two halves; the consumer requests stop on `cancel()`/
    // destruction and the worker hands the token to `perform_provider_https_exchange`, reaching Phase
    // C2's already-proven mid-flight bound.
    //
    // The assertions below are the ORIGINAL J3-R3/R4/R5 measurements, inverted. They are deliberately
    // kept as measurements of the same quantities rather than replaced with a generic "it cancels"
    // check: the point of J3 was that a plausible-sounding cancellation story was false in a way only
    // timing could expose, so the fix has to be demonstrated in the same currency.
    //
    // What is measured is the CLIENT's release, not the server's perception. A server dripping into a
    // socket whose peer has gone does not learn about it on the next write -- it may buffer several
    // more chunks before TCP surfaces the reset -- so `finished_at()` tracks the server's own pacing
    // and is NOT the bound under test. `t_released` (when the consumer's scope actually exits) is.
    {
        constexpr int kChunkCount = 6;
        constexpr auto kInterChunkDelay = std::chrono::milliseconds(150);  // ~900ms total, well past
                                                                            // any "bounded" release
        SlowDripServer server(leaf, kChunkCount, kInterChunkDelay);
        check(server.ok(), "J3: slow-drip server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            auto const t_create = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point t_cancel_requested;
            {
                stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
                // A brief grace period to let the TLS handshake actually start before cancelling --
                // NOT waiting for any item (the whole point is cancelling before the slow send is
                // anywhere near done).
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                t_cancel_requested = std::chrono::steady_clock::now();
                s.cancel();
                // `s` is dropped here too (destructor), matching the realistic "caller just stops
                // caring" shape as well as the explicit cancel() call.
            }
            auto const t_released = std::chrono::steady_clock::now();
            auto const t_cancel = t_released;  // kept for the server-side poll budget below

            // Poll (bounded, generous) for the server's write loop to finish one way or the other.
            bool observed = false;
            for (int i = 0; i < 100 && !observed; ++i) {
                if (server.finished_at().has_value()) observed = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            check(observed, "J3: the slow server's write loop finished (one way or another) within "
                             "the test's own generous poll budget");
            check(!server.completed_fully(),
                  "J3-R3 (ADR-017, was the FINDING, now inverted): the server's slow send did NOT run "
                  "to completion -- cancelling ~20ms in tears the underlying TLS connection down, so "
                  "the peer's later writes fail instead of all 6 delayed chunks landing. Before "
                  "ADR-017 this exact assertion held in the opposite direction.");

            // WHAT THIS HARNESS CAN AND CANNOT MEASURE, stated rather than papered over.
            //
            // Two tempting timestamps are both useless here:
            //   - the consumer's own scope exit. `s.cancel()` and `~stream()` are non-blocking and
            //     always were, so timing the scope passes identically with and without ADR-017. A
            //     check that cannot fail proves nothing (CLAUDE.md), so it is printed below as
            //     context and asserted on nowhere.
            //   - `finished_at()`, when the server's write loop stopped. Measured at ~2150ms, but that
            //     is the SERVER's own BIO timeout (kBioTimeoutMs, 2000ms) elapsing on a write to a
            //     socket whose peer has gone -- it says when the server noticed, not when the client
            //     released. Asserting a millisecond bound on it would be asserting on mbedTLS's
            //     timeout constant.
            //
            // The quantity that IS both meaningful and precisely measurable is how far the server got
            // through its own paced send before the connection died. That is a real time bound
            // expressed in the server's own units: each delivered chunk costs a known 150ms.
            {
                int const delivered = server.chunks_sent();
                check(delivered < kChunkCount,
                      "J3-R4 (ADR-017): the peer got strictly FEWER than its configured chunks out "
                      "before the connection died -- the teardown is driven by the consumer's "
                      "cancellation, not by the server running out of things to send");
                // <= 2 delivered means the connection was gone within ~2 drip intervals (~300ms) of
                // the exchange starting, i.e. within ~280ms of the cancel at t+20ms. Compare the
                // pre-ADR-017 behaviour, where all 6 landed and the bound was the 10s stall detector.
                check(delivered <= 2,
                      "J3-R5 (ADR-017): the connection died within roughly one to two 150ms drip "
                      "intervals of the cancellation -- a real bound in the server's own pacing units. "
                      "Before ADR-017 all 6 chunks landed and release was bounded only by the coarse "
                      "10s idle stall detector. This is the precise inversion of the original J3-R5 "
                      "finding, measured in the same currency.");
                std::fprintf(stderr,
                             "  .. chunks delivered before teardown = %d of %d (each costs %lld ms of "
                             "server pacing)\n",
                             delivered, kChunkCount, static_cast<long long>(kInterChunkDelay.count()));
            }
            // Context only -- see above for why neither of these is the assertion.
            std::fprintf(stderr,
                         "  .. consumer scope exit after cancel = %lld ms (non-blocking by design, "
                         "before and after ADR-017)\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     t_released - t_cancel_requested)
                                                     .count()));
            if (server.finished_at()) {
                std::fprintf(stderr,
                             "  .. server write loop gave up %lld ms after cancel (its own %d ms BIO "
                             "timeout, not the client's release)\n",
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         *server.finished_at() - t_cancel_requested)
                                                         .count()),
                             kBioTimeoutMs);
            }

            // J3-R6 (not a runtime check -- see this file's own top comment): Phase C2's mid-flight
            // cancellation (test_provider_http_client.cpp's P2 case, ~440ms, stop_token-driven) is the
            // mechanism ADR-017 reaches. It was always real and bounded; what was missing was a caller
            // threading a live stop_token through. `chat_stream()` is now such a caller.
        }
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_stream_cancellation_bounded: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_stream_cancellation_bounded: %d FAILURE(S)\n", g_failures);
    return 1;
}
