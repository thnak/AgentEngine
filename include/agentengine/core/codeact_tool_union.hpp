#pragma once
// Implements the CodeAct tool-bridge union: `agent.tools` (026 §4, native_jail's
// agent_tools_codegen.hpp) should expose the UNION of the agent's own declared tools, tools
// unlocked by currently mounted skills (core/skill_tool_scoping.hpp's own model-facing/invocable
// scoping, extended here to the CodeAct bridge), tools discovered from a connected MCP
// server (protocol/mcp/mcp_tool_bridge.hpp), and tools discovered from a loaded wasm plugin
// (backends/wasm/wasm_tool_bridge.hpp, ADR-040) -- not any one of those alone. Pure logic, no
// CPython dependency, matching native_jail/agent_tools_codegen.hpp's own "stay Python-free so it's
// unit-testable without an embedded interpreter" precedent.

#include <string>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// A tool name declared by more than one of the four sources is a hard error -- reject rather
// than guess, matching `SkillsProvider`'s own cross-source anti-shadowing precedent
// (`skill_provider.hpp`'s `skill.name_collision_across_sources`) and `register_agent`'s own
// `agent.tool_name_collision`: never a silent precedence order between an agent's own tool, a
// skill-unlocked one, an MCP-discovered one, or a wasm-plugin one of the same name.
[[nodiscard]] inline result<ToolTable> union_codeact_tools(
    ToolTable const& agent_tools, ToolTable const& skill_unlocked_tools,
    std::vector<ToolDescriptor> const& mcp_tools = {},
    std::vector<ToolDescriptor> const& wasm_tools = {}) {
    struct Source {
        std::string_view label;
        std::vector<ToolDescriptor> const& descriptors;
    };
    Source const sources[] = {
        {"the agent's own tools", agent_tools.descriptors()},
        {"a mounted skill's allowed-tools", skill_unlocked_tools.descriptors()},
        {"a connected MCP server", mcp_tools},
        {"a loaded wasm plugin", wasm_tools},
    };

    std::vector<ToolDescriptor> combined;
    std::vector<std::string> claimed_names;
    std::vector<std::string_view> claimed_sources;

    for (auto const& source : sources) {
        for (ToolDescriptor const& descriptor : source.descriptors) {
            for (std::size_t i = 0; i < claimed_names.size(); ++i) {
                if (claimed_names[i] == descriptor.name) {
                    return std::unexpected(error{
                        failure_class::contract,
                        "tool '" + descriptor.name + "' is declared by both " +
                            std::string(claimed_sources[i]) + " and " + std::string(source.label) +
                            " -- a CodeAct-bridged tool name must be unique across all three sources",
                        "codeact.tool_name_collision_across_sources"});
                }
            }
            claimed_names.push_back(descriptor.name);
            claimed_sources.push_back(source.label);
            combined.push_back(descriptor);
        }
    }

    return ToolTable::from_descriptors(std::move(combined));
}

}  // namespace agentengine
