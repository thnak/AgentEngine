// Proves core/codeact_tool_union.hpp's union_codeact_tools() (the agent's own tools + tools
// unlocked by mounted skills + MCP-discovered tools, merged into ONE bridge-ready ToolTable) and
// protocol/mcp/mcp_tool_bridge.hpp's mcp_tools_as_descriptors() (the adapter that makes an MCP
// server's tools a real, callable third source). No CPython dependency -- this is the pure-logic
// half; the "does agent.tools actually see a refreshed set inside a real interpreter" half is
// tests/test_mediated_python_runner_agent_tools.cpp's own scenario.

#include <cstdio>
#include <memory>
#include <string>
#include <variant>

#include "agentengine/core/codeact_tool_union.hpp"
#include "agentengine/protocol/mcp/mcp_tool_bridge.hpp"
#include "agentengine/protocol/mcp/server.hpp"

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

namespace ae = agentengine;
namespace mcp = agentengine::mcp;
namespace json = agentengine::json;

struct AgentOwnArgs { std::string text; };
AE_JSON_SCHEMA(AgentOwnArgs, text)
struct AgentOwnReply { std::string text; };
AE_JSON_SCHEMA(AgentOwnReply, text)
struct AgentOwnTool : ae::Tool<AgentOwnTool> {
    static constexpr std::string_view name = "agent_own_tool";
    static constexpr std::string_view description = "One of the agent's own declared tools.";
    using Args = AgentOwnArgs;
    using Reply = AgentOwnReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.text}; }
};

struct SkillUnlockedArgs { std::string text; };
AE_JSON_SCHEMA(SkillUnlockedArgs, text)
struct SkillUnlockedReply { std::string text; };
AE_JSON_SCHEMA(SkillUnlockedReply, text)
struct SkillUnlockedTool : ae::Tool<SkillUnlockedTool> {
    static constexpr std::string_view name = "skill_unlocked_tool";
    static constexpr std::string_view description = "A tool a mounted skill's allowed-tools names.";
    using Args = SkillUnlockedArgs;
    using Reply = SkillUnlockedReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.text}; }
};

// Reused for both "distinct name" fixtures AND the collision fixtures below -- a second tool
// sharing agent_own_tool's own name proves the cross-source collision check, not a same-source one
// (ToolTable::from_tools<...>() would already reject a same-source duplicate elsewhere).
struct CollidingArgs { std::string text; };
AE_JSON_SCHEMA(CollidingArgs, text)
struct CollidingReply { std::string text; };
AE_JSON_SCHEMA(CollidingReply, text)
struct CollidingWithAgentOwnTool : ae::Tool<CollidingWithAgentOwnTool> {
    static constexpr std::string_view name = "agent_own_tool";  // deliberately same as AgentOwnTool
    static constexpr std::string_view description = "Collides with AgentOwnTool's own name.";
    using Args = CollidingArgs;
    using Reply = CollidingReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.text}; }
};

struct McpEchoArgs { std::string text; };
AE_JSON_SCHEMA(McpEchoArgs, text)
struct McpEchoReply { std::string text; };
AE_JSON_SCHEMA(McpEchoReply, text)
struct McpEchoTool : ae::Tool<McpEchoTool> {
    static constexpr std::string_view name = "mcp_echo_tool";
    static constexpr std::string_view description = "An MCP-server-side tool, reached via a real round trip.";
    using Args = McpEchoArgs;
    using Reply = McpEchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{"mcp said: " + a.text}; }
};

struct McpAlwaysFailsArgs { bool noop; };
AE_JSON_SCHEMA(McpAlwaysFailsArgs, noop)
struct McpAlwaysFailsReply { bool ok; };
AE_JSON_SCHEMA(McpAlwaysFailsReply, ok)
struct McpAlwaysFailsTool : ae::Tool<McpAlwaysFailsTool> {
    static constexpr std::string_view name = "mcp_always_fails";
    static constexpr std::string_view description = "Always returns an MCP execution error.";
    using Args = McpAlwaysFailsArgs;
    using Reply = McpAlwaysFailsReply;
    static ae::result<Reply> invoke(McpAlwaysFailsArgs, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::contract, "deliberate MCP-side failure",
                                          "test.deliberate_failure"});
    }
};

}  // namespace

