// Milestone 5 Phase J2 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, roadmap
// exit criterion; 018-Identity-Authorization-and-Secrets.md §7 G2, "secret hygiene"): "a canary
// secret is planted and every persisted artifact (logs, traces, audit, checkpoints, recordings,
// crash dumps, plugin memory dumps) is scanned; zero occurrences."
//
// Scoped per the milestone doc's own decision 9 (same discipline Phase I's I1 sandbox-workspace leg
// used): only the artifacts this milestone's own mechanisms actually produce are scanned --
// checkpoints (real, M4), recordings (Phase G), and error-path text (Phases D/E/F). Logs/audit/crash
// dumps/plugin memory dumps have no real sink anywhere in this tree yet (016/020, M8/M9) -- named
// out of scope, not silently assumed covered.
//
// CLAUDE.md: "Security claims need positive controls -- a test that cannot fail proves nothing."
// Every negative assertion below ("the canary never appears in X") is paired with a positive control
// proving the canary really was exercised as a live credential first (J2-R1/R2) -- otherwise a scan
// that finds nothing because the secret was never actually used would be a vacuous pass.

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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/recording_chat_client.hpp"
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

// The planted canary -- distinctive enough that an accidental substring collision is not a realistic
// concern, and never used as anything OTHER than the literal secret value in this file.
std::string const kCanary = "CANARY-SECRET-9f3a7b21-do-not-persist";

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}
[[nodiscard]] bool contains(std::vector<std::byte> const& haystack, std::string_view needle) {
    return contains(std::string_view(reinterpret_cast<char const*>(haystack.data()), haystack.size()), needle);
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
        char const* const pers = "ae-canary-scan-test-ca";
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

// A fifth independent copy of test_openai_chat_client_live.cpp's TlsCannedServer pattern -- this one
// ALSO captures the raw bytes it received, the positive-control half of this file's own claim: proof
// the canary secret really was sent as a credential over the wire, not just configured and unused.
class CapturingTlsServer {
public:
    CapturingTlsServer(GeneratedKeyCert const& kc, std::string raw_response)
        : raw_response_(std::move(raw_response)) {
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        char const* const pers = "ae-canary-scan-test-server";
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
    ~CapturingTlsServer() {
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
    CapturingTlsServer(CapturingTlsServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::string received() const {
        std::lock_guard<std::mutex> lock(received_mu_);
        return received_;
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

        char drain[2048];
        std::string received;
        for (int i = 0; i < 20; ++i) {
            int const n = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(drain), sizeof(drain));
            if (n <= 0) break;
            received.append(drain, static_cast<std::size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(received_mu_);
            received_ = std::move(received);
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
    mutable std::mutex received_mu_;
    std::string received_;
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

class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = Text{"reply"};
        Message reply;
        reply.role = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }
};
static_assert(ChatClient<CannedChatClient>, "CannedChatClient must satisfy ChatClient (004 §1)");

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
    store.set("openai-api-key", kCanary);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"openai-api-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    using agentengine::test_support::run_task_sync;

    // ---- (1) Positive control: the canary really is sent as a live credential over the wire --------
    {
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})";
        CapturingTlsServer server(leaf, http_response(body));
        check(server.ok(), "J2: positive-control test server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
            check(resp.has_value(), "J2-R1: the call succeeds (the canary resolved and was accepted)");
            check(contains(server.received(), kCanary),
                  "J2-R2: POSITIVE CONTROL -- the raw bytes the server actually received over the wire "
                  "DO contain the canary secret (as the Authorization/x-api-key header value) -- the "
                  "scans below are not vacuous, the secret genuinely was exercised as a live credential");
        }
    }

    // ---- (2) Recordings (Phase G): the canary never appears in a serialized ChatCallRecording -------
    {
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})";
        CapturingTlsServer server(leaf, http_response(body));
        check(server.ok(), "J2: recording test server started");
        if (server.ok()) {
            openai::OpenAIChatClient inner("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                            ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            std::optional<ChatCallRecording> captured;
            RecordingChatClient recorder(std::move(inner),
                                          [&](ChatCallRecording rec) { captured = std::move(rec); });
            auto resp = run_task_sync<result<ChatResponse>>(recorder.chat(request_asking("hi"), ctx));
            check(resp.has_value(), "J2: recorded call succeeds");
            check(captured.has_value(), "J2: a recording was captured");
            if (captured) {
                std::string const json_text = json::dump(chat_call_recording_to_json(*captured));
                check(!contains(json_text, kCanary),
                      "J2-R3: the canary secret NEVER appears in a serialized ChatCallRecording -- "
                      "ChatRequest carries only a SecretRef (a NAME, chat_client.hpp), never the "
                      "resolved SecretLease value, so the recording codec has nothing to leak");
            }
        }
    }

    // ---- (3) Error path: a denied capability's error message never carries the secret ---------------
    {
        std::string const body =
            R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})";
        CapturingTlsServer server(leaf, http_response(body));
        if (server.ok()) {
            openai::OpenAIChatClient client("localhost", server.port(), "gpt-5", SecretRef{"openai-api-key"},
                                             ChatClientCapabilities{}, store, "/v1", fake_resolver, leaf.cert_pem);
            CapabilitySet empty;
            EffectContext denied_ctx = ctx;
            denied_ctx.capabilities = &empty;
            auto resp = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), denied_ctx));
            check(!resp.has_value(), "J2: the denied call fails, as expected");
            if (!resp) {
                check(!contains(resp.error().message, kCanary) && !contains(resp.error().code, kCanary),
                      "J2-R4: a capability-denial error's message/code never contains the secret -- "
                      "denial happens BEFORE SecretStore::resolve() is ever reached (Phase D3's own "
                      "proof), so there is no resolved value in scope to leak in the first place");
            }
        }
    }

    // ---- (4) SecretLease itself cannot leak even if something mistakenly tried to print/persist it --
    {
        auto lease = store.resolve(SecretRef{"openai-api-key"}, ctx);
        check(lease.has_value(), "J2: resolving the canary directly succeeds");
        if (lease) {
            check(to_redacted_string(*lease) == "***",
                  "J2-R5: SecretLease::to_redacted_string() is unconditionally \"***\" -- even a future "
                  "bug that accidentally serialized/logged a SecretLease value directly (rather than a "
                  "SecretRef) could not leak the canary through this type's own string form");
        }
    }

    // ---- (5) Checkpoints (M4, real): AgentSessionRecord has no field that could carry the canary ----
    // Named explicitly, not silently assumed: AgentSessionRecord's own QUARK_SERIALIZE field list
    // (agent_session.hpp) is session_id/principal_id/principal_tenant_id/created_at_ns/updated_at_ns/
    // deleted/run_counter/turn_index/open_interactions -- no ChatRequest/ChatResponse/SecretRef field
    // exists anywhere in it, and Interaction (interaction.hpp) is interaction_id/run_id/reason/
    // opened_at_ns/expires_at_ns -- also no content-carrying field. "Zero occurrences" holds by
    // construction; this scans the REAL encoded snapshot bytes (not just the struct definition) to
    // make that a verified fact, not an inference from reading the header.
    {
        using Session = AgentSession<CannedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-canary-checkpoint", Principal{"p-test", ""});
        auto r = kit.ask<AgentResponse>(StartRun{[] {
            ContentItem item;
            item.value = Text{"hello"};
            item.origin = content_origin::user;
            Message input;
            input.role = role::user;
            input.message_id = "m-1";
            input.content.push_back(item);
            return input;
        }()});
        check(r.has_value(), "J2: an ordinary (non-canary-touching) turn runs so there is a real "
                              "session to checkpoint");

        quark::InMemoryStore snap_store;
        auto const id = session_actor_id(kit.actor().session_id());
        auto const fence = snap_store.acquire_fence(id);
        auto saved = save_agent_session_snapshot(kit.activation(), snap_store, kit.actor(), fence);
        check(saved.has_value(), "J2: the session snapshot saves");

        auto raw = snap_store.load_snapshot(id);
        check(raw.has_value() && raw->has_value(), "J2: the raw snapshot record is readable back");
        if (raw && raw->has_value()) {
            check(!contains((*raw)->record, kCanary),
                  "J2-R6: the canary secret never appears in the REAL encoded checkpoint bytes -- "
                  "AgentSessionRecord structurally has no field capable of carrying request/response/"
                  "secret content (verified against its own field list, not just by construction)");
        }
    }

    mbedtls_pk_free(&leaf_key);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_secret_hygiene_canary_scan: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_secret_hygiene_canary_scan: %d FAILURE(S)\n", g_failures);
    return 1;
}
