// PROVE-PHASE INTEGRATION PROBE: wires this design's SandboxReflector into the REAL
// agentengine::ComposedContextProvider, calls the REAL on_context()/assemble_context() pipeline, and
// prints exactly what ContextContribution (-> ChatRequest, per AgentSession::run_rounds(),
// agent_session.hpp:2086-2127) a real ChatClient would receive.

#include "sandbox_reflector.hpp"
#include "trivial_provider.hpp"

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {

// Drives a task<T> to completion for a plain, single-threaded main() -- no contention here (unlike
// §21/§22's probes), so a bounded resume()-loop is sufficient and doesn't need the block_on()
// utility's cross-thread signaling machinery.
template <class T>
T run_to_completion(agentengine::task<T> t) {
    t.resume();
    if constexpr (std::is_void_v<T>) {
        return;
    } else {
        return t.take_value();
    }
}

}  // namespace

int main() {
    using namespace probe;

    // --- Set up the REAL agentengine machinery FIRST -- the grant below must be issued to the
    // BRIDGED Principal (via adopt()) that the reflector's closure will independently re-derive from
    // ctx.principal at invocation time, closing §24.3's gap for real (§25) ---
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    agentengine::Principal real_principal = agentengine::make_embedded_principal("probe-user");
    agentengine::CapabilitySet real_caps = agentengine::CapabilitySet::grant_root({});  // empty --
                                              // matches ScheduleWakeupTool's real precedent: a tool
                                              // with no static Capabilities<> ceiling needs none here
    agentengine::EffectContext ctx;
    ctx.principal = real_principal;
    ctx.capabilities = agentengine::borrow_capabilities(real_caps);
    ctx.run_id = "probe-run-1";
    ctx.turn_index = 0;

    // --- Set up this design's own identity/grant machinery (already proven in §20/§25) -----------
    Principal bridged_owner = authority.adopt(real_principal.id, real_principal.on_behalf_of);
    GrantSet grants_with_shell;
    grants_with_shell.insert(authority.mint_grant(RollbackAuthority{5}, bridged_owner, bridged_owner));

    SandboxStandIn sandbox_with_shell(/*has_execution_surface=*/true, std::move(grants_with_shell));
    SandboxStandIn sandbox_without_shell(/*has_execution_surface=*/false, GrantSet{});

    std::vector<agentengine::Message> empty_history;
    agentengine::SessionContext session_ctx{"probe-session", real_principal, empty_history};

    // --- Test 1: a session WITH a real execution surface contributes reset_sandbox ---------------
    {
        SandboxReflector reflector(&sandbox_with_shell);
        agentengine::ComposedContextProvider<SandboxReflector> composed(
            std::tuple{std::move(reflector)});

        auto contribution = run_to_completion(composed.on_context(session_ctx, ctx));
        CHECK(contribution.has_value());

        std::printf("[1] Session WITH execution surface -- ContextContribution the REAL "
                    "assemble_context() pipeline produced:\n");
        std::printf("    tools.size() = %zu\n", contribution->tools.size());
        CHECK(contribution->tools.size() == 1);
        auto const& tool = contribution->tools[0];
        std::printf("    tools[0].name          = \"%s\"\n", tool.name.c_str());
        std::printf("    tools[0].description   = \"%s\"\n", tool.description.c_str());
        std::printf("    tools[0].capability_ceiling.size() = %zu (empty -- matches "
                    "ScheduleWakeupTool's real, no-static-Capabilities<> precedent)\n",
                    tool.capability_ceiling.size());
        std::printf("    tools[0].args_schema_json  = %s\n", tool.args_schema_json.c_str());
        std::printf("    tools[0].reply_schema_json = %s\n", tool.reply_schema_json.c_str());
        std::printf("    tools[0].captures_session_state = %d (true -- set unconditionally by "
                    "make_tool_descriptor_with_invoke, real precedent tool_pipeline.hpp:154)\n",
                    (int)tool.captures_session_state);
        std::printf("    tools[0].attribution has_value() = %d, contributor_type = \"%s\" (REAL "
                    "attribution machinery from context_assembly.hpp:239, stamped by the pipeline "
                    "itself, not this reflector)\n",
                    (int)tool.attribution.has_value(),
                    tool.attribution.has_value() ? tool.attribution->contributor_type.c_str() : "");
        CHECK(tool.name == "reset_sandbox");
        CHECK(tool.capability_ceiling.empty());
        CHECK(tool.captures_session_state == true);
        CHECK(tool.attribution.has_value());
        CHECK(tool.attribution->contributor_type == "sandbox_reflector");

        // --- Real invocation through the tool's own real closure, exactly as tool_pipeline.hpp's
        // real invoke_tool() step would call it (json in, json out) ---
        agentengine::json::Value args_json = agentengine::json::Value::make_object(
            {{"turns_back", agentengine::json::Value::make_number(3)}});
        auto invoke_result = tool.invoke(args_json, ctx);
        CHECK(invoke_result.has_value());
        std::printf("    REAL invocation via tool.invoke(args_json, ctx) with turns_back=3: %s\n",
                    agentengine::json::dump(*invoke_result).c_str());

        // Authorized-but-over-the-grant's-ceiling case (grant allows max 5, request 999):
        agentengine::json::Value over_json = agentengine::json::Value::make_object(
            {{"turns_back", agentengine::json::Value::make_number(999)}});
        auto over_result = tool.invoke(over_json, ctx);
        CHECK(!over_result.has_value());
        std::printf("    REAL invocation with turns_back=999 (exceeds grant's max_turns_back=5): "
                    "REJECTED (%s)\n", over_result.error().message.c_str());
    }

    // --- Test 2: a session WITHOUT an execution surface contributes NOTHING -----------------------
    {
        SandboxReflector reflector(&sandbox_without_shell);
        agentengine::ComposedContextProvider<SandboxReflector> composed(
            std::tuple{std::move(reflector)});
        auto contribution = run_to_completion(composed.on_context(session_ctx, ctx));
        CHECK(contribution.has_value());
        CHECK(contribution->tools.empty());
        std::printf("[2] Session WITHOUT execution surface -- tools.size() = %zu (matches this "
                    "design's own §2 rule: no execution surface, no run_shell/reset_sandbox "
                    "contributed at all)\n", contribution->tools.size());
    }

    // --- Test 3: composed alongside a SECOND, unrelated real ContextProvider -- no special-casing,
    // no coupling (this design's own §9 principle, OQ-18's already-judged fan-out-not-chain rule) ---
    {
        SandboxReflector reflector(&sandbox_with_shell);
        agentengine::ComposedContextProvider<TrivialInstructionsProvider, SandboxReflector> composed(
            std::tuple{TrivialInstructionsProvider{}, std::move(reflector)});
        auto contribution = run_to_completion(composed.on_context(session_ctx, ctx));
        CHECK(contribution.has_value());
        CHECK(contribution->tools.size() == 1);
        CHECK(contribution->tools[0].name == "reset_sandbox");
        CHECK(contribution->instructions.has_value());
        CHECK(contribution->instructions->unsafe_view() == "Be concise.");
        std::printf("[3] Composed alongside a second, unrelated ContextProvider: PASS "
                    "(instructions=\"%s\" from provider 0, tools=[\"%s\"] from provider 1 (this "
                    "design's reflector) -- both real assemble_context()'s own union/merge logic, "
                    "no special-casing needed for this design's new component)\n",
                    contribution->instructions->unsafe_view().c_str(),
                    contribution->tools[0].name.c_str());
    }

    // --- Test 4: a DIFFERENT, unrelated real agentengine::Principal calling the SAME tool is
    // REJECTED -- proves the check genuinely derives identity from ctx.principal AT INVOCATION TIME
    // via the bridge (§25), not from a value captured once when the tool was contributed. -----------
    {
        SandboxReflector reflector(&sandbox_with_shell);
        agentengine::ComposedContextProvider<SandboxReflector> composed(
            std::tuple{std::move(reflector)});
        auto contribution = run_to_completion(composed.on_context(session_ctx, ctx));
        CHECK(contribution.has_value());
        auto const& tool = contribution->tools[0];

        agentengine::EffectContext stranger_ctx;
        stranger_ctx.principal = agentengine::make_embedded_principal("a-completely-different-user");
        stranger_ctx.capabilities = ctx.capabilities;
        agentengine::json::Value args_json = agentengine::json::Value::make_object(
            {{"turns_back", agentengine::json::Value::make_number(1)}});
        auto stranger_result = tool.invoke(args_json, stranger_ctx);
        CHECK(!stranger_result.has_value());
        CHECK(stranger_result.error().code == "sandbox.no_rollback_grant");
        std::printf("[4] A DIFFERENT real agentengine::Principal ('a-completely-different-user') "
                    "invoking the SAME tool descriptor: REJECTED (%s) -- confirms the dynamic check "
                    "really re-derives identity from ctx.principal per call via adopt(), not from a "
                    "value captured once at contribution time\n", stranger_result.error().message.c_str());
    }

    // --- Test 5: a REAL delegated principal (agentengine::derive_on_behalf_of) correctly inherits
    // the grant through the bridge's own ancestry recognition (§25's "parent adopted first" path). --
    {
        auto delegated = agentengine::derive_on_behalf_of(real_principal, "delegated-sub-agent");
        CHECK(delegated.has_value());

        SandboxReflector reflector(&sandbox_with_shell);
        agentengine::ComposedContextProvider<SandboxReflector> composed(
            std::tuple{std::move(reflector)});
        auto contribution = run_to_completion(composed.on_context(session_ctx, ctx));
        CHECK(contribution.has_value());
        auto const& tool = contribution->tools[0];

        agentengine::EffectContext delegated_ctx;
        delegated_ctx.principal = *delegated;
        delegated_ctx.capabilities = ctx.capabilities;
        agentengine::json::Value args_json = agentengine::json::Value::make_object(
            {{"turns_back", agentengine::json::Value::make_number(2)}});
        auto delegated_result = tool.invoke(args_json, delegated_ctx);
        CHECK(delegated_result.has_value());
        std::printf("[5] A REAL delegated principal (agentengine::derive_on_behalf_of(\"probe-user\", "
                    "\"delegated-sub-agent\")) correctly INHERITS the grant through the bridge's "
                    "ancestry recognition: %s\n", agentengine::json::dump(*delegated_result).c_str());
    }

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
