// Proves decisions/ADR-018-agent-session-real-chat-client-wiring.md, closing the Milestone 5 Phase J1
// residual: "`AgentSession<ChatClientT>` cannot host a non-default-constructible `ChatClient`
// conformer -- a real backend built for Phase D/E has never actually been driven through a live
// `AgentSession` turn loop."
//
// That residual was not a configuration inconvenience. `quark::TestKit<A>` declares `A actor_;` and
// default-constructs it, and a real Phase D/E backend is not default-constructible --
// `OpenAIChatClient<Store>` holds a `Store const&`. So `AgentSession<OpenAIChatClient<...>>` did not
// merely lack a way to be configured, it failed to COMPILE at all, which is exactly why every session
// test to date used a hand-written mock and the real backends were only ever exercised standalone.
// Two things were therefore never demonstrated together: 001 §3's turn loop, and a backend that
// actually talks to a server.
//
// ADR-018 closes it without touching Quark (CLAUDE.md: the submodule is never forked or patched
// in-tree, and this needed no upstream change once the real constraint was identified): `chat_client_`
// becomes a `std::optional<ChatClientT>`, default-ENGAGED whenever `ChatClientT` allows it -- so every
// pre-existing conformer behaves exactly as before -- and `emplace_chat_client(...)` constructs a real
// one in place, after the actor exists.
//
// This test drives that end to end against a REAL local HTTP server: real `AgentSession::handle()`,
// real context assembly, real `OpenAIChatClient::chat()`, real secret resolution against a real
// capability grant, real socket, real response parsing, real history append. The server is canned
// (this is a deterministic default-suite test, not a live-model one -- `test_llamacpp_live_e2e.cpp`
// covers a real model), but everything between `StartRun` and the assistant turn landing in history is
// product code.
//
// Plain HTTP over loopback via `ProviderTransport::plaintext_http` (ADR-016), deliberately: it needs no
// certificate plumbing, so this file stays about the session/backend wiring rather than re-proving TLS
// that `test_openai_chat_client_live.cpp` already covers exhaustively.

#ifdef AGENTENGINE_WITH_HTTPS

#include "pal/net.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;

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

// The canned assistant turn this server answers with. Distinctive on purpose: it could not have come
// from a default-constructed mock, so seeing it in `history()` proves the emplaced client really ran.
constexpr char const* kReplyText = "answered by a real OpenAIChatClient over a real socket";

// A minimal plain-HTTP/1.1 loopback server returning one canned Chat Completions body. Same shape as
// test_provider_egress_address_policy.cpp's, kept as an independent copy per this suite's own
// established discipline (test_provider_http_client.cpp's top comment) -- it answers a different
// question and would otherwise couple two unrelated tests.
class CannedCompletionServer {
public:
    CannedCompletionServer() {
        auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *quark::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~CannedCompletionServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) quark::pal::close_fd(listen_fd_);
    }
    CannedCompletionServer(CannedCompletionServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int requests_served() const { return requests_served_.load(); }
    // The last request body the server actually received -- lets the test confirm the SESSION's
    // assembled history reached the wire, not just that some request did.
    [[nodiscard]] std::string last_request_body() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_body_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = quark::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a, st);
            quark::pal::close_fd(*a);
        }
    }

    void serve_one(quark::pal::fd_t fd, std::stop_token const& st) {
        std::string buf;
        std::byte chunk[2048];
        std::size_t want = 0;
        for (int i = 0; i < 400 && !st.stop_requested(); ++i) {
            auto r = quark::pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == quark::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return;
            }
            if (*r == 0) return;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            auto const head_end = buf.find("\r\n\r\n");
            if (head_end == std::string::npos) continue;
            if (want == 0) want = head_end + 4 + content_length_of(std::string_view(buf).substr(0, head_end));
            if (buf.size() >= want) break;
        }
        auto const head_end = buf.find("\r\n\r\n");
        if (head_end == std::string::npos) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            last_body_ = buf.substr(head_end + 4);
        }

        std::string const body = std::string(R"({"model":"canned-model","choices":[{"index":0,)") +
                                 R"("finish_reason":"stop","message":{"role":"assistant","content":")" +
                                 kReplyText + R"("}}],)" +
                                 R"("usage":{"prompt_tokens":11,"completion_tokens":13}})";
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            auto w = quark::pal::send_some(fd, reinterpret_cast<std::byte const*>(response.data() + sent),
                                            response.size() - sent);
            if (!w) {
                if (w.error() == quark::pal::would_block()) continue;
                return;
            }
            sent += *w;
        }
        requests_served_.fetch_add(1);
    }

    [[nodiscard]] static std::size_t content_length_of(std::string_view head) {
        auto const pos = head.find("Content-Length:");
        if (pos == std::string_view::npos) return 0;
        std::size_t out = 0;
        for (char c : head.substr(pos + 15)) {
            if (c == ' ') continue;
            if (c < '0' || c > '9') break;
            out = out * 10 + static_cast<std::size_t>(c - '0');
        }
        return out;
    }

    bool ok_ = false;
    quark::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> requests_served_{0};
    mutable std::mutex mu_;
    std::string last_body_;
    std::jthread thread_;
};

