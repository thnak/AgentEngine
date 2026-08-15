// Milestone 7 Phase C2 (011-MCP-Conformance.md §4, docs/planning/milestone-7-protocol-conformance-
// breakdown.md). Proves McpServer (protocol/mcp/server.hpp) dispatches server/discover, tools/list,
// and tools/call correctly against a REAL ToolTable and the REAL 10-step tool pipeline
// (tool_pipeline.hpp::invoke_tool) -- and specifically that 011 §3.1's "isError vs JSON-RPC error"
// split is honoured: an unknown tool/method is a JSON-RPC error, while a tool that ran and failed is
// a JSON-RPC RESULT with isError:true.

#include <cstdio>
#include <string>

#include "agentengine/protocol/mcp/server.hpp"

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

namespace ae   = agentengine;
namespace mcp  = agentengine::mcp;
namespace json = agentengine::json;

struct EchoArgs {
    std::string text;
};
AE_JSON_SCHEMA(EchoArgs, text)
struct EchoReply {
    std::string text;
};
AE_JSON_SCHEMA(EchoReply, text)

struct EchoTool : ae::Tool<EchoTool> {
    static constexpr std::string_view name        = "echo";
    static constexpr std::string_view description = "Echoes its input text back.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.text}; }
};

struct FailArgs {
    bool noop;
};
AE_JSON_SCHEMA(FailArgs, noop)
struct FailReply {
    bool ok;
};
AE_JSON_SCHEMA(FailReply, ok)

// Always fails -- proves 011 §3.1's isError:true result path, distinct from an unknown-tool
// JSON-RPC error.
struct AlwaysFailsTool : ae::Tool<AlwaysFailsTool> {
    static constexpr std::string_view name        = "always_fails";
    static constexpr std::string_view description = "Always returns an execution error.";
    using Args  = FailArgs;
    using Reply = FailReply;
    static ae::result<Reply> invoke(FailArgs, ae::EffectContext&) {
        return std::unexpected(
            ae::error{ae::failure_class::contract, "deliberate failure", "test.deliberate_failure"});
    }
};

}  // namespace

