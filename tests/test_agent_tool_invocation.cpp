// Proves M2 Phase E task E3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// the milestone's own headline exit-criterion sentence, made real -- "an agent declares Tools<...>,
// a capability-gated native tool call is enforced end to end." An agent declaring both Tools<...>
// and a covering Capabilities<...> ceiling, compiled by register_agent<A>() (E2), actually invokes a
// real native tool through core/tool_pipeline.hpp's real ten-step pipeline (Phase B) via
// invoke_agent_tool() (E3's own glue, core/agent_registry.hpp) -- not a stub, not a mock pipeline.
//
// decisions/ADR-059-invoke-agent-tool-capability-attenuation.md: invoke_agent_tool() attenuates the
// CALLER's own held `ctx.capabilities` down to the target's declared ceiling (CapabilitySet::
// attenuate(), ADR-009) instead of minting the target's own ceiling unconditionally (the I2 hole
// ADR-059 fixes) -- every case below therefore needs a real, covering `ctx.capabilities` to prove
// the SAME thing this file always proved (the glue works end to end), now correctly reflecting that
// attenuation:
//
//   1. Positive: a caller holding a covering CapabilitySet invokes EchoAgent's declared tool; the
//      compiled metadata's tools/capability_ceiling round-trip through invoke_agent_tool() to a real
//      computed result (the tool's own Args->Reply logic actually ran).
//   2. Negative (pipeline forwarding): with a covering caller CapabilitySet still attached, an
//      unknown tool name surfaces the pipeline's own "tool.unknown_name" contract error through the
//      same E3 entry point -- proving the wiring forwards failures too, not just the success path
//      (and proving this negative case is reaching the REAL pipeline, not failing earlier at the
//      attenuation gate).
//   3. Negative (ADR-059 R1, the security property this ADR exists for): a caller holding LESS than
//      the target's declared ceiling (an empty CapabilitySet) is rejected before the pipeline ever
//      runs -- "capability.attenuation_not_subsumed".
//   4. Positive (ADR-059 R3, no leak-through): a caller holding MORE than the target needs (the
//      covering capability plus an unrelated extra one) still succeeds identically to case 1, and
//      the same `attenuate()` primitive invoke_agent_tool() calls internally is asserted directly to
//      derive a set bounded to exactly the target's own ceiling size, not the caller's larger held
//      set.
//   5. Negative (ADR-059 R4, fail closed): `ctx.capabilities == nullptr` (no caller capabilities at
//      all to attenuate from) is rejected with "agent_call.no_caller_capabilities", never silently
//      read as "the caller holds everything."

#include <cstdio>
#include <string>

#include "agentengine/core/agent_registry.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// Same trivial native tool shape test_agent_registry.cpp/test_tool_pipeline.cpp already use --
// gated by a single Entropy capability, no reason to invent a second shape for the same job.
struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)

struct EchoReply {
    std::string echoed;
};
AE_JSON_SCHEMA(EchoReply, echoed)

struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo the input message back.";
    using Args = EchoArgs;
    using Reply = EchoReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{"echo: " + args.message};
    }
};

