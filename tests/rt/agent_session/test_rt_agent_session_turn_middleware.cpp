// Proves decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md's mechanism actually wired
// into a real rt::AgentSession round, not just proven standalone (tests/test_turn_middleware.cpp).
// AgentSession::set_turn_middleware_hook() (agent_session.hpp) is the real seam this file exercises:
// a TurnMiddlewareHook runs once per round, after context assembly (including the dynamically-
// injected schedule_wakeup tool) and before that round's ChatRequest is built.

#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::task;

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

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::ContextProvider;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolDescriptor;
using agentengine::TurnContext;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::run_turn_middleware_chain;

class RecordingChatClient {
public:
    RecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ChatRequest> requests;
    };

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest req, EffectContext&) {
        state_->requests.push_back(req);
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused by these tests
    }

    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

struct FixedProvider {
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        ToolDescriptor td;
        td.name        = "provider_tool";
        td.description = "a real tool contributed by the fan-out provider, not by the turn middleware";
        c.tools.push_back(std::move(td));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<FixedProvider>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// Redacts the first tool it sees and annotates nothing else -- proves ToolSurfaceView's mutation
// actually reaches the real outbound ChatRequest, not just an in-memory ContextAssemblyResult a test
// built by hand.
struct RedactingMiddleware {
    static constexpr std::string_view name = "redacting";
    task<agentengine::result<std::monostate>> on_turn(TurnContext& ctx) {
        if (!ctx.assembled.combined.tools.empty()) ctx.tool_surface.redact(0);
        co_return agentengine::result<std::monostate>{};
    }
};

struct DenyingMiddleware {
    static constexpr std::string_view name = "denying";
    task<agentengine::result<std::monostate>> on_turn(TurnContext&) {
        co_return std::unexpected(
            agentengine::error{agentengine::failure_class::policy, "denied for test", "test.turn_denied"});
    }
};

}  // namespace

int main() {
    using agentengine::Principal;

    // T1: a redacting turn middleware's decision actually reaches the real outbound ChatRequest.
    {
        AgentSession<RecordingChatClient, NoSessionState, FixedProvider> session;
        session.initialize("tm-t1", Principal{"p1", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        std::tuple<RedactingMiddleware> chain{};
        session.set_turn_middleware_hook([&chain](TurnContext& ctx) {
            return run_turn_middleware_chain(chain, ctx);
        });

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T1: the run converges with a turn middleware wired in");
        check(client.requests().size() == 1, "T1: exactly one model call happened");
        if (!client.requests().empty()) {
            check(client.requests().front().tools.empty(),
                  "T1: the redacted tool never reaches the real outbound ChatRequest -- the turn "
                  "middleware's decision, made against TurnContext, is the SAME state that becomes "
                  "the round's own request, not a disconnected copy");
        }
    }

    // T2: a denying turn middleware fails the round BEFORE the model is ever called.
    {
        AgentSession<RecordingChatClient, NoSessionState, FixedProvider> session;
        session.initialize("tm-t2", Principal{"p1", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        std::tuple<DenyingMiddleware> chain{};
        session.set_turn_middleware_hook([&chain](TurnContext& ctx) {
            return run_turn_middleware_chain(chain, ctx);
        });

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(!outcome.has_value(), "T2: a denying turn middleware fails the round");
        if (!outcome.has_value()) {
            check(outcome.error().code == "test.turn_denied",
                  "T2: the failure carries the middleware's OWN error code, unmodified");
        }
        check(client.requests().empty(),
              "T2: the model was NEVER called -- this is a real pre_model denial, not a post-hoc "
              "check after the call already happened");
    }

    // T3: no hook set at all -- byte-identical to a session with no turn middleware concept, the
    // dominant, existing case every OTHER AgentSession test in this tree already exercises.
    {
        AgentSession<RecordingChatClient, NoSessionState, FixedProvider> session;
        session.initialize("tm-t3", Principal{"p1", ""});
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T3: with no turn middleware hook set, the run converges normally");
        if (!client.requests().empty()) {
            check(!client.requests().front().tools.empty(),
                  "T3: with no hook, the provider's own tool reaches the request unredacted -- "
                  "nothing about this seam changes behavior when unused");
        }
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_turn_middleware: OK\n"
                                 : "test_rt_agent_session_turn_middleware: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
