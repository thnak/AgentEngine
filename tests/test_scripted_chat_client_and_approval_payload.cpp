// Proof for ADR-182 P1 and P2 (decisions/ADR-182-agent-test-driver-mcp.md).
//
//   P2 -- agentengine::testing::ScriptedChatClient (include/agentengine/testing/scripted_chat_client.hpp)
//     SC1 -- chat() plays scripted turns in order and captures each request.
//     SC2 -- a call with the script empty fails with scripted_chat_client.script_exhausted; it never
//            repeats the last turn. Positive control: the "repeat the last reply" client every ad-hoc
//            copy uses is modelled inline and shown to NOT fail the same call -- so SC2 can fail.
//     SC3 -- chat_stream() yields the same message and usage as chat(), and a scripted failure turn
//            arrives as a failed stream.
//     SC4 -- an AgentSession driven by it converges on scripted text, and an extra start_run() with
//            nothing scripted fails with the exhaustion code instead of silently succeeding.
//
//   P1 -- approval_requested carries tool name, arguments and needs_approval (BUG-3, BUG-1 info)
//     AP1 -- a round with one gated call: the event names the tool, carries the model's arguments,
//            and needs_approval == true.
//     AP2 -- a mixed round (gated + ungated): both events fire (BUG-1's semantics unchanged), the
//            gated one says needs_approval == true, the ungated one false, each with its own name
//            and arguments.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/chat_stream_drain.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

using agentengine::task;
using agentengine::text_of;
using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::testing::ScriptedChatClient;
using agentengine::testing::ScriptedToolCall;
using agentengine::testing::text_turn;
using agentengine::testing::tool_calls_turn;

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

agentengine::Message user_message(std::string text) {
    agentengine::Message m;
    m.role = agentengine::role::user;
    agentengine::ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value = agentengine::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

struct ValueArgs { int value = 0; };
AE_JSON_SCHEMA(ValueArgs, value)
struct ValueReply { int value = 0; };
AE_JSON_SCHEMA(ValueReply, value)

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<>,
                                       agentengine::EffectClass<agentengine::effect_class::pure>,
                                       agentengine::Approval<agentengine::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Needs approval before every call.";
    using Args = ValueArgs;
    using Reply = ValueReply;
    static agentengine::result<Reply> invoke(Args a, agentengine::EffectContext&) { return Reply{a.value}; }
};

struct FreeTool : agentengine::Tool<FreeTool, agentengine::Capabilities<>,
                                      agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "free_tool";
    static constexpr std::string_view description = "Never needs approval.";
    using Args = ValueArgs;
    using Reply = ValueReply;
    static agentengine::result<Reply> invoke(Args a, agentengine::EffectContext&) { return Reply{a.value}; }
};

class ToolsHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, agentengine::EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<GatedTool, FreeTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};

using Session = AgentSession<ScriptedChatClient, NoSessionState, ToolsHistoryProvider>;

std::vector<agentengine::run_event_payload::ApprovalRequested> approval_events(
    std::vector<agentengine::RunEvent> const& events) {
    std::vector<agentengine::run_event_payload::ApprovalRequested> out;
    for (auto const& ev : events) {
        if (ev.kind != agentengine::run_event_kind::approval_requested) continue;
        if (auto const* p = std::get_if<agentengine::run_event_payload::ApprovalRequested>(&ev.payload))
            out.push_back(*p);
    }
    return out;
}

}  // namespace

