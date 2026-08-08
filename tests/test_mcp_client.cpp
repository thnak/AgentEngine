// Milestone 7 Phase C3 (011-MCP-Conformance.md §3, docs/planning/milestone-7-protocol-conformance-
// breakdown.md). Proves McpClient (protocol/mcp/client.hpp) against two senders: a REAL McpServer
// (server.hpp, Phase C2) for the happy-path tools/list + tools/call shape, and a hand-rolled mock
// sender for the caching/pagination/rug-pull properties that need fine control over what a "peer"
// returns across multiple calls.

#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>

#include "agentengine/protocol/mcp/client.hpp"
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

// A minimal, hand-rolled tools/list-only mock server whose returned listing (and per-cursor listing)
// can be changed between calls -- what a real McpServer (which always reflects a fixed ToolTable
// truthfully) cannot help with, and what the caching/pagination/rug-pull tests below specifically need.
struct MockListServer {
    int call_count = 0;
    // cursor -> the tool name/description pair currently "live" for that cursor.
    std::unordered_map<std::string, std::pair<std::string, std::string>> live;

    mcp::JsonRpcResponse operator()(mcp::JsonRpcRequest const& req) {
        ++call_count;
        std::string cursor;
        if (auto const* c = req.params.find("cursor"); c && c->is_string()) cursor = c->as_string();
        auto it = live.find(cursor);
        std::string const name = it != live.end() ? it->second.first : "unknown";
        std::string const desc = it != live.end() ? it->second.second : "";
        json::Value entry = json::Value::make_object(
            {{"name", json::Value::make_string(name)}, {"description", json::Value::make_string(desc)},
             {"inputSchema", json::Value::make_object({})}, {"outputSchema", json::Value::make_object({})}});
        json::Value result = json::Value::make_object({{"tools", json::Value::make_array({entry})}});
        return mcp::JsonRpcResponse::make_result(req.id, std::move(result));
    }
};

}  // namespace

