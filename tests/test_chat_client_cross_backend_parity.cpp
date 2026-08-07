// Milestone 5 Phase J1 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, roadmap
// exit criterion; 004-Model-Provider-Plane.md §7 G1): "the same agent, unchanged, runs on >= 3
// backends; behavioural differences are limited to those the capability table predicts, and every
// applied fallback appears in the trace."
//
// Scoping decision, named honestly rather than silently assumed: "agent" here is proven at the
// ChatClient-plane call site (004's own scope), not by instantiating a real `ae::AgentSession<...>`
// actor. `AgentSession<ChatClientT>`'s `chat_client_` member (core/agent_session.hpp) has no public
// setter and no non-default constructor -- by design, per `initialize()`'s own comment: "the mock
// ChatClient's own canned behavior is already baked in by construction rather than passed through
// TestKit." `quark::TestKit<A>` (third_party/quark/include/quark/core/testkit.hpp:313) only ever
// default-constructs its actor ("A actor_; // default-init (implicit ctor)") -- Quark is a submodule
// this project never patches in-tree (CLAUDE.md), so there is no way to forward a runtime-configured
// `OpenAIChatClient<Store>`/`AnthropicChatClient<Store>` (Phase D/E -- neither is default-
// constructible; both need a host/port/SecretRef at construction) through `TestKit` today. Rather than
// invent new, unreviewed `AgentSession` wiring under this proof phase's own time budget (CLAUDE.md:
// invariant-adjacent, hot-path-adjacent surfaces go through design -> red-team -> prove -> judge, not
// an ad-hoc change), this file proves G1's actual claim at the layer 004 itself scopes to: ONE
// unchanged call site (`run_it`, below), executed verbatim against three real, product-code `ChatClient`
// conformers (Phase D's `OpenAIChatClient`, Phase E's `AnthropicChatClient`, Phase G's
// `ReplayChatClient`) plus a `FailoverChatClient<Primary, Fallback>` composition of two of them.
//
// Covers:
//  (1) Identical caller-visible reply text ("PARITY_OK") comes back from all three backends through
//      the exact same `run_it` call, proving the calling code needs zero per-backend branching.
//  (2) Metadata differences (ChatResponse::model, Usage numbers) differ EXACTLY as each backend's own
//      canned server/recording was independently configured to report -- not unpredicted drift.
//  (3) `select_output_schema_strategy` (agent_registry.hpp's own consumer, Phase B5) picks a DIFFERENT
//      strategy per backend, exactly as each backend's own declared `ChatClientCapabilities` predicts
//      (native / tool_shaped / parse_and_repair) -- the concrete, testable form of "capability-table-
//      predicted differences."
//  (4) `FailoverChatClient<OpenAIChatClient<Store>, AnthropicChatClient<Store>>`, with a primary
//      pointed at a real closed loopback port (a genuine network failure, not a synthetic test double)
//      and a fallback that genuinely succeeds -- `ChatResponse::fallback_tier == 1`, the "response
//      metadata" half of the trace gate (Phase F3), proven here through two REAL backend conformers,
//      not `test_failover_chat_client.cpp`'s synthetic doubles.
//
// TLS test-server plumbing is a fourth, independent copy of `test_openai_chat_client_live.cpp`'s own
// `TlsCannedServer`/`TestCertAuthority` pattern -- that file's own top comment already names this
// discipline ("a THIRD, independent copy rather than a shared header").

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

#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/failover_chat_client.hpp"
#include "agentengine/core/replay_chat_client.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
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
        char const* const pers = "ae-parity-test-ca";
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
        char const* const pers = "ae-parity-test-server";
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

