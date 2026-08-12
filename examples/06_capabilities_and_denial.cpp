// AgentEngine "get started" examples, 6 -- no ambient authority (I2).
//
// This one has no direct MAF equivalent -- it demonstrates an AgentEngine invariant that shapes
// every tool call in this engine: a tool call runs only against capabilities the SESSION was
// explicitly granted, never because the tool merely declared it needs them (007-Capability-and-
// Trust-Model.md, invariant I2). `WriteNoteTool` below declares `Capabilities<cap::decl::FsWrite<
// "work">>` -- its ceiling -- but declaring a ceiling is not a grant. What actually authorizes a
// given call is the `CapabilitySet` `set_capabilities()` installed on the session, checked fresh on
// every call.
//
// The same tool, the same scripted request, run against two sessions that differ only in what was
// granted: one with an EMPTY CapabilitySet (nothing granted at all) and one with `FsWrite{"work"}`
// actually granted. The first denies the call -- as an ordinary tool error fed back to the model,
// not a run-level failure -- and the tool's `invoke()` never runs; the second lets it run for real.
//
// Run: ./agentengine_example_06_capabilities_and_denial

#include <cstdio>
#include <memory_resource>
#include <string>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/trust/capability.hpp"
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

struct WriteArgs { std::string text; };
AE_JSON_SCHEMA(WriteArgs, text)
struct WriteReply { bool written = false; };
AE_JSON_SCHEMA(WriteReply, written)

[[nodiscard]] bool& write_tool_invoked() {
    static bool invoked = false;
    return invoked;
}

// Declares its ceiling -- FsWrite scoped to the "work" mount -- but that is a declaration, not a
// grant. `never_require` (the default, undeclared here) is the honest approval mode for it: the
// capability check IS the gate for this tool, not a human approval step (see 05_human_approval.cpp
// for a tool that needs the latter instead of, or in addition to, a capability).
struct WriteNoteTool
    : Tool<WriteNoteTool, Capabilities<cap::decl::FsWrite<"work">>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "write_note";
    static constexpr std::string_view description = "Writes a note into the work mount.";
    using Args = WriteArgs;
    using Reply = WriteReply;
    static result<Reply> invoke(Args, EffectContext&) {
        write_tool_invoked() = true;
        return Reply{true};
    }
};

class WriteHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<WriteNoteTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<WriteHistoryProvider>);

// Always asks for the same tool call, then answers with plain text once it's been resolved one way
// or the other (denied or actually run) -- what matters here is what CapabilitySet the session was
// given, not what the model asks for.
class ScriptedWriteChatClient {
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
            item.value = ToolCall{"c1", "write_note", R"({"text":"remember this"})",
                                   content_origin::assistant, call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-answer";
            item.value = Text{"done"};
        }
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        if (call_count == 0) {
            upd.delta.value = ToolCall{"c1", "write_note", R"({"text":"remember this"})",
                                        content_origin::assistant, call_provenance::vendor_structured};
        } else {
            upd.delta.value = Text{"done"};
        }
        upd.is_final = true;
        upd.usage    = Usage{1, 1, 0, 0, 0.0};
        auto pushed  = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        ++call_count;
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<ScriptedWriteChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

using WriteAgent = AgentSession<ScriptedWriteChatClient, NoSessionState, WriteHistoryProvider>;

}  // namespace

int main() {
    // ---- Session 1: nothing granted -- I2 denies the call, the run still converges --------------
    {
        write_tool_invoked() = false;

        quark::TestKit<WriteAgent> kit;
        kit.actor().initialize("s-no-grant", Principal{"p-demo", ""});
        CapabilitySet const held = CapabilitySet::grant_root({});  // deliberately empty
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<AgentResponse>(StartRun{user_message("Write a note for me.")});
        check(r.has_value(), "no grant: the run still converges -- the denial is fed back as an "
                              "ordinary tool error, not a run-level failure");
        check(!write_tool_invoked(),
              "no grant: write_note's invoke() never ran -- I2 denied the call before it got there");
        if (r.has_value()) std::printf("[no grant]    %s\n", text_of(r->message).c_str());
    }

    // ---- Session 2: FsWrite{"work"} actually granted -- the SAME tool call now runs --------------
    {
        write_tool_invoked() = false;

        quark::TestKit<WriteAgent> kit;
        kit.actor().initialize("s-granted", Principal{"p-demo", ""});
        CapabilitySet const held = CapabilitySet::grant_root(
            {Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<AgentResponse>(StartRun{user_message("Write a note for me.")});
        check(r.has_value(), "granted: the run converges");
        check(write_tool_invoked(),
              "granted: write_note's invoke() ran for real -- same tool, same call, only the "
              "session's granted CapabilitySet differed");
        if (r.has_value()) std::printf("[FsWrite work] %s\n", text_of(r->message).c_str());
    }

    std::fprintf(stderr, g_failures == 0 ? "example_06_capabilities_and_denial: OK\n"
                                          : "example_06_capabilities_and_denial: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
