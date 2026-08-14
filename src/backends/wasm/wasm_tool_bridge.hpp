#pragma once
// Adapts a loaded WasmBackend component's discovered tools (wasm_backend.hpp's own `list_tools()`)
// into real `agentengine::ToolDescriptor`s (core/tool_pipeline.hpp) -- the piece that lets a wasm
// `ae:tool` plugin become a fourth CodeAct tool source alongside the agent's own tools,
// skill-unlocked tools, and MCP-discovered tools (core/codeact_tool_union.hpp), all going through
// the SAME real 006 §3 `invoke_tool` pipeline. Mirrors protocol/mcp/mcp_tool_bridge.hpp's shape
// deliberately -- same problem, same precedent, not a new pattern.
//
// I2: loading a wasm component grants NO ambient authority to call its tools. Each generated
// descriptor's `capability_ceiling` is exactly one `cap::ToolCall{plugin_id + "::" + tool_name}` --
// the session's own capability set must explicitly name that exact plugin-qualified tool, the same
// shape mcp_tool_bridge.hpp's own descriptors already require, never widened just because a
// component happens to be loaded. This is a DIFFERENT, independent authority layer from
// `WasmBackend::invoke_tool()`'s own internal `operator_grant`/`manifest.requested_capabilities`
// check (ADR-010 §3.2/§3.3) -- that layer answers "what may the plugin's OWN guest code touch
// (fs/http/secrets/...)"; this layer answers "may this run call this specific plugin tool at all."
// Two independent checks at two different points, not a double-binding conflict -- decisions/
// ADR-040-wasm-tool-pipeline-bridge.md §2 records why.
//
// Delimiter-collision guard: `PluginManifest.id` is plugin-author-supplied, not host-assigned, so
// naive concatenation would let `(plugin_id="x", tool="y::z")` and `(plugin_id="x::y", tool="z")`
// collide onto the same capability name. Rejecting the delimiter in either input, fail-closed,
// before building anything, is simpler and more robust than escaping (ADR-040 §2).
//
// Output-content scope (a fresh, output-direction decision -- distinct from ADR-010 §3.1's
// input-direction blob/tool-call residual): `core/tool_pipeline.hpp`'s step 9 wraps `invoke`'s
// entire return into exactly one `Data` `ContentItem` -- there is no path today for a success reply
// to become a `Media`/blob item. A wasm tool result containing a `Media` item anywhere fails closed
// (`wasm.tool_result_unsupported_content`) rather than silently dropping it; a `Data` item's `.json`
// is decoded back into the returned `json::Value`; a `Text` item wraps as `{"text": ...}`; empty
// content returns `null`. The wasm guest's own WIT `text-item.tainted` bit is irrelevant here --
// `invoke_tool()`'s step 9 unconditionally taints every successful result regardless, inherited for
// free, same as the MCP/native_jail bridges already document.
//
// Loader scope, named rather than silently assumed: this file does not call `create()` or
// `load_component()` -- it takes an already-loaded `handle` exactly as `mcp_tools_as_descriptors()`
// takes an already-connected `McpClient`. No production loader exists yet anywhere in this repo
// (ADR-040's own residual).

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/wasm/wasm_backend.hpp"

