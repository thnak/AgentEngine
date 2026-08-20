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

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/protocol/mcp/json_rpc.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secure_random.hpp"

namespace agentengine::mcp {

// ADR-061 §39/§40 (decisions/ADR-061-host-provided-inbound-transport.md): a per-request capability
// grant `McpServer::dispatch()` MAY consult instead of `held_` (the construction-time capability set).
// Unblocked by §20.3's earlier fix, which made `EffectContext::capabilities` a `shared_ptr` rather
// than a raw borrowed pointer -- the specific hazard that made a per-request, function-local
// `CapabilitySet` unsafe to hand to a detached background thread (`handle_tools_call_as_task()`'s own
// `this`-outlives-every-task contract, below) no longer applies once the grant is copied in as an
// owned, refcounted value.
//
// REPLACE semantics, not a narrowing bound (matching `RequestAuthority`'s own already-Judged
// precedent, §20.1: when present, authority replaces the session-level default entirely, never
// intersected with it) -- named `CapabilityGrant`, not "Ceiling," for exactly that reason. A
// per-request grant MAY exceed what `held_` itself grants; `held_` is this object's OWN
// construction-time ceiling, not an upper bound imposed on every future per-request credential a host
// might separately verify.
//
// Carries a `Principal` deliberately: `principal` is who this grant was actually verified for, and
// `dispatch()` checks it against its own `caller` argument via `principal_admitted_for()` before ever
// using the grant. Without this, `caller` and the grant would be two independently-suppliable values
// with nothing checking they describe the same party -- a confused-deputy vector (a caller could
// supply `caller = Alice` alongside a grant verified for Bob; the effect would be attributed to Alice
// while authorized by Bob's grant). Bundling identity with the capability grant is what
// `RequestAuthority` already does for exactly this reason (ADR-061 §20.1); this type restores that
// precedent rather than dropping the field to (incorrectly) avoid it.
// ae-naming-lint: allow CapabilityGrant — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct CapabilityGrant {
    Principal principal;
    std::shared_ptr<CapabilitySet const> capabilities;  // never null once accepted -- dispatch() guards
    std::chrono::steady_clock::time_point expiry{};
    [[nodiscard]] bool live(std::chrono::steady_clock::time_point now) const noexcept {
        return now < expiry;
    }
};

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

// §12: "servers MUST generate [task ids] with sufficient entropy" and §8a: "handles are
// high-entropy".
//
// ADR-061 §7 R3: this was a `std::random_device`-seeded `std::mt19937_64`, whose own comment claimed
// it satisfied §12 because it was "not a predictable counter." That bar is wrong -- MT19937 is not a
// CSPRNG, and its internal state is fully recoverable from 312 consecutive 64-bit outputs (156 task
// creations, at two draws each), after which every future task id is predictable. Now a real system
// CSPRNG (trust/secure_random.hpp), shared with `protocol/a2a/server.hpp` rather than duplicated.
//
// Fails closed: a handle that cannot be generated securely is not generated at all.
[[nodiscard]] inline result<std::string> generate_task_id() {
    return trust::secure_random_hex(16);
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
    //
    // ADR-061 §7 R3: `caller` is the principal this REQUEST established, supplied per call. It is
    // required, with no defaulted overload, because a default would reintroduce exactly the
    // fail-open R2 found on the A2A path (`StartRun::caller`'s `nullopt`-skips-admission default,
    // relied on by a protocol surface). 011 §4 states the rule this parameter exists to make
    // possible: "every inbound request establishes a principal... and executes with that
    // principal's capability set, never the host's."
    //
    // `caller` currently binds the TASK STORE (§8a's "bound server-side as `<user_id>:<handle>`")
    // unconditionally, and -- since ADR-061 §39/§40 -- ALSO selects the capability set whenever a
    // `grant` is supplied: `held_` (construction-time, per-server) remains the default, but a live,
    // principal-admitted `grant` replaces it for this one call, closing ADR-021 §8's "per-request,
    // never per-connection" constraint (ADR-061 §7 R16/R27) for capability authority the same way
    // `caller` itself already closed it for identity. `grant`/`now` are additive, defaulted parameters
    // -- every pre-existing 2-argument call is unaffected (§39.4 claim 1).
    [[nodiscard]] JsonRpcResponse dispatch(
            JsonRpcRequest const& req, Principal const& caller,
            std::optional<CapabilityGrant> const& grant = std::nullopt,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        if (grant.has_value()) {
            // Three independent fail-closed guards, all required before a grant is ever consulted.
            if (!grant->capabilities) {
                return JsonRpcResponse::make_error(
                    req.id, JsonRpcError{kRpcInvalidRequest, "capability grant has no capabilities",
                                          json::Value{}});
            }
            if (!grant->live(now)) {
                return JsonRpcResponse::make_error(
                    req.id, JsonRpcError{kRpcInvalidRequest, "capability grant expired", json::Value{}});
            }
            // Identity binding: refuses a grant verified for a DIFFERENT principal than `caller`
            // claims to be -- see CapabilityGrant's own comment for why this check exists at all.
            if (!principal_admitted_for(caller, grant->principal)) {
                return JsonRpcResponse::make_error(
                    req.id, JsonRpcError{kRpcInvalidRequest,
                                          "capability grant does not admit this caller", json::Value{}});
            }
        }
        // Blanket, unconditional across every method including `server/discover`/`tools/list` (which
        // consult no capability today) -- deliberate: matches AgentSession's own `require_authority_`
        // precedent exactly, which has no read-only exemption either (rt::AgentSession::start_run()/
        // resolve_interaction() both apply the identical admission check unconditionally, ADR-061
        // §20.4). A host that supplies a `grant` at all is choosing per-request mode for this call; a
        // malformed grant is symptomatic of a broken credential, not something safe to partially honor.
        if (req.method == "server/discover") return handle_discover(req);
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req, caller, grant);
        if (req.method == "tasks/get") return handle_tasks_get(req, caller);
        if (req.method == "tasks/cancel") return handle_tasks_cancel(req, caller);
        return JsonRpcResponse::make_error(
            req.id, JsonRpcError{kRpcMethodNotFound, "unknown method: " + req.method, json::Value{}});
    }

