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
#include <string>

#include "agentengine/core/skill_tool_scoping.hpp"

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
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};
struct BetaTool : Tool<BetaTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "beta";
    static constexpr std::string_view description = "The beta tool.";
    using Args = NoteArgs;
    using Reply = NoteReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};
struct GammaTool : Tool<GammaTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "gamma";
    static constexpr std::string_view description = "The gamma tool.";
    using Args = NoteArgs;
    using Reply = NoteReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

}  // namespace

int main() {
    ToolTable const universe = ToolTable::from_tools<AlphaTool, BetaTool, GammaTool>();
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
        ToolResult const allowed_result = invoke_tool(scoped, held, allowed_req, ctx, {}, &allowed_audit);
        check(!allowed_result.is_error, "R3 setup: calling 'beta' (in the scoped table) succeeds");

        ToolCallRequest const denied_req{"call-2", "alpha", json::Value::make_object({{"note", json::Value::make_string("x")}}),
                                          /*arguments_tainted=*/false, 0};
        ToolInvocationAudit denied_audit;
        ToolResult const denied_result = invoke_tool(scoped, held, denied_req, ctx, {}, &denied_audit);
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
        ToolResult const universe_result = invoke_tool(universe, held, denied_req, ctx, {}, &universe_audit);
        check(!universe_result.is_error,
              "R3 control: the SAME call against the unscoped universe succeeds -- 'alpha' itself is "
              "fine, only its absence from the scoped table caused the rejection above");
    }

    // ---- R4: ToolTable::from_descriptors round-trips a hand-built descriptor list -------------------
    {
        ToolTable const t = ToolTable::from_descriptors({make_tool_descriptor<AlphaTool>()});
        check(t.descriptors().size() == 1 && t.find("alpha") != nullptr,
              "R4: from_descriptors builds a real, queryable table from a runtime vector");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_tool_table_scoping: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_table_scoping: %d FAILURE(S)\n", g_failures);
    return 1;
}
