// Implements core/skill_tool_scoping.hpp -- proves `scope_tools_to_mounted_skills`/
// `ToolTable::from_descriptors` filter a compile-time-declared tool universe down to a runtime-chosen
// subset, AND that the resulting table is a REAL invocation-time restriction, not merely a shorter
// list handed to a model. The negative control (R3 below) is the load-bearing check: a tool that is a
// real, registered `Tool<>` type -- fully capable of succeeding if called -- must still be REJECTED by
// `invoke_tool` with `tool.unknown_name` when it is not in the scoped table, proving the gate is
// enforced at the actual `ToolTable const&` `invoke_tool` receives, not just at whatever subset a
// caller happens to declare to the model (skill_tool_scoping.hpp's own top comment explains why that
// distinction is the entire point of this mechanism).

#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/core/skill_tool_scoping.hpp"
#include "../../support/run_task_sync.hpp"

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

// Three trivial, capability-free, pure tools -- deliberately EffectClass<pure> with no Capabilities<>
// so this test is entirely about table membership (ToolTable::find/invoke_tool step 1), not about
// approval or capability-binding mechanics those other axes would otherwise entangle it with.
struct NoteArgs {
    std::string note;
};
AE_JSON_SCHEMA(NoteArgs, note)
struct NoteReply {
    bool ok;
};
AE_JSON_SCHEMA(NoteReply, ok)

struct AlphaTool : Tool<AlphaTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "alpha";
    static constexpr std::string_view description = "The alpha tool.";
    using Args = NoteArgs;
    using Reply = NoteReply;
    agentengine::task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};
struct BetaTool : Tool<BetaTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "beta";
    static constexpr std::string_view description = "The beta tool.";
    using Args = NoteArgs;
    using Reply = NoteReply;
    agentengine::task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};
struct GammaTool : Tool<GammaTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "gamma";
    static constexpr std::string_view description = "The gamma tool.";
    using Args = NoteArgs;
    using Reply = NoteReply;
    agentengine::task<result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{true}; }
};

// ADR-237 §5.2: an INSTANCE-form `Tool<>` -- non-static `const task<result<Reply>> invoke`, with the
// tool object holding state -- so R5 can prove `ToolTable::from(...)` / the value-taking
// `make_tool_descriptor(ToolT{...})` register a real tool and keep that instance alive for the
// descriptor's whole lifetime (the shared counter must survive the drive).
struct LiveTool : Tool<LiveTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name        = "live";
    static constexpr std::string_view description = "An instance-form tool carrying state.";
    using Args  = NoteArgs;
    using Reply = NoteReply;

    explicit LiveTool(std::shared_ptr<int> calls) : calls_(std::move(calls)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args, EffectContext&) const {
        ++*calls_;
        co_return Reply{true};
    }

private:
    std::shared_ptr<int> calls_;
};

}  // namespace

