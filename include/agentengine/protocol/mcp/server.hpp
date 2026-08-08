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
//
// Milestone 7 Phase C4 (011 §3.6/§12, docs/research/2026-mcp-protocol-detail.md §12): the
// `io.modelcontextprotocol/tasks` extension for `tools/call` on a `Backgroundable` tool, real since
// Phase B's own `background_task()` (tool_pipeline.hpp) -- this dispatcher calls that SAME primitive
// directly rather than reinventing backgrounding, exactly the way `handle_tools_call` below already
// reuses `invoke_tool()` for the synchronous path. Extension opt-in is REQUEST-scoped (§12: "a server
// MUST NOT return CreateTaskResult to a client that did not include the extension capability on that
// request") -- read from `params.extensions` containing `"io.modelcontextprotocol/tasks"`, never a
// blanket accept (§3.6's "never assumed enabled on either side"). `tasks/get` polls; `tasks/cancel`
// marks a task cancelled so a subsequent poll never reports `"completed"` for it, but -- inheriting
// Phase B's own documented limit -- cannot stop the in-flight `std::thread` doing the real work; its
// eventual completion is simply discarded once the task is already `"cancelled"`. Per §12's own rule
// ("The failed status MUST NOT represent a tool result with isError:true -- that is completed with
// error details in result"), a tool that ran and failed is task status `"completed"` with
// `isError:true`, exactly mirroring `handle_tools_call`'s own synchronous isError mapping; this
// implementation has no real producer of task status `"failed"` (every failure this pipeline can
// detect before step 8 -- unknown tool, not-Backgroundable, capacity exceeded, approval denied --
// happens synchronously, before a task is ever created, and is rejected as a JSON-RPC error instead).
// `tasks/update` and `notifications/tasks` are NOT built (named, not silently claimed): update is MCP
// client-to-server input mid-task, which needs the MRTR `InputRequiredResult` wiring itself (a further
// C4+ follow-up, not yet built -- `ApprovalDecider` is a binary decider today, not a three-state one
// that could ever produce `"input_required"`); `notifications/tasks` needs a push channel this
// request/response-only dispatcher does not have.

#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
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

// §3.6/§12's extension identifier form `{vendor-prefix}/{extension-name}` -- the official
// `io.modelcontextprotocol/tasks` extension, the one supported here.
inline constexpr std::string_view kMcpTasksExtension = "io.modelcontextprotocol/tasks";

namespace server_detail {

// §12: "servers MUST generate [task ids] with sufficient entropy" -- a real `std::random_device`-seeded
// generator, not a predictable counter (a predictable id would let one caller guess another's task,
// exactly the bearer-token-shaped hazard §12 itself calls out).
[[nodiscard]] inline std::string generate_task_id() {
    std::random_device rd;
    std::mt19937_64     gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(gen) << dist(gen);
    return oss.str();
}

[[nodiscard]] inline bool request_wants_tasks_extension(JsonRpcRequest const& req) {
    json::Value const* extensions = req.params.find("extensions");
    if (!extensions || !extensions->is_array()) return false;
    for (json::Value const& item : extensions->as_array()) {
        if (item.is_string() && item.as_string() == kMcpTasksExtension) return true;
    }
    return false;
}

}  // namespace server_detail

class McpServer {
public:
    McpServer(ToolTable const& table, CapabilitySet const& held, ApprovalDecider approve,
              std::string server_name)
        : table_(table), held_(held), approve_(std::move(approve)), server_name_(std::move(server_name)) {}

    // Serves `server/discover`/`tools/list`/`tools/call`/`tasks/get`/`tasks/cancel` and nothing else --
    // an unrecognized method is `MethodNotFound`, never silently ignored.
    [[nodiscard]] JsonRpcResponse dispatch(JsonRpcRequest const& req) const {
        if (req.method == "server/discover") return handle_discover(req);
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        if (req.method == "tasks/get") return handle_tasks_get(req);
        if (req.method == "tasks/cancel") return handle_tasks_cancel(req);
        return JsonRpcResponse::make_error(
            req.id, JsonRpcError{kRpcMethodNotFound, "unknown method: " + req.method, json::Value{}});
    }

private:
    // One backgrounded task's server-side bookkeeping. Guarded by `tasks_mutex_` since
    // `background_task()`'s own `on_complete` fires from the DETACHED worker thread (tool_pipeline.hpp),
    // never the thread that called `dispatch()`.
    struct TaskRecord {
        std::string task_status = "working";  // "working" | "completed" | "cancelled"
        bool        has_result  = false;
        ToolResult  result;
    };

