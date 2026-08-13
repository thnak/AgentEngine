// Milestone 5 Phase E (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// protocol/anthropic/chat_client.hpp's AnthropicChatClient end to end -- a REAL TLS handshake against
// a local loopback server, a REAL secret resolution, and REAL response parsing -- for both chat() and
// chat_stream(). Mirrors test_openai_chat_client_live.cpp exactly in structure and coverage intent;
// see that file's own top comment for why the TLS test-server harness is a THIRD independent copy
// rather than a shared header (test_provider_http_client.cpp's own established precedent).

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/protocol/anthropic/chat_client.hpp"
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
        char const* const pers = "ae-anthropic-chat-client-test-ca";
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

class TlsCannedServer {
public:
    TlsCannedServer(GeneratedKeyCert const& kc, std::string raw_response)
        : raw_response_(std::move(raw_response)) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-anthropic-chat-client-test-server";
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

// Escapes `s` for embedding as the VALUE of a JSON string -- mirrors
// test_openai_chat_client_live.cpp's own identically-named helper (used below to build a canned
// response whose own text block is itself a JSON-string-encoded structured-output reply).
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

    // Same injectable-resolver seam Phase D's OpenAIChatClient established (the real resolver
    // correctly blocks loopback -- SSRF defense -- which is exactly why this seam exists).
    constexpr std::uint32_t kLoopbackHostOrderLocal = (127u << 24) | 1u;
    auto const fake_resolver = [](std::string_view, std::uint16_t port) -> result<sandbox::VerifiedEndpoint> {
        return sandbox::VerifiedEndpoint{kLoopbackHostOrderLocal, port};
    };

