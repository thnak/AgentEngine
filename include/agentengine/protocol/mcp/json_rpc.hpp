#pragma once
// Implements 011-MCP-Conformance.md's wire layer: JSON-RPC 2.0, the transport-agnostic envelope every
// MCP request/response/notification rides (Streamable HTTP and stdio alike, 011 §7). Milestone 7
// Phase C1 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Deliberately generic JSON-RPC 2.0, not yet MCP-specific: no `_meta` (011 §2), no `resultType`
// discriminator (011 §3.4's MRTR), no MCP method-name vocabulary. Phase C2 (server role) and C3
// (client role) build MCP's OWN semantics on top of this envelope; this file only proves the envelope
// itself is spec-correct -- id round-tripping (String or Number, JSON-RPC 2.0 §5), notification-vs-
// request (id presence, §4), and result/error mutual exclusivity (§5) are true BY CONSTRUCTION (two
// named factories on `JsonRpcResponse`, a distinct `JsonRpcNotification` type), not by caller
// discipline a later phase could get wrong.

#include <optional>
#include <string>
#include <variant>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::mcp {

namespace json = agentengine::json;

// 011 §5's error-code partition, reused verbatim rather than re-derived per caller: "-32000..-32019
// implementation-defined ... -32020..-32099 reserved for the specification ... resource-not-found
// moved from -32002 to -32602 (Invalid Params)." The four bare JSON-RPC 2.0 codes below are the
// spec's own (jsonrpc.org §5.1), not an 011-specific addition.
inline constexpr int kRpcParseError     = -32700;
inline constexpr int kRpcInvalidRequest = -32600;
inline constexpr int kRpcMethodNotFound = -32601;
inline constexpr int kRpcInvalidParams  = -32602;
inline constexpr int kRpcInternalError  = -32603;
// 011 §5's revision-introduced codes, renumbered into the spec-reserved range at this revision.
inline constexpr int kMcpHeaderMismatch                 = -32020;
inline constexpr int kMcpMissingRequiredClientCapability = -32021;
inline constexpr int kMcpUnsupportedProtocolVersion      = -32022;

// JSON-RPC 2.0 §5's id type: a String or a Number (fractional discouraged, not forbidden by the
// spec). Null is reserved for "the server could not determine the request's id" and is never minted
// by anything in this codebase -- we always construct STRING ids for outgoing requests (011's own
// client role never needs numeric ids, and a string sidesteps every double-precision integer-identity
// question a Number id would raise); `double` is carried here only so a PEER's numeric id round-trips
// losslessly through us, never invented by us.
using RpcId = std::variant<std::string, double>;

struct JsonRpcRequest {
    RpcId       id;
    std::string method;
    json::Value params;  // object, array, or Value{} (absent) per §4
};

// §4: "A Notification is a Request object without an 'id' member." A distinct TYPE, not
// `JsonRpcRequest` with an optional id -- Phase C2's dispatcher never has to ask "does this need a
// response" via a runtime branch on a field; the type already answers it, and `parse_message()`'s own
// two-way `std::variant` return makes that the caller's compile-time-checked decision too.
struct JsonRpcNotification {
    std::string method;
    json::Value params;
};

struct JsonRpcError {
    int         code = 0;
    std::string message;
    json::Value data;  // json::Value{} (null-kind) when absent
};

// §5: result XOR error, enforced by construction (the two named factories below), not by a caller
// remembering to set exactly one of two optional fields and never both.
struct JsonRpcResponse {
    RpcId                       id;
    std::optional<json::Value>  result;
    std::optional<JsonRpcError> error;

    [[nodiscard]] static JsonRpcResponse make_result(RpcId id, json::Value result) {
        JsonRpcResponse r;
        r.id     = std::move(id);
        r.result = std::move(result);
        return r;
    }
    [[nodiscard]] static JsonRpcResponse make_error(RpcId id, JsonRpcError err) {
        JsonRpcResponse r;
        r.id    = std::move(id);
        r.error = std::move(err);
        return r;
    }
};

// What an incoming message on either side of a connection actually is -- a request awaiting a
// response, or a notification that gets none. Responses are parsed separately (`parse_response()`
// below): a caller reading off a transport already knows which shape it's expecting (a client reads
// responses to its own outstanding requests; a server reads requests/notifications from a peer), so
// there is no real "any JSON-RPC message" case this codebase needs to parse blind.
using IncomingMessage = std::variant<JsonRpcRequest, JsonRpcNotification>;

namespace detail {

[[nodiscard]] inline result<RpcId> id_from_json(json::Value const& v) {
    if (v.is_string()) return RpcId{v.as_string()};
    if (v.is_number()) return RpcId{v.as_number()};
    return std::unexpected(error{failure_class::contract, "id must be a string or a number",
                                  "jsonrpc.invalid_id"});
}

[[nodiscard]] inline json::Value id_to_json(RpcId const& id) {
    if (std::holds_alternative<std::string>(id)) return json::Value::make_string(std::get<std::string>(id));
    return json::Value::make_number(std::get<double>(id));
}

[[nodiscard]] inline result<void> check_jsonrpc_version(json::Value const& v) {
    json::Value const* ver = v.find("jsonrpc");
    if (!ver || !ver->is_string() || ver->as_string() != "2.0") {
        return std::unexpected(error{failure_class::contract, "missing or wrong \"jsonrpc\":\"2.0\"",
                                      "jsonrpc.wrong_version"});
    }
    return {};
}

}  // namespace detail

