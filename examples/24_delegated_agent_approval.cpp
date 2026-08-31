// AgentEngine "get started" examples, 19 -- a reference PolicyDecider for delegated/spawned-agent
// calls (GitHub issue #30, decisions/ADR-151-delegated-agent-approval-policy.md).
//
// Not part of MAF's own sample progression -- closes a real, previously-named gap: `Principal`
// already carries `on_behalf_of`/`delegation_depth` (007 §2/018 §2), so a PolicyDecider auto-
// approving a delegated caller's `policy_driven` tool calls was fully EXPRESSIBLE with zero engine
// changes, but nothing in this codebase demonstrated it. `agentengine::trust::approve_delegated_
// calls()` (trust/delegated_approval_policy.hpp) is that reference implementation. Per ADR-070 §3
// this is NEVER an engine default -- adopting it is a real trust decision a host opts into,
// explicitly, at one or both of the two distinct wiring points this example shows SIDE BY SIDE,
// deliberately contrasted so a reader doesn't conflate them:
//
//   (A) AgentSession::set_policy_decider() -- governs the HOST's own TOP-LEVEL session. The
//       top-level Principal here is a real human/service identity (`on_behalf_of` empty) -- this
//       policy does NOT recognize it as delegated, so its own policy_driven tool call still falls
//       through to a genuine human-approval suspend (ADR-029), exactly like example 05. Wiring this
//       policy changes NOTHING for a non-delegated caller.
//
//   (B) SpawnTargetDescriptor::policy_decider (rt/agent_spawn.hpp, ADR-151) -- governs a SPECIFIC
//       agent.spawn TARGET's own spawned children. The child's Principal is a REAL, structurally
//       derived delegation (`derive_on_behalf_of()`, run_child_agent_session()'s own unconditional
//       call, unchanged by this ADR) -- this policy DOES recognize it, so the child's own
//       policy_driven tool call auto-approves with no human involved at all.
//
// This is deterministic and offline like every other example here -- no real human is waited on;
// main() plays both sides of (A)'s suspend, then drives (B) through the real perform_agent_spawn()
// call path (the same production entry point AgentSpawnTool's real descriptor dispatches into).
//
// Run: ./agentengine_example_24_delegated_agent_approval

#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_spawn.hpp"
#include "agentengine/trust/delegated_approval_policy.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::AgentSpawnArgs;
using agentengine::rt::ChildSpawnRequest;
using agentengine::rt::InMemoryAppendLogStore;
using agentengine::rt::NoSessionState;
using agentengine::rt::SpawnCostBudget;
using agentengine::rt::SpawnPump;
using agentengine::rt::SpawnQuota;
using agentengine::rt::SpawnQuotaTracker;
using agentengine::rt::SpawnTargetDescriptor;
using agentengine::rt::SpawnTargetRegistry;
using agentengine::rt::StartRun;
using agentengine::rt::perform_agent_spawn;
using agentengine::rt::run_child_agent_session;

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

// ---- A policy_driven tool, reused for both (A) and (B) -- the mechanism doesn't care which side
//      of the call graph it's declared on. ------------------------------------------------------

struct ReviewedActionArgs { std::string summary; };
AE_JSON_SCHEMA(ReviewedActionArgs, summary)
struct ReviewedActionReply { bool done = false; };
AE_JSON_SCHEMA(ReviewedActionReply, done)

[[nodiscard]] bool& action_invoked() {
    static bool invoked = false;
    return invoked;
}

// Empty capability ceiling -- this example is about the APPROVAL mechanism, not the capability
// system (matches example 05's own SendMessageTool).
struct ReviewedActionTool
    : Tool<ReviewedActionTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::policy_driven>> {
    static constexpr std::string_view name = "reviewed_action";
    static constexpr std::string_view description = "An action gated by PolicyDecider.";
    using Args = ReviewedActionArgs;
    using Reply = ReviewedActionReply;
    static result<Reply> invoke(Args, EffectContext&) {
        action_invoked() = true;
        return Reply{true};
    }
};

class ActionHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<ReviewedActionTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<ActionHistoryProvider>);

