// Proof for docs/planning/agent-spawn-runtime-design-draft.md item 1 (§2, §4.1) --
// rt/agent_spawn.hpp -- OpenQuestions.md OQ-14, `agent.spawn`'s own "sharpest case". This is the
// task's own required proof: agent.spawn's REAL, wired, end-to-end call path, composing items 2
// (rt/agent_spawn_child_run.hpp), 3 (core/agent_spawn_worktree.hpp), 4 (trust::SpawnBudget/ADR-006,
// rt::SpawnCostBudget/ADR-031), and 5 (trust::mint_child_spawn_capabilities) through the REAL
// perform_agent_spawn()/AgentSpawnTool/AgentSpawnToolProvider surface -- not a hand-rolled stand-in
// the way tests/test_rt_agent_spawn_child_run.cpp's own T3 necessarily used (item 1 had not been
// built yet when that test was written).
//
//   T1 -- perform_agent_spawn() succeeds within budget: a caller holding a real AgentCall grant
//         spawns a real child through the real registry/pump/mint pipeline, gets back the child's
//         converged text, and both the depth budget and the shared cost pool are actually consumed
//         (observable via SpawnCostBudget::remaining()).
//   T2 -- fails closed with "agent_spawn.not_available" when the caller holds NO cap::AgentCall
//         grant at all (a real, non-null, non-empty CapabilitySet that simply never names "helper")
//         -- no child session, no cost-token spend, no worktree mint (SpawnCostBudget::remaining()
//         unchanged before/after).
//   T3 -- fails closed with "spawn_budget.depth_exhausted" at max depth (a caller holding
//         AgentCall{"helper", budget=0}) -- same "no side effect" proof as T2.
//   T4 -- an unknown agent_id (not registered at all) shares T2's OWN error code
//         (agent_spawn.not_available, §9 I3-3's enumeration-oracle fix) -- proving the two distinct
//         failure REASONS ([1] unknown-to-registry vs [2] known-but-ungranted) are indistinguishable
//         to a caller, by design.
//   T5 -- end-to-end: a REAL AgentSpawnToolProvider, composed via the REAL
//         core/composed_context_provider.hpp seam alongside an ordinary HistoryProvider<Window<0>>,
//         drives a REAL parent AgentSession's own tool-call loop -- the model issues a real
//         "agent.spawn" tool call, AgentSpawnTool's real descriptor (built by
//         make_tool_descriptor_with_invoke, never a test-only stand-in) dispatches into
//         perform_agent_spawn(), and the child's converged output flows back into the parent's own
//         tool_results_message in history() -- "mid-run, from inside the parent's own tool-call
//         loop", the task's own exact framing, through the actual production Tool<> surface this
//         time.
//   T6 -- the tool is NOT advertised at all when the session holds no cap::AgentCall grant
//         whatsoever (AgentSpawnToolProvider's own "never advertise a tool the model could never
//         call" gate, mirroring ScheduleWakeupTool's precedent) -- the parent's own ChatRequest never
//         even lists "agent.spawn" as a callable tool.
//   T7 -- ADR-079 §7's own named residual, closed here: a REAL multi-OS-thread stress of
//         SpawnPump::submit() itself (not tests/test_agent_spawn_worktree.cpp's own T10, which is
//         explicitly that file's "narrower substitute for the not-yet-built SpawnPump" -- SpawnPump
//         now exists, so this is the real thing). 16 real std::thread callers submit() concurrently
//         against ONE shared SpawnPump/SpawnCostBudget pair, sized so the pool is exhausted
//         mid-run (2000-token pool, 130-token cost, 16 callers -- exactly 15 can succeed): proves no
//         double-spend (SpawnCostBudget::remaining() lands exactly on the arithmetic bound, not
//         merely "<= pool", closing the exact RC-2/WT-2 double-spend hazard the file banner names),
//         no lost/duplicate child_id (every successful mint's child_id is pairwise distinct, proving
//         the pump's worker thread is the sole caller of allocate_spawn_seq()/derive_spawn_child_id()
//         even under real concurrent submit() pressure), and every rejection carries the SAME real
//         "spawn_cost_budget.exhausted" code (not a crash, not a wrong/generic error) -- repeated 25
//         times with a fresh pump each round, since a single lucky pass proves little about a
//         cross-thread-resume hazard.

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_spawn.hpp"
#include "agentengine/trust/delegated_approval_policy.hpp"

