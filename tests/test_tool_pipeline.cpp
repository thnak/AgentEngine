// Proves M2 Phase B tasks B2 (the ten-step invocation pipeline, 006 §3) and B3 (one trivial native
// tool end to end, miniature 006 §8 G2): a capability-gated call succeeds when the session holds
// the declared capability and is denied -- as a structured ToolResult{is_error}, never a crash or
// silent bypass -- for each of the negative cases this milestone's scope covers: unknown tool
// name, schema violation, capability not held, and approval denied. (deadline/sandbox-crash/
// oversized-result are explicitly out of scope, per 006 §8 G2's own list and this milestone's
// Phase B breakdown -- they need 008/010 machinery not yet built.)
//
// "TestKit-driven" in the breakdown doc's original B3 wording assumed tool-calling would already
// be wired into AgentSession's turn loop; that wiring is real ChatClient/004 integration work
// deferred past M2 (chat_client.hpp's own comment: "004's real ChatClient seam... isn't due until
// Milestone 5"). This proves the pipeline function itself directly and exhaustively instead, which
// is what's actually testable at this milestone's real scope.

#include <cstdio>
#include <optional>
#include <string>
#include <variant>

#include "agentengine/core/tool_pipeline.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// -- a trivial native tool, gated by a single Entropy capability (chosen for having no parameters
// -- the ceiling/grant mechanics under test don't need a second axis of string-matching) ---------

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

// A second tool gated behind Approval<always_require>, for the approve-step negative case.
struct GatedArgs {
    std::string action;
};
AE_JSON_SCHEMA(GatedArgs, action)

struct GatedReply {
    bool done;
};
AE_JSON_SCHEMA(GatedReply, done)

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Approval<agentengine::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_action";
    static constexpr std::string_view description = "An action that always requires approval.";
    using Args = GatedArgs;
    using Reply = GatedReply;

    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

// -- ADR-023 §6 point 4 / 007 §4 amendment: two tools for the text_derived declassifier gate --------

struct PureArgs {
    std::string note;
};
AE_JSON_SCHEMA(PureArgs, note)
struct PureReply {
    bool ok;
};
AE_JSON_SCHEMA(PureReply, ok)

// Declares NO Capabilities<...> (empty ceiling by construction) and EffectClass<pure> -- the ONLY
// shape `is_auto_declassifiable_text_derived_call` (tool_pipeline.hpp) accepts without a human.
struct PureNoCapTool
    : agentengine::Tool<PureNoCapTool, agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "pure_no_cap";
    static constexpr std::string_view description = "A capability-free, pure tool.";
    using Args = PureArgs;
    using Reply = PureReply;

    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

struct DangerousArgs {
    std::string account;
};
AE_JSON_SCHEMA(DangerousArgs, account)
struct DangerousReply {
    bool sent;
};
AE_JSON_SCHEMA(DangerousReply, sent)

// A real, capability-bearing tool that ALSO declares Approval<never_require> -- a tool author who
// decided their own VENDOR-STRUCTURED calls never need a human. ADR-023 §4b Finding 1's whole point:
// a text_derived call must not inherit that decision. Declares NetOut, one of the kinds
// `is_inert_for_text_derived_declassification` (trust/capability.hpp) classifies as dangerous.
struct DangerousNeverRequireTool
    : agentengine::Tool<DangerousNeverRequireTool,
                          agentengine::Capabilities<agentengine::cap::decl::NetOut<"api.example.com">>,
                          agentengine::Approval<agentengine::approval_mode::never_require>> {
    static constexpr std::string_view name = "send_email";
    static constexpr std::string_view description = "Sends an email (real egress capability).";
    using Args = DangerousArgs;
    using Reply = DangerousReply;

    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) { return Reply{true}; }
};