    [[nodiscard]] static json::Value task_to_json(std::string const& task_id, TaskRecord const& rec) {
        return json::Value::make_object({{"taskId", json::Value::make_string(task_id)},
                                          {"status", json::Value::make_string(rec.task_status)}});
    }
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
        ToolDescriptor const* tool = table_.find(name_field->as_string());
        if (!tool) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcMethodNotFound, "unknown tool: " + name_field->as_string(),
                                      json::Value{}});
        }

        // Phase C4 (§12): a per-request opt-in into the tasks extension backgrounds this call instead
        // of running it synchronously -- but only for a `Backgroundable` tool; a caller cannot force a
        // synchronous-only tool into the background any more than it could via `background_task()`
        // itself (tool_pipeline.hpp's own "an undeclared tool may never be backgrounded" rule).
        if (server_detail::request_wants_tasks_extension(req)) {
            if (!tool->backgroundable) {
                return JsonRpcResponse::make_error(
                    req.id, JsonRpcError{kRpcInvalidParams,
                                          "tool is not declared Backgroundable; the tasks extension "
                                          "cannot background it: " + name_field->as_string(),
                                          json::Value{}});
            }
            return handle_tools_call_as_task(req, name_field->as_string(), args_value);
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
             // an MRTR InputRequiredResult needs a three-state ApprovalDecider this codebase does not
             // have yet (see file-top comment), a further C4+ follow-up, not built here.
             {"resultType", json::Value::make_string("complete")}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    // Phase C4: starts the call via `background_task()` (tool_pipeline.hpp, real since Phase B) and
    // returns a task handle immediately instead of blocking. `current_background_count` is the number
    // of tasks THIS server currently considers `"working"` -- counted fresh, not a separately
    // maintained counter that could drift from `tasks_`'s own truth.
    //
    // Lifetime note: the completion lambda below captures `this` (non-owning), the same hazard
    // `AgentSession::start_background_task()`'s own closure already documents -- this `McpServer` must
    // outlive every task it starts. A real transport sub-phase, which owns the server for a connection's
    // whole lifetime, satisfies this; nothing here invents a stronger guarantee.
    [[nodiscard]] JsonRpcResponse handle_tools_call_as_task(JsonRpcRequest const& req,
                                                             std::string const& tool_name,
                                                             json::Value const& args_value) const {
        std::string const task_id = server_detail::generate_task_id();
        std::size_t live_count = 0;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            for (auto const& kv : tasks_) {
                if (kv.second.task_status == "working") ++live_count;
            }
            tasks_.emplace(task_id, TaskRecord{});
        }

        ToolCallRequest call{task_id, tool_name, args_value, /*arguments_tainted=*/true};
        EffectContext ctx;
        auto started = background_task(
            table_, held_, call, ctx, approve_, live_count,
            [this, task_id](ToolResult result, ToolInvocationAudit) {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto it = tasks_.find(task_id);
                if (it == tasks_.end()) return;
                // §12: a task already cancelled by `tasks/cancel` never flips back to `"completed"` --
                // the in-flight worker thread cannot actually be stopped (Phase B's own documented
                // limit), so its eventual result is simply discarded here.
                if (it->second.task_status == "cancelled") return;
                it->second.has_result  = true;
                it->second.result      = std::move(result);
                // §12: "The failed status MUST NOT represent a tool result with isError:true -- that is
                // completed with error details in result." A tool that ran and failed is still
                // `"completed"`, mirroring `handle_tools_call`'s own synchronous isError mapping exactly.
                it->second.task_status = "completed";
            });

        if (!started) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.erase(task_id);
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, started.error().message, json::Value{}});
        }

        std::lock_guard<std::mutex> lock(tasks_mutex_);
        json::Value result = json::Value::make_object({{"task", task_to_json(task_id, tasks_.at(task_id))}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    [[nodiscard]] JsonRpcResponse handle_tasks_get(JsonRpcRequest const& req) const {
        json::Value const* task_id_field = req.params.find("taskId");
        if (!task_id_field || !task_id_field->is_string()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "\"taskId\" must be a string", json::Value{}});
        }
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(task_id_field->as_string());
        if (it == tasks_.end()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "unknown taskId", json::Value{}});
        }
        TaskRecord const& rec = it->second;
        if (!rec.has_result) {
            json::Value result = json::Value::make_object({{"task", task_to_json(it->first, rec)}});
            return JsonRpcResponse::make_result(req.id, std::move(result));
        }
        // Same isError/content/resultType mapping `handle_tools_call`'s synchronous path uses -- a
        // caller that switches between a synchronous call and a polled task sees the identical result
        // shape either way, just delivered through a different method.
        json::Value result = json::Value::make_object(
            {{"task", task_to_json(it->first, rec)},
             {"content", tool_result_content_to_json(rec.result)},
             {"isError", json::Value::make_bool(rec.result.is_error)},
             {"resultType", json::Value::make_string("complete")}});
        return JsonRpcResponse::make_result(req.id, std::move(result));
    }

    [[nodiscard]] JsonRpcResponse handle_tasks_cancel(JsonRpcRequest const& req) const {
        json::Value const* task_id_field = req.params.find("taskId");
        if (!task_id_field || !task_id_field->is_string()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "\"taskId\" must be a string", json::Value{}});
        }
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(task_id_field->as_string());
        if (it == tasks_.end()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "unknown taskId", json::Value{}});
        }
        if (it->second.has_result) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "task already completed, cannot cancel",
                                      json::Value{}});
        }
        it->second.task_status = "cancelled";
        json::Value result = json::Value::make_object({{"task", task_to_json(it->first, it->second)}});
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
    mutable std::mutex                          tasks_mutex_;
    mutable std::unordered_map<std::string, TaskRecord> tasks_;
};

}  // namespace agentengine::mcp
