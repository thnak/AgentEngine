// ADR-177 §7 Q1, answered by RUNNING it rather than reading it: does a stream that dies mid-answer on the
// REAL OpenAI client and the REAL HTTP transport reach `AgentSession` as something its retry decision
// accepts -- `failure_class::transient` from `stream::fail_error()`, with updates already delivered?
//
// The unit test (test_rt_agent_session_stream_retry.cpp) scripts the error it is handed. This one does
// not: a loopback server kills connection #1 halfway through an SSE body, then answers connection #2
// properly, and everything between the socket and the session is production code. If any layer
// re-wrapped the error (the worry that made Q1 a question), the session would not retry and the run
// would fail -- so a green run here is the answer.
//
// What it does NOT cover, said plainly: the incident that started this was a provider going SILENT until
// `kIoTimeoutMs` (90 s) fired, not a dropped connection. Both surface through the same
// `perform_http(s)_exchange_streaming` read loop as `failure_class::transient` (net_egress_proxy.cpp),
// but the 90 s path is not exercised here -- waiting 90 s per run is not a default-suite test.

#ifdef AGENTENGINE_WITH_HTTPS

#include "agentengine/pal/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
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

// Serves connection N with script N: send the head, then each event as one chunk, then either finish
// the body properly or drop the connection with no terminating chunk (the mid-answer death).
struct Script {
    std::vector<std::string> events;
    bool                     finish_cleanly = true;
};

class ScriptedSseServer {
public:
    explicit ScriptedSseServer(std::vector<Script> scripts) : scripts_(std::move(scripts)) {
        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~ScriptedSseServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    ScriptedSseServer(ScriptedSseServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::size_t connections() const { return served_.load(); }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::size_t const n = served_.fetch_add(1);
            if (n < scripts_.size()) serve_one(*a, scripts_[n], st);
            agentengine::pal::close_fd(*a);
        }
    }

    bool write_all(agentengine::pal::fd_t fd, std::string const& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto w = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent),
                                                  data.size() - sent);
            if (!w) {
                if (w.error() == agentengine::pal::would_block()) continue;
                return false;
            }
            sent += *w;
        }
        return true;
    }

    void serve_one(agentengine::pal::fd_t fd, Script const& script, std::stop_token const& st) {
        std::string buf;
        std::byte chunk[1024];
        for (int i = 0; i < 400 && !st.stop_requested(); ++i) {
            auto r = agentengine::pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == agentengine::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return;
            }
            if (*r == 0) return;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            // The request body follows the head; the head alone is enough to know a request arrived.
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        }
        if (buf.find("\r\n\r\n") == std::string::npos) return;

        if (!write_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n")) {
            return;
        }
        for (auto const& ev : script.events) {
            char size_buf[32];
            std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", ev.size());
            if (!write_all(fd, std::string(size_buf) + ev + "\r\n")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (script.finish_cleanly) write_all(fd, "0\r\n\r\n");
        // else: fall out and close with the chunked body unterminated -- the stream dies mid-answer.
    }

    std::vector<Script>       scripts_;
    bool                      ok_ = false;
    agentengine::pal::fd_t    listen_fd_{};
    std::uint16_t             port_ = 0;
    std::atomic<std::size_t>  served_{0};
    std::jthread              thread_;
};

[[nodiscard]] std::string sse(std::string const& json) { return "data: " + json + "\n\n"; }

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

class NoToolsHistory {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    InMemorySecretStore store;
    store.set("provider-key", "sk-loopback-server-ignores-this");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});

    auto const usage_chunk =
        sse(R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":7,"total_tokens":12}})");

    auto run_against = [&](ScriptedSseServer& server, std::uint32_t retries) {
        using OpenAISession = AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState, NoToolsHistory>;
        OpenAISession session;
        session.initialize("s-real", Principal{"p", ""});
        session.emplace_chat_client("127.0.0.1", server.port(), "m", SecretRef{"provider-key"},
                                     ChatClientCapabilities{}, store, "/v1", sandbox::resolve_host,
                                     std::string{}, std::string{}, std::string{}, std::string{},
                                     std::nullopt, ProviderTransport::plaintext_http);
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);
        session.set_stream_retries(retries);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        std::size_t warnings = 0;
        while (auto ev = viewer.next()) {
            if (ev->kind != run_event_kind::warning) continue;
            auto const& w = std::get<run_event_payload::Warning>(ev->payload);
            if (w.message.starts_with(agentengine::rt::detail::kStreamRetryWarningPrefix)) ++warnings;
        }
        return std::pair{std::move(r), warnings};
    };

    // ---- the run that survives: connection 1 dies mid-body, connection 2 is complete --------------------
    {
        Script dying;
        dying.events = {sse(R"({"choices":[{"delta":{"content":"par"}}]})"),
                        sse(R"({"choices":[{"delta":{"content":"tial"}}]})")};
        dying.finish_cleanly = false;
        Script whole;
        whole.events = {sse(R"({"choices":[{"delta":{"content":"the whole answer"}}]})"), usage_chunk,
                        "data: [DONE]\n\n"};
        ScriptedSseServer server({dying, whole});
        check(server.ok(), "loopback server started");
        if (server.ok()) {
            auto [r, warnings] = run_against(server, /*retries=*/1);
            check(r.has_value(),
                  "Q1: a stream killed mid-body on the REAL client + REAL transport is retried and the "
                  "run converges -- so its class survived to the session's decision");
            if (r.has_value()) {
                check(text_of(r->message) == "the whole answer",
                      "Q1: the answer is the second connection's, with none of the dead one's text");
            }
            check(server.connections() == 2, "Q1: exactly two connections were made");
            check(warnings == 1, "Q1: the retry was announced once");
        }
    }

    // ---- control: the same dying server with retries OFF fails, with the stream's own error ---------------
    {
        Script dying;
        dying.events = {sse(R"({"choices":[{"delta":{"content":"par"}}]})")};
        dying.finish_cleanly = false;
        ScriptedSseServer server({dying, dying});
        if (server.ok()) {
            auto [r, warnings] = run_against(server, /*retries=*/0);
            check(!r.has_value(),
                  "Q1 control: with retries at 0 the same dying stream ends the run, so the success above "
                  "is the retry's doing and not a server that never really died");
            check(!r.has_value() && r.error().code == "run.stream_incomplete",
                  "Q1 control: and it fails as run.stream_incomplete");
            if (!r.has_value()) {
                std::fprintf(stderr, "  .. the failure reads: %s\n", r.error().message.c_str());
            }
            check(server.connections() == 1 && warnings == 0, "Q1 control: one connection, no retry warning");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}

#else
int main() { return 0; }
#endif