[[nodiscard]] Message user_turn(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
#if defined(_WIN32)
    quark::pal::ensure_winsock();
#endif

    CannedCompletionServer server;
    check(server.ok(), "the canned completion server started on a loopback port");
    if (!server.ok()) {
        std::fprintf(stderr, "test_agent_session_real_backend: 1 FAILURE(S)\n");
        return 1;
    }

    // The store must outlive the session -- `OpenAIChatClient<Store>` holds `Store const&`, which is
    // the very reason it is not default-constructible and therefore the reason this residual existed.
    InMemorySecretStore store;
    store.set("provider-key", "sk-canned-server-ignores-this");

    using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;
    static_assert(!std::is_default_constructible_v<RealClient>,
                  "the point of ADR-018: a real Phase D/E backend is NOT default-constructible, so "
                  "before it, AgentSession<RealClient> could not even be declared under TestKit");

    using Session = AgentSession<RealClient>;
    static_assert(std::is_default_constructible_v<Session>,
                  "ADR-018: AgentSession stays default-constructible even when its ChatClientT is not "
                  "-- exactly what quark::TestKit<A>'s `A actor_;` member requires");

    // ---- J1-R1: the actor exists, with no chat client yet ------------------------------------------
    quark::TestKit<Session> kit;
    check(!kit.actor().has_chat_client(),
          "J1-R1: a freshly constructed session holding a non-default-constructible ChatClientT has no "
          "client yet -- the optional is disengaged, which is what let the actor be constructed at all");

    ChatClientCapabilities caps;
    caps.tool_calling = true;

    // ---- J1-R2: a REAL backend is constructed in place, inside the live actor ----------------------
    kit.actor().emplace_chat_client("127.0.0.1", server.port(), "canned-model", SecretRef{"provider-key"},
                                     caps, store, "/v1", sandbox::resolve_host, /*ca_bundle=*/std::string{},
                                     /*http_referer=*/std::string{}, /*x_title=*/std::string{},
                                     /*end_user_id=*/std::string{}, /*seed=*/std::nullopt,
                                     ProviderTransport::plaintext_http);
    check(kit.actor().has_chat_client(),
          "J1-R2: emplace_chat_client constructs a real OpenAIChatClient in place inside the live "
          "actor -- an assignment-based setter could not do this, since a backend holding a reference "
          "member is not assignable");

    // The session's own principal and its EffectContext's capability grant. The credential is resolved
    // at the point of use inside chat(), against this grant -- the real 004 §1 / 018 §4 path, driven
    // here through the actor rather than from a test's own main().
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});
    kit.actor().initialize("session-real-backend", Principal{"owner", "tenant-a"});
    kit.actor().set_capabilities(&held);

    // ---- J1-R3: ONE REAL TURN through the real turn loop -------------------------------------------
    quark::result<AgentResponse> r =
        kit.ask<AgentResponse>(StartRun{user_turn("hello from a real session turn")});

    check(r.has_value(),
          "J1-R3: a StartRun ask completes through 001 §3's real turn loop with a REAL ChatClient "
          "backend behind it -- the combination Phase J1 found had never been exercised: real "
          "AgentSession::handle(), real context assembly, real secret resolution, real socket, real "
          "response parse");
    if (r.has_value()) {
        check(!r->message.content.empty(), "J1-R3: the AgentResponse carries content");
        if (!r->message.content.empty()) {
            auto const* text = std::get_if<Text>(&r->message.content.front().value);
            check(text && text->text == kReplyText,
                  "J1-R3: the reply text is the SERVER's, arriving through the emplaced client -- a "
                  "default-constructed mock could not have produced it, so this could not pass with "
                  "the client left disengaged");
        }
        check(r->usage.input_tokens == 11 && r->usage.output_tokens == 13,
              "J1-R3: Usage parsed from the real HTTP response body rides back through the actor's "
              "own AgentResponse");
    }

    check(server.requests_served() == 1,
          "J1-R3: the server served exactly one request -- the turn really crossed a socket rather "
          "than being satisfied locally");

    // ---- J1-R4: the session's OWN assembled history is what reached the wire ------------------------
    {
        std::string const sent = server.last_request_body();
        check(sent.find("hello from a real session turn") != std::string::npos,
              "J1-R4: the user turn the SESSION assembled (via its HistoryProvider, not a request the "
              "test hand-built) is present in the body the server actually received -- the whole "
              "context-assembly -> translation -> wire path ran for real");
        check(sent.find("\"model\":\"canned-model\"") != std::string::npos,
              "J1-R4: the emplaced client's own configured model reached the wire, so the constructor "
              "arguments passed to emplace_chat_client really took effect");
    }

    // ---- J1-R5: history grew by exactly the user turn + the assistant reply -------------------------
    check(kit.actor().history().size() == 2,
          "J1-R5: 001 §3's turn loop appended exactly the user turn and the assistant reply");
    if (kit.actor().history().size() == 2) {
        check(kit.actor().history()[0].role == role::user, "J1-R5: history[0] is the user turn");
        check(kit.actor().history()[1].role == role::assistant,
              "J1-R5: history[1] is the assistant reply, sourced from the real backend");
    }

    // ---- J1-R6: a second turn reuses the same emplaced client ----------------------------------------
    // A bound backend is reused across turns in production (a real ChatClientRegistry binds one
    // instance per ChatClientId). Emplacement must not be a one-shot.
    {
        quark::result<AgentResponse> r2 =
            kit.ask<AgentResponse>(StartRun{user_turn("second turn")});
        check(r2.has_value(), "J1-R6: a second run succeeds against the same emplaced client");
        check(server.requests_served() == 2, "J1-R6: it crossed the socket again");
        check(kit.actor().history().size() == 4,
              "J1-R6: history now holds two full turns -- the emplaced client is durable across runs, "
              "not consumed by the first one");
    }

    // ---- J1-R7: fail-closed when a non-default-constructible client was never emplaced ---------------
    // The one new failure mode ADR-018 introduces, asserted rather than left implicit. The session
    // must not fabricate an AgentResponse for a run that never reached a model -- the same fail-closed
    // shape every other branch in handle() already uses.
    {
        quark::TestKit<Session> bare;
        bare.actor().initialize("session-no-client", Principal{"owner", "tenant-a"});
        check(!bare.actor().has_chat_client(), "J1-R7: this session has no client emplaced");
        quark::result<AgentResponse> r3 = bare.ask<AgentResponse>(StartRun{user_turn("hi")});
        check(!r3.has_value(),
              "J1-R7: a run on a session whose ChatClientT was never emplaced FAILS CLOSED -- it never "
              "responds, rather than fabricating an AgentResponse for a turn that never reached a "
              "model (I3: nothing is invented on a path that produced no model output)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_session_real_backend: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_session_real_backend: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