class ScriptedActionChatClient {
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
            item.value = ToolCall{"c1", "reviewed_action", R"({"summary":"do the reviewed thing"})",
                                   content_origin::assistant, call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-answer";
            item.value = Text{"Done."};
        }
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) { return {}; }
};
static_assert(ChatClient<ScriptedActionChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

using ActionAgent = AgentSession<ScriptedActionChatClient, NoSessionState, ActionHistoryProvider>;

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    PolicyDecider const delegated_policy = trust::approve_delegated_calls();

    // ==== (A) Top-level session: NOT delegated -- this policy changes nothing =======================
    {
        ActionAgent session;
        // A real human/service identity: on_behalf_of is empty -- derive_on_behalf_of() (018 §2) is
        // never called for a genuine top-level principal, only for a delegated/spawned one.
        session.initialize("s-top-level", Principal{"p-human-operator", ""});
        session.emplace_chat_client();
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_policy_decider(delegated_policy);       // (A) wired HERE, at the session level
        session.set_suspend_for_approval(true);              // no ApprovalDecider -> genuine suspend

        auto r1 = drive(session.start_run(StartRun{user_message("Do the reviewed thing.")}));
        check(!r1.has_value(),
              "(A) a non-delegated caller's policy_driven call is NOT auto-approved -- the run "
              "genuinely suspends for a real human, exactly as if approve_delegated_calls() had "
              "never been wired");
        check(!action_invoked(), "(A) reviewed_action was NOT called -- suspension precedes invocation");
        check(session.has_open_interactions(), "(A) a real Interaction opened, same mechanism as example 05");

        std::string const interaction_id = session.open_interactions().front().interaction_id;
        auto r2 = drive(session.resolve_interaction(
            agentengine::rt::ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt}));
        check(r2.has_value(), "(A) a real human approving resumes the run to completion");
        check(action_invoked(), "(A) reviewed_action ran only after the real human said yes");
        std::printf("(A) top-level call: suspended for a human, then approved -- \"%s\"\n",
                     r2.has_value() ? text_of(r2->message).c_str() : "");
    }

    // ==== (B) A spawned child of a real agent.spawn target: delegated -- auto-approves ===============
    {
        action_invoked() = false;

        SpawnTargetDescriptor target;
        target.metadata          = AgentMetadata{};
        target.spawn_cost         = 1;
        target.worktree_mode       = agentengine::sharing_mode::scratch;
        target.child_token_budget = 1000;
        target.policy_decider     = delegated_policy;  // (B) wired HERE, per spawn TARGET
        // Deliberately no `target.approval_decider` -- see delegated_approval_policy.hpp's own
        // "spawned-child residual" comment: this policy's require_approval fallback would DENY, not
        // defer, for a caller this policy doesn't recognize as delegated. Not exercised in this
        // example (every call the child makes here IS delegated), but real for a host to plan around.
        target.run_child = [](std::string child_id, ChildSpawnRequest req) {
            return run_child_agent_session<ScriptedActionChatClient, NoSessionState, ActionHistoryProvider>(
                std::move(child_id), std::move(req), [](ScriptedActionChatClient&) {});
        };

        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target("reviewer-child", std::move(target));
        check(registered.has_value(), "(B) setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;
        // The PARENT's own principal is itself NOT delegated -- what makes the CHILD delegated is
        // run_child_agent_session()'s own unconditional derive_on_behalf_of() call, unchanged here.
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({cap::AgentCall{"reviewer-child", trust::SpawnBudget::mint_root(2)}});
        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"p-human-operator", ""};

        Ref const caller_ref{"session:example-19", ""};
        AgentSpawnArgs const args{"reviewer-child", "review and act"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");

        check(reply.has_value(), "(B) perform_agent_spawn() succeeds");
        check(action_invoked(),
              "(B) the SPAWNED CHILD's own reviewed_action auto-approved via SpawnTargetDescriptor::"
              "policy_decider (approve_delegated_calls()) -- no human, no ApprovalDecider, because "
              "the child's own Principal is genuinely delegated");
        std::printf("(B) spawned-child call: auto-approved via delegation, no human involved\n");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_24_delegated_agent_approval: OK\n"
                                          : "example_24_delegated_agent_approval: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
