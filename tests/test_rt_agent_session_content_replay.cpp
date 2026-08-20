// Proves decisions/ADR-069-content-triggered-model-response-replay.md's ContentReplayGateway<Inner>
// plugs into a real rt::AgentSession's own ChatClientT slot with ZERO changes to agent_session.hpp --
// AgentSession already accepts anything satisfying ChatClient<ChatClientT> OR
// ModelCallGatewayLike<ChatClientT> (agent_session.hpp's own class template requires-clause), and
// ContentReplayGateway<Inner> satisfies ModelCallGatewayLike directly. This is the real end-to-end
// proof tests/test_content_replay_gateway.cpp's own standalone unit tests didn't attempt: a settled,
// policy-violating response is discarded and replaced BEFORE it ever reaches AgentSession's own turn
// loop or durable history, and AgentSession's own loop never even knows a replay happened.

#include <cstdio>
#include <memory>
#include <string>
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
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

// A ModelCallGatewayLike Inner scripted to return a specific sequence of response texts. Shares its
// own `received` log via a shared_ptr, since ContentReplayGateway (and then AgentSession's own
// chat_client_) owns its own copy of this by value.
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
static_assert(agentengine::ModelCallGatewayLike<ContentReplayGateway<ScriptedGatewayInner>>,
              "ContentReplayGateway<Inner> must itself satisfy ModelCallGatewayLike -- the whole "
              "point of this file: it plugs into AgentSession's existing ChatClientT slot unmodified");

struct FixedProvider {
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
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

[[nodiscard]] std::string message_text_of(Message const& m) {
    if (m.content.empty()) return {};
    if (auto const* t = std::get_if<Text>(&m.content.front().value)) return t->text;
    return {};
}

}  // namespace

int main() {
    using agentengine::Principal;

    // T1: a discard-and-retry gateway is completely transparent to AgentSession's own turn loop --
    // the run converges with the RETRIED response, in ONE turn, even though the gateway made TWO
    // real backend calls underneath.
    {
        auto state = std::make_shared<ScriptedGatewayInner::State>();
        state->markers = {"BAD", "GOOD"};
        ContentReplayTrigger trigger = [](ChatResponse const& r) -> ContentReplayDecision {
            if (message_text_of(r.message) == "BAD") return ContentReplayDecision{true, "please try again"};
            return ContentReplayDecision{};
        };

        AgentSession<ContentReplayGateway<ScriptedGatewayInner>, NoSessionState, FixedProvider> session;
        session.initialize("cr-t1", Principal{"p1", ""});
        session.emplace_chat_client(ScriptedGatewayInner{state}, trigger);

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(outcome.has_value(), "T1: the run converges");
        if (outcome.has_value()) {
            check(message_text_of(outcome->message) == "GOOD",
                  "T1: the FINAL kept answer is the retry's -- the discarded 'BAD' response never "
                  "becomes this run's own AgentResponse");
        }
        check(state->received.size() == 2,
              "T1: the gateway made exactly 2 real backend calls, invisibly to AgentSession's own "
              "turn loop -- ONE round, ONE AgentResponse, TWO underlying calls");
        check(session.history().size() == 2,
              "T1: durable history holds exactly the ORIGINAL user message plus the FINAL 'GOOD' "
              "response -- the discarded 'BAD' response never commits to durable history at all, "
              "the mechanism's own core promise (ADR-069 §2)");
        if (session.history().size() == 2) {
            check(message_text_of(session.history().back()) == "GOOD",
                  "T1: the durably-committed assistant message is the retry's, not the discarded one");
        }
    }

    // T2: session-lifetime replay exhaustion surfaces as a real, ordinary AgentSession run failure --
    // the same failure shape every other run.* error already uses, nothing gateway-specific leaking
    // through as an unhandled exception or a malformed response.
    {
        auto state = std::make_shared<ScriptedGatewayInner::State>();
        state->markers = {"BAD", "BAD"};
        ContentReplayTrigger trigger = [](ChatResponse const&) -> ContentReplayDecision {
            return ContentReplayDecision{true, "please try again"};
        };

        AgentSession<ContentReplayGateway<ScriptedGatewayInner>, NoSessionState, FixedProvider> session;
        session.initialize("cr-t2", Principal{"p1", ""});
        session.emplace_chat_client(ScriptedGatewayInner{state}, trigger, /*max_replay_attempts=*/2,
                                     /*session_lifetime_cap=*/8);

        auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
        check(!outcome.has_value(), "T2: an always-triggering response exhausts max_replay_attempts and fails the run");
        if (!outcome.has_value()) {
            check(outcome.error().code == "content_replay.max_attempts_exhausted",
                  "T2: AgentSession surfaces the gateway's OWN error code unchanged, via the ordinary "
                  "run.chat_failed path -- no special-casing needed anywhere in agent_session.hpp");
        }
        check(session.history().size() == 1,
              "T2: on a failed round, ONLY the original user message is in durable history -- no "
              "partial, discarded, or final response was ever committed");
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_content_replay: OK\n"
                                 : "test_rt_agent_session_content_replay: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
