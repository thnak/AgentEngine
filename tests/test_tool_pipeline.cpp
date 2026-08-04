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

    auto const table = ToolTable::from_tools<EchoTool, GatedTool>();

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

    if (g_failures == 0) {
        std::fprintf(stdout, "test_tool_pipeline: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_pipeline: %d check(s) failed\n", g_failures);
    return 1;
}