int main() {
    agentengine::EffectContext ctx;

    // ---- SC1 ---------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        check(client.push({text_turn("one"), text_turn("two")}).has_value(), "SC1: push two turns");
        agentengine::ChatRequest r1;
        r1.messages.push_back(user_message("first"));
        auto a = drive(client.chat(r1, ctx));
        auto b = drive(client.chat(agentengine::ChatRequest{}, ctx));
        check(a.has_value() && text_of(a->message) == "one", "SC1: first call plays the first turn");
        check(b.has_value() && text_of(b->message) == "two", "SC1: second call plays the second turn");
        auto reqs = client.requests();
        check(reqs.size() == 2 && reqs[0].messages.size() == 1 && text_of(reqs[0].messages[0]) == "first",
              "SC1: each request is captured in order, with its messages");
    }

    // ---- SC2 -------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        (void)client.push(text_turn("only"));
        (void)drive(client.chat(agentengine::ChatRequest{}, ctx));
        auto extra = drive(client.chat(agentengine::ChatRequest{}, ctx));
        check(!extra.has_value() && extra.error().code == agentengine::testing::kScriptExhaustedCode,
              "SC2: a call past the script fails with script_exhausted");
        check(client.call_count() == 2, "SC2: the failed call is still captured");

        // Positive control: the ad-hoc "repeat the last reply" policy, which SC2 exists to reject.
        std::vector<std::string> script{"only"};
        std::size_t calls = 0;
        auto repeat_last = [&]() -> std::optional<std::string> {
            std::size_t const i = calls < script.size() ? calls : script.size() - 1;
            ++calls;
            return script[i];
        };
        (void)repeat_last();
        check(repeat_last().has_value(),
              "SC2 control: the repeat-last policy answers the extra call, so it would pass silently");
    }

    // ---- SC3 -------------------------------------------------------------------------------------
    {
        ScriptedChatClient client;
        (void)client.push({tool_calls_turn({{"c1", "free_tool", R"({"value":3})"}}, agentengine::Usage{5, 7, 0, 0, 0.0}),
                           agentengine::testing::failure_turn(agentengine::error{
                               agentengine::failure_class::transient, "scripted outage", "test.outage"})});
        auto drained = agentengine::drain_chat_stream(client.chat_stream(agentengine::ChatRequest{}, ctx));
        bool ok_call = drained.ok && drained.accumulated.content.size() == 1;
        if (ok_call) {
            auto const* call = std::get_if<agentengine::ToolCall>(&drained.accumulated.content[0].value);
            ok_call = call != nullptr && call->tool_name == "free_tool" && call->arguments_json == R"({"value":3})";
        }
        check(ok_call, "SC3: chat_stream() yields the scripted tool call");
        check(drained.usage.has_value() && drained.usage->input_tokens == 5,
              "SC3: the final update carries the scripted usage");
        auto failed = agentengine::drain_chat_stream(client.chat_stream(agentengine::ChatRequest{}, ctx));
        check(!failed.ok && failed.failure.code == "test.outage",
              "SC3: a scripted failure turn arrives as a failed stream with its own code");
    }

    // ---- SC4 -------------------------------------------------------------------------------------
    {
        Session session;
        session.initialize("sc4", agentengine::Principal{"p", ""});
        auto& client = session.emplace_chat_client();
        (void)client.push(text_turn("hello back"));
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        auto r1 = drive(session.start_run(StartRun{user_message("hi")}));
        check(r1.has_value() && text_of(r1->message) == "hello back", "SC4: the run converges on scripted text");
        auto r2 = drive(session.start_run(StartRun{user_message("again")}));
        check(!r2.has_value(), "SC4: a run with nothing scripted fails instead of inventing a reply");
    }

    // ---- AP1 -------------------------------------------------------------------------------------
    {
        Session session;
        session.initialize("ap1", agentengine::Principal{"p", ""});
        (void)session.emplace_chat_client().push(
            tool_calls_turn({{"c1", "gated_tool", R"({"value":7})"}}));
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(!r.has_value() && r.error().code == Session::kSuspendedForApproval, "AP1: the run suspends");
        std::vector<agentengine::RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        auto approvals = approval_events(events);
        check(approvals.size() == 1, "AP1: one approval_requested event");
        check(!approvals.empty() && approvals[0].tool_name == "gated_tool",
              "AP1: the event names the tool (BUG-3)");
        check(!approvals.empty() && approvals[0].arguments_json == R"({"value":7})",
              "AP1: the event carries the call's arguments (BUG-3)");
        check(!approvals.empty() && approvals[0].needs_approval, "AP1: needs_approval is true");
        check(!approvals.empty() && !session.open_interactions().empty() &&
                  approvals[0].interaction_id == session.open_interactions().front().interaction_id,
              "AP1: the event's interaction_id is the open interaction");
    }

    // ---- AP2 -------------------------------------------------------------------------------------
    {
        Session session;
        session.initialize("ap2", agentengine::Principal{"p", ""});
        (void)session.emplace_chat_client().push(tool_calls_turn(
            {{"c1", "gated_tool", R"({"value":1})"}, {"c2", "free_tool", R"({"value":2})"}}));
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_suspend_for_approval(true);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(!r.has_value() && r.error().code == Session::kSuspendedForApproval, "AP2: the run suspends");
        std::vector<agentengine::RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        auto approvals = approval_events(events);
        check(approvals.size() == 2, "AP2: both calls in the round get an event (BUG-1 semantics unchanged)");
        bool gated_ok = false;
        bool free_ok = false;
        for (auto const& a : approvals) {
            if (a.call_id == "c1")
                gated_ok = a.tool_name == "gated_tool" && a.arguments_json == R"({"value":1})" && a.needs_approval;
            if (a.call_id == "c2")
                free_ok = a.tool_name == "free_tool" && a.arguments_json == R"({"value":2})" && !a.needs_approval;
        }
        check(gated_ok, "AP2: the gated call's event says needs_approval == true");
        check(free_ok, "AP2: the ungated call's event says needs_approval == false");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