// Parses ONE incoming JSON-RPC 2.0 message. `id` PRESENT (any of string/number/null) -> a Request;
// `id` ABSENT -> a Notification (§4's own distinguishing rule, applied literally -- not "id is falsy",
// an explicit `"id": null` is still id-PRESENT and therefore a Request whose reply target the spec
// itself leaves ambiguous, which is the caller's problem to reject, not this parser's to silently
// reinterpret as a notification).
[[nodiscard]] inline result<IncomingMessage> parse_message(json::Value const& v) {
    if (auto ver = detail::check_jsonrpc_version(v); !ver) return std::unexpected(ver.error());
    json::Value const* method = v.find("method");
    if (!method || !method->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "missing or non-string \"method\"", "jsonrpc.missing_method"});
    }
    json::Value const* params = v.find("params");
    json::Value const  params_value = params ? *params : json::Value{};

    json::Value const* id = v.find("id");
    if (!id) {
        return IncomingMessage{JsonRpcNotification{method->as_string(), params_value}};
    }
    auto parsed_id = detail::id_from_json(*id);
    if (!parsed_id) return std::unexpected(parsed_id.error());
    return IncomingMessage{JsonRpcRequest{std::move(*parsed_id), method->as_string(), params_value}};
}

// Parses a peer's RESPONSE to one of OUR outgoing requests (client role). §5: exactly one of
// result/error must be present -- both or neither is a peer protocol violation, rejected rather than
// guessed at.
[[nodiscard]] inline result<JsonRpcResponse> parse_response(json::Value const& v) {
    if (auto ver = detail::check_jsonrpc_version(v); !ver) return std::unexpected(ver.error());
    json::Value const* id = v.find("id");
    if (!id) {
        return std::unexpected(
            error{failure_class::contract, "response is missing \"id\"", "jsonrpc.missing_id"});
    }
    auto parsed_id = detail::id_from_json(*id);
    if (!parsed_id) return std::unexpected(parsed_id.error());

    json::Value const* result_field = v.find("result");
    json::Value const* error_field  = v.find("error");
    if (static_cast<bool>(result_field) == static_cast<bool>(error_field)) {
        return std::unexpected(error{failure_class::contract,
                                      "response must carry exactly one of \"result\"/\"error\"",
                                      "jsonrpc.result_error_not_exclusive"});
    }
    if (result_field) return JsonRpcResponse::make_result(std::move(*parsed_id), *result_field);

    json::Value const* code    = error_field->find("code");
    json::Value const* message = error_field->find("message");
    if (!code || !code->is_number() || !message || !message->is_string()) {
        return std::unexpected(error{failure_class::contract,
                                      "\"error\" object missing numeric code / string message",
                                      "jsonrpc.malformed_error"});
    }
    json::Value const* data = error_field->find("data");
    JsonRpcError rpc_err{static_cast<int>(code->as_number()), message->as_string(),
                          data ? *data : json::Value{}};
    return JsonRpcResponse::make_error(std::move(*parsed_id), std::move(rpc_err));
}

[[nodiscard]] inline json::Value to_json(JsonRpcRequest const& r) {
    return json::Value::make_object(
        {{"jsonrpc", json::Value::make_string("2.0")}, {"id", detail::id_to_json(r.id)},
         {"method", json::Value::make_string(r.method)}, {"params", r.params}});
}

[[nodiscard]] inline json::Value to_json(JsonRpcNotification const& n) {
    return json::Value::make_object({{"jsonrpc", json::Value::make_string("2.0")},
                                      {"method", json::Value::make_string(n.method)},
                                      {"params", n.params}});
}

[[nodiscard]] inline json::Value to_json(JsonRpcResponse const& resp) {
    if (resp.result.has_value()) {
        return json::Value::make_object({{"jsonrpc", json::Value::make_string("2.0")},
                                          {"id", detail::id_to_json(resp.id)},
                                          {"result", *resp.result}});
    }
    JsonRpcError const& e = *resp.error;  // make_result()/make_error() guarantee exactly one is set
    std::vector<std::pair<std::string, json::Value>> err_members{
        {"code", json::Value::make_number(e.code)}, {"message", json::Value::make_string(e.message)}};
    if (!e.data.is_null()) err_members.emplace_back("data", e.data);
    return json::Value::make_object({{"jsonrpc", json::Value::make_string("2.0")},
                                      {"id", detail::id_to_json(resp.id)},
                                      {"error", json::Value::make_object(std::move(err_members))}});
}

}  // namespace agentengine::mcp
