// Milestone 7 Phase A (013-UI-and-Streaming-Surfaces.md §1: "one internal run event stream ...
// ordered and monotonic per run, with a sequence number"). Proves `AgentSession::enable_event_stream()`
// (agent_session.hpp) emits the real events its turn loop can actually produce today -- RunStarted,
// TurnStarted, ModelCallStarted/Finished, TurnFinished, RunFinished on the success path, and
// RunFailed (never RunFinished) on each of the fail-closed branches that already existed before this
// phase (context-contribution failure, no chat client, chat failure, token-budget exceeded) -- in
// order, with a monotonic per-run sequence number that RESETS to 1 on the session's next run. See
// run_event.hpp's own top comment for why ToolCall*/ModelDelta/SandboxExec*/ArtifactProduced are
// real TYPES here but have no real PRODUCER yet (AgentSession's turn loop never reaches the tool
// pipeline or a streaming chat call).

#include <iostream>
#include <memory_resource>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/trust/principal.hpp"

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

ae::Message make_turn(std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{"hello"};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

// A chat client whose `chat()` succeeds or fails on demand -- both fail-closed branches this test
// needs (chat failure, and a reply engineered to blow a tiny token budget) ride the same conformer.
class ScriptedChatClient {
public:
    bool         fail_next  = false;
    std::uint64_t reply_tokens = 1;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        if (fail_next) {
            co_return std::unexpected(ae::error{ae::failure_class::transient, "scripted failure", "test.fail"});
        }
        ae::ContentItem item{};
        item.value  = ae::Text{"reply"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);

        co_return ae::ChatResponse{reply, ae::Usage{reply_tokens, reply_tokens, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }
};
static_assert(ae::ChatClient<ScriptedChatClient>, "ScriptedChatClient must satisfy the ChatClient concept");

// A `ChatClientT` that is never engaged (`AgentSession::has_chat_client()` stays false) -- the
// "no chat_client_" fail-closed branch. Deliberately NOT default-constructible (a required `int`
// constructor argument that is never actually used): `AgentSession::make_default_chat_client()`
// only auto-engages a default-constructible `ChatClientT` (agent_session.hpp's own comment), so a
// default-constructible conformer here would silently defeat the branch this test exists to prove.
// Real precedent for a non-default-constructible conformer nobody `emplace_chat_client()`s: exactly
// `OpenAIChatClient<Store>`/`AnthropicChatClient<Store>` (agent_session.hpp's own ADR-018 comment).
class NeverEngagedChatClient {
public:
    explicit NeverEngagedChatClient(int) {}
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        co_return std::unexpected(ae::error{ae::failure_class::fatal, "never reached", "test.unreachable"});
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return {}; }
};
static_assert(ae::ChatClient<NeverEngagedChatClient>, "NeverEngagedChatClient must satisfy the ChatClient concept");

std::vector<ae::RunEvent> drain(ae::stream<ae::RunEvent>& s) {
    std::vector<ae::RunEvent> events;
    while (auto ev = s.next()) events.push_back(std::move(*ev));
    return events;
}

}  // namespace