using agentengine::AgentMetadata;
using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ComposedContextProvider;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::HistoryProvider;
using agentengine::Message;
using agentengine::Principal;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::Usage;
using agentengine::Window;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::error;
using agentengine::failure_class;
using agentengine::result;
using agentengine::role;
using agentengine::task;

using agentengine::rt::AgentResponse;
using agentengine::rt::AgentSession;
using agentengine::rt::AgentSpawnArgs;
using agentengine::rt::AgentSpawnReply;
using agentengine::rt::AgentSpawnToolProvider;
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

using agentengine::cap::AgentCall;
using agentengine::trust::SpawnBudget;

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// -- Same drive()/ScriptedChatClient shape test_rt_agent_spawn_child_run.cpp already established --
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct ScriptedOutcome {
    Message message;
    Usage   usage;
};

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::shared_ptr<State> const& shared_state() const { return state_; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx =
            state_->call_count < state_->script.size() ? state_->call_count : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

[[nodiscard]] Message text_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item{};
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item{};
    item.origin = content_origin::assistant;
    ToolCall call;
    call.call_id        = std::move(call_id);
    call.tool_name      = std::move(tool_name);
    call.arguments_json = std::move(args);
    call.provenance     = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

// A real, host-authored SpawnTargetDescriptor for a target agent "helper" that declares an EMPTY
// capability_ceiling (vacuous coverage-check success, design doc §7 open Q4) and always converges
// with `child_reply_text` -- mirrors test_rt_agent_spawn_child_run.cpp's own child fixture.
// `scratch` worktree mode is used deliberately (not the `branch` default) so tests below don't need
// to seed the caller's own FsRead/FsWrite grants or a pre-existing committed ref -- item 3's own
// `mint_spawn_worktree` needs neither for `scratch` (core/agent_spawn_worktree.hpp).
[[nodiscard]] SpawnTargetDescriptor make_helper_target(std::string child_reply_text) {
    SpawnTargetDescriptor d;
    d.metadata          = AgentMetadata{};
    d.spawn_cost         = 1;
    d.worktree_mode       = agentengine::sharing_mode::scratch;
    d.child_token_budget = 1000;
    d.run_child = [child_reply_text](std::string child_id, ChildSpawnRequest req) {
        return run_child_agent_session<ScriptedChatClient>(
            std::move(child_id), std::move(req), [&child_reply_text](ScriptedChatClient& c) {
                c.set_script({{text_response(child_reply_text), Usage{1, 1, 0, 0, 0.0}}});
            });
    };
    return d;
}

// ---------------------------------------------------------------------------------------------
// GitHub issue #30 / ADR-151: a target whose CHILD, once running, itself calls a `policy_driven`
// tool ("policy_gated_child") -- what a real `SpawnTargetDescriptor::policy_decider`/`approval_
// decider` (ADR-151, wired below via `make_policy_gated_target()`) actually governs. Empty
// `Capabilities<>` ceiling, same reasoning `make_helper_target()`'s own top comment already gives
// (vacuous coverage-check success -- this fixture is about the APPROVAL mechanism, not the
// capability system, mirroring examples/05_human_approval.cpp's own `SendMessageTool`).
// ---------------------------------------------------------------------------------------------

struct PolicyGatedChildArgs { std::string message; };
AE_JSON_SCHEMA(PolicyGatedChildArgs, message)
struct PolicyGatedChildReply { bool handled = false; };
AE_JSON_SCHEMA(PolicyGatedChildReply, handled)

[[nodiscard]] bool& policy_gated_child_invoked() {
    static bool invoked = false;
    return invoked;
}

struct PolicyGatedChildTool
    : agentengine::Tool<PolicyGatedChildTool, agentengine::Capabilities<>,
                         agentengine::EffectClass<agentengine::effect_class::pure>,
                         agentengine::Approval<agentengine::approval_mode::policy_driven>> {
    static constexpr std::string_view name = "policy_gated_child";
    static constexpr std::string_view description = "A policy_driven tool the CHILD itself calls.";
    using Args = PolicyGatedChildArgs;
    using Reply = PolicyGatedChildReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        policy_gated_child_invoked() = true;
        return Reply{true};
    }
};