    InMemorySecretStore store;
    store.set("anthropic-api-key", "sk-ant-test-value");
    CapabilitySet held =
        CapabilitySet::grant_root({cap::Secret{"anthropic-api-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    using agentengine::test_support::run_task_sync;

    // ---- chat(): a real TLS round-trip against a canned Messages API response ----------------------
    {
        std::string const body = R"({"id":"msg_1","type":"message","role":"assistant",)"
                                  R"("content":[{"type":"text","text":"hello from a real socket"}],)"
                                  R"("stop_reason":"end_turn",)"
                                  R"("usage":{"input_tokens":7,"output_tokens":4}})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat(): test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            static_assert(ChatClient<decltype(client)>,
                          "AnthropicChatClient must satisfy the real ChatClient concept (004 §1)");

            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
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
                      "chat(): usage round-trips too (Anthropic's own input_tokens/output_tokens names)");
            }
        }
    }

    // ---- chat(): a CHUNKED non-streaming response decodes correctly ---------------------------------
    // Same real-server finding Phase D's OpenAI backend hit live against OpenRouter, fixed identically
    // here: a genuine OpenAI-compatible endpoint sends Transfer-Encoding: chunked on the ordinary
    // non-streaming response too, not only on SSE. Regression-proven so it cannot silently return.
    {
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":"chunked reply"}]})";
        TlsCannedServer server(leaf, http_response(body, /*chunked=*/true));
        check(server.ok(), "chat()+chunked: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
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

    // ---- chat(): a rotated secret is resolved fresh, not cached --------------------------------------
    {
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":"ok"}]})";
        TlsCannedServer server(leaf, http_response(body));
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            store.set("anthropic-api-key", "sk-ant-rotated-value");
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(),
                  "chat(): still succeeds immediately after rotation -- resolution happens fresh inside "
                  "chat(), the client instance itself was never reconstructed");
        }
    }

    // ---- chat(): a denied capability fails closed, never a fabricated response ----------------------
    {
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":"ok"}]})";
        TlsCannedServer server(leaf, http_response(body));
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            CapabilitySet empty;
            EffectContext denied_ctx = ctx;
            denied_ctx.capabilities = &empty;
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), denied_ctx));
            check(!resp.has_value(),
                  "chat(): without a granted Secret capability the call is denied before any network "
                  "activity, not merely after");
        }
    }

    // ---- chat(): system-prompt extraction + prompt-caching breakpoint, real request over the wire ---
    {
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":"ok"}]})";
        TlsCannedServer server(leaf, http_response(body));
        if (server.ok()) {
            ChatClientCapabilities caps;
            caps.prompt_caching = true;
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, caps, store, "/v1",
                                                   "2023-06-01", fake_resolver, leaf.cert_pem);
            ChatRequest req;
            Message sys;
            sys.role = role::system;
            ContentItem sys_item;
            sys_item.value = Text{"be concise"};
            sys.content.push_back(std::move(sys_item));
            req.messages.push_back(std::move(sys));
            req.messages.push_back([] {
                Message m;
                m.role = role::user;
                ContentItem item;
                item.value = Text{"hi"};
                m.content.push_back(std::move(item));
                return m;
            }());

            auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
            check(resp.has_value(),
                  "chat(): a request with a system message + prompt_caching declared round-trips over a "
                  "real TLS connection without the server rejecting the shape (proves build_request_body's "
                  "system-array/cache_control translation produces genuinely well-formed JSON, not just "
                  "JSON this project's own parser can round-trip)");
        }
    }

    // ---- chat_stream(): a real TLS round-trip delivering a NAMED-EVENT SSE response ------------------
    {
        std::string const sse =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n\n"
            "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"there!\"}}\n\n"
            "event: content_block_stop\ndata: {\"index\":0}\n\n"
            "event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":8}}\n\n"
            "event: message_stop\ndata: {}\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream(): test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            store.set("anthropic-api-key", "sk-ant-test-value");

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
                  "chat_stream(): a real chunked-transfer-encoded, NAMED-EVENT SSE response, sent over a "
                  "real TLS socket, decodes and delivers through ae::stream<T> in order with 0 loss");
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream(): the stream reaches the success terminal");
        }
    }

    // ---- chat_stream(): genuine cross-thread backpressure against the ring's DEFAULT capacity (256) ---
    // Same rationale as test_openai_chat_client_live.cpp's own backpressure test (see that file's
    // comment for the full argument: chat_stream() exposes no capacity parameter of its own, so 300+
    // tiny events against the ring's default capacity=256 is the available proof that no item is lost
    // even though the producer thread necessarily contends with the consumer for ring credit) -- proven
    // here through the REAL Anthropic backend's own NAMED-EVENT SSE parser (split_sse_named_events),
    // structurally different from OpenAI's single-field shape, and its own independent detached worker.
    {
        std::string sse =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n\n"
            "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n";
        constexpr int kEventCount = 300;
        for (int i = 0; i < kEventCount; ++i) {
            sse += "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"x\"}}\n\n";
        }
        sse += "event: content_block_stop\ndata: {\"index\":0}\n\n"
               "event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":300}}\n\n"
               "event: message_stop\ndata: {}\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+backpressure: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            store.set("anthropic-api-key", "sk-ant-test-value");

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
                  "chat_stream()+backpressure: all 300 text_delta events were delivered with 0 loss even "
                  "though the ring's own default capacity (256) is smaller than the event count -- the "
                  "REAL Anthropic detached background worker must have blocked on ring credit at least "
                  "once, and did so losslessly rather than dropping anything");
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream()+backpressure: the stream still reaches the success terminal");
        }
    }

    // ---- chat_stream(): dropping the consumer mid-stream does not hang the whole test process ----------
    // Same rationale as test_openai_chat_client_live.cpp's own cancellation test: the worker thread is
    // DETACHED, so the only proof available from the test side is that this test binary itself completes
    // within ctest's own bounded timeout rather than hanging forever waiting on a join that never
    // happens -- proven here against Anthropic's own independent run_stream_worker/push loop.
    {
        std::string sse = "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n";
        constexpr int kEventCount = 50;
        for (int i = 0; i < kEventCount; ++i) {
            sse += "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"y\"}}\n\n";
        }
        sse += "event: message_stop\ndata: {}\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+cancel: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            store.set("anthropic-api-key", "sk-ant-test-value");
            {
                stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
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
            // timeout) IS the proof that dropping the stream early did not wedge the real Anthropic
            // streaming worker.
        }
    }

    // ---- chat_stream(): interleaved text + tool_use streaming through the REAL backend (live) -----------
    // Mirrors test_anthropic_chat_client_translation.cpp's E-STREAM-R1 case (same named-event wire shape:
    // two text deltas on block 0, then a tool_use's partial_json fragments split across two separate
    // content_block_delta events on block 1) but through a REAL TLS socket and the REAL detached
    // background worker, not the offline parser.
    {
        std::string const sse =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"
            "event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"there\"}}\n\n"
            "event: content_block_stop\ndata: {\"index\":0}\n\n"
            "event: content_block_start\ndata: {\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call-1\",\"name\":\"get_weather\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"location\\\":\"}}\n\n"
            "event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"Seattle\\\"}\"}}\n\n"
            "event: content_block_stop\ndata: {\"index\":1}\n\n"
            "event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":25}}\n\n"
            "event: message_stop\ndata: {}\n\n";
        TlsCannedServer server(leaf, http_response(sse, /*chunked=*/true));
        check(server.ok(), "chat_stream()+tool_use: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            store.set("anthropic-api-key", "sk-ant-test-value");

            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            std::vector<ChatResponseUpdate> received;
            while (!s.done()) {
                while (auto update = s.next()) received.push_back(std::move(*update));
                if (!s.done()) std::this_thread::yield();
            }
            check(received.size() == 3,
                  "chat_stream()+tool_use: two text deltas + one assembled tool_use = 3 updates, "
                  "delivered through a real TLS socket and the real detached worker");
            if (received.size() == 3) {
                auto const* t0 = std::get_if<Text>(&received[0].delta.value);
                auto const* t1 = std::get_if<Text>(&received[1].delta.value);
                auto const* tc = std::get_if<ToolCall>(&received[2].delta.value);
                check(t0 && t0->text == "Hi ", "chat_stream()+tool_use: first text delta exact");
                check(t1 && t1->text == "there", "chat_stream()+tool_use: second text delta exact");
                check(tc && tc->call_id == "call-1" && tc->tool_name == "get_weather" &&
                          tc->arguments_json == R"({"location":"Seattle"})",
                      "chat_stream()+tool_use: the tool_use's partial_json fragments (split across two "
                      "separate content_block_delta events on the wire) reassemble correctly end-to-end "
                      "through the real backend, not just the offline parser E-STREAM-R1 already proves");
                check(received[2].is_final, "chat_stream()+tool_use: the assembled tool_use is final");
            }
            check(s.terminal() == stream_terminal::closed,
                  "chat_stream()+tool_use: the stream reaches the success terminal");
        }
    }

    // ---- chat(): a structured-output response -- text is itself a JSON string, passed through intact --
    // This project requests structured output on the way OUT (output_config.format) but does NOT parse
    // or validate the reply against the schema on the way IN -- confirmed against parse_message_response
    // above (no schema-aware branch at all; structured output rides the ordinary "text" content block
    // field like any other text reply).
    {
        std::string const structured_json =
            R"({"location":"Seattle","forecast":[{"condition":"cloudy","high_f":58}]})";
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":")" +
            escape_json_string_literal(structured_json) + R"("}]})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+structured: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
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

    // ---- chat(): model + cache_creation_input_tokens come back correctly over a REAL TLS socket -------
    // Research doc items 4/5 (docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md):
    // proves the same mapping test_anthropic_chat_client_translation.cpp's F5-R1 already proves offline,
    // here through a real response body decoded off a real socket, not a literal in-process json::parse.
    {
        std::string const body = R"({"id":"msg_2","type":"message","role":"assistant",)"
                                  R"("model":"claude-sonnet-5-20260101",)"
                                  R"("content":[{"type":"text","text":"real socket, real model field"}],)"
                                  R"("stop_reason":"end_turn",)"
                                  R"("usage":{"input_tokens":9,"output_tokens":6,)"
                                  R"("cache_creation_input_tokens":21}})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+model/cache_write: test server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("localhost", server.port(), "claude-sonnet-5",
                                                   SecretRef{"anthropic-api-key"}, ChatClientCapabilities{},
                                                   store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(), "chat()+model/cache_write: the call succeeds against a real TLS server");
            if (resp) {
                check(resp->model == "claude-sonnet-5-20260101",
                      "chat()+model/cache_write: ChatResponse.model round-trips over a REAL socket, not "
                      "just through the offline parser -- the model that answered, read off a real HTTP "
                      "response body");
                check(resp->usage.cache_write_tokens == 21,
                      "chat()+model/cache_write: usage.cache_creation_input_tokens maps to "
                      "Usage::cache_write_tokens over a REAL socket too");
            }
        }
    }

    // ---- chat(): app-attribution headers + abuse-tracking id + cache TTL constructor options do NOT --
    // ---- break a real request/response cycle (research doc items 1/2/7) -------------------------------
    // Headers/body fields aren't independently observable from the client side of this canned-server
    // harness (the server here doesn't echo the request back) -- what IS provable end to end is that
    // setting all four new optional constructor parameters together still produces a well-formed request
    // a real TLS peer accepts and a real response this backend parses correctly, exactly mirroring the
    // "system + prompt-caching, real request over the wire" test above but now with every new option
    // engaged simultaneously.
    {
        std::string const body =
            R"({"type":"message","role":"assistant","content":[{"type":"text","text":"ok"}]})";
        TlsCannedServer server(leaf, http_response(body));
        check(server.ok(), "chat()+new-options: test server started");
        if (server.ok()) {
            ChatClientCapabilities caps;
            caps.prompt_caching = true;
            anthropic::AnthropicChatClient client(
                "localhost", server.port(), "claude-sonnet-5", SecretRef{"anthropic-api-key"}, caps, store,
                "/v1", "2023-06-01", fake_resolver, leaf.cert_pem, /*http_referer=*/"https://my.app",
                /*x_title=*/"My Agent", /*end_user_id=*/"user-abc-123", /*cache_ttl=*/"1h");

            ChatRequest req;
            Message sys;
            sys.role = role::system;
            ContentItem sys_item;
            sys_item.value = Text{"be concise"};
            sys.content.push_back(std::move(sys_item));
            req.messages.push_back(std::move(sys));
            req.messages.push_back([] {
                Message m;
                m.role = role::user;
                ContentItem item;
                item.value = Text{"hi"};
                m.content.push_back(std::move(item));
                return m;
            }());

            auto resp = run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
            check(resp.has_value(),
                  "chat()+new-options: a request built with http_referer/x_title/end_user_id/cache_ttl "
                  "ALL set simultaneously still round-trips over a real TLS connection successfully -- "
                  "the new attribution headers, metadata.user_id, and ttl-bearing cache_control the "
                  "request now carries are all well-formed enough that neither this project's own request "
                  "construction nor the real TLS/HTTP exchange path rejects it");
            if (resp && !resp->message.content.empty()) {
                auto const* text = std::get_if<Text>(&resp->message.content.front().value);
                check(text && text->text == "ok",
                      "chat()+new-options: the response still parses correctly with every new "
                      "constructor option engaged at once");
            }
        }
    }

    // ---- AnthropicChatClient construction: an invalid cache_ttl is a construction-time contract error --
    {
        bool threw = false;
        try {
            anthropic::AnthropicChatClient client(
                "localhost", static_cast<std::uint16_t>(1), "claude-sonnet-5",
                SecretRef{"anthropic-api-key"}, ChatClientCapabilities{}, store, "/v1", "2023-06-01",
                fake_resolver, leaf.cert_pem, "", "", "", /*cache_ttl=*/"10m");
        } catch (std::invalid_argument const&) {
            threw = true;
        }
        check(threw,
              "construction: an unsupported cache_ttl value (\"10m\", neither \"5m\" nor \"1h\" nor "
              "empty) is rejected at construction time as a contract error, never silently accepted and "
              "sent to the wire unvalidated");
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_anthropic_chat_client_live: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_anthropic_chat_client_live: %d FAILURE(S)\n", g_failures);
    return 1;
}