int main() {
    ToolTable const universe = ToolTable::from(AlphaTool{}, BetaTool{}, GammaTool{});
    check(universe.descriptors().size() == 3, "setup: the universe has all three tools");

    // ---- R1: scope_tools_to_mounted_skills filters to exactly the allowed names --------------------
    {
        ToolTable const scoped = scope_tools_to_mounted_skills(universe, {"beta"});
        check(scoped.descriptors().size() == 1, "R1: exactly one tool survives filtering to {\"beta\"}");
        check(scoped.find("beta") != nullptr, "R1: 'beta' (allowed) is present in the scoped table");
        check(scoped.find("alpha") == nullptr, "R1: 'alpha' (not allowed) is absent from the scoped table");
        check(scoped.find("gamma") == nullptr, "R1: 'gamma' (not allowed) is absent from the scoped table");
    }

    // ---- R2: always_on is unioned with allowed, independent of the skill-derived set ----------------
    {
        ToolTable const scoped = scope_tools_to_mounted_skills(universe, {}, {"gamma"});
        check(scoped.descriptors().size() == 1,
              "R2: with an empty allowed set, only the always_on tool survives");
        check(scoped.find("gamma") != nullptr, "R2: 'gamma' (always_on) is present with no skills mounted");
    }

    // ---- R3: NEGATIVE CONTROL -- invoke_tool rejects a real, registered tool absent from the scoped
    // table, proving the restriction is enforced at invocation, not merely at declaration -----------
    {
        ToolTable const scoped = scope_tools_to_mounted_skills(universe, {"beta"});
        CapabilitySet const held = CapabilitySet::grant_root({});
        EffectContext ctx;
        ctx.capabilities = agentengine::borrow_capabilities(held);

        ToolCallRequest const allowed_req{"call-1", "beta", json::Value::make_object({{"note", json::Value::make_string("x")}}),
                                           /*arguments_tainted=*/false, 0};
        ToolInvocationAudit allowed_audit;
        ToolResult const allowed_result = agentengine::test_support::run_task_sync<agentengine::ToolResult>(invoke_tool(scoped, held, allowed_req, ctx, {}, &allowed_audit));
        check(!allowed_result.is_error, "R3 setup: calling 'beta' (in the scoped table) succeeds");

        ToolCallRequest const denied_req{"call-2", "alpha", json::Value::make_object({{"note", json::Value::make_string("x")}}),
                                          /*arguments_tainted=*/false, 0};
        ToolInvocationAudit denied_audit;
        ToolResult const denied_result = agentengine::test_support::run_task_sync<agentengine::ToolResult>(invoke_tool(scoped, held, denied_req, ctx, {}, &denied_audit));
        check(denied_result.is_error,
              "R3: calling 'alpha' -- a REAL, registered tool, absent only from the scoped table -- "
              "is rejected, even though it would succeed if called directly against the universe");
        check(denied_audit.error_code == "tool.unknown_name",
              "R3: the rejection is specifically 'unknown tool', from invoke_tool's own step-1 "
              "table.find() -- the exact enforcement boundary skill_tool_scoping.hpp's top comment "
              "names as the real security property");

        // Same call against the UNSCOPED universe succeeds -- proving 'alpha' is a real, working tool
        // and R3's rejection above came from table membership, not from some unrelated defect in it.
        ToolInvocationAudit universe_audit;
        ToolResult const universe_result = agentengine::test_support::run_task_sync<agentengine::ToolResult>(invoke_tool(universe, held, denied_req, ctx, {}, &universe_audit));
        check(!universe_result.is_error,
              "R3 control: the SAME call against the unscoped universe succeeds -- 'alpha' itself is "
              "fine, only its absence from the scoped table caused the rejection above");
    }

    // ---- R4: ToolTable::from_descriptors round-trips a hand-built descriptor list -------------------
    {
        ToolTable const t = ToolTable::from_descriptors({make_tool_descriptor(AlphaTool{})});
        check(t.descriptors().size() == 1 && t.find("alpha") != nullptr,
              "R4: from_descriptors builds a real, queryable table from a runtime vector");
    }

    // ---- R5: ToolTable::from(...) registers instance-form tools (ADR-237 §5.2) ----------------------
    {
        ToolTable const empty = ToolTable::from();
        check(empty.descriptors().empty(), "R5: from() with no arguments is an empty table");

        auto             calls = std::make_shared<int>(0);
        ToolTable const  table = ToolTable::from(LiveTool{calls});
        check(table.descriptors().size() == 1 && table.find("live") != nullptr,
              "R5: from(LiveTool{...}) builds a real, queryable table");

        ToolDescriptor const& d = table.descriptors().front();
        ToolDescriptor const  via_value =
            make_tool_descriptor<LiveTool>(LiveTool{std::make_shared<int>(0)});
        check(d.name == via_value.name && d.description == via_value.description &&
                  d.approval == via_value.approval && d.effect_class == via_value.effect_class &&
                  d.captures_session_state == via_value.captures_session_state &&
                  d.deterministic == via_value.deterministic &&
                  d.enforces_granted_caps == via_value.enforces_granted_caps &&
                  d.capability_ceiling.size() == via_value.capability_ceiling.size() &&
                  d.args_schema_json == via_value.args_schema_json &&
                  d.reply_schema_json == via_value.reply_schema_json &&
                  d.parallelizable == via_value.parallelizable && d.backgroundable == via_value.backgroundable,
              "R5: from(...) and make_tool_descriptor(ToolT{...}) produce identical policy metadata");

        EffectContext    ctx;
        json::Value      args = schema::to_json(NoteArgs{"hi"});
        auto             reply = test_support::run_task_sync<result<json::Value>>(d.invoke(args, ctx));
        check(reply.has_value() && *calls == 1,
              "R5: driving the descriptor runs the LIVE instance's invoke (the tool object outlives "
              "registration) and the shared state observes the call");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_tool_table_scoping: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_table_scoping: %d FAILURE(S)\n", g_failures);
    return 1;
}
