// Proves decisions/ADR-066-context-provider-attribution-provenance.md's stamping mechanism actually
// reaches a real rt::AgentSession round's outbound ChatRequest -- not just the standalone
// assemble_context() unit tests (tests/test_context_provenance.cpp). AgentSession itself calls
// history_provider_.on_context() directly, never assemble_context() -- so attribution is only real
// when AgentSession's own HistoryProviderT slot is occupied by a COMPOSED provider
// (ComposedContextProvider/HistoryAndSkillsProvider), which internally routes through
// assemble_context(). Zero changes to agent_session.hpp were needed for this -- this file proves
// that existing composition path carries provenance all the way to the wire.

#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
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
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

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
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

// A second, distinct contributor -- proves the SAME attribution mechanism distinguishes it from
// HistoryProvider by contributor_type, not just by index.
struct SkillLikeProvider {
    static constexpr std::string_view name = "skill-like";

    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        Message advertisement;
        advertisement.role = role::system;
        ContentItem item;
        item.origin = content_origin::system;
        item.value  = Text{"skill advertisement"};
        advertisement.content.push_back(std::move(item));
        c.messages.push_back(std::move(advertisement));

        ToolDescriptor td;
        td.name        = "skill_tool";
        td.description = "a tool this provider contributes";
        c.tools.push_back(std::move(td));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<SkillLikeProvider>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

}  // namespace

int main() {
    using agentengine::Principal;
    using ComposedProvider =
        agentengine::ComposedContextProvider<agentengine::HistoryProvider<agentengine::Window<0>>,
                                               SkillLikeProvider>;

    AgentSession<RecordingChatClient, NoSessionState, ComposedProvider> session;
    session.initialize("prov-t1", Principal{"p1", ""});
    // Default-constructed history_provider() starts UNENGAGED unconditionally (2026-08-23:
    // ComposedContextProvider's default ctor no longer auto-engages even when every Ms is
    // default-constructible -- see composed_context_provider.hpp's own comment on why).
    auto engaged = session.history_provider().engage(
        std::tuple{agentengine::HistoryProvider<agentengine::Window<0>>{}, SkillLikeProvider{}});
    check(engaged.has_value(), "T1 setup: engage() succeeds");
    RecordingChatClient& client = session.emplace_chat_client();

    auto outcome = drive(session.start_run(StartRun{user_message("hi")}));
    check(outcome.has_value(), "T1: the run converges with a real 2-provider composition");
    check(client.requests().size() == 1, "T1: exactly one model call happened");

    if (!client.requests().empty()) {
        auto const& msgs  = client.requests().front().messages;
        auto const& tools = client.requests().front().tools;

        Message const* history_msg = nullptr;
        Message const* skill_msg   = nullptr;
        for (Message const& m : msgs) {
            if (m.role == role::user) history_msg = &m;
            if (m.role == role::system) skill_msg = &m;
        }
        check(history_msg != nullptr && skill_msg != nullptr,
              "T1: both contributors' messages reach the real outbound ChatRequest");

        if (history_msg != nullptr) {
            check(history_msg->attribution.has_value(),
                  "T1: the real user message, replayed by HistoryProvider, is stamped");
            if (history_msg->attribution.has_value()) {
                check(history_msg->attribution->contributor_type == "history",
                      "T1: its contributor_type is \"history\" -- HistoryProvider's own declared name");
                check(history_msg->attribution->contributor_index == 0,
                      "T1: HistoryProvider was declared FIRST in ComposedProvider's own Ms... pack");
            }
            check(!history_msg->content.empty() && history_msg->content.front().origin == content_origin::user,
                  "T1: a genuine historical replay keeps content_origin::user -- ADR-066's own fix "
                  "doesn't corrupt the dominant, correct case");
        }
        if (skill_msg != nullptr) {
            check(skill_msg->attribution.has_value(),
                  "T1: the skill-like provider's own synthesized message is ALSO stamped");
            if (skill_msg->attribution.has_value()) {
                check(skill_msg->attribution->contributor_type == "skill-like",
                      "T1: its contributor_type names the REAL contributor -- distinguishable from "
                      "\"history\" by type, not just by which slot happened to run second");
                check(skill_msg->attribution->contributor_index == 1,
                      "T1: SkillLikeProvider was declared SECOND");
            }
            check(!skill_msg->content.empty() && skill_msg->content.front().origin == content_origin::system,
                  "T1: SkillLikeProvider's legitimate content_origin::system claim is left untouched "
                  "-- ADR-066's user-forgery check doesn't regress this real, shipped pattern");
        }

        check(!tools.empty() && tools.front().name == "skill_tool" && tools.front().attribution.has_value(),
              "T1: a contributed ToolDescriptor is ALSO stamped end to end, reaching the real request");
        if (!tools.empty() && tools.front().attribution.has_value()) {
            check(tools.front().attribution->contributor_type == "skill-like",
                  "T1: the tool's attribution names the provider that contributed it");
        }
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_context_provenance: OK\n"
                                 : "test_rt_agent_session_context_provenance: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
