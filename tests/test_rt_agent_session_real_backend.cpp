// ADR-037 Phase 2: porting the real behavioral claims from the OLD, Quark-actor-based
// test_agent_session_real_backend.cpp (decisions/ADR-018) and test_agent_session_skills_real_backend.cpp
// (009 §8 + ADR-018) onto agentengine::rt::AgentSession (rt/agent_session.hpp) directly. Both old files
// share the identical "canned local HTTP server, real OpenAIChatClient, real socket" shape -- fully
// portable and CI-safe (no live network egress, no live API key), just no longer wired through
// quark::TestKit<A>/quark::Ask<>: `rt::AgentSession` is a plain, directly-constructible local value
// (no actor-framework construction ceremony to satisfy), driven here the same way every other
// test_rt_agent_session*.cpp file already does -- `drive<T>()`, one manual `resume()` loop, safe here
// because nothing in either fixture below genuinely suspends externally (OpenAIChatClient::chat()'s own
// blocking socket I/O happens as an ordinary synchronous call inside the coroutine body, never a real
// suspension point -- the exact same property `sandbox::perform_provider_https_exchange`'s own top
// comment already documents, and cli_chat.cpp already proves end to end that a real, non-default-
// constructible ChatClientT backend runs cleanly through `rt::AgentSession`).
//
// Kept as ONE file (not two), unlike the split the task's own instructions offered as a default,
// because both source files test the exact same underlying wiring question (does a REAL backend +ONE
// specific HistoryProviderT reach the wire correctly through `rt::AgentSession`'s turn loop) against
// the exact same `CannedCompletionServer` fixture shape -- the skills half is a strict superset of the
// real-backend half's own claims (a differently-composed HistoryProviderT), not an independent topic.
// `test_rt_model_call_gateway_session.cpp` (G5, from test_model_call_gateway.cpp) is its own separate
// file instead: that claim needs no HTTPS build, no canned server, and no socket at all -- a fully
// deterministic, offline, scripted-backend integration test of a DIFFERENT ChatClientT shape
// (ModelCallGatewayLike, not a raw ChatClient), so folding it in here would blur what this file is
// actually proving.
//
// Ported claims, labeled to match each source file's own original label prefix:
//   J1-R1..R8 (from test_agent_session_real_backend.cpp): a non-default-constructible RealClient
//         (OpenAIChatClient<InMemorySecretStore>) can be emplaced into a still-default-constructible
//         rt::AgentSession; a real StartRun-equivalent turn (start_run()) runs the real turn loop --
//         real context assembly, real secret resolution, real socket, real response parse; the
//         session's OWN assembled history (not a hand-built request) reaches the wire; history grows
//         by exactly user+assistant per turn; a second turn reuses the same emplaced client; a session
//         whose ChatClientT was never emplaced fails closed (no fabricated response, I3); a declared
//         tool reaches the wire's top-level `tools` array through a real run.
//   R1-R4 (from test_agent_session_skills_real_backend.cpp): a real turn through
//         `HistoryAndSkillsProvider<HistoryProvider<Window<0>>, BuiltinSkillsProvider>` still carries
//         ordinary history to the wire AND the skill advertisement (name+description) reaches the same
//         wire request, ordered BEFORE history (the real regression this file's ordering check exists
//         for), and the AgentResponse carries real server content.
//
// NOT re-ported (named, not silently dropped): test_agent_session_skills_real_backend.cpp's own final
// comment already deferred "read a mounted skill's content back via mount_read" to
// test_skill_provider_mount.cpp's own R1 (a standalone SkillsProvider<> proof, no AgentSession
// involved at all) rather than re-testing it through a full session -- that division of labor is
// unaffected by this port and is not repeated here either.

#ifdef AGENTENGINE_WITH_HTTPS

#include "agentengine/pal/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/history_and_skills_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

// ---- CannedCompletionServer: an independent copy per this suite's own established discipline
// (the old files' own top comments) -- kept byte-for-byte equivalent to both old fixtures' shared
// shape, just consolidated into one copy since both halves of THIS file use the identical server.
class CannedCompletionServer {
public:
    explicit CannedCompletionServer(std::string reply_text) : reply_text_(std::move(reply_text)) {
        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~CannedCompletionServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    CannedCompletionServer(CannedCompletionServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int requests_served() const { return requests_served_.load(); }
    [[nodiscard]] std::string last_request_body() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_body_;
    }

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

    void serve_one(agentengine::pal::fd_t fd, std::stop_token const& st) {
        std::string buf;
        std::byte chunk[2048];
        std::size_t want = 0;
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
                                 reply_text_ + R"("}}],)" +
                                 R"("usage":{"prompt_tokens":11,"completion_tokens":13}})";
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            auto w = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(response.data() + sent),
                                            response.size() - sent);
            if (!w) {
                if (w.error() == agentengine::pal::would_block()) continue;
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

    std::string reply_text_;
    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> requests_served_{0};
    mutable std::mutex mu_;
    std::string last_body_;
    std::jthread thread_;
};

