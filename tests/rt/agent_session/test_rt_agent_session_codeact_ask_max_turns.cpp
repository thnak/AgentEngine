// Regression proof for the fix to AgentSession::resolve_codeact_ask()'s own "codeact.ask_pending"
// branch (rt/agent_session.hpp) -- found by a red-team round dedicated to session_builder.hpp's own
// "finding 7" (docs/planning/quickstart-session-builder-design-draft.md §0i): that branch used to
// `co_return` WITHOUT ever touching `effect_context_.turn_index`, the ONLY field `run_rounds()`'s
// `max_turns_` bound inspects -- and `run_rounds()` is never re-entered while an interaction keeps
// resolving to ask-pending, so a CodeAct script that keeps asking follow-up questions forever was
// COMPLETELY unbounded by `.max_turns()`/`.token_budget()`. LIVE-REPRODUCED by that red-team round: 50
// `resolve_interaction()` round trips against a scripted always-ask tool, `max_turns_ == 3`, never once
// produced `run.max_turns_exceeded`.
//
// Deliberately does NOT use a real `MediatedPythonRunner` (unlike tests/test_agent_session_suspend_
// codeact_ask.cpp, this file's own sibling covering the ask/resume MECHANISM itself) -- nothing here
// needs real CPython, since ANY tool whose `invoke()` returns `error_code == "codeact.ask_pending"`
// triggers the same suspend path (confirmed by reading rt/agent_session.hpp's own producer/consumer
// contract comment). A lightweight, always-ask-pending scripted tool is sufficient to prove the
// turn-bound mechanism itself, matching this project's own "no heavier dependency than the claim
// needs" convention.
//
// R3 (added by round 8's own dedicated re-examination of this fix, docs/planning/quickstart-session-
// builder-design-draft.md's own "finding 7" thread) closes finding 17: `clear_in_process_state()`
// never cleared `pending_codeact_asks_`, unlike every other piece of interaction state it resets --
// a pooled/reused `AgentSession` (the `Stateless<N>` pooling pattern) that clears+reinitializes after
// an in-flight codeact-ask permanently retained that record (full script source + every answer given)
// for the C++ object's remaining lifetime. Round 8 also found and fixed a related, LOWER-severity gap
// (finding 16: `resolve_codeact_ask()`'s "no stored record" branch is genuinely reachable after a
// session restore mid-ask, since `restore_from_record()` restores `open_interactions_` but never
// `pending_codeact_asks_` -- previously left the interaction stuck open forever; now erases it too) --
// NOT regression-tested here, since reaching that branch through the public API alone would require
// reconstructing `history_` (no public mutator exists) to look like a genuinely suspended tool-call
// turn; verified by code review only, matching this project's own disclosed-scope-limit convention
// for round 4's `ask_mutex_` concurrency fix (this file's own sibling, session_builder.hpp's top
// comment).

#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/capability.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::task;
using agentengine::CapabilitySet;
using agentengine::Capability;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::interaction_reason;
using agentengine::role;

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

// Same "nothing here genuinely suspends on an external wake" reasoning every other rt::AgentSession
// test file in this suite already documents for its own drive<T>().
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct AskArgs {
    std::string code;
    std::string language;
};
AE_JSON_SCHEMA(AskArgs, code, language)
struct AskReply {
    bool ok = false;
};
AE_JSON_SCHEMA(AskReply, ok)

// Same public shape as tools/cli_chat.cpp's real execute_code tool (name/args), so the ask-pending
// suspend path is reached via the identical producer contract a real CodeAct script would use -- only
// the invoke BODY is faked, via make_tool_descriptor_with_invoke below, never this dead static one.
struct AskPendingTool : agentengine::Tool<AskPendingTool, agentengine::Capabilities<>,
                                            agentengine::EffectClass<agentengine::effect_class::at_most_once>> {
    static constexpr std::string_view name = "execute_code";
    static constexpr std::string_view description = "Always ask-pends -- a scripted regression fixture.";
    using Args = AskArgs;
    using Reply = AskReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "AskPendingTool::invoke() must never run directly in this test -- only through the "
            "make_tool_descriptor_with_invoke lambda below",
            "test.dead_static_invoke_path"});
    }
};

class AlwaysAskHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = {agentengine::make_tool_descriptor_with_invoke<AskPendingTool>(
            [](AskArgs, EffectContext&) -> agentengine::result<AskReply> {
                return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                             "still waiting on an answer",
                                                             "codeact.ask_pending"});
            })};
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<AlwaysAskHistoryProvider>);

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::size_t call_count = 0;
    };

    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        ++state_->call_count;
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall call;
        call.call_id      = "c" + std::to_string(state_->call_count);
        call.tool_name     = "execute_code";
        call.arguments_json =
            R"json({"code":"import agent\nagent.ask('again?')","language":"python"})json";
        call.provenance     = call_provenance::vendor_structured;
        item.value = call;
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

using Session = AgentSession<ScriptedChatClient, NoSessionState, AlwaysAskHistoryProvider>;

}  // namespace

