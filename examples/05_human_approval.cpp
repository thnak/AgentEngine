// AgentEngine "get started" examples, 5 -- pausing a run for a real human approval.
//
// Roughly mirrors Microsoft Agent Framework's samples/03-workflows/HumanInTheLoop, but this is not
// a workflow-level pattern in AgentEngine -- it lives directly on `AgentSession` (ADR-029). A tool
// declared `Approval<approval_mode::always_require>` cannot just be denied and reported back to the
// model as an ordinary tool error; when `set_suspend_for_approval(true)` is set and no synchronous
// `approval_decider_` is configured, the whole `StartRun` ask genuinely SUSPENDS -- it never
// resolves -- and a real `Interaction` opens instead. A separate `ResolveInteraction` ask, sent
// later (in a real deployment: after a human actually looked at it), resumes the SAME run, not a
// new one -- 001's attributability invariant (I4) means the resumed call is still attributed to the
// run that first asked for it.
//
// This is deterministic and offline like the earlier examples -- no real human is waited on here,
// `main()` just plays both sides (the suspend, then the resolution) to show the mechanism.
//
// Run: ./agentengine_example_05_human_approval

#include <cstdio>
#include <memory_resource>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

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

// ---- The tool: sending a message is exactly the kind of effect a human should sign off on -------

struct SendArgs { std::string recipient; std::string body; };
AE_JSON_SCHEMA(SendArgs, recipient, body)
struct SendReply { bool sent = false; };
AE_JSON_SCHEMA(SendReply, sent)

[[nodiscard]] bool& send_tool_invoked() {
    static bool invoked = false;
    return invoked;
}

// Empty capability ceiling, on purpose -- this example is about the APPROVAL mechanism, not the
// capability system (see 06_capabilities_and_denial.cpp for that one). `always_require` is what
// needs a real human answer once `suspend_for_approval_` is on.
struct SendMessageTool
    : Tool<SendMessageTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "send_message";
    static constexpr std::string_view description = "Sends a message to a recipient.";
    using Args = SendArgs;
    using Reply = SendReply;
    static result<Reply> invoke(Args, EffectContext&) {
        send_tool_invoked() = true;
        return Reply{true};
    }
};

class SendHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<SendMessageTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<SendHistoryProvider>);

class ScriptedSendChatClient {
public:
    std::size_t call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply;
        reply.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        if (call_count == 0) {
            reply.message_id = "m-call";
            item.value = ToolCall{"c1", "send_message",
                                   R"({"recipient":"team@example.com","body":"Ship it."})",
                                   content_origin::assistant, call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-answer";
            item.value = Text{"Message sent."};
        }
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }  // unused
};
static_assert(ChatClient<ScriptedSendChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

using SendAgent = AgentSession<ScriptedSendChatClient, NoSessionState, SendHistoryProvider>;

}  // namespace

int main() {
    quark::TestKit<SendAgent> kit;
    kit.actor().initialize("s-approval", Principal{"p-demo", ""});
    CapabilitySet const held = CapabilitySet::grant_root({});
    kit.actor().set_capabilities(&held);
    kit.actor().set_suspend_for_approval(true);  // ADR-029: no decider configured -> genuinely suspend

    auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());
    auto r1 = kit.ask<AgentResponse>(StartRun{user_message("Message the team that we're shipping.")});
    check(!r1.has_value(),
          "the StartRun ask never resolves while the tool call awaits approval -- fail-closed, not "
          "a hang, exactly like every other unresolved branch in AgentSession's turn loop");
    check(!send_tool_invoked(), "send_message was NOT called -- suspension happens before invocation");
    check(kit.actor().has_open_interactions(), "a real Interaction opened for this suspension");

    bool saw_approval_requested = false;
    while (auto ev = viewer.next()) {
        if (ev->kind == run_event_kind::approval_requested) saw_approval_requested = true;
    }
    check(saw_approval_requested, "an approval_requested event fired for the pending call");
    std::printf("[suspended] waiting for human approval to send: \"Ship it.\"\n");

    // In a real deployment this ResolveInteraction ask arrives later, from whatever surface a human
    // actually approves things through (a CLI prompt, a web console) -- here it's just the next line.
    std::string const interaction_id = kit.actor().open_interactions().front().interaction_id;
    auto r2 = kit.ask<AgentResponse>(ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt});
    check(r2.has_value(), "approving resumes the SAME run and it converges to a final answer");
    check(send_tool_invoked(), "send_message's invoke() ran for real after approval, through the "
                                "ordinary capability-checked tool pipeline");
    check(!kit.actor().has_open_interactions(), "the interaction closed once resolved");
    if (r2.has_value()) std::printf("%s\n", text_of(r2->message).c_str());

    std::fprintf(stderr,
                 g_failures == 0 ? "example_05_human_approval: OK\n" : "example_05_human_approval: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