[[nodiscard]] agentengine::Message user_turn(std::string text) {
    agentengine::Message m;
    m.role = agentengine::role::user;
    agentengine::ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value = agentengine::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// Regression fixture for the tools-forwarding claim (J1-R8): a real declared tool, reachable through a
// real rt::AgentSession run. Default-constructible (no runtime state to inject), matching this
// project's own CRTP-policy-tag idiom.
struct OneWeatherToolArgs {
    std::string location;
};
AE_JSON_SCHEMA(OneWeatherToolArgs, location)

struct OneWeatherToolReply {
    std::string condition;
};
AE_JSON_SCHEMA(OneWeatherToolReply, condition)

struct OneWeatherTool : agentengine::Tool<OneWeatherTool> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description = "Get the current weather for a city.";
    using Args = OneWeatherToolArgs;
    using Reply = OneWeatherToolReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{"sunny"}; }
};

class ToolDeclaringHistoryProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3: required to satisfy
    // HasContextProviderName, needed to compose via HistoryAndSkillsProvider below.
    static constexpr std::string_view name = "real-backend-history";

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& session_ctx, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        contribution.messages.assign(session_ctx.history.begin(), session_ctx.history.end());
        contribution.tools = agentengine::ToolTable::from_tools<OneWeatherTool>().descriptors();
        co_return contribution;
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<ToolDeclaringHistoryProvider>,
              "the test fixture itself must satisfy the concept rt::AgentSession requires of "
              "HistoryProviderT");

// Default-constructible wrapper around `SkillsProvider<>` for the skills half below -- same shape
// test_agent_session_skills_real_backend.cpp's own BuiltinSkillsProvider established, ported verbatim
// (no Quark actor coupling in the original either -- `SkillsProvider<>` itself is a plain
// `ContextProvider` conformer).
class BuiltinSkillsProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "real-backend-skills";

    BuiltinSkillsProvider() : inner_({agentengine::make_builtin_skills_source()}) {}
    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, agentengine::EffectContext& ec) {
        return inner_.on_context(sc, ec);
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView tv, agentengine::EffectContext& ec) {
        return inner_.on_turn_end(tv, ec);
    }

private:
    agentengine::SkillsProvider<> inner_;
};
static_assert(agentengine::ContextProvider<BuiltinSkillsProvider>);

using RealClient = agentengine::openai::OpenAIChatClient<agentengine::InMemorySecretStore>;
static_assert(!std::is_default_constructible_v<RealClient>,
              "the point of ADR-018 (still true under rt::AgentSession): a real Phase D/E backend is "
              "NOT default-constructible");

using Session = AgentSession<RealClient>;
static_assert(std::is_default_constructible_v<Session>,
              "rt::AgentSession stays default-constructible even when its ChatClientT is not -- no "
              "quark::TestKit<A>-style construction ceremony to satisfy anymore, but the property "
              "itself (ADR-018) is unchanged and still worth asserting");

using ToolSession = AgentSession<RealClient, NoSessionState, ToolDeclaringHistoryProvider>;
static_assert(std::is_default_constructible_v<ToolSession>,
              "J1-R8: a tool-declaring HistoryProviderT stays default-constructible too");

using SkillsSession = AgentSession<RealClient, NoSessionState,
                                    agentengine::HistoryAndSkillsProvider<
                                        agentengine::HistoryProvider<agentengine::Window<0>>,
                                        BuiltinSkillsProvider>>;