agentengine::EffectContext make_ctx() {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    return ctx;
}

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::CapabilitySet;
    using agentengine::ToolCallRequest;
    using agentengine::ToolTable;
    using agentengine::invoke_tool;

    auto const table = ToolTable::from_tools<EchoTool, GatedTool, PureNoCapTool, DangerousNeverRequireTool>();

    // -- G2 positive case: capability held, tool succeeds ----------------------------------------
    {
        CapabilitySet held =
            CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        auto ctx = make_ctx();
        ToolCallRequest req{"call-1", "echo", *json::parse(R"({"message":"hi"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_tool(table, held, req, ctx, nullptr, &audit);
        check(!result.is_error, "granted capability: echo call succeeds");
        check(audit.ok, "audit records success");
        check(audit.error_code.empty(), "audit has no error code on success");
        if (!result.content.empty()) {
            auto const* data = std::get_if<agentengine::Data>(&result.content[0].value);
            check(data != nullptr, "success result is a Data content item");
            if (data) {
                auto parsed = json::parse(data->json);
                check(parsed.has_value() && parsed->find("echoed")->as_string() == "echo: hi",
                      "echo reply round-trips through the pipeline's JSON codec");
            }
            check(result.content[0].tainted, "tool results are provenance-marked tainted (006 §7)");
        }
    }

    // -- G2: unknown tool name -> contract error, no guess ----------------------------------------
    {
        CapabilitySet held;  // empty -- shouldn't matter, resolve fails before authorize
        auto ctx = make_ctx();
        ToolCallRequest req{"call-2", "does_not_exist", json::Value::make_object({}), false};
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(result.is_error, "unknown tool name is an error");
        auto const* err = std::get_if<agentengine::Error>(&result.content[0].value);
        check(err != nullptr && err->message.find("does_not_exist") != std::string::npos,
              "unknown-tool error names the tool, not a generic message");
    }

    // -- G2: schema violation (missing required field) -> contract error, not coerced -------------
    {
        CapabilitySet held = CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        auto ctx = make_ctx();
        ToolCallRequest req{"call-3", "echo", json::Value::make_object({}), false};  // missing "message"
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(result.is_error, "missing required argument is rejected");
    }

    // -- G2/G3-adjacent: capability not held -> policy error, no leak, tool never invoked ---------
    {
        CapabilitySet held;  // empty: no Entropy grant
        auto ctx = make_ctx();
        ToolCallRequest req{"call-4", "echo", *json::parse(R"({"message":"should not run"})"), false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_tool(table, held, req, ctx, nullptr, &audit);
        check(result.is_error, "capability not held is denied");
        check(audit.error_code == "tool.capability_not_held", "denial carries the expected error code");
        auto const* err = std::get_if<agentengine::Error>(&result.content[0].value);
        check(err != nullptr && err->message.find("Entropy") == std::string::npos &&
                  err->message.find("held") != std::string::npos,
              "capability-denial message doesn't leak which capability was checked");
    }

    // -- §4 approval: always_require, decider says no -> denied, tool never invoked ---------------
    {
        CapabilitySet held;  // GatedTool declares no Capabilities<...>, only Approval
        auto ctx = make_ctx();
        ToolCallRequest req{"call-5", "gated_action", *json::parse(R"({"action":"go"})"), false};
        bool decider_called = false;
        agentengine::ApprovalDecider deny = [&](std::string_view, std::string const&) {
            decider_called = true;
            return false;
        };
        auto result = invoke_tool(table, held, req, ctx, deny);
        check(result.is_error, "approval denied blocks the call");
        check(decider_called, "the approval decider was actually consulted");
    }

    // -- §4 approval: always_require, decider says yes -> succeeds, sees the exact call's args ----
    {
        CapabilitySet held;
        auto ctx = make_ctx();
        ToolCallRequest req{"call-6", "gated_action", *json::parse(R"({"action":"go"})"), false};
        std::string seen_args;
        agentengine::ApprovalDecider allow = [&](std::string_view tool_name, std::string const& args_json) {
            check(tool_name == "gated_action", "decider sees the correct tool name");
            seen_args = args_json;
            return true;
        };
        auto result = invoke_tool(table, held, req, ctx, allow);
        check(!result.is_error, "approval granted allows the call");
        check(seen_args.find("\"go\"") != std::string::npos,
              "decider saw the exact call's canonicalized arguments (006 §4 binding)");
    }

    // -- no approval decider supplied, and the tool doesn't need one: never_require just runs -----
    {
        CapabilitySet held = CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        auto ctx = make_ctx();
        ToolCallRequest req{"call-7", "echo", *json::parse(R"({"message":"no decider needed"})"), false};
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(!result.is_error, "never_require tool succeeds with no approval decider at all");
    }

    // ==============================================================================================
    // ADR-023 §6 point 4 / 007 §4 amendment: the text_derived declassifier gate at invoke_tool step 5
    // ==============================================================================================

    // -- Positive control: a text_derived call to a capability-free, pure tool auto-declassifies --
    // no approve() callback consulted at all, even though none is supplied (nullptr, same as it
    // would fail on a tool that DID need one -- proving this isn't "no decider means allow").
    {
        using agentengine::call_provenance;
        CapabilitySet held;  // PureNoCapTool declares no Capabilities<...> -- nothing to grant
        auto ctx = make_ctx();
        ToolCallRequest req{.call_id = "call-8",
                              .tool_name = "pure_no_cap",
                              .arguments = *json::parse(R"({"note":"leaked but harmless"})"),
                              .provenance = call_provenance::text_derived};
        bool decider_called = false;
        agentengine::ApprovalDecider tripwire = [&](std::string_view, std::string const&) {
            decider_called = true;
            return false;  // if this ever runs, the call MUST still fail -- proving auto-declassify
                            // isn't secretly routing through a permissive decider
        };
        auto result = invoke_tool(table, held, req, ctx, tripwire);
        check(!result.is_error,
              "ADR-023 P2-T1: text_derived call to a capability-free pure tool auto-declassifies "
              "and succeeds");
        check(!decider_called,
              "ADR-023 P2-T1: the approval decider is never even consulted for an auto-declassifiable "
              "call -- this is a real bypass of the approve() step, not a decider that happens to "
              "always say yes");
    }

    // -- THE injection-scenario regression test (ADR-023 §4b Finding 1). A text_derived call to a
    // real, capability-bearing tool that ALSO declares Approval<never_require> for its own
    // vendor-structured calls must STILL be refused without an approving decider -- proving the
    // text_derived gate overrides the tool's own approval_mode rather than deferring to it. This is
    // the exact confused-deputy scenario the red-team pass found: attacker-controlled tool-result
    // text, echoed by the model, reconstructed into a call addressed at a tool the agent genuinely
    // holds NetOut for.
    {
        using agentengine::call_provenance;
        // Matches EXACTLY what DangerousNeverRequireTool's declared NetOut<"api.example.com">
        // converts to (trust/capability.hpp's to_capability(cap::decl::NetOut<Host>)) -- the agent
        // genuinely holds this capability, so the call reaches step 5, not a step-4 capability
        // denial (an empty host_allowlist would deny-all and fail for the wrong reason).
        CapabilitySet held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        auto ctx = make_ctx();
        ToolCallRequest req{.call_id = "call-9",
                              .tool_name = "send_email",
                              .arguments = *json::parse(R"({"account":"attacker-controlled"})"),
                              .provenance = call_provenance::text_derived};
        auto result_no_decider = invoke_tool(table, held, req, ctx, nullptr);
        check(result_no_decider.is_error,
              "ADR-023 P2-T2 (injection regression): a text_derived call to a NetOut-capable tool is "
              "refused with no decider, EVEN THOUGH the tool itself declares Approval<never_require> "
              "-- the override holds");

        bool decider_called = false;
        agentengine::ApprovalDecider deny = [&](std::string_view, std::string const&) {
            decider_called = true;
            return false;
        };
        auto result_denied = invoke_tool(table, held, req, ctx, deny);
        check(result_denied.is_error && decider_called,
              "ADR-023 P2-T2 (injection regression): with a decider present, it IS consulted (proving "
              "the gate routes through the real approval step, not a separate silent-deny path) and "
              "a 'no' from it still blocks the call");

        agentengine::ApprovalDecider allow = [](std::string_view, std::string const&) { return true; };
        auto result_approved = invoke_tool(table, held, req, ctx, allow);
        check(!result_approved.is_error,
              "ADR-023 P2-T2: an EXPLICIT human/policy approval still lets a text_derived call to a "
              "capability-bearing tool through -- this is a gate, not a permanent block");
    }

    // -- vendor_structured calls are byte-for-byte unaffected: the SAME two tools, default provenance
    {
        using agentengine::call_provenance;
        CapabilitySet held;
        auto ctx = make_ctx();
        // PureNoCapTool: vendor_structured + never_require (its actual declared_approval() default,
        // since it declares no Approval<...> policy) succeeds with no decider -- same as echo's own
        // "no decider needed" case above, now proven for a DIFFERENT tool too.
        ToolCallRequest vendor_req{.call_id = "call-10",
                                     .tool_name = "pure_no_cap",
                                     .arguments = *json::parse(R"({"note":"normal wire call"})"),
                                     .provenance = call_provenance::vendor_structured};
        auto result = invoke_tool(table, held, vendor_req, ctx, nullptr);
        check(!result.is_error,
              "ADR-023 P2-T3: a vendor_structured call is completely unaffected by this amendment -- "
              "identical behavior to before it existed");
    }
    {
        // DangerousNeverRequireTool, held, vendor_structured: succeeds with NO decider at all --
        // proving `tool->approval == never_require` is still honored EXACTLY as before for the
        // vendor-structured path, even though the SAME tool now requires approval unconditionally
        // when the SAME capability ceiling is reached via provenance = text_derived (the two tests
        // above). The only thing that changed is which branch a given call takes; neither branch's
        // own behavior changed.
        using agentengine::call_provenance;
        CapabilitySet held =
            CapabilitySet::grant_root({agentengine::cap::NetOut{{"api.example.com"}, std::nullopt, {}}});
        auto ctx = make_ctx();
        ToolCallRequest vendor_req{.call_id = "call-11",
                                     .tool_name = "send_email",
                                     .arguments = *json::parse(R"({"account":"legit-user"})"),
                                     .provenance = call_provenance::vendor_structured};
        auto result = invoke_tool(table, held, vendor_req, ctx, nullptr);
        check(!result.is_error,
              "ADR-023 P2-T3: the SAME capability-bearing, never_require tool still runs with no "
              "decider for a vendor_structured call -- the amendment narrows ONLY the text_derived "
              "branch, never the existing vendor_structured one");
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_tool_pipeline: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_pipeline: %d check(s) failed\n", g_failures);
    return 1;
}