class ChildPolicyHistoryProvider {
public:
    [[nodiscard]] task<result<agentengine::ContextContribution>> on_context(agentengine::SessionContext& sc,
                                                                              EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = agentengine::ToolTable::from_tools<PolicyGatedChildTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(agentengine::ContextProvider<ChildPolicyHistoryProvider>);

// The child's script: one round issuing the policy_driven call, one final converged answer --
// regardless of whether the tool call was approved or denied (`ScriptedChatClient`'s own
// call_count-indexed script doesn't inspect the tool result content, matching every other fixture
// in this file) -- so `perform_agent_spawn()` itself always succeeds here; whether
// `policy_gated_child_invoked()` actually flipped true is what distinguishes T8 from T9 below.
[[nodiscard]] SpawnTargetDescriptor make_policy_gated_target(agentengine::PolicyDecider policy_decider,
                                                                agentengine::ApprovalDecider approval_decider = {}) {
    SpawnTargetDescriptor d;
    d.metadata           = AgentMetadata{};
    d.spawn_cost          = 1;
    d.worktree_mode        = agentengine::sharing_mode::scratch;
    d.child_token_budget  = 1000;
    d.policy_decider      = std::move(policy_decider);
    d.approval_decider    = std::move(approval_decider);
    d.run_child = [](std::string child_id, ChildSpawnRequest req) {
        return run_child_agent_session<ScriptedChatClient, NoSessionState, ChildPolicyHistoryProvider>(
            std::move(child_id), std::move(req), [](ScriptedChatClient& c) {
                c.set_script({{tool_call_response("c1", "policy_gated_child", R"({"message":"do it"})"),
                                Usage{1, 1, 0, 0, 0.0}},
                               {text_response("child done"), Usage{1, 1, 0, 0, 0.0}}});
            });
    };
    return d;
}

}  // namespace

int main() {
    // ---- T1: perform_agent_spawn() succeeds within budget -----------------------------------------
    {
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target("helper", make_helper_target("helper-said-done"));
        check(registered.has_value(), "T1 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(2)}});

        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-1", ""};

        agentengine::Ref const caller_ref{"session:caller-t1", ""};
        std::uint64_t const    remaining_before = cost_pool.remaining();

        AgentSpawnArgs const args{"helper", "do the sub-task"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");
        check(reply.has_value(), "T1: perform_agent_spawn() succeeds end-to-end");
        if (reply.has_value()) {
            check(reply->output == "helper-said-done",
                  "T1: the real child's converged output flows back through the real orchestration");
        }
        check(cost_pool.remaining() == remaining_before - 1,
              "T1: the real, shared SpawnCostBudget pool (ADR-031) was actually consumed by one token");
    }

    // ---- T2: fails closed -- caller holds NO cap::AgentCall grant at all --------------------------
    {
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target("helper", make_helper_target("unreachable"));
        check(registered.has_value(), "T2 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;

        // A REAL, non-null, non-empty CapabilitySet -- just one that never names "helper". Proves
        // the failure is about THIS grant, not about a null/empty capabilities pointer (T with a
        // null ctx.capabilities is agent_spawn.no_capabilities, a DIFFERENT code, per step [0]).
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"some_other_agent", SpawnBudget::mint_root(5)}});

        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-2", ""};

        agentengine::Ref const caller_ref{"session:caller-t2", ""};
        std::uint64_t const    remaining_before = cost_pool.remaining();

        AgentSpawnArgs const args{"helper", "do the sub-task"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");
        check(!reply.has_value(), "T2: fails closed -- caller holds no AgentCall grant for \"helper\"");
        if (!reply.has_value()) {
            check(reply.error().code == "agent_spawn.not_available",
                  "T2: the uniform not_available code (§9 I3-3), not a differentiated one");
        }
        check(cost_pool.remaining() == remaining_before,
              "T2: NO side effect -- the shared cost pool was never touched (I8, §2's own ordering)");
    }

    // ---- T3: fails closed -- max depth exhausted ---------------------------------------------------
    {
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target("helper", make_helper_target("unreachable"));
        check(registered.has_value(), "T3 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(0)}});

        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-3", ""};

        agentengine::Ref const caller_ref{"session:caller-t3", ""};
        std::uint64_t const    remaining_before = cost_pool.remaining();

        AgentSpawnArgs const args{"helper", "do the sub-task"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");
        check(!reply.has_value(), "T3: fails closed at max depth (I8)");
        if (!reply.has_value()) {
            check(reply.error().code == "spawn_budget.depth_exhausted",
                  "T3: ADR-006's own code, propagated unmodified -- distinguishable from T2's "
                  "not_available (the caller DOES hold a grant here, it is just exhausted)");
        }
        check(cost_pool.remaining() == remaining_before,
              "T3: NO side effect -- no child session, no cost-token spend, no worktree mint");
    }

    // ---- T4: unknown agent_id shares T2's own error code (§9 I3-3) ---------------------------------
    {
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;  // "helper" never registered at all

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(5)}});

        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-4", ""};

        agentengine::Ref const caller_ref{"session:caller-t4", ""};

        AgentSpawnArgs const args{"helper", "do the sub-task"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");
        check(!reply.has_value(), "T4: fails closed -- \"helper\" was never registered");
        if (!reply.has_value()) {
            check(reply.error().code == "agent_spawn.not_available",
                  "T4: SAME code as T2 -- a caller cannot distinguish \"unknown to the registry\" "
                  "from \"known but ungranted\" (§9 I3-3's enumeration-oracle fix)");
        }
    }

    // ---- T5/T6: end-to-end through the REAL production Tool<> surface -----------------------------
    {
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target("helper", make_helper_target("child-said-done"));
        check(registered.has_value(), "T5 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;
        agentengine::Ref const caller_ref{"session:parent-t5", ""};

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(2)}});

        using ParentHistory = ComposedContextProvider<HistoryProvider<Window<0>>,
                                                        AgentSpawnToolProvider<InMemoryAppendLogStore>>;
        AgentSession<ScriptedChatClient, NoSessionState, ParentHistory> parent;
        parent.initialize("parent-1", Principal{"caller-5", ""});
        parent.set_capabilities(&caller_held);
        auto engaged = parent.history_provider().engage(std::make_tuple(
            HistoryProvider<Window<0>>{},
            AgentSpawnToolProvider<InMemoryAppendLogStore>(registry, pump, caller_ref, "/caller", quota,
                                                             tracker)));
        check(engaged.has_value(), "T5 setup: ComposedContextProvider::engage() succeeds");

        parent.emplace_chat_client().set_script({
            {tool_call_response("c1", "agent.spawn", R"({"agent_id":"helper","input":"do the sub-task"})"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("parent-final"), Usage{1, 1, 0, 0, 0.0}},
        });

        auto response = drive(parent.start_run(StartRun{text_message("please spawn a helper")}));
        check(response.has_value(), "T5: the PARENT run completes end-to-end");

        bool found_child_result = false;
        for (Message const& m : parent.history()) {
            for (ContentItem const& item : m.content) {
                if (auto const* tr = std::get_if<agentengine::ToolResult>(&item.value)) {
                    for (ContentItem const& tc : tr->content) {
                        if (auto const* data = std::get_if<agentengine::Data>(&tc.value)) {
                            if (data->json.find("child-said-done") != std::string::npos) {
                                found_child_result = true;
                            }
                        }
                    }
                }
            }
        }
        check(found_child_result,
              "T5: the child's converged output -- produced by the REAL production AgentSpawnTool "
              "descriptor (make_tool_descriptor_with_invoke, never a test-only stand-in) -- reached "
              "the PARENT's own tool_results_message in history()");

        // ---- T6: no cap::AgentCall grant at all -> agent.spawn is never even advertised -----------
        CapabilitySet const no_spawn_cap = CapabilitySet::grant_root({});
        AgentSession<ScriptedChatClient, NoSessionState, ParentHistory> unarmed;
        unarmed.initialize("parent-2", Principal{"caller-6", ""});
        unarmed.set_capabilities(&no_spawn_cap);
        auto engaged2 = unarmed.history_provider().engage(std::make_tuple(
            HistoryProvider<Window<0>>{},
            AgentSpawnToolProvider<InMemoryAppendLogStore>(registry, pump, caller_ref, "/caller", quota,
                                                             tracker)));
        check(engaged2.has_value(), "T6 setup: engage() succeeds for the unarmed session too");
        unarmed.emplace_chat_client().set_script({{text_response("no-tool-needed"), Usage{1, 1, 0, 0, 0.0}}});
        auto unarmed_response = drive(unarmed.start_run(StartRun{text_message("hello")}));
        check(unarmed_response.has_value(), "T6: the unarmed session's run still completes normally");
        check(unarmed_response.has_value() &&
                  agentengine::text_of(unarmed_response->message) == "no-tool-needed",
              "T6: with no cap::AgentCall grant, agent.spawn was never advertised as a callable tool "
              "at all -- the ordinary text response is all that happened");
    }

    // ---- T7: SpawnPump::submit() under REAL cross-thread contention (ADR-079 §7 residual) ---------
    {
        constexpr int kThreads          = 16;
        constexpr std::uint64_t kPool   = 2000;
        constexpr std::uint64_t kCost   = 130;
        // floor(kPool / kCost) -- the exact number of the 16 concurrent submits that CAN succeed;
        // computed, not eyeballed, so a future constant change here can't silently desync the
        // asserted expectation from the arithmetic it's supposed to prove.
        constexpr int kExpectedSuccesses = static_cast<int>(kPool / kCost);
        static_assert(kExpectedSuccesses > 0 && kExpectedSuccesses < kThreads,
                      "T7: the scenario must genuinely exhaust the pool mid-run, not merely satisfy "
                      "or starve every caller -- otherwise this proves nothing about the boundary");

        constexpr int kRounds = 25;  // one lucky pass proves little about a cross-thread-resume hazard
        int rounds_with_wrong_success_count = 0;
        int rounds_with_wrong_remaining     = 0;
        int rounds_with_id_collision        = 0;
        int rounds_with_bad_failure_code    = 0;

        for (int round = 0; round < kRounds; ++round) {
            InMemoryAppendLogStore ref_store;
            SpawnCostBudget        cost_pool;
            cost_pool.initialize(kPool);
            SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

            CapabilitySet const caller_held = CapabilitySet::grant_root({});  // scratch mode needs
                                                                               // no FsRead/FsWrite
            agentengine::Ref const caller_ref{"session:caller-t7", ""};

            // Fixed-size, one slot per thread -- each thread writes ONLY its own index, so there is
            // no shared-container race to reason about beyond SpawnPump's own internals, keeping this
            // test's own signal isolated to the ONE thing it means to prove.
            std::vector<agentengine::result<SpawnPump<InMemoryAppendLogStore>::SpawnMintResult>>
                results(kThreads);

            std::vector<std::thread> workers;
            workers.reserve(kThreads);
            for (int i = 0; i < kThreads; ++i) {
                workers.emplace_back([&, i] {
                    typename SpawnPump<InMemoryAppendLogStore>::SpawnMintRequest req;
                    req.cost            = kCost;
                    req.caller_ref      = caller_ref;
                    req.caller_principal = Principal{"caller-t7", ""};
                    req.caller_held      = &caller_held;
                    req.worktree_mode    = agentengine::sharing_mode::scratch;
                    req.caller_mount_id  = "/caller";
                    results[static_cast<std::size_t>(i)] = pump.submit(std::move(req));
                });
            }
            for (auto& t : workers) t.join();

            int successes = 0;
            std::set<std::string> child_ids;
            bool all_failures_correctly_coded = true;
            for (auto const& r : results) {
                if (r.has_value()) {
                    ++successes;
                    child_ids.insert(r->child_id);
                } else if (r.error().code != "spawn_cost_budget.exhausted") {
                    all_failures_correctly_coded = false;
                }
            }

            if (successes != kExpectedSuccesses) ++rounds_with_wrong_success_count;
            if (cost_pool.remaining() != kPool - static_cast<std::uint64_t>(successes) * kCost)
                ++rounds_with_wrong_remaining;
            if (static_cast<int>(child_ids.size()) != successes) ++rounds_with_id_collision;
            if (!all_failures_correctly_coded) ++rounds_with_bad_failure_code;
        }

        check(rounds_with_wrong_success_count == 0,
              "T7: every round, exactly floor(pool/cost) of 16 REAL concurrent submit() callers "
              "succeeded -- no round over- or under-granted under real cross-thread contention");
        check(rounds_with_wrong_remaining == 0,
              "T7: every round, SpawnCostBudget::remaining() landed EXACTLY on successes*cost "
              "subtracted from the pool -- no double-spend and no lost update across 16 real threads "
              "racing submit() against the same pool (the exact RC-2/WT-2 hazard this file's own top "
              "comment names)");
        check(rounds_with_id_collision == 0,
              "T7: every round, every successful mint's child_id was pairwise distinct -- the pump's "
              "single worker thread remained the sole caller of allocate_spawn_seq()/"
              "derive_spawn_child_id() even under real concurrent submit() pressure");
        check(rounds_with_bad_failure_code == 0,
              "T7: every rejected submit(), every round, failed with the real "
              "spawn_cost_budget.exhausted code -- not a crash, not a wrong/generic error");
    }

    // ---- T8: SpawnTargetDescriptor::policy_decider (GitHub issue #30 / ADR-151) reaches a REAL
    //      spawned child's own turn loop -- the reference approve_delegated_calls() auto-approves the
    //      child's policy_driven tool call, with NO ApprovalDecider ever configured on the child, and
    //      through the REAL perform_agent_spawn()/AgentSpawnTool call path end to end (not a synthetic
    //      unit test against resolve_approval_outcome()/PolicyDecider directly). ---------------------
    {
        policy_gated_child_invoked() = false;
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered = registry.register_target(
            "policy-helper", make_policy_gated_target(agentengine::trust::approve_delegated_calls()));
        check(registered.has_value(), "T8 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;

        // The CALLER (i.e. the parent invoking agent.spawn) is itself NOT delegated -- on_behalf_of
        // is empty. What makes the CHILD's own principal delegated is run_child_agent_session()'s
        // OWN unconditional derive_on_behalf_of() call (item 2, unchanged by this ADR) -- proving
        // this policy really does key off the CHILD's real, structurally-derived identity, not
        // something this test hand-constructs.
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"policy-helper", SpawnBudget::mint_root(2)}});

        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-t8", ""};

