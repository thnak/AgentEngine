#pragma once
// Adapts a connected `McpClient`'s (client.hpp) discovered tools into real `ToolDescriptor`s
// (core/tool_pipeline.hpp) -- the piece that lets `core/codeact_tool_union.hpp` treat an MCP
// server's tools as a third source alongside the agent's own tools and mounted-skill-unlocked
// tools, all going through the SAME real 006 §3 `invoke_tool` pipeline once bridged into CodeAct.
//
// I2: connecting an `McpClient` grants NO ambient authority to call anything it lists. Each
// generated descriptor's `capability_ceiling` is `{cap::ToolCall{name}}` -- the session's own
// `ToolBridgeConfig::capabilities` must explicitly name that exact MCP tool, the same shape any
// other capability-gated tool already requires, never widened just because the client exists.

#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/protocol/mcp/client.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::mcp {

[[nodiscard]] inline std::vector<ToolDescriptor> mcp_tools_as_descriptors_from(
    std::vector<McpToolInfo> const& listed, std::shared_ptr<McpClient> const& client) {
    std::vector<ToolDescriptor> out;
    out.reserve(listed.size());
    for (McpToolInfo const& info : listed) {
        ToolDescriptor descriptor;
        descriptor.name = info.name;
        descriptor.description = info.description;
        descriptor.capability_ceiling = {cap::ToolCall{info.name}};
        descriptor.args_schema_json = json::dump(info.input_schema);
        descriptor.reply_schema_json = json::dump(info.output_schema);

        std::string const tool_name = info.name;  // captured by value, not a reference into `listed`
        descriptor.invoke = [client, tool_name](json::Value const& args,
                                                  EffectContext&) -> result<json::Value> {
            auto outcome = client->call_tool(tool_name, args);
            if (!outcome) return std::unexpected(outcome.error());
            // 011 §3.1: "surfaced to the model for self-correction" -- an MCP-side execution
            // failure becomes a real pipeline error here, never a silently successful empty
            // result; the RPC-level failure case above already took the same path.
            if (outcome->is_error) {
                return std::unexpected(error{failure_class::contract,
                                              "MCP tool '" + tool_name + "' returned isError: true",
                                              "mcp.tool_call_error"});
            }
            // §3.1's own content-part shape: an array of {"type":"text","text":...} parts (this
            // codebase's own McpServer -- server.hpp's tool_result_content_to_json -- puts a real
            // tool's JSON-dumped reply into that ONE text field, never the raw reply object at the
            // top level). Unwrap the first text part and parse it back into a real json::Value, so
            // a bridged CodeAct call sees the actual reply shape, not an MCP content-part envelope.
            // A THIRD-PARTY server's text part need not itself be JSON -- if it doesn't parse,
            // fall back to the raw string rather than failing a call that genuinely succeeded.
            if (!outcome->content.is_array() || outcome->content.as_array().empty()) {
                return json::Value::make_null();
            }
            json::Value const& first_part = outcome->content.as_array().front();
            json::Value const* text_field = first_part.find("text");
            if (!text_field || !text_field->is_string()) return json::Value::make_null();
            auto parsed = json::parse(text_field->as_string());
            if (parsed) return *parsed;
            return json::Value::make_string(text_field->as_string());
        };
        out.push_back(std::move(descriptor));
    }
    return out;
}

// Calls `client->list_tools()` and adapts the result. Kept separate from the `listed`-taking
// overload above so a caller that already has a listing (e.g. from its own caching/paging loop)
// doesn't pay for a second `list_tools()` round trip.
[[nodiscard]] inline result<std::vector<ToolDescriptor>> mcp_tools_as_descriptors(
    std::shared_ptr<McpClient> client) {
    auto listed = client->list_tools();
    if (!listed) return std::unexpected(listed.error());
    return mcp_tools_as_descriptors_from(*listed, client);
}

}  // namespace agentengine::mcp