int main() {
    // ---- U1: agent tools alone (empty skill/mcp sources) pass through unchanged -------------------
    {
        auto const agent_tools = ae::ToolTable::from_tools<AgentOwnTool>();
        auto const skill_tools = ae::ToolTable::from_tools<>();
        auto unioned = ae::union_codeact_tools(agent_tools, skill_tools);
        check(unioned.has_value() && unioned->descriptors().size() == 1,
              "U1: agent tools alone, with no skill/mcp tools, pass through as the whole union");
        if (unioned) {
            check(unioned->find("agent_own_tool") != nullptr, "U1: the agent's own tool is present");
        }
    }

    // ---- U2: three distinct-named sources merge into one table, all three present -----------------
    {
        auto const agent_tools = ae::ToolTable::from_tools<AgentOwnTool>();
        auto const skill_tools = ae::ToolTable::from_tools<SkillUnlockedTool>();
        std::vector<ae::ToolDescriptor> mcp_tools;
        {
            ae::ToolDescriptor d;
            d.name = "mcp_tool_stub";
            d.description = "a stand-in MCP descriptor for the pure-merge case";
            mcp_tools.push_back(std::move(d));
        }
        auto unioned = ae::union_codeact_tools(agent_tools, skill_tools, mcp_tools);
        check(unioned.has_value() && unioned->descriptors().size() == 3,
              "U2: three distinct-named sources merge into one table of three");
        if (unioned) {
            check(unioned->find("agent_own_tool") != nullptr &&
                      unioned->find("skill_unlocked_tool") != nullptr &&
                      unioned->find("mcp_tool_stub") != nullptr,
                  "U2: all three tools are individually findable by name after the merge");
        }
    }

    // ---- U3: a name collision between the agent's own tools and skill-unlocked tools is a hard ----
    // ---- error, never a silent precedence order --------------------------------------------------
    {
        auto const agent_tools = ae::ToolTable::from_tools<AgentOwnTool>();
        auto const skill_tools = ae::ToolTable::from_tools<CollidingWithAgentOwnTool>();
        auto unioned = ae::union_codeact_tools(agent_tools, skill_tools);
        check(!unioned.has_value(),
              "U3: a tool name declared by both the agent's own tools and a mounted skill is "
              "rejected, not silently resolved by precedence");
        if (!unioned) {
            check(unioned.error().code == "codeact.tool_name_collision_across_sources",
                  "U3: rejected with the real, specific collision error code");
        }
    }

    // ---- U4: the same collision check applies to the skill/MCP pair, not only the agent/skill -----
    // ---- pair ---------------------------------------------------------------------------------------
    {
        auto const agent_tools = ae::ToolTable::from_tools<>();
        auto const skill_tools = ae::ToolTable::from_tools<SkillUnlockedTool>();
        std::vector<ae::ToolDescriptor> mcp_tools;
        {
            ae::ToolDescriptor d;
            d.name = "skill_unlocked_tool";  // deliberately collides with SkillUnlockedTool
            d.description = "an MCP descriptor colliding with a skill-unlocked tool's own name";
            mcp_tools.push_back(std::move(d));
        }
        auto unioned = ae::union_codeact_tools(agent_tools, skill_tools, mcp_tools);
        check(!unioned.has_value() &&
                  unioned.error().code == "codeact.tool_name_collision_across_sources",
              "U4: a skill-unlocked tool colliding with an MCP-sourced tool of the same name is "
              "rejected too, not just the agent/skill pair");
    }

    // ---- U5/U6: mcp_tools_as_descriptors() against a REAL McpServer -- the adapter's generated ----
    // ---- ToolDescriptor.invoke actually round-trips through the real MCP pipeline, both success ---
    // ---- and isError:true -----------------------------------------------------------------------
    {
        auto const server_table = ae::ToolTable::from_tools<McpEchoTool, McpAlwaysFailsTool>();
        ae::CapabilitySet const held;
        mcp::McpServer server(server_table, held, ae::ApprovalDecider{}, "codeact-union-test-server");
        mcp::RequestSender sender = [&server](mcp::JsonRpcRequest const& req) { return server.dispatch(req); };
        auto client = std::make_shared<mcp::McpClient>(sender, "codeact-union-test-client");

        auto descriptors = mcp::mcp_tools_as_descriptors(client);
        check(descriptors.has_value() && descriptors->size() == 2,
              "U5: mcp_tools_as_descriptors() lists both real MCP-side tools");

        if (descriptors) {
            ae::ToolDescriptor const* echo_descriptor = nullptr;
            ae::ToolDescriptor const* fails_descriptor = nullptr;
            for (auto const& d : *descriptors) {
                if (d.name == "mcp_echo_tool") echo_descriptor = &d;
                if (d.name == "mcp_always_fails") fails_descriptor = &d;
            }
            check(echo_descriptor != nullptr && fails_descriptor != nullptr,
                  "U5: both descriptors are individually findable by their real MCP tool names");
            check(echo_descriptor && echo_descriptor->capability_ceiling.size() == 1 &&
                      std::holds_alternative<ae::cap::ToolCall>(echo_descriptor->capability_ceiling[0]) &&
                      std::get<ae::cap::ToolCall>(echo_descriptor->capability_ceiling[0]).tool_name ==
                          "mcp_echo_tool",
                  "U5: an MCP-sourced descriptor is gated behind cap::ToolCall for its own exact "
                  "name -- connecting the client grants no ambient authority to call it (I2)");

            ae::EffectContext ctx{};
            if (echo_descriptor) {
                auto called = echo_descriptor->invoke(
                    json::Value::make_object({{"text", json::Value::make_string("hello")}}), ctx);
                check(called.has_value(), "U5: invoking the adapted descriptor succeeds");
                if (called) {
                    auto const* text_field = called->find("text");
                    check(text_field && text_field->is_string() &&
                              text_field->as_string() == "mcp said: hello",
                          "U5: the invoke closure genuinely round-tripped through the real "
                          "McpClient/McpServer pair -- the reply is the REAL server-side tool's "
                          "own output, not a stub");
                }
            }

            if (fails_descriptor) {
                auto called = fails_descriptor->invoke(
                    json::Value::make_object({{"noop", json::Value::make_bool(true)}}), ctx);
                check(!called.has_value(),
                      "U6: an MCP-side isError:true surfaces as a real pipeline error through the "
                      "adapted descriptor's invoke, never a silently successful empty result");
                if (!called) {
                    check(called.error().code == "mcp.tool_call_error",
                          "U6: rejected with the real, specific MCP-call-error code");
                }
            }
        }
    }

    if (g_failures == 0) {
        std::printf("test_codeact_tool_union: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_codeact_tool_union: %d failure(s)\n", g_failures);
    return 1;
}