private:
    // One backgrounded task's server-side bookkeeping. Guarded by `tasks_mutex_` since
    // `background_task()`'s own `on_complete` fires from the DETACHED worker thread (tool_pipeline.hpp),
    // never the thread that called `dispatch()`.
    //
    // ADR-061 §7 R3: `owner` implements 011 §8a's binding requirement -- "handles are... bound
    // server-side as `<user_id>:<handle>` where the user id comes from the verified token, never
    // from the client." Held as a field rather than mangled into the map key so the wire-visible
    // taskId is unchanged while the binding is enforced on every lookup.
    struct TaskRecord {
        std::string task_status = "working";  // "working" | "completed" | "cancelled"
        bool        has_result  = false;
        ToolResult  result;
        Principal   owner;
    };

    // 018 §6: a cross-tenant id collision is not ownership. Mirrors `principal_admitted_for`'s own
    // tenant-first rule (trust/principal.hpp) rather than re-deriving it, and additionally refuses
    // an empty id outright: `McpServer` used to default-construct `EffectContext`, so `Principal{}`
    // with an empty id is a real value that reaches this code, and treating two empty ids as "the
    // same principal" would make every unauthenticated caller the owner of every unauthenticated
    // caller's tasks (ADR-061 §7 R13).
    [[nodiscard]] static bool owned_by(TaskRecord const& rec, Principal const& caller) {
        if (caller.id.empty() || rec.owner.id.empty()) return false;
        return rec.owner.id == caller.id && rec.owner.tenant_id == caller.tenant_id;
    }

    // §4/§8a: a caller who does not own a task must not be able to distinguish it from one that was
    // never created. One construction site for both cases, so they cannot drift apart.
    [[nodiscard]] static JsonRpcResponse unknown_task_response(RpcId const& id) {
        return JsonRpcResponse::make_error(
            id, JsonRpcError{kRpcInvalidParams, "unknown taskId", json::Value{}});
    }

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

    [[nodiscard]] JsonRpcResponse handle_tools_call(
            JsonRpcRequest const& req, Principal const& caller,
            std::optional<CapabilityGrant> const& grant) const {
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
            return handle_tools_call_as_task(req, name_field->as_string(), args_value, caller, grant);
        }

        // An inbound MCP tool call is external input (003 §2) -- arguments_tainted=true unconditionally,
        // the same "a tool result is external content" provenance discipline `invoke_tool()` already
        // applies to what it RETURNS, applied here to what it RECEIVES from an MCP peer.
        ToolCallRequest call{req_id_to_call_id(req.id), name_field->as_string(), args_value,
                              /*arguments_tainted=*/true};
        // ADR-061 §7 R4/R26: `ctx` was default-constructed here, so `ctx.principal` was an empty
        // `Principal{}` and `ctx.run_id` was "" on every inbound call. Two consequences, both real:
        // 007 §8 requires the principal on every audit record and got none, and `IdempotencyKey`
        // ({run_id, turn_index, call_index, argument_digest}, tool_pipeline.hpp) collapsed to
        // ":0:0:<digest>" for EVERY caller and tenant -- so a journal deduping on it could serve one
        // principal a result computed for another. Carrying the real caller here is the identity half
        // of that fix; `IdempotencyKey`'s own derivation is fixed in tool_pipeline.hpp.
        //
        // ADR-061 §39/§40: `effective` is `held_` unless `grant` was supplied (already validated live
        // and principal-admitted by `dispatch()`), in which case it REPLACES `held_` for this call.
        // `ctx.capabilities` is populated ONLY on the grant-supplied path -- deliberately NOT from
        // `held_` on the no-grant path, which stays exactly as before this section: real, already-
        // shipped consumers (`native_jail::has_capability()`, `trust::require_secret_capability()`)
        // treat a null `ctx.capabilities` as deny-all today, and populating it unconditionally would
        // silently flip that fail-closed behavior for any shell/secret-consuming tool served through
        // this dispatcher -- a real authorization change this section does not make as a side effect.
        CapabilitySet const& effective = grant.has_value() ? *grant->capabilities : held_;
        EffectContext ctx;
        ctx.principal = caller;
        if (grant.has_value()) ctx.capabilities = grant->capabilities;
        ToolResult tool_result = invoke_tool(table_, effective, call, ctx, approve_);

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
    [[nodiscard]] JsonRpcResponse handle_tools_call_as_task(
            JsonRpcRequest const& req, std::string const& tool_name, json::Value const& args_value,
            Principal const& caller, std::optional<CapabilityGrant> const& grant) const {
        // ADR-061 §7 R3: fails closed if the CSPRNG fails, rather than minting a weaker handle.
        result<std::string> minted = server_detail::generate_task_id();
        if (!minted) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInternalError, minted.error().message, json::Value{}});
        }
        std::string const task_id = *minted;
        std::size_t live_count = 0;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            for (auto const& kv : tasks_) {
                if (kv.second.task_status == "working") ++live_count;
            }
            // §8a's binding, recorded at creation: the establishing principal, not the client's
            // claim about who it is.
            TaskRecord rec{};
            rec.owner = caller;
            tasks_.emplace(task_id, std::move(rec));
        }

        ToolCallRequest call{task_id, tool_name, args_value, /*arguments_tainted=*/true};
        // ADR-061 §39/§40: same rule as the synchronous path above -- `effective` replaces `held_`
        // only when `grant` was supplied (already validated by `dispatch()`), and `ctx.capabilities`
        // is populated only in that same case. `ctx` is copied BY VALUE into `background_task()`'s
        // detached closure below (tool_pipeline.hpp), so `grant->capabilities` (an owned, refcounted
        // shared_ptr) survives into the background thread regardless of this call having already
        // returned -- the property `EffectContext::capabilities` becoming a shared_ptr (§20.3) made
        // possible, and the reason this fix could not have been written before that one landed.
        CapabilitySet const& effective = grant.has_value() ? *grant->capabilities : held_;
        EffectContext ctx;
        ctx.principal = caller;  // ADR-061 §7 R4/R26, same reasoning as the synchronous path above.
        if (grant.has_value()) ctx.capabilities = grant->capabilities;
        auto started = background_task(
            table_, effective, call, ctx, approve_, live_count,
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

    // ADR-061 §7 R3: the ownership check runs before ANY observable difference -- a non-owner gets
    // the identical response an unknown id produces, including for a task that exists and has a
    // result waiting. Returning anything else would leak existence, which §4 forbids.
    [[nodiscard]] JsonRpcResponse handle_tasks_get(JsonRpcRequest const& req,
                                                    Principal const& caller) const {
        json::Value const* task_id_field = req.params.find("taskId");
        if (!task_id_field || !task_id_field->is_string()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "\"taskId\" must be a string", json::Value{}});
        }
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(task_id_field->as_string());
        if (it == tasks_.end() || !owned_by(it->second, caller)) {
            return unknown_task_response(req.id);
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

    // ADR-061 §7 R3, same ordering rule as `handle_tasks_get`: ownership before the
    // already-completed rejection, so a non-owner cannot distinguish "exists but finished" from
    // "no such task".
    [[nodiscard]] JsonRpcResponse handle_tasks_cancel(JsonRpcRequest const& req,
                                                       Principal const& caller) const {
        json::Value const* task_id_field = req.params.find("taskId");
        if (!task_id_field || !task_id_field->is_string()) {
            return JsonRpcResponse::make_error(
                req.id, JsonRpcError{kRpcInvalidParams, "\"taskId\" must be a string", json::Value{}});
        }
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(task_id_field->as_string());
        if (it == tasks_.end() || !owned_by(it->second, caller)) {
            return unknown_task_response(req.id);
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