// Tools<EchoTool> + a Capabilities<...> ceiling that covers exactly what EchoTool needs -- the
// headline sentence's own worked example, made real.
struct EchoAgent
    : agentengine::Agent<EchoAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Tools<EchoTool>, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo-agent";
    static constexpr std::string_view instructions = "Declares Tools<EchoTool> and a covering ceiling.";
};

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::register_agent;
    using agentengine::invoke_agent_tool;
    using agentengine::ToolCallRequest;

    auto meta = register_agent<EchoAgent>();
    check(meta.has_value(), "EchoAgent registers cleanly (Tools<...> + a covering Capabilities<...>)");
    if (!meta) {
        std::fprintf(stderr, "test_agent_tool_invocation: registration failed, aborting: %s\n",
                     meta.error().message.c_str());
        return 1;
    }

    // A covering caller CapabilitySet -- exactly what EchoAgent's own ceiling declares (a single
    // Entropy capability). ADR-059: this is what `ctx.capabilities` must now carry for the glue to
    // grant anything at all; a legitimate caller about to invoke EchoAgent would hold at least this.
    agentengine::CapabilitySet const covering_caps =
        agentengine::CapabilitySet::grant_root({agentengine::cap::Entropy{}});

    // -- 1. positive: end-to-end, through the real pipeline, with a covering caller ------------------
    {
        agentengine::EffectContext ctx;
        ctx.principal = agentengine::Principal{"test-principal", ""};
        ctx.capabilities = &covering_caps;
        ToolCallRequest req{"call-1", "echo", *json::parse(R"({"message":"hi"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_agent_tool(*meta, req, ctx, {}, &audit);

        check(!result.is_error, "agent-declared tool call succeeds end-to-end");
        check(audit.ok, "audit records success");
        check(audit.error_code.empty(), "audit has no error code on success");
        if (!result.content.empty()) {
            auto const* data = std::get_if<agentengine::Data>(&result.content[0].value);
            check(data != nullptr, "success result is a Data content item");
            if (data) {
                auto parsed = json::parse(data->json);
                check(parsed.has_value() && parsed->find("echoed")->as_string() == "echo: hi",
                      "the tool's own logic actually ran -- not a stub, a real computed reply");
            }
            check(result.content[0].tainted, "tool results are provenance-marked tainted (006 §7)");
        }
    }

    // -- 2. negative: the pipeline's own errors surface through this entry point too, with a
    //    covering caller attached -- proves this failure comes from the REAL pipeline (tool.
    //    unknown_name), not from the attenuation gate rejecting the caller first ---------------------
    {
        agentengine::EffectContext ctx;
        ctx.capabilities = &covering_caps;
        ToolCallRequest req{"call-2", "does_not_exist", json::Value::make_object({}), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_agent_tool(*meta, req, ctx, {}, &audit);
        check(result.is_error, "unknown tool name is rejected, not silently ignored");
        check(audit.error_code == "tool.unknown_name",
              "the rejection is the pipeline's own unknown-tool error, proving this call reached "
              "the real pipeline and wasn't rejected earlier at the attenuation gate");
    }

    // -- 3. negative (ADR-059 R1): a caller holding LESS than the target's declared ceiling is
    //    rejected before the pipeline ever runs -- the security property this ADR exists for --------
    {
        agentengine::CapabilitySet const empty_caps = agentengine::CapabilitySet::grant_root({});
        agentengine::EffectContext ctx;
        ctx.capabilities = &empty_caps;
        ToolCallRequest req{"call-3", "echo", *json::parse(R"({"message":"hi"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_agent_tool(*meta, req, ctx, {}, &audit);
        check(result.is_error, "a caller holding nothing cannot invoke a tool needing Entropy");
        check(!audit.ok, "audit records the denial");
        check(audit.error_code == "capability.attenuation_not_subsumed",
              "denied by CapabilitySet::attenuate() itself -- the caller doesn't cover the target's "
              "declared ceiling");
    }

    // -- 4. positive (ADR-059 R3): a caller holding MORE than the target needs still succeeds, and
    //    the derived grant is bounded to the target's own ceiling, not the caller's larger held set --
    {
        agentengine::CapabilitySet const generous_caps = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::Entropy{}, agentengine::cap::NetOut{{"unrelated.example:443:https"}, std::nullopt, {}}});
        agentengine::EffectContext ctx;
        ctx.capabilities = &generous_caps;
        ToolCallRequest req{"call-4", "echo", *json::parse(R"({"message":"hi"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_agent_tool(*meta, req, ctx, {}, &audit);
        check(!result.is_error, "extra, unrelated caller capabilities don't block a covered call");
        check(audit.ok, "audit records success");

        // The exact primitive invoke_agent_tool() now calls internally, asserted directly: the
        // derived set's size equals the TARGET's own ceiling size (1, just Entropy), never the
        // caller's larger held set (2) -- no leak-through of the unrelated NetOut grant.
        auto attenuated = generous_caps.attenuate(meta->capability_ceiling);
        check(attenuated.has_value(), "the same attenuation invoke_agent_tool() performs succeeds here too");
        if (attenuated) {
            check(attenuated->size() == meta->capability_ceiling.size(),
                  "the derived grant is bounded to the target's own declared ceiling size, not the "
                  "caller's larger held set");
        }
    }

    // -- 5. negative (ADR-059 R4): ctx.capabilities == nullptr fails closed, never silently read as
    //    "the caller holds everything" -----------------------------------------------------------
    {
        agentengine::EffectContext ctx;  // ctx.capabilities defaults to nullptr
        ToolCallRequest req{"call-5", "echo", *json::parse(R"({"message":"hi"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_agent_tool(*meta, req, ctx, {}, &audit);
        check(result.is_error, "a null caller CapabilitySet cannot invoke any tool");
        check(!audit.ok, "audit records the denial");
        check(audit.error_code == "agent_call.no_caller_capabilities",
              "fails closed with the dedicated null-capabilities error code, distinct from an "
              "ordinary attenuation rejection");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_tool_invocation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_tool_invocation: %d FAILURE(S)\n", g_failures);
    return 1;
}