int main() {
    using agentengine::Principal;

    // R1: a session with max_turns == 3 whose script keeps calling agent.ask() forever must NOT
    // suspend indefinitely -- it must fail closed with run.max_turns_exceeded within a small, bounded
    // number of resolve_interaction() round trips, matching the SAME cap a non-codeact tool-call loop
    // (test_rt_agent_session_tool_call_loop.cpp's own R4/R5) is already held to.
    {
        Session session;
        session.initialize("codeact-max-turns", Principal{"p", ""}, /*token_budget=*/std::nullopt,
                             /*max_turns=*/std::uint64_t{3});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        ScriptedChatClient& client = session.emplace_chat_client();

        auto first = drive(session.start_run(StartRun{user_message("go")}));
        check(!first.has_value() && first.error().code == Session::kSuspendedForCodeActAsk,
              "R1: the first round suspends for agent.ask(), as expected");

        bool saw_max_turns_exceeded = false;
        std::size_t rounds_run       = 0;
        for (std::size_t i = 0; i < 50 && !saw_max_turns_exceeded; ++i) {
            check(session.has_open_interactions(),
                  "R1: an interaction stays open while the ask loop continues");
            if (!session.has_open_interactions()) break;
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            auto resumed = drive(session.resolve_interaction(
                ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt, std::string("42")}));
            ++rounds_run;
            if (!resumed.has_value() && resumed.error().code == "run.max_turns_exceeded") {
                saw_max_turns_exceeded = true;
            } else {
                check(!resumed.has_value() && resumed.error().code == Session::kSuspendedForCodeActAsk,
                      "R1: while still under the cap, the round keeps suspending for another ask, not "
                      "some other, unrelated failure");
            }
        }

        check(saw_max_turns_exceeded,
              "R1: the ask loop eventually fails closed with run.max_turns_exceeded -- it does NOT run "
              "away unbounded the way it did before the fix (round-7 red-team's own live reproduction: "
              "50 rounds, never once triggered, before this fix existed)");
        check(saw_max_turns_exceeded && rounds_run <= 3,
              "R1: the cap fires within max_turns_ (3) rounds, not merely 'eventually, some large "
              "number of rounds later'");
        check(!session.has_open_interactions(),
              "R1: the interaction is closed once the cap fires -- not left open/leaked");
        check(client.call_count() == 1,
              "R1: the underlying model (chat()) is called exactly once for the WHOLE exchange -- the "
              "ask loop itself never re-invokes chat(), so this proves the bound is enforced on the "
              "resolve_codeact_ask() resume path itself, not merely inherited from a chat()-side cap");
    }

    // R2: a session with NO max_turns bound (the explicit, documented opt-out -- std::nullopt) is
    // still allowed to ask indefinitely -- this fix must not regress that legitimate host choice.
    // Bounded to a moderate, deliberately-larger-than-R1's-cap round count here (not run forever)
    // purely so this test itself terminates; the session's OWN bound is genuinely absent.
    {
        Session session;
        session.initialize("codeact-unbounded", Principal{"p", ""});  // max_turns defaults to nullopt
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        (void)session.emplace_chat_client();

        auto first = drive(session.start_run(StartRun{user_message("go")}));
        check(!first.has_value() && first.error().code == Session::kSuspendedForCodeActAsk,
              "R2 setup: the first round suspends for agent.ask()");

        bool saw_unexpected_failure = false;
        for (std::size_t i = 0; i < 10; ++i) {
            std::string const interaction_id = session.open_interactions().front().interaction_id;
            auto resumed = drive(session.resolve_interaction(
                ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt, std::string("42")}));
            if (!resumed.has_value() && resumed.error().code != Session::kSuspendedForCodeActAsk) {
                saw_unexpected_failure = true;
                break;
            }
        }
        check(!saw_unexpected_failure,
              "R2: with max_turns_ explicitly left unbounded (nullopt), the ask loop keeps suspending "
              "normally -- the fix only enforces a cap when the host actually set one, matching every "
              "other max_turns_ consumer in this file");
    }

    // R3: round 8's finding 17 -- clear_in_process_state() must actually drop a pending codeact-ask
    // record, not silently retain it forever (the leak this file's own top comment documents).
    {
        Session session;
        session.initialize("codeact-clear-leak", Principal{"p", ""});  // max_turns defaults to nullopt
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        (void)session.emplace_chat_client();

        auto first = drive(session.start_run(StartRun{user_message("go")}));
        check(!first.has_value() && first.error().code == Session::kSuspendedForCodeActAsk,
              "R3 setup: the first round suspends for agent.ask(), opening a pending record");
        check(session.pending_codeact_ask_count() == 1,
              "R3 setup: exactly one PendingCodeActAsk record now exists");

        session.clear_in_process_state();
        check(session.pending_codeact_ask_count() == 0,
              "R3: clear_in_process_state() actually drops the pending codeact-ask record -- before "
              "the finding-17 fix this stayed at 1 forever, a permanent leak of the record (including "
              "the full script source and every answer given) for a pooled/reused session object");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_rt_agent_session_codeact_ask_max_turns: ALL PASS\n"
                                          : "test_rt_agent_session_codeact_ask_max_turns: %d FAILURE(S)\n",
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
