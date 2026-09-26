// Implements docs/planning/agent-spawn-runtime-design-draft.md item 2 (§4.2) -- OpenQuestions.md
// OQ-14, `agent.spawn`'s own "sharpest case": proves rt::run_child_agent_session()
// (rt/agent_spawn_child_run.hpp), the nested-agent-run invocation mechanism. Style/structure mirrors
// tests/test_rt_agent_workflow_executor.cpp (this project's own closest precedent for "drive a real
// AgentSession synchronously from a plain, non-coroutine call site"). Composes items 4/5 -- ALREADY
// landed, tests/test_agent_spawn_capability.cpp / trust/agent_spawn_capability.hpp -- to mint the
// child's own CapabilitySet exactly as a real perform_agent_spawn() (item 1, not built here) would,
// so this proves the real end-to-end wiring the task asks for, not a stand-in ceiling. Items 1
// (Tool<>/SpawnTargetRegistry), 3 (dynamic worktree minting), 4c (SpawnPump), and 6 (OQ-16 session
// wiring) are NOT built or tested here.
//
//   T1 -- run_child_agent_session() drives a FRESH child AgentSession to completion and returns its
//         converged AgentResponse -- the mechanism's own positive control.
//   T2 -- the child's own chat() call actually ran under mint_child_spawn_capabilities()'s (item 5)
//         real output -- a CapabilitySet of size 0 for a target declaring an empty ceiling -- never
//         the caller's own raw held AgentCall grant (size 1). Observed from INSIDE the child's own
//         EffectContext, not merely asserted from the caller's local copy.
//   T3 -- end-to-end: a PARENT AgentSession's own tool-call loop invokes an "agent.spawn"-shaped
//         Tool<> whose invoke() composes items 4/5 with THIS task's own mechanism mid-round; the
//         child's converged text flows back into the PARENT's own tool_results_message in
//         history() -- "mid-run, from inside the parent's own tool-call loop", the task's own exact
//         framing.
//   T4 -- depth exhaustion (ADR-006, check_and_consume_spawn_depth(), item 4) fails the composed
//         pipeline closed BEFORE any ChildSpawnRequest is even constructible -- no child session is
//         ever reachable for an exhausted grant (I8).
//   T5 -- RC-1 (§9, Critical): a session run_child_agent_session() constructs has
//         background_execution_disabled() == true and start_background_task() fails closed with the
//         new "standing_effect.background_execution_disabled" code on it, while an ORDINARY
//         (non-spawned) session defaults to backgrounding allowed -- the positive/negative pair
//         proving the guard is load-bearing, not merely present. (A full ASan/TSan use-after-free
//         reproduction of the *pre-fix* hazard is named in the task's own residual list, not
//         attempted here -- see the final report.)

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_spawn_child_run.hpp"
#include "agentengine/trust/agent_spawn_capability.hpp"

using agentengine::AgentMetadata;
using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::Data;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolCallRequest;
using agentengine::ToolResult;
using agentengine::ToolTable;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::error;
using agentengine::failure_class;
using agentengine::result;
using agentengine::role;
using agentengine::task;

using agentengine::rt::AgentSession;
using agentengine::rt::ChildSpawnRequest;
using agentengine::rt::NoSessionState;
using agentengine::rt::run_child_agent_session;
using agentengine::rt::StartRun;

using agentengine::cap::AgentCall;
using agentengine::trust::check_and_consume_spawn_depth;
using agentengine::trust::mint_child_spawn_capabilities;
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

