// Milestone 4 Phase B2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// `AgentSession::handle()` used to build `ChatRequest{history_}` directly -- "the full history,
// trivially" (agent_session.hpp's own M1-scope top comment). This proves the replacement (routing
// through a real `HistoryProviderT` via `ContextProvider::on_context`) end to end: (a) the default
// `HistoryProvider<Window<0>>` is behaviorally identical to the old direct-history-access code (no
// regression for every Phase A test, which all use the 2-parameter `AgentSession<ChatClientT,
// StateT>` form), and (b) a real `Window<N>` actually changes what the `ChatClient` receives, not
// just what a standalone `HistoryProvider` computes in isolation (test_history_provider.cpp's job).

#include <iostream>
#include <memory_resource>
#include <sstream>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/history_provider.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// Reports back exactly what `request.messages` it was actually called with -- proves what the
// ChatClient sees, the same "report the real input back" technique test_agent_session_run_identity
// already uses for EffectContext, applied here to the assembled ChatRequest instead.
class EchoingCountChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const& request, ae::EffectContext&) {
        std::ostringstream joined;
        for (auto const& m : request.messages) {
            if (!m.content.empty()) {
                joined << std::get<ae::Text>(m.content.front().value).text << ";";
            }
        }

        ae::ContentItem item{};
        item.value  = ae::Text{"count=" + std::to_string(request.messages.size()) +
                                " seen=" + joined.str()};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const& request, ae::EffectContext&) {
        std::ostringstream joined;
        for (auto const& m : request.messages) {
            if (!m.content.empty()) {
                joined << std::get<ae::Text>(m.content.front().value).text << ";";
            }
        }

        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"count=" + std::to_string(request.messages.size()) +
                                     " seen=" + joined.str()};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<EchoingCountChatClient>,
              "EchoingCountChatClient must satisfy the ChatClient concept (004 §1)");

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

[[nodiscard]] std::string reply_text(ae::AgentResponse const& r) {
    return std::get<ae::Text>(r.message.content.front().value).text;
}

} // namespace

int main() {
    // --- Default third template parameter (Window<0>, unbounded) behaves exactly like the old
    // direct-history-access code: the ChatClient sees the WHOLE accumulated history every turn ----
    {
        using DefaultSession = ae::AgentSession<EchoingCountChatClient>;
        quark::TestKit<DefaultSession> kit;
        kit.actor().initialize("s-default", ae::Principal{"p-hank", ""});

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("first", "m-1")});
        AE_CHECK(r1.has_value() && reply_text(*r1) == "count=1 seen=first;",
                 "B2-R1: default HistoryProviderT (Window<0>) sees the full history on turn 1 "
                 "(just the new input, matching the old unwindowed behavior)");

        // After turn 1: history_ has [user:first, assistant:reply]. Turn 2 appends user:second, so
        // the ChatClient should see all 3 -- exactly what the old `ChatRequest{history_}` gave it.
        auto r2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("second", "m-2")});
        AE_CHECK(r2.has_value() && reply_text(*r2) == "count=3 seen=first;count=1 seen=first;;second;",
                 "B2-R2: default HistoryProviderT (Window<0>) sees the FULL accumulated history on "
                 "turn 2, no regression from Phase A's direct-history-access behavior");
    }

    // --- A real Window<N> actually drops older messages from what the ChatClient sees ------------
    {
        using WindowedSession =
            ae::AgentSession<EchoingCountChatClient, ae::NoSessionState, ae::HistoryProvider<ae::Window<2>>>;
        quark::TestKit<WindowedSession> kit;
        kit.actor().initialize("s-windowed", ae::Principal{"p-hank", ""});

        auto r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("alpha", "m-1")});
        AE_CHECK(r1.has_value() && reply_text(*r1) == "count=1 seen=alpha;",
                 "B2-R3: Window<2> with only 1 message so far sees all of it (nothing to drop yet)");

        // History is now [user:alpha, assistant:reply1, user:beta] (3 entries) when turn 2 runs --
        // Window<2> must keep only the LAST 2: [assistant:reply1, user:beta], dropping user:alpha.
        auto r2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("beta", "m-2")});
        AE_CHECK(r2.has_value() && reply_text(*r2) == "count=2 seen=count=1 seen=alpha;;beta;",
                 "B2-R4: Window<2> drops the oldest message (user:alpha) once history exceeds 2 -- "
                 "the ChatClient never sees it, proving the window reaches the real turn loop, not "
                 "just a standalone HistoryProvider computation");
        AE_CHECK(kit.actor().history().size() == 4,
                 "B2-R5: the WINDOW is what's budgeted for the model -- the session's own durable "
                 "history_ still keeps everything (windowing is a read-side view, never a mutation "
                 "of history itself, 005 §3's 'derived, never simply the history')");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_history_window: OK\n"
                                   : "test_agent_session_history_window: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
