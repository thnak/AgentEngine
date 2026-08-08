#pragma once
// Implements 011-MCP-Conformance.md §4 -- exposing AgentEngine's own ToolTable as an MCP server, over
// the JSON-RPC 2.0 envelope (protocol/mcp/json_rpc.hpp). Milestone 7 Phase C2
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Scope: `server/discover`, `tools/list` (from a real ToolTable -- 011 §4's own rule, "generated
// from tool metadata... one schema source, so an MCP listing cannot drift from the tool's actual
// contract"), `tools/call` (via `invoke_tool`). NO real transport yet (Streamable HTTP/stdio are a
// later sub-phase) -- this is a pure, transport-agnostic REQUEST DISPATCHER: hand it a
// `JsonRpcRequest`, get a `JsonRpcResponse` back, exactly the shape a later transport adapter wires
// bytes in and out of.
//
// `isError` vs JSON-RPC error (011 §3.1's "semantic split we honour on both sides"): a PROTOCOL error
// (unknown tool/method, malformed request) is a JSON-RPC error (`JsonRpcResponse::make_error`); an
// EXECUTION error (the tool ran and failed, including input validation failures) is a JSON-RPC
// RESULT whose body carries `isError: true` -- `ToolResult::is_error` (tool_pipeline.hpp) already IS
// exactly this distinction, so the mapping here is direct, not reinvented.
//
// Content mapping is narrower than the full MCP content-part vocabulary (text/image/audio/resource):
// every `ToolResult::content` item this codebase's own pipeline produces is currently either a
// `Data{json}` (success) or an `Error{message}` (failure) -- `tool_pipeline.hpp`'s own construction,
// not this file's choice -- so both map onto a single MCP `{"type":"text","text":...}` part. Richer
// content kinds (an image-returning tool, say) are a real, named follow-up once a tool that produces
// one exists; nothing here claims the full vocabulary.
//
// Authorization/principal establishment (011 §4: "every inbound request establishes a principal...
// and executes with that principal's capability set, never the host's") is TRANSPORT work -- a real
// inbound connection is what carries the credentials a principal is established FROM. This dispatcher
// takes `held`/`approve` as given (exactly `invoke_tool()`'s own step 4/5 inputs), the same layering
// `invoke_tool()` itself already has; a later transport sub-phase is what constructs `held` per
// inbound call from a real authenticated connection.

#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/protocol/mcp/json_rpc.hpp"

namespace agentengine::mcp {

// One entry of `tools/list`'s result array (011 §3.1/§4). `input_schema`/`output_schema` are
// `ToolDescriptor::args_schema_json`/`reply_schema_json`, parsed into real JSON so `tools/list`
// carries actual schema objects, not schema-shaped strings. Deliberately narrower than the full spec
// shape (no `annotations` -- `readOnlyHint`/`destructiveHint`/etc., no per-primitive `ttlMs`/
// `cacheScope` -- named here, not silently claimed; a tool declaration surface for those hints does
// not exist in `core/tool.hpp` yet).
struct McpToolListEntry {
    std::string name;
    std::string description;
    json::Value input_schema;
    json::Value output_schema;
};

[[nodiscard]] inline result<McpToolListEntry> to_mcp_tool_list_entry(ToolDescriptor const& d) {
    auto input = json::parse(d.args_schema_json);
    if (!input) return std::unexpected(input.error());
    auto output = json::parse(d.reply_schema_json);
    if (!output) return std::unexpected(output.error());
    return McpToolListEntry{d.name, d.description, std::move(*input), std::move(*output)};
}

// See file-top comment: every `ToolResult::content` item this pipeline produces today is Data or
// Error; both become one `{"type":"text","text":...}` part.
[[nodiscard]] inline json::Value tool_result_content_to_json(ToolResult const& r) {
    std::vector<json::Value> parts;
    for (ContentItem const& item : r.content) {
        std::string text;
        if (auto const* data = std::get_if<Data>(&item.value)) {
            text = data->json;
        } else if (auto const* err = std::get_if<Error>(&item.value)) {
            text = err->message;
        } else if (auto const* t = std::get_if<Text>(&item.value)) {
            text = t->text;
        }
        parts.push_back(json::Value::make_object(
            {{"type", json::Value::make_string("text")}, {"text", json::Value::make_string(text)}}));
    }
    return json::Value::make_array(std::move(parts));
}

class McpServer {
public:
    McpServer(ToolTable const& table, CapabilitySet const& held, ApprovalDecider approve,
              std::string server_name)
        : table_(table), held_(held), approve_(std::move(approve)), server_name_(std::move(server_name)) {}

    // Serves `server/discover`/`tools/list`/`tools/call` and nothing else -- an unrecognized method
    // is `MethodNotFound`, never silently ignored.
    [[nodiscard]] JsonRpcResponse dispatch(JsonRpcRequest const& req) const {
        if (req.method == "server/discover") return handle_discover(req);
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        return JsonRpcResponse::make_error(
            req.id, JsonRpcError{kRpcMethodNotFound, "unknown method: " + req.method, json::Value{}});
    }

private:
    [[nodiscard]] JsonRpcResponse handle_discover(JsonRpcRequest const& req) const {
        // 011 §12/§13 Q1: "as a server, 2026-07-28-only" -- a fixed literal, not a negotiated value,
        // matching that resolved open question exactly.
        json::Value result = json::Value::make_object(
            {{"protocolVersion", json::Value::make_string("2026-07-28")},
             {"serverInfo", json::Value::make_object({{"name", json::Value::make_string(server_name_)}})}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    [[nodiscard]] JsonRpcResponse handle_tools_list(JsonRpcRequest const& req) const {
        std::vector<json::Value> items;
        // 011 §3.1: "preserve server order and never re-sort" -- ToolTable::descriptors() is already
        // registration order, handed straight through.
        for (ToolDescriptor const& d : table_.descriptors()) {
            auto entry = to_mcp_tool_list_entry(d);
            if (!entry) {
                // A tool's own generated schema fails to parse as JSON -- our own bug, an internal
                // error, never surfaced as if the CALLER's request was malformed.
                return JsonRpcResponse::make_error(
                    req.id, JsonRpcError{kRpcInternalError,
                                          "tool schema failed to parse as JSON: " + entry.error().message,
                                          json::Value{}});
            }
            items.push_back(json::Value::make_object(
                {{"name", json::Value::make_string(entry->name)},
                 {"description", json::Value::make_string(entry->description)},
                 {"inputSchema", entry->input_schema},
                 {"outputSchema", entry->output_schema}}));
        }
        json::Value result = json::Value::make_object({{"tools", json::Value::make_array(std::move(items))}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    [[nodiscard]] JsonRpcResponse handle_tools_call(JsonRpcRequest const& req) const {
        json::Value const* name_field = req.params.find("name");
        if (!name_field || !name_field->is_string()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "\"name\" must be a string", json::Value{}});
        }
        json::Value const* args_field = req.params.find("arguments");
        json::Value const  args_value = args_field ? *args_field : json::Value::make_object({});

        // Unknown tool is a PROTOCOL error (JSON-RPC error), not an execution result -- resolution
        // (006 §3 step 1, `invoke_tool()`'s own first branch) failing means the call never reached
        // step 2 onward, the same "protocol errors (unknown tool...) are JSON-RPC errors" split 011
        // §3.1 states for the client side, applied symmetrically here on the server side.
        if (!table_.find(name_field->as_string())) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcMethodNotFound, "unknown tool: " + name_field->as_string(),
                                      json::Value{}});
        }

        // An inbound MCP tool call is external input (003 §2) -- arguments_tainted=true unconditionally,
        // the same "a tool result is external content" provenance discipline `invoke_tool()` already
        // applies to what it RETURNS, applied here to what it RECEIVES from an MCP peer.
        ToolCallRequest call{req_id_to_call_id(req.id), name_field->as_string(), args_value,
                              /*arguments_tainted=*/true};
        EffectContext ctx;
        ToolResult tool_result = invoke_tool(table_, held_, call, ctx, approve_);

        // 011 §3.1: "isError vs JSON-RPC error is a semantic split we honour on both sides": an
        // execution error -- including an input-validation failure `invoke_tool()` itself already
        // detected -- is a RESULT with isError:true, never a JSON-RPC error.
        json::Value result = json::Value::make_object(
            {{"content", tool_result_content_to_json(tool_result)},
             {"isError", json::Value::make_bool(tool_result.is_error)},
             // 011 §3.4: every result now carries a required resultType; ours is always "complete" --
             // an MRTR InputRequiredResult is Phase C4's own follow-up (needs Backgroundable/
             // StandingEffect's suspend shape wired to a real tool, not built here).
             {"resultType", json::Value::make_string("complete")}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    [[nodiscard]] static std::string req_id_to_call_id(RpcId const& id) {
        if (std::holds_alternative<std::string>(id)) return std::get<std::string>(id);
        return std::to_string(static_cast<long long>(std::get<double>(id)));
    }

    ToolTable const&     table_;
    CapabilitySet const& held_;
    ApprovalDecider       approve_;
    std::string          server_name_;
};

}  // namespace agentengine::mcp