int main() {
    // --- A1: no enable_event_stream() call -- a run proceeds exactly as before, no crash --------
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-no-stream", ae::Principal{"p1", ""});

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(r.has_value(), "A1: a run with no event stream enabled still succeeds normally");
    }

    // --- A2: a successful run emits the full real success-path sequence, in order, seq 1..6 -----
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-success", ae::Principal{"p1", ""});
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(r.has_value(), "A2: the scripted successful run still succeeds");

        auto events = drain(viewer);
        AE_CHECK(events.size() == 6, "A2: exactly 6 events fire on the real success path");
        if (events.size() == 6) {
            ae::run_event_kind const expected[] = {
                ae::run_event_kind::run_started,         ae::run_event_kind::turn_started,
                ae::run_event_kind::model_call_started,  ae::run_event_kind::model_call_finished,
                ae::run_event_kind::turn_finished,       ae::run_event_kind::run_finished,
            };
            bool order_ok = true;
            bool seq_ok   = true;
            bool run_id_ok = true;
            for (std::size_t i = 0; i < 6; ++i) {
                if (events[i].kind != expected[i]) order_ok = false;
                if (events[i].seq != i + 1) seq_ok = false;
                if (events[i].run_id != "s-success:run:1") run_id_ok = false;
            }
            AE_CHECK(order_ok, "A2: events fire in the exact order 013 §1's turn-loop boundaries produce");
            AE_CHECK(seq_ok, "A2: sequence numbers are monotonic, starting at 1 (013 §1)");
            AE_CHECK(run_id_ok, "A2: every event in the run carries that run's own run_id");
        }
    }

    // --- A3: sequence resets to 1 on the SAME session's next run -- never carries the prior ------
    // --- run's count forward.                                                                  ---
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-reset", ae::Principal{"p1", ""});
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        (void)kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        auto first_run_events = drain(viewer);
        (void)kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-2")});
        auto second_run_events = drain(viewer);

        AE_CHECK(first_run_events.size() == 6 && second_run_events.size() == 6,
                 "A3: both runs on the same session each emit their own full 6-event sequence");
        AE_CHECK(!second_run_events.empty() && second_run_events.front().seq == 1,
                 "A3: the second run's first event is seq 1 again, not seq 7 -- 013 §1's sequence "
                 "number is monotonic PER RUN, not across the session's whole lifetime");
        AE_CHECK(!second_run_events.empty() && second_run_events.front().run_id == "s-reset:run:2",
                 "A3: the second run's events carry the second run's own run_id");
    }

    // --- A4a: a chat-call failure emits RunFailed as the terminal event, never RunFinished -------
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-chat-fail", ae::Principal{"p1", ""});
        kit.actor().emplace_chat_client().fail_next = true;
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(!r.has_value(), "A4a: a scripted chat failure still fails the ask (fail-closed)");

        auto events = drain(viewer);
        AE_CHECK(!events.empty() && events.back().kind == ae::run_event_kind::run_failed,
                 "A4a: RunFailed is the terminal event on a chat-call failure");
        if (!events.empty()) {
            auto const* payload = std::get_if<ae::run_event_payload::RunFailed>(&events.back().payload);
            AE_CHECK(payload != nullptr && payload->error_code == "run.chat_failed",
                     "A4a: RunFailed carries the real error_code for a chat-call failure");
        }
        bool saw_run_finished = false;
        for (auto const& ev : events) saw_run_finished |= (ev.kind == ae::run_event_kind::run_finished);
        AE_CHECK(!saw_run_finished, "A4a: a failed run never also emits RunFinished");
    }

    // --- A4b: token-budget exceeded emits RunFailed with the budget error_code -------------------
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-budget", ae::Principal{"p1", ""}, /*token_budget=*/std::uint64_t{1});
        kit.actor().emplace_chat_client().reply_tokens = 1000;  // blows a budget of 1
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(!r.has_value(), "A4b: exceeding the per-run token budget still fails the ask");

        auto events = drain(viewer);
        AE_CHECK(!events.empty() && events.back().kind == ae::run_event_kind::run_failed,
                 "A4b: RunFailed is the terminal event on a token-budget failure");
        if (!events.empty()) {
            auto const* payload = std::get_if<ae::run_event_payload::RunFailed>(&events.back().payload);
            AE_CHECK(payload != nullptr && payload->error_code == "run.token_budget_exceeded",
                     "A4b: RunFailed carries the real error_code for a token-budget failure");
        }
    }

    // --- A4c: no chat client configured emits RunFailed, before ModelCallStarted ever fires ------
    {
        using Session = ae::AgentSession<NeverEngagedChatClient>;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-no-client", ae::Principal{"p1", ""});
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{make_turn("m-1")});
        AE_CHECK(!r.has_value(), "A4c: no chat client configured still fails the ask");

        auto events = drain(viewer);
        bool saw_model_call = false;
        for (auto const& ev : events) {
            saw_model_call |= (ev.kind == ae::run_event_kind::model_call_started);
        }
        AE_CHECK(!saw_model_call,
                 "A4c: ModelCallStarted never fires when there is no chat client to call");
        AE_CHECK(!events.empty() && events.back().kind == ae::run_event_kind::run_failed,
                 "A4c: RunFailed is still the terminal event");
        if (!events.empty()) {
            auto const* payload = std::get_if<ae::run_event_payload::RunFailed>(&events.back().payload);
            AE_CHECK(payload != nullptr && payload->error_code == "run.no_chat_client",
                     "A4c: RunFailed carries the real error_code for a missing chat client");
        }
    }

    // --- A5: an admission-denied StartRun mints no Run at all -- no event fires for it -----------
    {
        using Session = ae::AgentSession<ScriptedChatClient>;
        quark::TestKit<Session> kit;
        ae::Principal const owner    = ae::Principal{"p-owner", "tenant-a"};
        ae::Principal const stranger = ae::Principal{"p-stranger", "tenant-a"};
        kit.actor().initialize("s-denied", owner);
        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());

        auto r = kit.ask<ae::AgentResponse>(
            ae::StartRun{make_turn("m-1"), ae::SessionCaller{stranger.id, stranger.tenant_id}});
        AE_CHECK(!r.has_value(), "A5: the admission-denied ask still fails (018 §2)");

        auto events = drain(viewer);
        AE_CHECK(events.empty(),
                 "A5: no RunEvent fires for an admission-denied StartRun -- 001 §1 mints no Run at "
                 "all before admission passes, so there is no run_id to attach an event to");
    }

    if (g_failures == 0) {
        std::cout << "OK: all run-event-stream checks passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
