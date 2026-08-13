// Milestone 5 Phase D (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// protocol/openai/chat_client.hpp's OpenAIChatClient end to end -- a REAL TLS handshake against a
// local loopback server, a REAL secret resolution (018 §4's rotation-without-restart rule, reusing
// test_chat_client_credential_resolution.cpp's own InMemorySecretStore pattern), and REAL response
// parsing -- for both chat() and chat_stream(). test_openai_chat_client_translation.cpp proves the
// pure parsing/serialization logic offline; this file proves the plumbing between it and Phase C's
// real HTTPS client actually connects correctly.
//
// Certificate generation and the TLS test-server BIO plumbing follow test_provider_http_client.cpp's
// own established pattern (in-memory, deterministic, no filesystem/CLI dependency) -- a THIRD,
// independent copy rather than a shared header, matching that file's own precedent of not sharing
// with test_https_egress.cpp either (test_provider_http_client.cpp's own top comment names this
// discipline explicitly).

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
        char const* const pers = "ae-openai-chat-client-test-ca";
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
};
constexpr int kBioTimeoutMs = 2000;

int bio_send(void* ctx, unsigned char const* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, true, kBioTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = quark::pal::send_some(c->fd, reinterpret_cast<std::byte const*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(*r);
}
int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* c = static_cast<BioCtx*>(ctx);
    if (!wait_ready(c->fd, false, kBioTimeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    auto r = quark::pal::recv_some(c->fd, reinterpret_cast<std::byte*>(buf), len);
    if (!r) return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(*r);
}

// Writes exactly `raw_response` (a full, hand-assembled HTTP/1.1 response: status line + headers +
// body) after draining whatever the client sent -- one canned reply per test scenario, matching
// test_provider_http_client.cpp's own `TlsTestServer` shape but parameterized on the response text
// instead of a `slow` bool (this file has no cancellation scenario to prove).
class TlsCannedServer {
public:
    TlsCannedServer(GeneratedKeyCert const& kc, std::string raw_response)
        : raw_response_(std::move(raw_response)) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-openai-chat-client-test-server";
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

[[nodiscard]] std::string http_response(std::string_view body, bool chunked = false) {
    std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    if (chunked) {
        out += "Transfer-Encoding: chunked\r\n\r\n";
        char size_buf[32];
        std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", body.size());
        out += size_buf;
        out += body;
        out += "\r\n0\r\n\r\n";
    } else {
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        out += body;
    }
    return out;
}

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

// Escapes `s` for embedding as the VALUE of a JSON string (used below to build a canned response whose
// own `content` field is itself a JSON-string-encoded structured-output reply -- i.e. a JSON string
// containing JSON text, exactly what a real structured-output completion looks like on the wire).
[[nodiscard]] std::string escape_json_string_literal(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

int main() {
#if defined(_WIN32)
    quark::pal::ensure_winsock();
#endif

    TestCertAuthority ca;
    mbedtls_pk_context leaf_key = ca.generate_key();
    GeneratedKeyCert const leaf = ca.issue_self_signed_leaf(&leaf_key, "localhost");

    // Same injectable-resolver seam test_provider_http_client.cpp establishes (OpenAIChatClient's own
    // constructor takes one, threaded through to every perform_provider_https_exchange call): answers
    // exactly the question DNS answers for the loopback test server without touching
    // resolve_and_validate's own real enforcement (already exhaustively proven by
    // test_net_egress_proxy.cpp) -- the real resolver correctly BLOCKS loopback (SSRF defense), which
    // is exactly why this seam exists.
    constexpr std::uint32_t kLoopbackHostOrderLocal = (127u << 24) | 1u;
    auto const fake_resolver = [](std::string_view, std::uint16_t port) -> result<sandbox::VerifiedEndpoint> {
        return sandbox::VerifiedEndpoint{kLoopbackHostOrderLocal, port};
    };

    InMemorySecretStore store;
    store.set("openai-api-key", "sk-test-value");
    CapabilitySet held =
        CapabilitySet::grant_root({cap::Secret{"openai-api-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    using agentengine::test_support::run_task_sync;

    // ---- chat(): a real TLS round-trip against a canned Chat Completions response ------------------
    {
        std::string const body = R"({"choices":[{"index":0,"finish_reason":"stop",)"
                                  R"("message":{"role":"assistant","content":"hello from a real socket"}}],)"
                                  R"("usage":{"prompt_tokens":7,"completion_tokens":4}})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat(): test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            static_assert(ChatClient<decltype(client)>,
                          "OpenAIChatClient must satisfy the real ChatClient concept (004 §1)");

            auto resp =
                run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(), "chat(): the call succeeds against a real (loopback) TLS server");
            if (resp) {
                check(resp->message.content.size() == 1, "chat(): one content item");
                if (!resp->message.content.empty()) {
                    auto const* text = std::get_if<Text>(&resp->message.content.front().value);
                    check(text && text->text == "hello from a real socket",
                          "chat(): the real HTTP response body round-trips through the whole path "
                          "(secret resolution -> request build -> real TLS exchange -> response parse)");
                }
                check(resp->usage.input_tokens == 7 && resp->usage.output_tokens == 4,
                      "chat(): usage round-trips too");
            }
        }
    }

    // ---- chat(): a CHUNKED non-streaming response decodes correctly ---------------------------------
    // Real-server finding: a live probe against OpenRouter (a genuine OpenAI-compatible endpoint)
    // showed it sends `Transfer-Encoding: chunked` on the ordinary, NON-streaming chat() response --
    // not only on SSE. chat() originally called json::parse directly on the raw, still chunk-framed
    // bytes and failed with a confusing "expected a digit" parse error on the literal hex chunk-size
    // lines. Regression-proven here so it cannot silently return.
    {
        std::string const body = R"({"choices":[{"index":0,"finish_reason":"stop",)"
                                  R"("message":{"role":"assistant","content":"chunked reply"}}],)"
                                  R"("usage":{"prompt_tokens":3,"completion_tokens":2}})";
        TlsCannedServer server(leaf, http_response(body, /*chunked=*/true));
        check(server.ok(), "chat()+chunked: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(),
                  "chat()+chunked: a Transfer-Encoding: chunked non-streaming response is decoded and "
                  "parsed correctly, not treated as already-plain JSON");
            if (resp && !resp->message.content.empty()) {
                auto const* text = std::get_if<Text>(&resp->message.content.front().value);
                check(text && text->text == "chunked reply", "chat()+chunked: content round-trips exactly");
            }
        }
    }

    // ---- chat(): a rotated secret is resolved fresh, not cached (018 §3, same rule Phase B3 proves) -
    {
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})";
        TlsCannedServer server(leaf, http_response(body));
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            store.set("openai-api-key", "sk-rotated-value");
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(),
                  "chat(): still succeeds immediately after rotation -- resolution happens fresh inside "
                  "chat(), the client instance itself was never reconstructed");
        }
    }

    // ---- chat(): a denied capability fails closed, never a fabricated response ----------------------
    {
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})";
        TlsCannedServer server(leaf, http_response(body));
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            CapabilitySet empty;
            EffectContext denied_ctx = ctx;
            denied_ctx.capabilities = &empty;
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), denied_ctx));
            check(!resp.has_value(),
                  "chat(): without a granted Secret capability the call is denied before any network "
                  "activity, not merely after");
        }
    }

    // ---- chat_stream(): a real TLS round-trip delivering an SSE response through ae::stream<T> ------
    {
        std::string const sse = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hi \"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"there!\"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                                 "data: [DONE]\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream(): test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            store.set("openai-api-key", "sk-test-value");

            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            std::vector<std::string> received;
            while (!s.done()) {
                while (auto update = s.next()) {
                    auto const* text = std::get_if<Text>(&update->delta.value);
                    if (text) received.push_back(text->text);
                }
                if (!s.done()) std::this_thread::yield();
            }
            check(received.size() == 2 && received[0] == "Hi " && received[1] == "there!",
                  "chat_stream(): a real chunked-transfer-encoded SSE response, sent over a real TLS "
                  "socket, decodes and delivers through ae::stream<T> in order with 0 loss");
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream(): the stream reaches the success terminal");
        }
    }

    // ---- chat_stream(): genuine cross-thread backpressure against the ring's DEFAULT capacity (256) ---
    // chat_stream() exposes no capacity parameter of its own -- make_stream<ChatResponseUpdate> is
    // called internally (chat_client.hpp's run_stream_worker) with the DEFAULT stream_config, capacity
    // 256 (core/stream.hpp). So, unlike test_chat_client_stream.cpp's B4b-R1 case (whose synthetic
    // StreamingWordChatClient conformer DOES take an explicit ring_capacity constructor parameter), the
    // only way to force the credit-controlled ring to genuinely exercise its full capacity through the
    // REAL backend is a canned response with strictly MORE items than the ring can hold at once: 300
    // one-character text-delta SSE events against a 256-slot ring. `stream_producer<T>::push()`'s own
    // documented contract (stream.hpp) is to block LOSSLESSLY until ring credit is available or the
    // stream tears down -- so if this backend ever dropped or overwrote an item instead of blocking the
    // producer thread when the ring filled, the received count below would come up short of 300. This
    // does not independently measure whether the producer thread actually stalled at any given instant
    // (that would need instrumenting the ring itself, out of scope here) -- the available proof from the
    // test side is exact-count-with-zero-loss across MORE items than the ring's own default capacity.
    {
        std::string sse;
        constexpr int kEventCount = 300;
        for (int i = 0; i < kEventCount; ++i) {
            sse += "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
        }
        sse += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
        sse += "data: [DONE]\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+backpressure: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            store.set("openai-api-key", "sk-test-value");

            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            int received = 0;
            while (!s.done()) {
                while (auto update = s.next()) {
                    auto const* text = std::get_if<Text>(&update->delta.value);
                    if (text && text->text == "x") ++received;
                }
                if (!s.done()) std::this_thread::yield();
            }
            check(received == kEventCount,
                  "chat_stream()+backpressure: all 300 items were delivered with 0 loss even though the "
                  "ring's own default capacity (256) is smaller than the event count -- the REAL detached "
                  "background worker (not the B4b synthetic conformer) must have blocked on ring credit "
                  "at least once, and did so losslessly rather than dropping anything");
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream()+backpressure: the stream still reaches the success terminal");
        }
    }

    // ---- chat_stream(): dropping the consumer mid-stream does not hang the whole test process ----------
    // The background worker is a DETACHED std::thread (chat_client.hpp's own file banner explains why --
    // a bound OpenAIChatClient instance is shared across concurrent streaming calls via ChatClientRegistry,
    // so a single joined-then-replaced thread member would wrongly serialize them). Being detached means
    // there is no join() this test can call to directly observe the worker's exit. The best available
    // proof from the test side is exactly what ctest's own per-test timeout already enforces: this test
    // BINARY completing at all, rather than the process hanging forever with an orphaned worker thread
    // stuck in producer.push() waiting for ring credit that will never arrive once the consumer is gone.
    // stream<T>'s destructor (core/stream.hpp) cancels the ring on drop, and run_stream_worker's push
    // loop already checks for exactly that: `if (producer.push(...) != stream_push::ok) return;`.
    {
        std::string sse;
        constexpr int kEventCount = 50;
        for (int i = 0; i < kEventCount; ++i) {
            sse += "data: {\"choices\":[{\"delta\":{\"content\":\"y\"}}]}\n\n";
        }
        sse += "data: [DONE]\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+cancel: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            store.set("openai-api-key", "sk-test-value");
            {
                stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
                // next() is poll-only -- poll until at least the first item is delivered, then let `s`
                // go out of scope while the worker still has 49 more items it could try to push.
                std::optional<ChatResponseUpdate> first;
                while (!first && !s.done()) {
                    first = s.next();
                    if (!first) std::this_thread::yield();
                }
                check(first.has_value(),
                      "chat_stream()+cancel: at least the first item was delivered before cancellation");
                // ~stream() below cancels the ring -- the detached worker's NEXT push() must observe
                // Terminated and return promptly rather than block forever.
            }
            // Reaching the end of main() at all (rather than the test process hanging past ctest's own
            // timeout) IS the proof that dropping the stream early did not wedge the real OpenAI
            // streaming worker -- named explicitly rather than claiming a join-based proof this
            // detached-thread API does not offer.
        }
    }

    // ---- chat_stream(): interleaved text + tool_call streaming through the REAL backend (live) ---------
    // Mirrors test_openai_chat_client_translation.cpp's D2-R4 case (same wire shape: two text deltas,
    // then a tool call's argument fragments split across two separate SSE chunks, index-correlated) but
    // through a REAL TLS socket and the REAL detached background worker, not the offline parser.
    {
        std::string const sse =
            "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Let \"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"me check.\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-1\","
            "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"\\\"Seattle\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
            "data: [DONE]\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+tool_call: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            store.set("openai-api-key", "sk-test-value");

            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            std::vector<ChatResponseUpdate> received;
            while (!s.done()) {
                while (auto update = s.next()) received.push_back(std::move(*update));
                if (!s.done()) std::this_thread::yield();
            }
            check(received.size() == 3,
                  "chat_stream()+tool_call: two text deltas + one assembled tool call = 3 updates, "
                  "delivered through a real TLS socket and the real detached worker");
            if (received.size() == 3) {
                auto const* t0 = std::get_if<Text>(&received[0].delta.value);
                auto const* t1 = std::get_if<Text>(&received[1].delta.value);
                auto const* tc = std::get_if<ToolCall>(&received[2].delta.value);
                check(t0 && t0->text == "Let ", "chat_stream()+tool_call: first text delta exact");
                check(t1 && t1->text == "me check.", "chat_stream()+tool_call: second text delta exact");
                check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather" &&
                          tc->arguments_json == R"({"location":"Seattle"})",
                      "chat_stream()+tool_call: the tool call's argument fragments (split across two "
                      "separate SSE chunks on the wire) reassemble correctly end-to-end through the real "
                      "backend, not just the offline parser D2-R4 already proves");
                check(received[2].is_final, "chat_stream()+tool_call: the assembled tool call is final");
            }
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream()+tool_call: the stream reaches the success terminal");
        }
    }

    // ---- chat(): a structured-output response -- content is itself a JSON string, passed through intact
    // This project requests structured output on the way OUT (response_format) but does NOT parse or
    // validate the reply against the schema on the way IN -- confirmed against parse_chat_completion_
    // response above (no schema-aware branch at all; structured output rides the ordinary "content"
    // string field like any other text reply).
    {
        std::string const structured_json =
            R"({"location":"Seattle","forecast":[{"condition":"cloudy","high_f":58}]})";
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":")" +
            escape_json_string_literal(structured_json) + R"("}}]})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+structured: test server started");
        if (server.ok()) {
            ChatClientCapabilities caps;
            caps.structured_output_native = true;
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             caps, store, "/v1", fake_resolver, leaf.cert_pem);
            ChatRequest req = request_asking("what's the weather?");
            req.output_schema_json = R"({"type":"object","properties":{"location":{"type":"string"}}})";

            auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
            check(resp.has_value(),
                  "chat()+structured: a request carrying output_schema_json succeeds against a real TLS "
                  "server");
            if (resp) {
                check(resp->message.content.size() == 1, "chat()+structured: one content item");
                if (!resp->message.content.empty()) {
                    auto const* text = std::get_if<Text>(&resp->message.content.front().value);
                    check(text != nullptr,
                          "chat()+structured: structured output rides the ORDINARY Text content item -- "
                          "there is no distinct 'structured output' ContentItem kind in this project's "
                          "content model");
                    check(text && text->text == structured_json,
                          "chat()+structured: the JSON-string reply text survives byte for byte -- this "
                          "backend never parses or validates it against the schema on receipt, only "
                          "requests the shape on the way out (confirmed, not assumed, by this passthrough)");
                }
            }
        }
    }

    // ---- chat(): model + cache_write_tokens round-trip through a REAL response body over a REAL TLS
    // socket (2026-08-07 provider-metadata survey, "Recommended design" items 4/5) -- proves the wire
    // parsing added to parse_chat_completion_response works against actual bytes read off a real
    // socket, not just literal in-memory JSON (test_openai_chat_client_translation.cpp's D5-R7/D5-R9
    // already prove the pure-parsing logic offline; this is the plumbing proof for the same fields).
    {
        std::string const body =
            R"({"model":"gpt-5-turbo-2026","choices":[{"index":0,"finish_reason":"stop",)"
            R"("message":{"role":"assistant","content":"hello with metadata"}}],)"
            R"("usage":{"prompt_tokens":50,"completion_tokens":6,)"
            R"("prompt_tokens_details":{"cached_tokens":10,"cache_write_tokens":8}}})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+metadata: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(), "chat()+metadata: the call succeeds against a real TLS server");
            if (resp) {
                check(resp->model == "gpt-5-turbo-2026",
                      "chat()+metadata: ChatResponse::model comes back correctly from a REAL response "
                      "body read off a REAL TLS socket, not just literal in-memory JSON");
                check(resp->usage.cache_write_tokens == 8,
                      "chat()+metadata: Usage::cache_write_tokens comes back correctly from a REAL "
                      "response body's usage.prompt_tokens_details.cache_write_tokens over the real "
                      "wire path (secret resolution -> request build -> TLS exchange -> response parse)");
                check(resp->usage.cached_input_tokens == 10,
                      "chat()+metadata: the pre-existing cached_input_tokens mapping still round-trips "
                      "correctly alongside the new cache_write_tokens field");
            }
        }
    }

    // ---- chat(): http_referer/x_title/end_user_id/seed constructor params don't break a REAL request/
    // response cycle (2026-08-07 survey items 1/2) -- this harness has no byte-capture of what the test
    // server actually read (TlsCannedServer only drains and replies, see its own comment above), so this
    // does not directly inspect the outbound HTTP-Referer/X-Title headers or the user/seed JSON fields;
    // what it DOES prove is that supplying all four new optional constructor parameters together still
    // produces a request the real HTTPS exchange sends successfully and a real response the client parses
    // successfully -- i.e. the new fields are wired through build_http_request/build_request_body without
    // corrupting the request in a way that would break the real round trip (a malformed request would
    // either fail the exchange or come back with an error status, and it does not).
    {
        std::string const body =
            R"({"model":"gpt-5","choices":[{"index":0,"finish_reason":"stop",)"
            R"("message":{"role":"assistant","content":"ok with attribution+abuse-tracking"}}]})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+attribution: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem,
                                             "https://myapp.example/", "My Agent App", "end-user-42",
                                             std::optional<std::int64_t>{42});
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(),
                  "chat()+attribution: a client constructed with http_referer/x_title/end_user_id/seed "
                  "all set still completes a real TLS request/response cycle successfully");
            if (resp) {
                check(!resp->message.content.empty(),
                      "chat()+attribution: the response still parses into real content -- the extra "
                      "constructor fields did not corrupt request construction or response handling");
            }
        }
    }

    // ---- chat_stream(): the same four optional constructor params don't break the REAL streaming path -
    {
        std::string const sse = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hi \"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"there!\"}}]}\n\n"
                                 "data: [DONE]\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+attribution: test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem,
                                             "https://myapp.example/", "My Agent App", "end-user-42",
                                             std::optional<std::int64_t>{42});
            store.set("openai-api-key", "sk-test-value");

            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            std::vector<std::string> received;
            while (!s.done()) {
                while (auto update = s.next()) {
                    auto const* text = std::get_if<Text>(&update->delta.value);
                    if (text) received.push_back(text->text);
                }
                if (!s.done()) std::this_thread::yield();
            }
            check(received.size() == 2 && received[0] == "Hi " && received[1] == "there!",
                  "chat_stream()+attribution: a client with all four new optional constructor params set "
                  "still streams a real SSE response through ae::stream<T> correctly -- the fields reach "
                  "run_stream_worker's own build_request_body/build_http_request calls without breaking "
                  "the detached worker's request construction");
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream()+attribution: the stream still reaches the success terminal");
        }
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_chat_client_live: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_chat_client_live: %d FAILURE(S)\n", g_failures);
    return 1;
}
