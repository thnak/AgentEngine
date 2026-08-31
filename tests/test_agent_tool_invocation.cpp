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
#include "agentengine/trust/delegated_approval_policy.hpp"

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

// GitHub issue #30 / ADR-151: a second tool, declared `policy_driven` (unlike EchoTool above, which
// declares no `Approval<...>` and so defaults to `never_require`, core/tool.hpp) -- this is what lets
// case 6 below actually exercise `PolicyDecider`/`resolve_approval_outcome()`'s three-way outcome
// through the REAL `invoke_agent_tool()` entry point, not a synthetic `resolve_approval_outcome()`
// unit test (ADR-070's own tests already cover that; this file's whole point is proving the GLUE).
struct PolicyGatedTool
    : agentengine::Tool<PolicyGatedTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>,
                         agentengine::Approval<agentengine::approval_mode::policy_driven>> {
    static constexpr std::string_view name = "policy_gated";
    static constexpr std::string_view description = "A tool gated by PolicyDecider.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{"policy_gated: " + args.message};
    }
};

struct PolicyGatedAgent
    : agentengine::Agent<PolicyGatedAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Tools<PolicyGatedTool>,
                          agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "policy-gated-agent";
    static constexpr std::string_view instructions = "Declares Tools<PolicyGatedTool>, policy_driven.";
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

    auto policy_meta = register_agent<PolicyGatedAgent>();
    check(policy_meta.has_value(), "PolicyGatedAgent registers cleanly");
    if (!policy_meta) {
        std::fprintf(stderr, "test_agent_tool_invocation: PolicyGatedAgent registration failed: %s\n",
                     policy_meta.error().message.c_str());
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
        ctx.capabilities = agentengine::borrow_capabilities(covering_caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(covering_caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(empty_caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(generous_caps);
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

    // -- 6. PolicyDecider (ADR-070), driven by the reference delegated-approval policy (ADR-151,
    //    GitHub issue #30) -- through the REAL invoke_agent_tool() entry point, not a synthetic
    //    resolve_approval_outcome() unit test (ADR-070's own tests already cover that mechanism in
    //    the abstract; this file's whole point is proving the glue). --------------------------------
    {
        agentengine::PolicyDecider const policy = agentengine::trust::approve_delegated_calls();

        bool approval_decider_called = false;
        agentengine::ApprovalDecider const tripwire = [&approval_decider_called](
            agentengine::Principal const&, std::string_view, std::string const&) {
            approval_decider_called = true;
            return false;  // would deny if ever reached -- proves auto_approve short-circuits it
        };

        // 6a. non-delegated (top-level) caller -- require_approval, no ApprovalDecider configured ->
        //     denied. Proves this reference policy does NOT blanket-approve everyone.
        {
            agentengine::EffectContext ctx;
            ctx.principal    = agentengine::Principal{"top-level-user", ""};  // on_behalf_of empty
            ctx.capabilities = agentengine::borrow_capabilities(covering_caps);
            ToolCallRequest req{"call-6a", "policy_gated", *json::parse(R"({"message":"hi"})"),
                                 /*arguments_tainted=*/true};
            agentengine::ToolInvocationAudit audit;
            auto result = invoke_agent_tool(*policy_meta, req, ctx, {}, &audit, policy);
            check(result.is_error,
                  "6a: a non-delegated caller's policy_driven call is NOT auto-approved by "
                  "approve_delegated_calls() -- falls through to the unset ApprovalDecider, denied");
            check(audit.error_code == "tool.approval_denied",
                  "6a: denied via the ordinary fallback, not a policy-specific error code -- this "
                  "reference policy never invents a new denial reason");
        }

        // 6b. delegated caller (real derive_on_behalf_of() output, matching what agent.spawn/
        //     invoke_agent_tool()'s own real callers actually produce) -- auto_approve, and the
        //     ApprovalDecider tripwire is NEVER consulted (a real short-circuit, ADR-070 §5a's own
        //     claim, re-proven here through invoke_agent_tool() specifically). arguments_tainted is
        //     deliberately `true` (the real, always-true production value -- see
        //     delegated_approval_policy.hpp's own file banner) -- this is the exact scenario an
        //     earlier draft of this policy got wrong.
        {
            agentengine::Principal const top_level{"parent-agent", ""};
            auto delegated = agentengine::derive_on_behalf_of(top_level, "child-agent-1");
            check(delegated.has_value(), "6b setup: derive_on_behalf_of() succeeds");

            agentengine::EffectContext ctx;
            ctx.principal            = *delegated;
            ctx.capabilities         = agentengine::borrow_capabilities(covering_caps);
            approval_decider_called = false;
            ToolCallRequest req{"call-6b", "policy_gated", *json::parse(R"({"message":"hi"})"),
                                 /*arguments_tainted=*/true};
            agentengine::ToolInvocationAudit audit;
            auto result = invoke_agent_tool(*policy_meta, req, ctx, tripwire, &audit, policy);
            check(!result.is_error,
                  "6b: a delegated caller's policy_driven call auto-approves via "
                  "approve_delegated_calls(), even though arguments_tainted is true");
            check(!approval_decider_called,
                  "6b: auto_approve short-circuits the ApprovalDecider entirely -- never consulted "
                  "for a delegated caller this policy approves");
            check(audit.ok, "6b: audit records success, indistinguishable in shape from a "
                              "decider-approved call (I4)");
        }

        // 6c. delegated caller past a host-chosen max_depth -- falls back to require_approval exactly
        //     like a non-delegated caller, proving max_depth is a real, enforced ceiling distinct from
        //     kMaxDelegationDepth's own structural bound.
        {
            agentengine::PolicyDecider const bounded_policy = agentengine::trust::approve_delegated_calls(1);
            agentengine::Principal const top_level{"parent-agent", ""};
            auto depth1 = agentengine::derive_on_behalf_of(top_level, "child-1");
            check(depth1.has_value(), "6c setup: depth-1 derivation succeeds");
            auto depth2 = depth1.has_value() ? agentengine::derive_on_behalf_of(*depth1, "grandchild-1")
                                              : agentengine::result<agentengine::Principal>{};
            check(depth2.has_value() && depth2->delegation_depth == 2, "6c setup: depth is really 2");

            agentengine::EffectContext ctx;
            ctx.principal    = *depth2;
            ctx.capabilities = agentengine::borrow_capabilities(covering_caps);
            ToolCallRequest req{"call-6c", "policy_gated", *json::parse(R"({"message":"hi"})"), true};
            agentengine::ToolInvocationAudit audit;
            auto result = invoke_agent_tool(*policy_meta, req, ctx, {}, &audit, bounded_policy);
            check(result.is_error,
                  "6c: delegation_depth (2) exceeds max_depth (1) -- falls through to "
                  "require_approval, denied by the unset ApprovalDecider, exactly as if this policy "
                  "had never been wired");
        }

        // 6d. pinned boundary: delegation_depth == max_depth is STILL auto-approved (max_depth is an
        //     inclusive ceiling) -- removes any off-by-one ambiguity for a future reader.
        {
            agentengine::PolicyDecider const bounded_policy = agentengine::trust::approve_delegated_calls(2);
            agentengine::Principal const top_level{"parent-agent", ""};
            auto depth1 = agentengine::derive_on_behalf_of(top_level, "child-1");
            auto depth2 = depth1.has_value() ? agentengine::derive_on_behalf_of(*depth1, "grandchild-1")
                                              : agentengine::result<agentengine::Principal>{};
            check(depth2.has_value() && depth2->delegation_depth == 2, "6d setup: depth is really 2");

            agentengine::EffectContext ctx;
            ctx.principal    = *depth2;
            ctx.capabilities = agentengine::borrow_capabilities(covering_caps);
            ToolCallRequest req{"call-6d", "policy_gated", *json::parse(R"({"message":"hi"})"), true};
            agentengine::ToolInvocationAudit audit;
            auto result = invoke_agent_tool(*policy_meta, req, ctx, {}, &audit, bounded_policy);
            check(!result.is_error,
                  "6d: delegation_depth == max_depth is still auto-approved -- max_depth is inclusive");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_tool_invocation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_tool_invocation: %d FAILURE(S)\n", g_failures);
    return 1;
}