static_assert(std::is_default_constructible_v<SkillsSession>,
              "SkillsSession must stay default-constructible, matching HistoryAndSkillsProvider's own "
              "conditional default ctor");

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif

    // ================================================================================================
    // J1-R1..R8 (from test_agent_session_real_backend.cpp)
    // ================================================================================================
    constexpr char const* kReplyText = "answered by a real OpenAIChatClient over a real socket";
    {
        CannedCompletionServer server(kReplyText);
        check(server.ok(), "J1 setup: the canned completion server started on a loopback port");
        if (server.ok()) {
            agentengine::InMemorySecretStore store;
            store.set("provider-key", "sk-canned-server-ignores-this");

            agentengine::ChatClientCapabilities caps;
            caps.tool_calling = true;

            // ---- J1-R1: the session exists, with no chat client yet -----------------------------------
            Session session;
            check(!session.has_chat_client(),
                  "J1-R1: a freshly constructed session holding a non-default-constructible ChatClientT "
                  "has no client yet -- the optional is disengaged, which is what lets the session be "
                  "constructed at all");

            // ---- J1-R2: a REAL backend is constructed in place, inside the live session ----------------
            session.emplace_chat_client(
                "127.0.0.1", server.port(), "canned-model", agentengine::SecretRef{"provider-key"}, caps,
                store, "/v1", agentengine::sandbox::resolve_host, /*ca_bundle=*/std::string{},
                /*http_referer=*/std::string{}, /*x_title=*/std::string{}, /*end_user_id=*/std::string{},
                /*seed=*/std::nullopt, ProviderTransport::plaintext_http);
            check(session.has_chat_client(),
                  "J1-R2: emplace_chat_client constructs a real OpenAIChatClient in place inside the "
                  "live session -- an assignment-based setter could not do this, since a backend "
                  "holding a reference member is not assignable");

            agentengine::CapabilitySet held = agentengine::CapabilitySet::grant_root(
                {agentengine::Capability{agentengine::cap::Secret{"provider-key", std::chrono::seconds{0}}}});
            session.initialize("session-real-backend", agentengine::Principal{"owner", "tenant-a"});
            session.set_capabilities(&held);

            // ---- J1-R3: ONE REAL TURN through the real turn loop ---------------------------------------
            agentengine::result<agentengine::rt::AgentResponse> r =
                drive(session.start_run(StartRun{user_turn("hello from a real session turn")}));

            check(r.has_value(),
                  "J1-R3: a start_run() call completes through the real turn loop with a REAL "
                  "ChatClient backend behind it -- real context assembly, real secret resolution, "
                  "real socket, real response parse");
            if (r.has_value()) {
                check(!r->message.content.empty(), "J1-R3: the AgentResponse carries content");
                if (!r->message.content.empty()) {
                    auto const* text = std::get_if<agentengine::Text>(&r->message.content.front().value);
                    check(text != nullptr && text->text == kReplyText,
                          "J1-R3: the reply text is the SERVER's, arriving through the emplaced client "
                          "-- a default-constructed mock could not have produced it");
                }
                check(r->usage.input_tokens == 11 && r->usage.output_tokens == 13,
                      "J1-R3: Usage parsed from the real HTTP response body rides back through the "
                      "session's own AgentResponse");
            }

            check(server.requests_served() == 1,
                  "J1-R3: the server served exactly one request -- the turn really crossed a socket "
                  "rather than being satisfied locally");

            // ---- J1-R4: the session's OWN assembled history is what reached the wire ------------------
            {
                std::string const sent = server.last_request_body();
                check(sent.find("hello from a real session turn") != std::string::npos,
                      "J1-R4: the user turn the SESSION assembled (via its HistoryProvider, not a "
                      "request the test hand-built) is present in the body the server actually "
                      "received");
                check(sent.find("\"model\":\"canned-model\"") != std::string::npos,
                      "J1-R4: the emplaced client's own configured model reached the wire");
            }

            // ---- J1-R5: history grew by exactly the user turn + the assistant reply --------------------
            check(session.history().size() == 2,
                  "J1-R5: the real turn loop appended exactly the user turn and the assistant reply");
            if (session.history().size() == 2) {
                check(session.history()[0].role == agentengine::role::user, "J1-R5: history[0] is the user turn");
                check(session.history()[1].role == agentengine::role::assistant,
                      "J1-R5: history[1] is the assistant reply, sourced from the real backend");
            }

            // ---- J1-R6: a second turn reuses the same emplaced client -----------------------------------
            {
                agentengine::result<agentengine::rt::AgentResponse> r2 =
                    drive(session.start_run(StartRun{user_turn("second turn")}));
                check(r2.has_value(), "J1-R6: a second run succeeds against the same emplaced client");
                check(server.requests_served() == 2, "J1-R6: it crossed the socket again");
                check(session.history().size() == 4,
                      "J1-R6: history now holds two full turns -- the emplaced client is durable "
                      "across runs, not consumed by the first one");
            }

            // ---- J1-R7: fail-closed when a non-default-constructible client was never emplaced --------
            {
                Session bare;
                bare.initialize("session-no-client", agentengine::Principal{"owner", "tenant-a"});
                check(!bare.has_chat_client(), "J1-R7: this session has no client emplaced");
                agentengine::result<agentengine::rt::AgentResponse> r3 =
                    drive(bare.start_run(StartRun{user_turn("hi")}));
                check(!r3.has_value(),
                      "J1-R7: a run on a session whose ChatClientT was never emplaced FAILS CLOSED -- "
                      "it never fabricates an AgentResponse for a turn that never reached a model (I3)");
            }

            // ---- J1-R8: a declared tool actually reaches the wire through a REAL session run -----------
            {
                ToolSession tool_session;
                tool_session.emplace_chat_client(
                    "127.0.0.1", server.port(), "canned-model", agentengine::SecretRef{"provider-key"},
                    caps, store, "/v1", agentengine::sandbox::resolve_host, /*ca_bundle=*/std::string{},
                    /*http_referer=*/std::string{}, /*x_title=*/std::string{},
                    /*end_user_id=*/std::string{}, /*seed=*/std::nullopt, ProviderTransport::plaintext_http);
                tool_session.initialize("session-with-tools", agentengine::Principal{"owner", "tenant-a"});
                tool_session.set_capabilities(&held);

                agentengine::result<agentengine::rt::AgentResponse> r4 =
                    drive(tool_session.start_run(StartRun{user_turn("what's the weather?")}));
                check(r4.has_value(), "J1-R8: a run on a tool-declaring session still completes");

                std::string const sent = server.last_request_body();
                check(sent.find(R"("tools":[)") != std::string::npos,
                      "J1-R8: the wire request the server ACTUALLY received now carries a top-level "
                      "'tools' array");
                check(sent.find(R"("name":"get_weather")") != std::string::npos,
                      "J1-R8: the declared tool's own name is present inside that array -- the real "
                      "ToolDescriptor reached the wire through the real turn loop");
            }
        }
    }

    // ================================================================================================
    // R1-R4 (from test_agent_session_skills_real_backend.cpp)
    // ================================================================================================
    {
        CannedCompletionServer server("answered with skills mounted");
        check(server.ok(), "Skills setup: the canned completion server started on a loopback port");
        if (server.ok()) {
            agentengine::InMemorySecretStore store;
            store.set("provider-key", "sk-canned-server-ignores-this");
            agentengine::CapabilitySet held = agentengine::CapabilitySet::grant_root(
                {agentengine::Capability{agentengine::cap::Secret{"provider-key", std::chrono::seconds{0}}}});
            agentengine::ChatClientCapabilities caps;
            caps.tool_calling = true;

            SkillsSession session;
            session.emplace_chat_client(
                "127.0.0.1", server.port(), "canned-model", agentengine::SecretRef{"provider-key"}, caps,
                store, "/v1", agentengine::sandbox::resolve_host, /*ca_bundle=*/std::string{},
                /*http_referer=*/std::string{}, /*x_title=*/std::string{}, /*end_user_id=*/std::string{},
                /*seed=*/std::nullopt, ProviderTransport::plaintext_http);
            session.initialize("session-with-skills", agentengine::Principal{"owner", "tenant-a"});
            session.set_capabilities(&held);

            // ---- R1: a real turn through a real, skills-composed session ----------------------------
            agentengine::result<agentengine::rt::AgentResponse> r =
                drive(session.start_run(StartRun{user_turn("hello with skills mounted")}));
            check(r.has_value(), "R1: a start_run() call completes through the real turn loop with a "
                                  "HistoryAndSkillsProvider-composed session");

            std::string const sent = server.last_request_body();
            auto const history_pos = sent.find("hello with skills mounted");
            auto const skill_pos = sent.find("using-the-code-interpreter");
            check(history_pos != std::string::npos,
                  "R2: the session's own assembled HISTORY still reaches the wire -- composing with "
                  "skills did not break the ordinary history contribution");
            check(skill_pos != std::string::npos && sent.find("execute_code") != std::string::npos,
                  "R3: the SKILL ADVERTISEMENT (name + description, from the real builtin skill) ALSO "
                  "reaches the wire request -- present here only because HistoryAndSkillsProvider "
                  "actually composed both contributors through the real assemble_context()");
            if (history_pos != std::string::npos && skill_pos != std::string::npos) {
                check(skill_pos < history_pos,
                      "R3b: the skill advertisement is ORDERED BEFORE the conversation history on the "
                      "wire, not just present somewhere in the body -- the regression test for a real "
                      "bug where HistoryAndSkillsProvider pushed history before skills");
            }

            if (r.has_value()) {
                check(!r->message.content.empty(), "R4: the AgentResponse carries content from the real server");
            }
            // Reading a mounted skill's own content back out via mount_read is proven, directly and
            // more thoroughly, by test_skill_provider_mount.cpp's own R1 (unaffected by this port; the
            // old skills-real-backend file's own comment already deferred it there, and rt::AgentSession
            // has no accessor into its private history_provider_ member either, for the same reason).
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_real_backend: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_real_backend: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