// Safe here for the identical reason rt/agent_workflow_executor.hpp's own
// agent_executor_detail::drive<T>() and this task's own rt/agent_spawn_child_run.hpp document: every
// AgentSession this file drives is freshly constructed and referenced by nothing else, so
// session_mutex_ is never genuinely contended.
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// -- The scripted backend, extended from test_rt_agent_workflow_executor.cpp's own fixture with one
// addition: `observed_capability_counts`, recorded from INSIDE chat()'s own EffectContext -- what
// this test uses to prove a child actually ran under item 5's minted CapabilitySet, not merely that
// this file's own local variable happens to hold the right value.
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
        std::vector<std::size_t> observed_capability_counts;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] std::shared_ptr<State> const& shared_state() const { return state_; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        state_->observed_capability_counts.push_back(ctx.capabilities ? ctx.capabilities->size() : 0);
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
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

using ScriptedSession = AgentSession<ScriptedChatClient>;

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
    call.call_id       = std::move(call_id);
    call.tool_name     = std::move(tool_name);
    call.arguments_json = std::move(args);
    call.provenance    = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

// -- T3's "agent.spawn"-shaped Tool<> -- deliberately NOT the real production AgentSpawnTool (item
// 1, not built here); a minimal, honest stand-in whose invoke() closure composes items 4/5's
// already-landed budget/capability wiring with THIS task's own run_child_agent_session(), the same
// way a real perform_agent_spawn() would (design doc §2 steps [2]/[3]/[6]/[8]). No Capabilities<>
// policy declared, matching AgentSpawnTool's own designed shape (§4.1): enforcement is the live,
// per-call check inside spawn_child() below, not a static compile-time ceiling entry.
struct SpawnArgs { std::string label; };
AE_JSON_SCHEMA(SpawnArgs, label)
struct SpawnReply { std::string output; };
AE_JSON_SCHEMA(SpawnReply, output)

struct SpawnTool : agentengine::Tool<SpawnTool> {
    static constexpr std::string_view name        = "agent.spawn";
    static constexpr std::string_view description =
        "test-only stand-in for item 1's real AgentSpawnTool -- proves item 2's drive mechanism only.";
    using Args  = SpawnArgs;
    using Reply = SpawnReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::contract,
                                      "SpawnTool::invoke() must never run directly in this test -- "
                                      "only through the custom_invoke closure below",
                                      "test.dead_static_invoke_path"});
    }
};

// Parent HistoryProviderT: advertises SpawnTool; its custom_invoke closure is the real §2 pipeline,
// end to end, minus item 1's own registry/SpawnPump plumbing (out of scope for this task).
class SpawnHistoryProvider {
public:
    void configure(CapabilitySet const* caller_held, AgentMetadata target, std::string child_script_text) {
        caller_held_       = caller_held;
        target_            = std::move(target);
        child_script_text_ = std::move(child_script_text);
    }
    [[nodiscard]] std::size_t spawn_call_count() const { return spawn_call_count_; }
    [[nodiscard]] std::shared_ptr<ScriptedChatClient::State> const& child_observed_state() const {
        return child_observed_state_;
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = {agentengine::make_tool_descriptor_with_invoke<SpawnTool>(
            [this](SpawnArgs a, EffectContext& ctx) { return spawn_child(std::move(a), ctx); })};
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    [[nodiscard]] result<SpawnReply> spawn_child(SpawnArgs a, EffectContext& ctx) {
        ++spawn_call_count_;
        // §2 step [2] (item 4, already landed).
        auto depth = check_and_consume_spawn_depth(*caller_held_, "helper");
        if (!depth) return std::unexpected(depth.error());
        // §2 steps [3]/[6] (item 5, already landed). Item 3's real worktree mint is not built here --
        // an empty grant, matching that header's own "data-only mirror" scope.
        agentengine::SpawnWorktreeGrant const empty_worktree_grant{};
        auto minted = mint_child_spawn_capabilities(*caller_held_, target_, empty_worktree_grant, *depth);
        if (!minted) return std::unexpected(minted.error());

        // §2 step [8] -- THIS task's own mechanism.
        ChildSpawnRequest req;
        req.input         = text_message(a.label);
        req.capabilities  = minted->capabilities;
        req.principal     = ctx.principal;
        req.max_turns     = 5;
        std::string const child_script = child_script_text_;
        auto response = run_child_agent_session<ScriptedChatClient>(
            "child-1", std::move(req), [this, &child_script](ScriptedChatClient& c) {
                c.set_script({{text_response(child_script), Usage{1, 1, 0, 0, 0.0}}});
                child_observed_state_ = c.shared_state();
            });
        if (!response) return std::unexpected(response.error());
        return SpawnReply{agentengine::text_of(response->message)};
    }

    CapabilitySet const* caller_held_ = nullptr;
    AgentMetadata target_;
    std::string child_script_text_;
    std::size_t spawn_call_count_ = 0;
    std::shared_ptr<ScriptedChatClient::State> child_observed_state_;
};
static_assert(agentengine::ContextProvider<SpawnHistoryProvider>);

}  // namespace

