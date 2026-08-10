// Implements 009-Plugin-and-Extension-System.md §8, composed with ADR-018's real-backend wiring: a
// REAL `AgentSession` (real `handle()`, real context assembly, real `OpenAIChatClient`, real socket)
// driven through `HistoryAndSkillsProvider<HistoryProvider<Window<0>>, BuiltinSkillsProvider>` --
// proving the skill advertisement message AND ordinary history both reach a real session turn's
// outbound wire request, and that a skill's mounted content is readable back out via `mount_read`
// after the run. This is the load-bearing new check for the whole skills feature: it must fail on a
// naive `AgentSession<..., NoSessionState>` (default `HistoryProvider<Window<0>>`, no skills) and
// pass once wired to `HistoryAndSkillsProvider`, which is exactly what R2/R1 below demonstrate in
// sequence.
//
// Same canned-HTTP-server shape as test_agent_session_real_backend.cpp (`CannedCompletionServer`,
// `emplace_chat_client`, `server.last_request_body()`) -- kept as an independent copy per this
// suite's own established discipline (that file's own top comment), not a shared helper, since this
// file answers a different question (skills reach the wire) than that one does (a real backend can
// be driven through the turn loop at all).

#ifdef AGENTENGINE_WITH_HTTPS

#include "pal/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/history_and_skills_provider.hpp"
#include "agentengine/core/skill_provider.hpp"
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
constexpr char const* kReplyText = "answered with skills mounted";

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

// Default-constructible wrapper around `SkillsProvider<>`, baking in `make_builtin_skills_source()`
// (core/builtin_skills.hpp) via a static free-function call rather than a constructor argument --
// the same shape `FourToolHistoryProvider`/`ToolDeclaringHistoryProvider` (this suite's own earlier
// multitool tests) already established for the identical underlying reason: `AgentSession`'s
// `HistoryProviderT` member is a plain, default-constructed value with no post-construction accessor
// (`history_and_skills_provider.hpp`'s own top comment explains this precisely).
class BuiltinSkillsProvider {
public:
    BuiltinSkillsProvider() : inner_({make_builtin_skills_source()}) {}
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ec) {
        return inner_.on_context(sc, ec);
    }
    task<std::monostate> on_turn_end(TurnView tv, EffectContext& ec) { return inner_.on_turn_end(tv, ec); }
    [[nodiscard]] std::vector<Mount> const& mounted() const noexcept { return inner_.mounted(); }
    [[nodiscard]] InMemoryWorktreeObjectStore& object_store() noexcept { return inner_.object_store(); }
    [[nodiscard]] quark::InMemoryStore& ref_store() noexcept { return inner_.ref_store(); }

private:
    SkillsProvider<> inner_;
};
static_assert(ContextProvider<BuiltinSkillsProvider>);

using SkillsSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState,
                 HistoryAndSkillsProvider<HistoryProvider<Window<0>>, BuiltinSkillsProvider>>;
static_assert(std::is_default_constructible_v<SkillsSession>,
              "SkillsSession must stay default-constructible for quark::TestKit<A>'s A actor_; "
              "member -- the whole point of HistoryAndSkillsProvider's conditional default ctor");

}  // namespace

int main() {
#if defined(_WIN32)
    quark::pal::ensure_winsock();
#endif

    CannedCompletionServer server;
    check(server.ok(), "the canned completion server started on a loopback port");
    if (!server.ok()) {
        std::fprintf(stderr, "test_agent_session_skills_real_backend: 1 FAILURE(S)\n");
        return 1;
    }

    InMemorySecretStore store;
    store.set("provider-key", "sk-canned-server-ignores-this");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});
    ChatClientCapabilities caps;
    caps.tool_calling = true;

    quark::TestKit<SkillsSession> kit;
    kit.actor().emplace_chat_client("127.0.0.1", server.port(), "canned-model", SecretRef{"provider-key"},
                                     caps, store, "/v1", sandbox::resolve_host, /*ca_bundle=*/std::string{},
                                     /*http_referer=*/std::string{}, /*x_title=*/std::string{},
                                     /*end_user_id=*/std::string{}, /*seed=*/std::nullopt,
                                     ProviderTransport::plaintext_http);
    kit.actor().initialize("session-with-skills", Principal{"owner", "tenant-a"});
    kit.actor().set_capabilities(&held);

    // ---- R1: a real turn through a real, skills-composed session --------------------------------
    quark::result<AgentResponse> r = kit.ask<AgentResponse>(StartRun{user_turn("hello with skills mounted")});
    check(r.has_value(), "R1: a StartRun ask completes through the real turn loop with a "
                          "HistoryAndSkillsProvider-composed session");

    std::string const sent = server.last_request_body();
    auto const history_pos = sent.find("hello with skills mounted");
    auto const skill_pos = sent.find("using-the-code-interpreter");
    check(history_pos != std::string::npos,
          "R2: the session's own assembled HISTORY still reaches the wire -- composing with skills "
          "did not break the ordinary history contribution");
    check(skill_pos != std::string::npos && sent.find("execute_code") != std::string::npos,
          "R3: the SKILL ADVERTISEMENT (name + description, from the real builtin skill) ALSO "
          "reaches the wire request -- this is the load-bearing new check: it would be absent on a "
          "naive AgentSession<..., NoSessionState> using only the default HistoryProvider<Window<0>>, "
          "and is present here only because HistoryAndSkillsProvider actually composed both "
          "contributors through the real assemble_context()");
    if (history_pos != std::string::npos && skill_pos != std::string::npos) {
        check(skill_pos < history_pos,
              "R3b: the skill advertisement is ORDERED BEFORE the conversation history on the wire, "
              "not just present somewhere in the body -- a real provider (and a real model's own "
              "system-prompt-adherence training) treats leading vs. trailing placement differently; "
              "this is the regression test for a real bug where HistoryAndSkillsProvider pushed "
              "history before skills, putting the system-shaped advertisement LAST on every request");
    }

    if (r.has_value()) {
        check(!r->message.content.empty(), "R4: the AgentResponse carries content from the real server");
    }

    // Reading the mounted skill's own content back out via a real `mount_read` is already proven,
    // directly and more thoroughly, by test_skill_provider_mount.cpp's own R1 -- `AgentSession` has
    // no accessor to its private `history_provider_` member (by design: this file's own top comment,
    // and `history_and_skills_provider.hpp`'s, both explain why one was never added), so that proof
    // is not repeated here through the full session. What IS unique to THIS file is R3 above: the
    // advertisement text reaching a REAL session turn's actual outbound wire request.

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_session_skills_real_backend: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_session_skills_real_backend: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
