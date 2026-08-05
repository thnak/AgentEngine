// Proves M2 Phase E task E3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// the milestone's own headline exit-criterion sentence, made real -- "an agent declares Tools<...>,
// a capability-gated native tool call is enforced end to end." An agent declaring both Tools<...>
// and a covering Capabilities<...> ceiling, compiled by register_agent<A>() (E2), actually invokes a
// real native tool through core/tool_pipeline.hpp's real ten-step pipeline (Phase B) via
// invoke_agent_tool() (E3's own glue, core/agent_registry.hpp) -- not a stub, not a mock pipeline.
//
//   1. Positive: the compiled metadata's tools/capability_ceiling round-trip through invoke_agent_
//      tool() to a real computed result (the tool's own Args->Reply logic actually ran).
//   2. Negative: an unknown tool name surfaces the pipeline's own "tool.unknown_name" contract error
//      through the same E3 entry point -- proving the wiring forwards failures too, not just the
//      success path.

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

    // -- positive: end-to-end, through the real pipeline -------------------------------------------
    {
        agentengine::EffectContext ctx;
        ctx.principal = agentengine::Principal{"test-principal", ""};
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

    // -- negative: the pipeline's own errors surface through this entry point too ------------------
    {
        agentengine::EffectContext ctx;
        ToolCallRequest req{"call-2", "does_not_exist", json::Value::make_object({}), false};
        auto result = invoke_agent_tool(*meta, req, ctx);
        check(result.is_error, "unknown tool name is rejected, not silently ignored");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_tool_invocation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_tool_invocation: %d FAILURE(S)\n", g_failures);
    return 1;
}
