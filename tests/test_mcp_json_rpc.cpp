// Milestone 7 Phase C1 (011-MCP-Conformance.md's wire layer, docs/planning/milestone-7-protocol-
// conformance-breakdown.md). Proves protocol/mcp/json_rpc.hpp's envelope is spec-correct: request vs
// notification is decided by id PRESENCE (JSON-RPC 2.0 §4), a response carries exactly one of
// result/error (§5), string and numeric ids both round-trip losslessly, and malformed envelopes
// (wrong/missing "jsonrpc" version, a response with both or neither of result/error, a non-string/
// non-number id) are rejected rather than guessed at.

#include <cstdio>
#include <string>

#include "agentengine/protocol/mcp/json_rpc.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    namespace mcp  = agentengine::mcp;
    namespace json = agentengine::json;

    // --- C1-1: a request with a string id round-trips through to_json -> parse_message -----------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"req-1"}}, "tools/list",
                                 json::Value::make_object({{"cursor", json::Value::make_string("")}})};
        json::Value wire = mcp::to_json(req);
        auto parsed = mcp::parse_message(wire);
        check(parsed.has_value(), "C1-1: a well-formed request parses");
        if (parsed.has_value()) {
            auto const* r = std::get_if<mcp::JsonRpcRequest>(&*parsed);
            check(r != nullptr, "C1-1: it round-trips as a Request, not a Notification");
            if (r) {
                check(r->method == "tools/list", "C1-1: method round-trips");
                check(std::holds_alternative<std::string>(r->id) &&
                          std::get<std::string>(r->id) == "req-1",
                      "C1-1: the string id round-trips exactly");
                check(r->params.find("cursor") != nullptr &&
                          r->params.find("cursor")->as_string().empty(),
                      "C1-1: an empty-string cursor round-trips as itself, not as absent (011 §3.1: "
                      "\"an empty string is a valid cursor, not end-of-results\")");
            }
        }
    }

    // --- C1-2: a request with a NUMERIC id round-trips too (§5: id MAY be a Number) ---------------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{7.0}, "ping", json::Value{}};
        auto parsed = mcp::parse_message(mcp::to_json(req));
        check(parsed.has_value(), "C1-2: a numeric-id request parses");
        if (parsed.has_value()) {
            auto const* r = std::get_if<mcp::JsonRpcRequest>(&*parsed);
            check(r && std::holds_alternative<double>(r->id) && std::get<double>(r->id) == 7.0,
                  "C1-2: the numeric id round-trips exactly");
        }
    }

    // --- C1-3: a notification (no "id" at all) parses as a Notification, never a Request ----------
    {
        mcp::JsonRpcNotification note{"notifications/progress",
                                       json::Value::make_object({{"progress", json::Value::make_number(1)}})};
        auto parsed = mcp::parse_message(mcp::to_json(note));
        check(parsed.has_value(), "C1-3: a well-formed notification parses");
        if (parsed.has_value()) {
            check(std::holds_alternative<mcp::JsonRpcNotification>(*parsed),
                  "C1-3: §4's own rule -- absent id means Notification, not Request with an implicit id");
        }
    }

    // --- C1-4: an explicit "id": null is still id-PRESENT -- rejected, not silently reinterpreted -
    // --- as a notification (this codebase never guesses at an ambiguous peer message).           ---
    {
        json::Value wire = json::Value::make_object(
            {{"jsonrpc", json::Value::make_string("2.0")}, {"id", json::Value::make_null()},
             {"method", json::Value::make_string("x")}});
        auto parsed = mcp::parse_message(wire);
        check(!parsed.has_value(), "C1-4: an explicit null id is rejected (id must be string or number)");
        if (!parsed.has_value()) {
            check(parsed.error().code == "jsonrpc.invalid_id",
                  "C1-4: rejected with the real error_code");
        }
    }

    // --- C1-5: a response carrying a result round-trips; error is absent ---------------------------
    {
        mcp::JsonRpcResponse resp = mcp::JsonRpcResponse::make_result(
            mcp::RpcId{std::string{"req-1"}}, json::Value::make_object({{"tools", json::Value::make_array({})}}));
        auto parsed = mcp::parse_response(mcp::to_json(resp));
        check(parsed.has_value(), "C1-5: a well-formed result response parses");
        if (parsed.has_value()) {
            check(parsed->result.has_value() && !parsed->error.has_value(),
                  "C1-5: result XOR error holds after round-tripping a result response");
        }
    }

    // --- C1-6: a response carrying an error (with optional data) round-trips -----------------------
    {
        mcp::JsonRpcResponse resp = mcp::JsonRpcResponse::make_error(
            mcp::RpcId{std::string{"req-2"}},
            mcp::JsonRpcError{mcp::kRpcMethodNotFound, "unknown method",
                               json::Value::make_string("tools/frobnicate")});
        auto parsed = mcp::parse_response(mcp::to_json(resp));
        check(parsed.has_value(), "C1-6: a well-formed error response parses");
        if (parsed.has_value()) {
            check(!parsed->result.has_value() && parsed->error.has_value(),
                  "C1-6: result XOR error holds after round-tripping an error response");
            check(parsed->error->code == mcp::kRpcMethodNotFound, "C1-6: the error code round-trips");
            check(!parsed->error->data.is_null(), "C1-6: optional error.data round-trips when present");
        }
    }

    // --- C1-7: a response with NEITHER result nor error is rejected --------------------------------
    {
        json::Value wire = json::Value::make_object(
            {{"jsonrpc", json::Value::make_string("2.0")}, {"id", json::Value::make_string("x")}});
        auto parsed = mcp::parse_response(wire);
        check(!parsed.has_value(), "C1-7: a response with neither result nor error is rejected");
        if (!parsed.has_value()) {
            check(parsed.error().code == "jsonrpc.result_error_not_exclusive",
                  "C1-7: rejected with the real error_code");
        }
    }

    // --- C1-8: a response with BOTH result and error is rejected -- never silently prefer one -----
    {
        json::Value wire = json::Value::make_object(
            {{"jsonrpc", json::Value::make_string("2.0")},
             {"id", json::Value::make_string("x")},
             {"result", json::Value::make_bool(true)},
             {"error", json::Value::make_object({{"code", json::Value::make_number(-1)},
                                                  {"message", json::Value::make_string("m")}})}});
        auto parsed = mcp::parse_response(wire);
        check(!parsed.has_value(), "C1-8: a response with BOTH result and error is rejected");
        if (!parsed.has_value()) {
            check(parsed.error().code == "jsonrpc.result_error_not_exclusive",
                  "C1-8: rejected with the same result_error_not_exclusive code as the neither case");
        }
    }

    // --- C1-9: a missing/wrong "jsonrpc" version is rejected on every parse path -------------------
    {
        json::Value bad_version = json::Value::make_object(
            {{"jsonrpc", json::Value::make_string("1.0")},
             {"id", json::Value::make_string("x")},
             {"method", json::Value::make_string("m")}});
        auto parsed_msg = mcp::parse_message(bad_version);
        check(!parsed_msg.has_value(), "C1-9a: parse_message rejects a wrong \"jsonrpc\" version");

        json::Value missing_version = json::Value::make_object(
            {{"id", json::Value::make_string("x")}, {"result", json::Value::make_bool(true)}});
        auto parsed_resp = mcp::parse_response(missing_version);
        check(!parsed_resp.has_value(), "C1-9b: parse_response rejects a missing \"jsonrpc\" field");
    }

    // --- C1-10: a request missing "method" is rejected ----------------------------------------------
    {
        json::Value wire = json::Value::make_object(
            {{"jsonrpc", json::Value::make_string("2.0")}, {"id", json::Value::make_string("x")}});
        auto parsed = mcp::parse_message(wire);
        check(!parsed.has_value(), "C1-10: a message with no \"method\" is rejected");
        if (!parsed.has_value()) {
            check(parsed.error().code == "jsonrpc.missing_method", "C1-10: rejected with the real error_code");
        }
    }

    if (g_failures == 0) {
        std::printf("test_mcp_json_rpc: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_json_rpc: %d failure(s)\n", g_failures);
    return 1;
}
