// Answers decisions/ADR-069-content-triggered-model-response-replay.md §7's own open question with
// real evidence for the first time: "whether ADR-067's pre_model mechanism and this ADR's
// post_model-adjacent mechanism are genuinely non-overlapping... a future pass implementing both
// together, wired into one real session, must confirm this before either ships." Both are now wired
// into rt::AgentSession (set_turn_middleware_hook() and the ContentReplayGateway<Inner> ChatClientT
// slot) -- this file is that future pass, run against ONE real session using both simultaneously.

#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/content_replay_gateway.hpp"
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
using agentengine::ContentItem;
using agentengine::ContentReplayDecision;
using agentengine::ContentReplayGateway;
using agentengine::ContentReplayTrigger;
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

struct ScriptedGatewayInner {
    struct State {
        std::vector<std::string> markers;
        std::vector<ChatRequest> received;
        std::size_t              next_index = 0;
    };
    std::shared_ptr<State> state;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    [[nodiscard]] task<agentengine::result<ChatResponse>> call(ChatRequest request, EffectContext&) {
        state->received.push_back(request);
        std::string const marker = state->markers.at(state->next_index++);
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{marker};
        m.content.push_back(item);
        Usage usage{};
        usage.output_tokens = 5;
        co_return ChatResponse{m, usage};
    }
};
static_assert(agentengine::ModelCallGatewayLike<ScriptedGatewayInner>);

struct FixedProvider {
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        ToolDescriptor sensitive;
        sensitive.name        = "sensitive_tool";
        sensitive.description = "a tool the turn middleware redacts before the model ever sees it";
        c.tools.push_back(std::move(sensitive));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<FixedProvider>);

struct RedactingMiddleware {
    static constexpr std::string_view name = "redacting";
    task<agentengine::result<std::monostate>> on_turn(TurnContext& ctx) {
        if (!ctx.assembled.combined.tools.empty()) ctx.tool_surface.redact(0);
        co_return agentengine::result<std::monostate>{};
    }
};

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string message_text_of(Message const& m) {
    if (m.content.empty()) return {};
    if (auto const* t = std::get_if<Text>(&m.content.front().value)) return t->text;
    return {};
}

}  // namespace

int main() {
    using agentengine::Principal;

    auto state = std::make_shared<ScriptedGatewayInner::State>();
    state->markers = {"BAD", "GOOD"};
    ContentReplayTrigger trigger = [](ChatResponse const& r) -> ContentReplayDecision {
        if (message_text_of(r.message) == "BAD") return ContentReplayDecision{true, "please try again"};
        return ContentReplayDecision{};
    };

    AgentSession<ContentReplayGateway<ScriptedGatewayInner>, NoSessionState, FixedProvider> session;
    session.initialize("combo-t1", Principal{"p1", ""});
    session.emplace_chat_client(ScriptedGatewayInner{state}, trigger);

    std::tuple<RedactingMiddleware> chain{};
    session.set_turn_middleware_hook([&chain](TurnContext& ctx) { return run_turn_middleware_chain(chain, ctx); });

    auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
    check(outcome.has_value(), "both mechanisms wired into ONE session: the run still converges");
    if (outcome.has_value()) {
        check(message_text_of(outcome->message) == "GOOD",
              "the content-replay gateway's own retry still resolves the final answer correctly, "
              "with a turn middleware also active in the same round");
    }

    check(state->received.size() == 2,
          "the gateway still made exactly 2 real backend calls -- the turn middleware (which runs "
          "ONCE, before run_model_call() even starts) does not re-run per replay attempt, and does "
          "not interfere with the gateway's own internal retry count");

    bool any_request_carried_the_redacted_tool = false;
    for (ChatRequest const& req : state->received) {
        for (ToolDescriptor const& td : req.tools) {
            if (td.name == "sensitive_tool") any_request_carried_the_redacted_tool = true;
        }
    }
    check(!any_request_carried_the_redacted_tool,
          "the turn middleware's redaction applies to EVERY attempt the gateway makes underneath it, "
          "including the retried one -- the two mechanisms compose correctly: pre_model shaping "
          "happens once, upstream of both the original AND every replayed call, never bypassed by a "
          "later retry reconstructing the request from some earlier, unredacted state");

    std::printf(g_failures == 0 ? "test_rt_agent_session_turn_and_replay_composition: OK\n"
                                 : "test_rt_agent_session_turn_and_replay_composition: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
