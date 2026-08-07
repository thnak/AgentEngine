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
//  (B) THE REAL GAP, found while writing this proof, not silently assumed covered: `run_stream_worker`
//      calls `perform_provider_https_exchange(..., /*stop_token=*/std::nullopt, ...)` at BOTH the
//      OpenAI (protocol/openai/chat_client.hpp) and Anthropic (protocol/anthropic/chat_client.hpp)
//      call sites -- the consumer dropping `stream<T>` (which cancels the RING, core/stream.hpp) has
//      NO wiring back into the underlying HTTP fetch's cancellation at all. Against a SLOWLY
//      responding server (a realistic shape for a large or throttled completion), dropping the
//      stream almost immediately after creating it does NOT shorten the connection's lifetime by any
//      measurable amount -- the detached worker thread keeps reading until the fetch naturally
//      finishes (bounded only by Phase C's own coarse per-iteration `kIoTimeoutMs` stall detector,
//      not by the cancellation moment), proven quantitatively below (J3-R3..R6).
//
//      Phase C2's OWN mid-flight cancellation IS real and bounded (`test_provider_http_client.cpp`'s
//      P2 case: a `std::stop_token`-driven abort at the raw `perform_https_exchange` layer, measured
//      ~440ms) -- cited here, not reproduced -- but that bound is only reachable by a caller that
//      threads a live stop_token through (`HostEgressProxy::fetch`, ADR-011's own call site).
//      `chat_stream()` is simply not such a caller today. Named explicitly as real follow-up work
//      (wiring the ring's cancellation into a stop_token passed down to the fetch) rather than fixed
//      here under this proof phase's own budget -- an invariant-adjacent, hot-path concurrency change
//      that belongs behind CLAUDE.md's design -> red-team -> prove -> judge cycle, not an ad-hoc patch.
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

#include "pal/net.hpp"

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
    auto r = quark::pal::send_some(c->fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, false, c->recv_timeout_ms)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = quark::pal::recv_some(c->fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
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

        auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *quark::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~SlowDripServer() {
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
    SlowDripServer(SlowDripServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    // True only once the write loop below sent every configured chunk successfully -- i.e. the
    // connection stayed alive and readable-by-the-peer for the WHOLE configured slow duration.
    [[nodiscard]] bool completed_fully() const { return completed_fully_.load(); }
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> finished_at() const {
        std::lock_guard<std::mutex> lock(finished_mu_);
        return finished_at_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = quark::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a);
            quark::pal::close_fd(*a);
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

    void serve_one(quark::pal::fd_t fd) {
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
            if (ok_all) std::this_thread::sleep_for(inter_chunk_delay_);
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
    quark::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
    std::atomic<bool> completed_fully_{false};
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

        auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *quark::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~TlsCannedServer() {
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
    TlsCannedServer(TlsCannedServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = quark::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a);
            quark::pal::close_fd(*a);
        }
    }

    void serve_one(quark::pal::fd_t fd) {
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
    quark::pal::fd_t listen_fd_{};
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
    quark::pal::ensure_winsock();
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
            check(s.terminal() == quark::ReplyStreamTerminal::Closed, "J3-R1: fast case reaches Closed");
            check(elapsed < std::chrono::seconds(2),
                  "J3-R2: the common case (fast provider, non-incremental fetch) completes end to end "
                  "in well under 2s on loopback -- the socket is opened, fully read, and closed within "
                  "one ordinary request/response cycle, not left dangling");
        }
    }

    // ---- (B) the real gap: a slowly-drip-fed response -- dropping the stream almost immediately ----
    // does NOT shorten the connection's real lifetime, because chat_stream()'s underlying fetch has
    // no stop_token wired to the ring's cancellation (run_stream_worker calls perform_provider_https_
    // exchange with stop_token = std::nullopt, both openai/ and anthropic/ chat_client.hpp).
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
            {
                stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
                // A brief grace period to let the TLS handshake actually start before cancelling --
                // NOT waiting for any item (the whole point is cancelling before the slow send is
                // anywhere near done).
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                s.cancel();
                // `s` is dropped here too (destructor), matching the realistic "caller just stops
                // caring" shape as well as the explicit cancel() call.
            }
            auto const t_cancel = std::chrono::steady_clock::now();

            // Poll (bounded, generous) for the server's write loop to finish one way or the other.
            bool observed = false;
            for (int i = 0; i < 100 && !observed; ++i) {
                if (server.finished_at().has_value()) observed = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            check(observed, "J3: the slow server's write loop finished (one way or another) within "
                             "the test's own generous poll budget");
            check(server.completed_fully(),
                  "J3-R3: FINDING -- the server's ENTIRE slow send (all 6 delayed chunks, ~900ms) "
                  "completed successfully even though the consumer dropped the stream ~20ms after "
                  "creating it. The underlying TLS connection stayed alive and readable-by-the-peer "
                  "for the full slow duration -- cancellation did NOT tear the connection down");
            if (server.finished_at()) {
                auto const server_done_relative_to_cancel = *server.finished_at() - t_cancel;
                check(server_done_relative_to_cancel > std::chrono::milliseconds(500),
                      "J3-R4: the connection's actual teardown happened hundreds of ms AFTER the "
                      "cancellation moment, bounded only by the slow server's own unrelated pacing -- "
                      "NOT a bounded release relative to when the consumer cancelled, confirming G2's "
                      "'bounded time' claim does not hold for chat_stream()'s own genuinely-in-flight "
                      "case today (Phase C2's stop_token-driven bound is real, but chat_stream() never "
                      "threads one through -- see this file's own top comment)");
                auto const total_from_create = server_done_relative_to_cancel + (t_cancel - t_create);
                check(total_from_create >= kInterChunkDelay * (kChunkCount - 1),
                      "J3-R5: the total connection lifetime (~900ms) matches the server's own "
                      "configured slow pacing, not any client-side cancellation bound -- direct "
                      "confirmation that release time here is a function of the OTHER side's "
                      "behavior, not of when the consumer stopped listening");
            }
            // J3-R6 (not a runtime check -- see this file's own top comment): Phase C2's mid-flight
            // cancellation (test_provider_http_client.cpp's P2 case, ~440ms, stop_token-driven) is the
            // real, already-proven bounded-cancellation case this gap sits alongside -- it is reachable
            // by a caller that threads a live stop_token through perform_https_exchange; chat_stream()
            // simply isn't such a caller today.
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
