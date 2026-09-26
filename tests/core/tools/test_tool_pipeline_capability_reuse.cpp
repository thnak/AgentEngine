// Proves M2 Phase B task B4 (miniature 006 §8 G3 / 007 §9 G3): "a capability handle from call n is
// unusable in call n+1" -- at the PIPELINE level, not just the primitive level
// (tests/test_capability_enforcement.cpp already proves ADR-009's BoundCapability/revoke() in
// isolation; this proves core/tool_pipeline.hpp actually WIRES that mechanism end to end: step 7
// really does hand a live, usable handle to `invoke()` via `EffectContext::bound_capabilities`, and
// step 10 really does revoke it before `invoke_tool()` returns to the caller).
//
// A tool that stashes a copy of its own per-call handle (simulating a hostile or buggy tool trying
// to retain authority past its invocation) is the adversarial case this test builds: `.use()` on
// that stashed copy must succeed WHILE the call is in flight (positive control -- without this, a
// pipeline that handed out dead-on-arrival handles would make the post-call checks below pass
// vacuously) and must fail immediately after the call returns, and must STILL be dead after a
// second, later call runs (no resurrection/reuse across calls).

#include <cstdio>
#include <optional>
#include <string>

#include "agentengine/core/tool_pipeline.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct StashArgs {
    bool noop;
};
AE_JSON_SCHEMA(StashArgs, noop)

struct StashReply {
    bool ok;
};
AE_JSON_SCHEMA(StashReply, ok)

// Populated by StashingTool::invoke() itself -- one slot per call, set in call order.
std::optional<agentengine::BoundCapability> g_stash[2];
bool g_live_during_call[2] = {false, false};
int g_call_index = -1;

struct StashingTool
    : agentengine::Tool<StashingTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "stashing_tool";
    static constexpr std::string_view description = "Stashes a copy of its own bound capability.";
    using Args = StashArgs;
    using Reply = StashReply;

    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext& ctx) {
        int slot = ++g_call_index;
        if (ctx.bound_capabilities && !ctx.bound_capabilities->empty()) {
            agentengine::BoundCapability const& live_handle = (*ctx.bound_capabilities)[0];
            // Positive control: the handle is genuinely usable DURING the call, before we stash it.
            g_live_during_call[slot] = live_handle.use().has_value();
            g_stash[slot] = live_handle;  // a copy -- shares the same ticket (ADR-009)
        }
        return Reply{true};
    }
};

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::CapabilitySet;
    using agentengine::ToolCallRequest;
    using agentengine::ToolTable;
    using agentengine::invoke_tool;

    auto const table = ToolTable::from_tools<StashingTool>();
    CapabilitySet held = CapabilitySet::grant_root({agentengine::cap::Entropy{}});

    // -- call n ------------------------------------------------------------------------------------
    {
        agentengine::EffectContext ctx;
        ToolCallRequest req{"call-n", "stashing_tool", *json::parse(R"({"noop":true})"), false};
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(!result.is_error, "call n succeeds");
    }
    check(g_live_during_call[0], "positive control: the handle WAS live during call n's own invoke()");
    check(g_stash[0].has_value(), "call n's tool actually received a bound capability to stash");
    if (g_stash[0]) {
        check(!g_stash[0]->use().has_value(),
              "call n's stashed handle is unusable immediately after invoke_tool() returns "
              "(step 10 revoked it)");
    }

    // -- call n+1: a second, later call ------------------------------------------------------------
    {
        agentengine::EffectContext ctx;
        ToolCallRequest req{"call-n+1", "stashing_tool", *json::parse(R"({"noop":true})"), false};
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(!result.is_error, "call n+1 succeeds");
    }
    check(g_live_during_call[1], "positive control: the handle WAS live during call n+1's own invoke()");
    check(g_stash[1].has_value(), "call n+1's tool actually received a bound capability to stash");

    if (g_stash[0]) {
        check(!g_stash[0]->use().has_value(),
              "006 §8 G3: call n's handle is STILL unusable after call n+1 has run "
              "(no resurrection, no cross-call reuse)");
    }
    if (g_stash[1]) {
        check(!g_stash[1]->use().has_value(),
              "call n+1's own handle is unusable after its own invoke_tool() call returned too "
              "(every call's handle dies at its own step 10, not just the previous one's)");
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_tool_pipeline_capability_reuse: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_pipeline_capability_reuse: %d check(s) failed\n", g_failures);
    return 1;
}