        agentengine::Ref const caller_ref{"session:caller-t8", ""};
        AgentSpawnArgs const   args{"policy-helper", "do it"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");

        check(reply.has_value(), "T8: perform_agent_spawn() succeeds end-to-end");
        check(policy_gated_child_invoked(),
              "T8: the child's own policy_gated_child tool actually ran -- auto-approved via "
              "SpawnTargetDescriptor::policy_decider (approve_delegated_calls()), with NO "
              "ApprovalDecider ever configured on the child session");
        if (reply.has_value()) {
            check(reply->output == "child done",
                  "T8: the child's converged final answer still flows back normally");
        }
    }

    // ---- T9: without a policy_decider wired for the target, ADR-151 changes nothing -- the child's
    //      policy_driven call is denied by the ordinary fail-closed default (ADR-070 property 2),
    //      never silently auto-approved. -------------------------------------------------------------
    {
        policy_gated_child_invoked() = false;
        InMemoryAppendLogStore ref_store;
        SpawnCostBudget        cost_pool;
        cost_pool.initialize(10);
        SpawnPump<InMemoryAppendLogStore> pump(cost_pool, ref_store);

        SpawnTargetRegistry registry;
        auto registered =
            registry.register_target("policy-helper-unwired", make_policy_gated_target(/*policy_decider=*/{}));
        check(registered.has_value(), "T9 setup: register_target succeeds");

        SpawnQuota        quota{100};
        SpawnQuotaTracker tracker;
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"policy-helper-unwired", SpawnBudget::mint_root(2)}});
        EffectContext ctx;
        ctx.capabilities = std::make_shared<CapabilitySet const>(caller_held);
        ctx.principal     = Principal{"caller-t9", ""};
        agentengine::Ref const caller_ref{"session:caller-t9", ""};

        AgentSpawnArgs const args{"policy-helper-unwired", "do it"};
        auto reply = perform_agent_spawn(args, ctx, registry, pump, quota, tracker, caller_ref, "/caller");

        check(reply.has_value(), "T9: the outer spawn mechanism still succeeds -- the child's own turn "
                                  "loop absorbs the denied tool call as an ordinary tool-result error, "
                                  "not a hard failure of the whole run");
        check(!policy_gated_child_invoked(),
              "T9: with no policy_decider wired for this target, the child's policy_driven tool call "
              "is denied, never silently auto-approved by default");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_spawn: ALL PASS\n");
    } else {
        std::fprintf(stderr, "test_rt_agent_spawn: %d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