namespace agentengine::wasm {

// Split out from the invoke closure below as its own, directly testable pure function: the two
// mapping decisions it makes (is_error -> a real pipeline error; a Media content item anywhere ->
// fail closed, never silently dropped) both need proving against hand-built `agentengine::
// ToolResult` values in tests/test_wasm_tool_bridge.cpp WITHOUT requiring the cargo-component
// toolchain or a real wasm component -- the existing fixture (tests/fixtures/wasm_ae_tool_fixture)
// has no tool that returns `is_error: true` or a `Media` item, so exercising these paths end-to-end
// isn't possible without growing that fixture's scope. This function is the single source of truth
// for both the real bridge and its own unit tests, never a second reimplementation either could
// drift from.
[[nodiscard]] inline result<json::Value> wasm_tool_result_to_json(std::string const& tool_name,
                                                                    agentengine::ToolResult const& outcome) {
    if (outcome.is_error) {
        return std::unexpected(error{failure_class::contract,
                                      "wasm tool '" + tool_name + "' returned is_error: true",
                                      "wasm.tool_call_error"});
    }

    for (ContentItem const& item : outcome.content) {
        if (std::holds_alternative<Media>(item.value)) {
            return std::unexpected(
                error{failure_class::contract,
                      "wasm tool '" + tool_name +
                          "' result contains a Media content item, not supported by this bridge yet",
                      "wasm.tool_result_unsupported_content"});
        }
    }
    for (ContentItem const& item : outcome.content) {
        if (auto const* data = std::get_if<Data>(&item.value)) {
            auto parsed = json::parse(data->json);
            if (parsed) return *parsed;
            return json::Value::make_string(data->json);
        }
    }
    for (ContentItem const& item : outcome.content) {
        if (auto const* text = std::get_if<Text>(&item.value)) {
            return json::Value::make_object({{"text", json::Value::make_string(text->text)}});
        }
    }
    return json::Value::make_null();
}

[[nodiscard]] inline result<std::vector<agentengine::ToolDescriptor>> wasm_tools_as_descriptors_from(
    std::shared_ptr<WasmBackend> const& backend, SandboxHandle const& handle,
    std::string const& plugin_id, EffectContext& ctx) {
    // Fail closed before touching the backend at all -- an ambiguous plugin_id poisons every tool
    // this call would otherwise produce.
    if (plugin_id.find("::") != std::string::npos) {
        return std::unexpected(error{failure_class::contract,
                                      "wasm plugin id '" + plugin_id + "' must not contain '::'",
                                      "wasm.tool_name_ambiguous"});
    }

    auto listed = backend->list_tools(handle, ctx);
    if (!listed) return std::unexpected(listed.error());

    std::vector<agentengine::ToolDescriptor> out;
    out.reserve(listed->size());
    for (ToolDescriptor const& wasm_tool : *listed) {
        if (wasm_tool.name.find("::") != std::string::npos) {
            return std::unexpected(
                error{failure_class::contract,
                      "wasm tool name '" + wasm_tool.name + "' (plugin '" + plugin_id +
                          "') must not contain '::'",
                      "wasm.tool_name_ambiguous"});
        }

        agentengine::ToolDescriptor descriptor;
        descriptor.name = plugin_id + "::" + wasm_tool.name;
        descriptor.description = wasm_tool.description;
        descriptor.capability_ceiling = {cap::ToolCall{descriptor.name}};
        descriptor.args_schema_json = wasm_tool.args_schema_json;
        descriptor.reply_schema_json = wasm_tool.result_schema_json;
        // The invoke closure below holds a shared_ptr into live, mutable WasmBackend::instances_
        // state -- exactly the condition ADR-028's captures_session_state exists to flag, so
        // backgrounding logic never detaches a thread holding this reference with no
        // synchronization against WasmBackend::destroy() (tool_pipeline.hpp's own comment on this
        // field; ADR-040 §2 names the WasmBackend-specific instance of the same hazard).
        descriptor.captures_session_state = true;

        std::string const tool_name = wasm_tool.name;  // captured by value, not a reference into `*listed`
        descriptor.invoke = [backend, handle, tool_name](
                                 json::Value const& args, EffectContext& call_ctx) -> result<json::Value> {
            auto outcome =
                backend->invoke_tool(handle, ToolInvokeRequest{tool_name, json::dump(args)}, call_ctx);
            if (!outcome) return std::unexpected(outcome.error());
            return wasm_tool_result_to_json(tool_name, *outcome);
        };

        out.push_back(std::move(descriptor));
    }
    return out;
}

}  // namespace agentengine::wasm