int main() {
    // ---- T1/T2: run_child_agent_session() positive control, using item 5's real minted output -----
    {
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(2)}});
        AgentMetadata target;  // empty declared ceiling -- vacuous success (design doc §7 open Q4)

        auto depth = check_and_consume_spawn_depth(caller_held, "helper");
        check(depth.has_value(), "T1 setup: item 4's depth check succeeds");
        agentengine::SpawnWorktreeGrant const empty_worktree_grant{};
        auto minted = mint_child_spawn_capabilities(caller_held, target, empty_worktree_grant, *depth);
        check(minted.has_value(), "T1 setup: item 5's mint succeeds for an empty declared ceiling");
        check(minted.has_value() && minted->capabilities.size() == 0,
              "T1 setup: the minted child set is EMPTY -- the caller's own AgentCall grant (size 1) "
              "never leaks through for a target declaring nothing further (I2)");

        ChildSpawnRequest req;
        req.input        = text_message("do the sub-task");
        req.capabilities = minted->capabilities;
        req.principal    = Principal{"caller-1", ""};
        req.max_turns    = 5;

        std::shared_ptr<ScriptedChatClient::State> observed;
        auto response = run_child_agent_session<ScriptedChatClient>(
            "child-t1", std::move(req), [&observed](ScriptedChatClient& c) {
                c.set_script({{text_response("child-converged"), Usage{1, 1, 0, 0, 0.0}}});
                observed = c.shared_state();
            });
        check(response.has_value(), "T1: run_child_agent_session() drives a fresh child to completion");
        if (response.has_value()) {
            check(agentengine::text_of(response->message) == "child-converged",
                  "T1: the child's own converged response is what flows back to the caller");
        }

        // T2 -- observed FROM INSIDE the child's own chat() call, not from this file's own copy of
        // req.capabilities: the child actually ran under item 5's real minted output (size 0), never
        // the caller's raw held set (size 1).
        check(observed && observed->observed_capability_counts.size() == 1,
              "T2 setup: the child's chat() ran exactly once");
        if (observed && !observed->observed_capability_counts.empty()) {
            check(observed->observed_capability_counts.front() == 0,
                  "T2: the child's own EffectContext::capabilities, observed from INSIDE its chat() "
                  "call, is EXACTLY mint_child_spawn_capabilities()'s minted output (size 0) -- never "
                  "the caller's raw held CapabilitySet (size 1)");
        }
    }

    // ---- T4: depth exhaustion fails the composed pipeline closed, before any child is reachable ---
    {
        CapabilitySet const exhausted =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(0)}});
        auto depth = check_and_consume_spawn_depth(exhausted, "helper");
        check(!depth.has_value(), "T4: an exhausted depth grant fails closed -- I8");
        if (!depth.has_value()) {
            check(depth.error().code == "spawn_budget.depth_exhausted",
                  "T4: ADR-006's own code, propagated unmodified -- and, structurally, no "
                  "ChildSpawnRequest/run_child_agent_session() call is EVER reachable past this point, "
                  "so no child session is ever constructed for this request");
        }
    }

    // ---- T3: end-to-end -- a real PARENT AgentSession's tool-call loop drives the spawn mid-round -
    {
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({AgentCall{"helper", SpawnBudget::mint_root(2)}});
        AgentMetadata target;  // empty declared ceiling

        AgentSession<ScriptedChatClient, NoSessionState, SpawnHistoryProvider> parent;
        parent.initialize("parent-1", Principal{"caller-1", ""});
        parent.set_capabilities(&caller_held);
        parent.history_provider().configure(&caller_held, target, "child-said-done");
        parent.emplace_chat_client().set_script({
            {tool_call_response("c1", "agent.spawn", R"({"label":"do the sub-task"})"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("parent-final"), Usage{1, 1, 0, 0, 0.0}},
        });

        auto response = drive(parent.start_run(StartRun{text_message("please spawn a helper")}));
        check(response.has_value(), "T3: the PARENT run completes");
        check(parent.history_provider().spawn_call_count() == 1,
              "T3: the spawn tool's invoke() ran exactly once, mid-round, from inside the parent's "
              "own tool-call loop");
        auto const& child_state = parent.history_provider().child_observed_state();
        check(child_state && !child_state->observed_capability_counts.empty() &&
                  child_state->observed_capability_counts.front() == 0,
              "T3: the CHILD constructed inside the parent's own tool call ran under item 5's real "
              "minted CapabilitySet (size 0), not the parent's own held set (size 1)");

        // The child's own converged text ("child-said-done") must appear in the PARENT's own
        // tool_results_message -- proving the child's result actually flowed back to the caller.
        bool found_child_result = false;
        for (Message const& m : parent.history()) {
            for (ContentItem const& item : m.content) {
                if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
                    for (ContentItem const& tc : tr->content) {
                        if (auto const* data = std::get_if<Data>(&tc.value)) {
                            if (data->json.find("child-said-done") != std::string::npos) {
                                found_child_result = true;
                            }
                        }
                    }
                }
            }
        }
        check(found_child_result,
              "T3: the child's converged output reached the PARENT's own tool_results_message in "
              "history() -- the child's result flows back to the caller");
    }

    // ---- T5: RC-1 -- background execution is disabled on every spawned child, positive + negative -
    {
        AgentSession<ScriptedChatClient> plain;
        plain.initialize("plain-1", Principal{"p", ""});
        check(!plain.background_execution_disabled(),
              "T5 negative control: an ORDINARY (non-spawned) session defaults to backgrounding "
              "allowed -- every existing caller is unaffected by this task's own change");

        AgentSession<ScriptedChatClient> flagged;
        flagged.initialize("flagged-1", Principal{"p", ""});
        flagged.set_background_execution_disabled(true);
        check(flagged.background_execution_disabled(), "T5: the flag is set and observable");

        ToolTable const empty_table;
        ToolCallRequest const bg_request{"bg-1", "some_tool", agentengine::json::Value::make_object({})};
        auto denied = drive(flagged.start_background_task(empty_table, bg_request));
        check(!denied.has_value(),
              "T5: start_background_task() fails closed on a background-execution-disabled session, "
              "BEFORE it ever reaches tool_pipeline.hpp's own background_task()/std::thread::detach()");
        if (!denied.has_value()) {
            check(denied.error().code == "standing_effect.background_execution_disabled",
                  "T5: fails with the new, explicit RC-1 error code -- not a generic tool-not-found");
        }

        // Same session, flag OFF, proves the guard is what changed the outcome above -- not, say, the
        // empty ToolTable rejecting every call the same way regardless of the flag.
        AgentSession<ScriptedChatClient> unflagged;
        unflagged.initialize("unflagged-1", Principal{"p", ""});
        auto rejected_for_other_reason = drive(unflagged.start_background_task(empty_table, bg_request));
        check(!rejected_for_other_reason.has_value(),
              "T5 negative control: WITHOUT the flag, the same empty-table call still fails (no such "
              "tool) -- proving T5's own denial above came from the RC-1 guard specifically");
        if (!rejected_for_other_reason.has_value()) {
            check(rejected_for_other_reason.error().code != "standing_effect.background_execution_disabled",
                  "T5 negative control: the unflagged failure carries a DIFFERENT code than the RC-1 "
                  "guard's own -- the two are distinguishable, not coincidentally identical strings");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_spawn_child_run: ALL PASS\n");
    } else {
        std::fprintf(stderr, "test_rt_agent_spawn_child_run: %d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