int main() {
    auto const table = ae::ToolTable::from_tools<EchoTool, AlwaysFailsTool>();
    ae::CapabilitySet const held;
    mcp::McpServer server(table, held, ae::ApprovalDecider{}, "agentengine-test-server");
    mcp::RequestSender to_real_server = [&server](mcp::JsonRpcRequest const& req) { return server.dispatch(req); };

    // --- C3-1/2/3/4: happy-path client behaviour against a REAL server ------------------------------
    {
        mcp::McpClient client(to_real_server, "test-client");

        auto tools = client.list_tools();
        check(tools.has_value() && tools->size() == 2, "C3-1: list_tools() returns both real tools");

        auto ok_call = client.call_tool("echo", json::Value::make_object({{"text", json::Value::make_string("hi")}}));
        check(ok_call.has_value(), "C3-2: a successful tools/call is a successful result<>");
        if (ok_call.has_value()) check(!ok_call->is_error, "C3-2: isError is false for a successful call");

        auto failing_call = client.call_tool("always_fails", json::Value::make_object({{"noop", json::Value::make_bool(true)}}));
        check(failing_call.has_value(),
              "C3-3: an execution failure is STILL a successful result<> -- isError is SURFACED, "
              "never turned into a client-side failure/exception (011 §3.1: \"surfaced to the model "
              "for self-correction\")");
        if (failing_call.has_value()) check(failing_call->is_error, "C3-3: isError is true, faithfully relayed");

        auto unknown_call = client.call_tool("nope", json::Value{});
        check(!unknown_call.has_value(),
              "C3-4: a PROTOCOL error (unknown tool) IS a client-side failure -- the isError "
              "surfacing rule applies only to execution errors, not protocol errors");
        if (!unknown_call.has_value()) {
            check(unknown_call.error().code == "mcp.rpc_error", "C3-4: rejected with the real error_code");
        }
    }

    // --- C3-5: caching -- a second list_tools() call within ttl never re-invokes the sender --------
    {
        MockListServer mock;
        mock.live[""] = {"tool-a", "v1"};
        mcp::McpClient client([&mock](mcp::JsonRpcRequest const& r) { return mock(r); }, "cache-client");
        client.set_ttl(std::chrono::milliseconds(5000));

        auto first = client.list_tools();
        check(first.has_value() && mock.call_count == 1, "C3-5: the first call reaches the sender");
        auto second = client.list_tools();
        check(second.has_value() && mock.call_count == 1,
              "C3-5: a second call within ttl is served from cache -- the sender is NOT re-invoked");
    }

    // --- C3-6: ttl == 0 (the default) means never cache -- §3.1: "ttlMs absent or negative -> 0" --
    {
        MockListServer mock;
        mock.live[""] = {"tool-a", "v1"};
        mcp::McpClient client([&mock](mcp::JsonRpcRequest const& r) { return mock(r); }, "no-cache-client");
        // ttl left at its default (0).
        (void)client.list_tools();
        (void)client.list_tools();
        check(mock.call_count == 2,
              "C3-6: with no ttl configured, every list_tools() call reaches the sender -- 0 means "
              "\"never serve from cache,\" not \"cache forever\"");
    }

    // --- C3-7: pagination -- an empty-string cursor and a real cursor are DIFFERENT cache keys, ----
    // --- never conflated (011 §3.1: "an empty string is a valid cursor, not end-of-results").    ---
    {
        MockListServer mock;
        mock.live[""]          = {"tool-page-1", "first page"};
        mock.live["cursor-x"]  = {"tool-page-2", "second page"};
        mcp::McpClient client([&mock](mcp::JsonRpcRequest const& r) { return mock(r); }, "page-client");
        client.set_ttl(std::chrono::milliseconds(5000));

        auto page1 = client.list_tools("");
        auto page2 = client.list_tools("cursor-x");
        check(page1.has_value() && page1->size() == 1 && page1->front().name == "tool-page-1",
              "C3-7: the empty-string-cursor page returns its own real content");
        check(page2.has_value() && page2->size() == 1 && page2->front().name == "tool-page-2",
              "C3-7: a real cursor's page returns DIFFERENT content -- not the empty-cursor page's "
              "cached entry reused by mistake");
        check(mock.call_count == 2, "C3-7: both pages actually reached the sender (two distinct cache keys)");

        // Re-fetching page1 within ttl still hits the cache -- proves the two keys don't clobber
        // each other's cache slot.
        auto page1_again = client.list_tools("");
        check(page1_again.has_value() && page1_again->front().name == "tool-page-1" && mock.call_count == 2,
              "C3-7: re-fetching the empty-cursor page within ttl is still a cache hit after a "
              "DIFFERENT cursor was fetched in between");
    }

    // --- C3-8: rug-pull detection -- a tool's description/schema changing under the SAME cache key -
    // --- between two (uncached, ttl=0) fetches is DETECTED, never silently re-trusted (011 §8).  ---
    {
        MockListServer mock;
        mock.live[""] = {"tool-a", "the original description"};
        mcp::McpClient client([&mock](mcp::JsonRpcRequest const& r) { return mock(r); }, "rugpull-client");
        // ttl left at 0 -- every call reaches the (mutable) mock, simulating "re-checked periodically".

        (void)client.list_tools();
        check(!client.rug_pull_detected(), "C3-8: no rug pull detected on the very first fetch");

        mock.live[""] = {"tool-a", "a DIFFERENT description -- the same name, changed content"};
        (void)client.list_tools();
        check(client.rug_pull_detected(),
              "C3-8: a tool's description changing under the same name between fetches is detected "
              "as a rug pull, per 011 §8's digest-pinning/re-approval discipline");
    }

    if (g_failures == 0) {
        std::printf("test_mcp_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_client: %d failure(s)\n", g_failures);
    return 1;
}