int main() {
    auto const table = ae::ToolTable::from_tools<EchoTool, AlwaysFailsTool>();
    ae::CapabilitySet const held;  // neither tool declares a Capabilities<...> ceiling
    mcp::McpServer server(table, held, ae::ApprovalDecider{}, "agentengine-test-server");
    // ADR-023 §7 R3: `dispatch()` now requires the principal the request established.
    ae::Principal const kCaller = ae::make_local_cli_principal("test-caller", "test-tenant");

    // --- C2-1: server/discover -----------------------------------------------------------------------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"r1"}}, "server/discover", json::Value{}};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(resp.result.has_value() && !resp.error.has_value(), "C2-1: server/discover succeeds");
        if (resp.result.has_value()) {
            auto const* ver = resp.result->find("protocolVersion");
            check(ver && ver->is_string() && ver->as_string() == "2026-07-28",
                  "C2-1: protocolVersion is the real, fixed revision (011 §12: server speaks only "
                  "the modern era)");
            auto const* info = resp.result->find("serverInfo");
            check(info && info->find("name") && info->find("name")->as_string() == "agentengine-test-server",
                  "C2-1: serverInfo.name carries what the server was constructed with");
        }
    }

    // --- C2-2: tools/list returns both real tools, in registration order, with real schemas --------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"r2"}}, "tools/list", json::Value{}};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(resp.result.has_value(), "C2-2: tools/list succeeds");
        if (resp.result.has_value()) {
            auto const* tools = resp.result->find("tools");
            check(tools && tools->is_array() && tools->as_array().size() == 2,
                  "C2-2: both registered tools appear");
            if (tools && tools->as_array().size() == 2) {
                auto const& first = tools->as_array()[0];
                check(first.find("name")->as_string() == "echo",
                      "C2-2: tools appear in registration order -- echo first");
                check(first.find("inputSchema")->is_object(),
                      "C2-2: inputSchema is a real parsed JSON object, not a schema-shaped string");
                auto const& second = tools->as_array()[1];
                check(second.find("name")->as_string() == "always_fails",
                      "C2-2: always_fails is second, matching registration order");
            }
        }
    }

    // --- C2-3: tools/call on echo succeeds -- isError:false, content carries the real reply --------
    {
        mcp::JsonRpcRequest req{
            mcp::RpcId{std::string{"r3"}}, "tools/call",
            json::Value::make_object(
                {{"name", json::Value::make_string("echo")},
                 {"arguments", json::Value::make_object({{"text", json::Value::make_string("hi")}})}})};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(resp.result.has_value() && !resp.error.has_value(),
              "C2-3: a successful tools/call is a JSON-RPC result, never a JSON-RPC error");
        if (resp.result.has_value()) {
            auto const* is_error = resp.result->find("isError");
            check(is_error && !is_error->as_bool(), "C2-3: isError is false for a successful call");
            auto const* result_type = resp.result->find("resultType");
            check(result_type && result_type->as_string() == "complete",
                  "C2-3: resultType is present and \"complete\" (011 §3.4's revision-required field)");
            auto const* content = resp.result->find("content");
            check(content && content->is_array() && !content->as_array().empty(),
                  "C2-3: content carries at least one part");
        }
    }

    // --- C2-4: tools/call on a tool that RUNS and FAILS is isError:true -- a RESULT, never a --------
    // --- JSON-RPC error (011 §3.1's own split, the property this test file exists to prove).      ---
    {
        mcp::JsonRpcRequest req{
            mcp::RpcId{std::string{"r4"}}, "tools/call",
            json::Value::make_object({{"name", json::Value::make_string("always_fails")},
                                       {"arguments", json::Value::make_object({{"noop", json::Value::make_bool(true)}})}})};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(resp.result.has_value() && !resp.error.has_value(),
              "C2-4: an execution failure is STILL a JSON-RPC result, not a JSON-RPC error");
        if (resp.result.has_value()) {
            auto const* is_error = resp.result->find("isError");
            check(is_error && is_error->as_bool(),
                  "C2-4: isError is true for a tool that ran and failed");
        }
    }

    // --- C2-5: tools/call on an UNKNOWN tool is a JSON-RPC error (protocol error), never isError ---
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"r5"}}, "tools/call",
                                 json::Value::make_object({{"name", json::Value::make_string("nope")}})};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(!resp.result.has_value() && resp.error.has_value(),
              "C2-5: an unknown tool is a JSON-RPC error, never a result with isError:true");
        if (resp.error.has_value()) {
            check(resp.error->code == mcp::kRpcMethodNotFound,
                  "C2-5: rejected with MethodNotFound, the real JSON-RPC code");
        }
    }

    // --- C2-6: tools/call with a missing "name" is InvalidParams ------------------------------------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"r6"}}, "tools/call", json::Value::make_object({})};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(!resp.result.has_value() && resp.error.has_value(),
              "C2-6: tools/call with no \"name\" is rejected");
        if (resp.error.has_value()) {
            check(resp.error->code == mcp::kRpcInvalidParams,
                  "C2-6: rejected with InvalidParams, the real JSON-RPC code");
        }
    }

    // --- C2-7: an unrecognized method is MethodNotFound, never silently ignored --------------------
    {
        mcp::JsonRpcRequest req{mcp::RpcId{std::string{"r7"}}, "resources/list", json::Value{}};
        mcp::JsonRpcResponse resp = server.dispatch(req, kCaller);
        check(!resp.result.has_value() && resp.error.has_value(),
              "C2-7: an unimplemented method is rejected, not silently no-op'd");
        if (resp.error.has_value()) {
            check(resp.error->code == mcp::kRpcMethodNotFound,
                  "C2-7: rejected with MethodNotFound");
        }
    }

    if (g_failures == 0) {
        std::printf("test_mcp_server: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_server: %d failure(s)\n", g_failures);
    return 1;
}