[[nodiscard]] std::string http_response(std::string_view body) {
    std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out += body;
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

[[nodiscard]] std::string first_text(ChatResponse const& r) {
    if (r.message.content.empty()) return {};
    auto const* t = std::get_if<Text>(&r.message.content.front().value);
    return t ? t->text : std::string{};
}

// The one, unchanged call site every backend below is driven through -- literally the same function,
// never specialized or branched per-backend.
template <class ChatClientT>
[[nodiscard]] result<ChatResponse> run_it(ChatClientT& client, ChatRequest const& req, EffectContext& ctx) {
    using agentengine::test_support::run_task_sync;
    return run_task_sync<result<ChatResponse>>(client.chat(req, ctx));
}

[[nodiscard]] std::uint16_t unused_loopback_port() {
    // Bind + listen to claim a real ephemeral port from the OS, then immediately release it without
    // ever accepting a connection -- a subsequent connect attempt gets a real, deterministic
    // "nothing is listening" failure (not a hand-picked port number that might collide with something
    // else on the test machine).
    auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
    if (!listen_r) return 1;
    std::uint16_t const port = *quark::pal::local_port(*listen_r);
    quark::pal::close_fd(*listen_r);
    return port;
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
    store.set("anthropic-api-key", "sk-ant-test-value");
    CapabilitySet held = CapabilitySet::grant_root(
        {cap::Secret{"openai-api-key", std::chrono::seconds{0}}, cap::Secret{"anthropic-api-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    ChatRequest const req = request_asking("hi");

    // ---- (1)-(3): identical call site, three real backends, predicted-only differences -------------
    {
        // OpenAI leg: structured_output_native declared true -> select_output_schema_strategy == native.
        ChatClientCapabilities openai_caps;
        openai_caps.structured_output_native = true;
        std::string const openai_body =
            R"({"model":"gpt-5-mock","choices":[{"index":0,"finish_reason":"stop",)"
            R"("message":{"role":"assistant","content":"PARITY_OK"}}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5}})";
        TlsCannedServer openai_server(leaf, http_response(openai_body));
        check(openai_server.ok(), "J1: OpenAI test server started");

        // Anthropic leg: tool_calling declared true (structured_output_native false) ->
        // select_output_schema_strategy == tool_shaped -- a DIFFERENT strategy than OpenAI's, exactly
        // as each backend's own declared capabilities predicts.
        ChatClientCapabilities anthropic_caps;
        anthropic_caps.tool_calling = true;
        std::string const anthropic_body =
            R"({"id":"msg_1","type":"message","role":"assistant","model":"claude-mock",)"
            R"("content":[{"type":"text","text":"PARITY_OK"}],)"
            R"("stop_reason":"end_turn","usage":{"input_tokens":10,"output_tokens":5}})";
        TlsCannedServer anthropic_server(leaf, http_response(anthropic_body));
        check(anthropic_server.ok(), "J1: Anthropic test server started");

        // Replay leg: default capabilities (neither bit set) -> select_output_schema_strategy ==
        // parse_and_repair -- the universal last resort, a THIRD distinct strategy.
        ChatCallRecording recording;
        recording.mode = recording_mode::unary;
        ChatResponse replay_resp;
        replay_resp.message.role = role::assistant;
        replay_resp.message.message_id = "m-replay";
        ContentItem replay_item;
        replay_item.origin = content_origin::assistant;
        replay_item.value = Text{"PARITY_OK"};
        replay_resp.message.content.push_back(replay_item);
        replay_resp.model = "replay-mock";
        replay_resp.usage.input_tokens = 10;
        replay_resp.usage.output_tokens = 5;
        recording.response = replay_resp;

        if (openai_server.ok() && anthropic_server.ok()) {
            openai::OpenAIChatClient openai_client("localhost", openai_server.port(), "gpt-5",
                                                     SecretRef{"openai-api-key"}, openai_caps, store, "/v1",
                                                     fake_resolver, leaf.cert_pem);
            anthropic::AnthropicChatClient anthropic_client(
                "localhost", anthropic_server.port(), "claude-sonnet-5", SecretRef{"anthropic-api-key"},
                anthropic_caps, store, "/v1", "2023-06-01", fake_resolver, leaf.cert_pem);
            ReplayChatClient replay_client(recording);

            auto openai_resp = run_it(openai_client, req, ctx);
            auto anthropic_resp = run_it(anthropic_client, req, ctx);
            auto replay_resp2 = run_it(replay_client, req, ctx);

            check(openai_resp.has_value() && anthropic_resp.has_value() && replay_resp2.has_value(),
                  "J1-R1: the same unchanged call site (run_it) succeeds against all three real "
                  "ChatClient conformers (OpenAI-compatible, Anthropic, recorded-replay)");

            if (openai_resp && anthropic_resp && replay_resp2) {
                check(first_text(*openai_resp) == "PARITY_OK" && first_text(*anthropic_resp) == "PARITY_OK" &&
                          first_text(*replay_resp2) == "PARITY_OK",
                      "J1-R2: identical caller-visible reply text comes back from all three backends -- "
                      "the calling code needed zero per-backend branching to get the same answer");

                check(openai_resp->model == "gpt-5-mock" && anthropic_resp->model == "claude-mock" &&
                          replay_resp2->model == "replay-mock",
                      "J1-R3: ChatResponse::model differs across backends EXACTLY as each backend's own "
                      "canned response was independently configured to report -- predicted metadata "
                      "drift, not unpredicted protocol drift");

                check(openai_resp->usage.input_tokens == 10 && anthropic_resp->usage.input_tokens == 10 &&
                          replay_resp2->usage.input_tokens == 10,
                      "J1-R4: usage numbers agree across backends when the canned data agrees -- the "
                      "same Usage shape is produced regardless of which backend answered");
            }

            check(select_output_schema_strategy(openai_client.capabilities()) == output_schema_strategy::native,
                  "J1-R5: OpenAI backend's declared structured_output_native=true predicts strategy=native");
            check(select_output_schema_strategy(anthropic_client.capabilities()) ==
                      output_schema_strategy::tool_shaped,
                  "J1-R6: Anthropic backend's declared tool_calling=true (structured_output_native=false) "
                  "predicts strategy=tool_shaped -- a DIFFERENT strategy than OpenAI's, exactly as its "
                  "own capabilities differ");
            check(select_output_schema_strategy(replay_client.capabilities()) ==
                      output_schema_strategy::parse_and_repair,
                  "J1-R7: the replay backend's default (all-false) capabilities predict the universal "
                  "last resort, parse_and_repair -- a THIRD distinct strategy, still exactly predicted");
        }
    }

    // ---- (4): FailoverChatClient<real OpenAI, real Anthropic> -- primary genuinely fails ------------
    {
        std::string const fallback_body =
            R"({"id":"msg_2","type":"message","role":"assistant","model":"claude-mock",)"
            R"("content":[{"type":"text","text":"FALLBACK_ANSWERED"}],)"
            R"("stop_reason":"end_turn","usage":{"input_tokens":3,"output_tokens":2}})";
        TlsCannedServer fallback_server(leaf, http_response(fallback_body));
        check(fallback_server.ok(), "J1: fallback (Anthropic) test server started");
        if (fallback_server.ok()) {
            using Primary = openai::OpenAIChatClient<InMemorySecretStore>;
            using Fallback = anthropic::AnthropicChatClient<InMemorySecretStore>;

            Primary primary("localhost", unused_loopback_port(), "gpt-5", SecretRef{"openai-api-key"},
                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            Fallback fallback("localhost", fallback_server.port(), "claude-sonnet-5",
                               SecretRef{"anthropic-api-key"}, ChatClientCapabilities{}, store, "/v1",
                               "2023-06-01", fake_resolver, leaf.cert_pem);
            FailoverChatClient<Primary, Fallback> composed(std::move(primary), std::move(fallback));
            static_assert(ChatClient<decltype(composed)>,
                          "FailoverChatClient<Primary, Fallback> must itself satisfy ChatClient (004 §1)");

            // The SAME unchanged call site as (1)-(3) above, now against a composed multi-backend
            // conformer -- no special-casing needed for the fact that this "backend" is itself two.
            auto resp = run_it(composed, req, ctx);
            check(resp.has_value(),
                  "J1-R8: FailoverChatClient succeeds even though its primary is a real, closed loopback "
                  "port (a genuine connection failure) -- the fallback answers");
            if (resp) {
                check(resp->fallback_tier == 1,
                      "J1-R9: ChatResponse::fallback_tier == 1 -- the trace's response-metadata half "
                      "(Phase F3) is real end-to-end through two REAL backend conformers, not "
                      "test_failover_chat_client.cpp's synthetic doubles");
                check(first_text(*resp) == "FALLBACK_ANSWERED",
                      "J1-R10: the fallback's own real response content comes through unchanged");
            }
        }
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_cross_backend_parity: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_cross_backend_parity: %d FAILURE(S)\n", g_failures);
    return 1;
}
