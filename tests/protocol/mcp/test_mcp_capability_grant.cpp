// ADR-061 prove phase, §39/§40 (decisions/ADR-061-host-provided-inbound-transport.md). Proves
// McpServer::dispatch()'s per-request CapabilityGrant -- the falsifiable claims table at §39.4 --
// against a REAL ToolTable, a REAL 10-step tool pipeline (both synchronous and backgrounded), and a
// gated tool whose capability_ceiling actually requires cap::Entropy, not a trivially-empty one.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

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

struct GatedArgs {
    bool unused = false;
};
AE_JSON_SCHEMA(GatedArgs, unused)
struct GatedReply {
    bool unused = false;
};
AE_JSON_SCHEMA(GatedReply, unused)

// Records ctx.capabilities' nullness/contents so claim 6 can inspect it from inside a real invoke().
std::shared_ptr<ae::CapabilitySet const> g_last_observed_capabilities;
bool                                     g_last_observed_capabilities_was_null = true;

struct GatedTool : ae::Tool<GatedTool, ae::Capabilities<ae::cap::decl::Entropy>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy -- gating probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext& ctx) {
        g_last_observed_capabilities_was_null = (ctx.capabilities == nullptr);
        g_last_observed_capabilities           = ctx.capabilities;
        return Reply{};
    }
};

// Same gate, but Backgroundable too -- claim 7's own target (the harder, detached-thread call site).
struct GatedBackgroundableTool
    : ae::Tool<GatedBackgroundableTool, ae::Capabilities<ae::cap::decl::Entropy>, ae::Backgroundable> {
    static constexpr std::string_view name        = "gated_backgroundable_tool";
    static constexpr std::string_view description = "Requires cap::Entropy, runs backgrounded.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext& ctx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        g_last_observed_capabilities_was_null = (ctx.capabilities == nullptr);
        g_last_observed_capabilities           = ctx.capabilities;
        return Reply{};
    }
};

mcp::JsonRpcRequest tools_call(std::string id, std::string tool_name) {
    return mcp::JsonRpcRequest{
        mcp::RpcId{std::move(id)}, "tools/call",
        json::Value::make_object(
            {{"name", json::Value::make_string(std::move(tool_name))},
             {"arguments", json::Value::make_object({{"unused", json::Value::make_bool(false)}})}})};
}

mcp::JsonRpcRequest tools_call_as_task(std::string id, std::string tool_name) {
    return mcp::JsonRpcRequest{
        mcp::RpcId{std::move(id)}, "tools/call",
        json::Value::make_object(
            {{"name", json::Value::make_string(std::move(tool_name))},
             {"arguments", json::Value::make_object({{"unused", json::Value::make_bool(false)}})},
             {"extensions", json::Value::make_array({json::Value::make_string(std::string{mcp::kMcpTasksExtension})})}})};
}

}  // namespace

int main() {
    auto const table = ae::ToolTable::from_tools<GatedTool, GatedBackgroundableTool>();
    // held_ grants NOTHING -- every claim below distinguishes "authorized via held_" from "authorized
    // via grant" by construction: held_ alone could never pass gated_tool's own capability_ceiling.
    ae::CapabilitySet const held;
    mcp::McpServer server(table, held, ae::ApprovalDecider{}, "capability-grant-test-server");

    ae::Principal const kCaller = ae::make_local_cli_principal("caller-1", "tenant-a");
    // Background<max_concurrent> is a SEPARATE capability from the tool's own declared ceiling
    // (cap::Entropy here) -- background_task()'s own G9 check needs it independently, for ANY
    // backgrounded call regardless of what the tool itself requires (matching
    // tests/test_mcp_tasks_extension.cpp's own precedent). Granted here so claim 7 tests the grant's
    // Entropy-vs-held_ distinction specifically, not accidentally fail on the unrelated G9 gate.
    auto const kEntropyGrant = std::make_shared<ae::CapabilitySet const>(
        ae::CapabilitySet::grant_root({ae::cap::Entropy{}, ae::cap::Background{4}}));
    auto const kNow        = std::chrono::steady_clock::now();
    auto const kFarFuture  = kNow + std::chrono::hours{1};
    auto const kAlreadyPast = kNow - std::chrono::hours{1};

    // --- claim 1: no-grant call is genuinely byte-for-byte identical to today -----------------------
    {
        auto req  = tools_call("c1a", "gated_tool");
        auto resp = server.dispatch(req, kCaller);  // no grant, no now -- the pre-existing 2-arg call
        check(resp.result.has_value(), "claim 1: 2-arg dispatch() still compiles/returns a result");
        auto const* is_error = resp.result->find("isError");
        check(is_error && is_error->as_bool(),
              "claim 1: held_ grants nothing, so the gated tool's ceiling check denies it -- isError:true");
        check(g_last_observed_capabilities_was_null,
              "claim 1: ctx.capabilities stayed null -- the no-grant path is unaffected by this section");

        // Control: an expired-in-the-past `now` passed alongside NO grant still reaches the tool
        // (denied for the SAME reason as above -- ceiling, not expiry -- proving the expiry check is
        // gated on grant.has_value(), not on `now` alone).
        auto req2  = tools_call("c1b", "gated_tool");
        auto resp2 = server.dispatch(req2, kCaller, std::nullopt, kAlreadyPast);
        check(resp2.result.has_value() && resp2.result->find("isError")->as_bool(),
              "claim 1 control: a past `now` with no grant behaves identically -- no expiry path reached");
    }

    // --- claim 2: a live grant is used instead of held_, and MAY exceed held_'s own grants -----------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};
        auto req  = tools_call("c2a", "gated_tool");
        auto resp = server.dispatch(req, kCaller, grant, kNow);
        check(resp.result.has_value() && !resp.result->find("isError")->as_bool(),
              "claim 2: a grant supplying cap::Entropy succeeds even though held_ grants nothing");

        // Control: the reverse (granted in held_, absent from grant) is DENIED once a grant is present
        // -- proves held_ is not silently consulted as a fallback.
        ae::CapabilitySet const held_with_entropy = ae::CapabilitySet::grant_root({ae::cap::Entropy{}});
        mcp::McpServer server_with_held(table, held_with_entropy, ae::ApprovalDecider{}, "s2");
        auto const kEmptyGrantCaps =
            std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({}));
        mcp::CapabilityGrant const empty_grant{kCaller, kEmptyGrantCaps, kFarFuture};
        auto req2  = tools_call("c2b", "gated_tool");
        auto resp2 = server_with_held.dispatch(req2, kCaller, empty_grant, kNow);
        check(resp2.result.has_value() && resp2.result->find("isError")->as_bool(),
              "claim 2 control: held_ grants Entropy, but an empty grant is supplied -- DENIED, "
              "proving held_ is not consulted as a fallback once a grant is present");
    }

    // --- claim 3: an expired grant denies outright, before any method branch, including read-only ---
    {
        mcp::CapabilityGrant const expired{kCaller, kEntropyGrant, kAlreadyPast};

        auto req  = tools_call("c3a", "gated_tool");
        auto resp = server.dispatch(req, kCaller, expired, kNow);
        check(!resp.result.has_value() && resp.error.has_value() &&
                  resp.error->code == mcp::kRpcInvalidRequest,
              "claim 3: an expired grant denies tools/call outright, a JSON-RPC error, not isError:true");

        mcp::JsonRpcRequest discover_req{mcp::RpcId{std::string{"c3b"}}, "server/discover", json::Value{}};
        auto discover_resp = server.dispatch(discover_req, kCaller, expired, kNow);
        check(!discover_resp.result.has_value() && discover_resp.error.has_value(),
              "claim 3: the SAME expired grant also denies server/discover, which consults no "
              "capability at all -- blanket, not scoped to capability-consuming methods");

        // Control: the same grant, `now` moved one tick earlier than `expiry`, succeeds.
        mcp::CapabilityGrant const barely_live{kCaller, kEntropyGrant,
                                                kNow + std::chrono::milliseconds{1}};
        auto req2  = tools_call("c3c", "gated_tool");
        auto resp2 = server.dispatch(req2, kCaller, barely_live, kNow);
        check(resp2.result.has_value() && !resp2.result->find("isError")->as_bool(),
              "claim 3 control: a grant that is still (barely) live succeeds");
    }

    // --- claim 4: capabilities == nullptr is rejected before any dereference, never a crash ---------
    {
        mcp::CapabilityGrant const null_caps{kCaller, nullptr, kFarFuture};
        auto req  = tools_call("c4", "gated_tool");
        auto resp = server.dispatch(req, kCaller, null_caps, kNow);
        check(!resp.result.has_value() && resp.error.has_value() &&
                  resp.error->code == mcp::kRpcInvalidRequest,
              "claim 4: a grant with null capabilities is rejected, not dereferenced -- no crash");
    }

    // --- claim 5: a grant verified for a DIFFERENT principal than `caller` is refused ---------------
    {
        ae::Principal const attacker = ae::make_local_cli_principal("attacker", "tenant-a");
        ae::Principal const victim   = ae::make_local_cli_principal("victim", "tenant-a");
        mcp::CapabilityGrant const victims_grant{victim, kEntropyGrant, kFarFuture};

        auto req  = tools_call("c5a", "gated_tool");
        auto resp = server.dispatch(req, attacker, victims_grant, kNow);
        check(!resp.result.has_value() && resp.error.has_value() &&
                  resp.error->code == mcp::kRpcInvalidRequest,
              "claim 5: caller=attacker with a grant verified for victim is refused -- distinct ids, "
              "same tenant, no delegation");

        // Control: caller == grant.principal exactly succeeds.
        mcp::CapabilityGrant const own_grant{victim, kEntropyGrant, kFarFuture};
        auto req2  = tools_call("c5b", "gated_tool");
        auto resp2 = server.dispatch(req2, victim, own_grant, kNow);
        check(resp2.result.has_value() && !resp2.result->find("isError")->as_bool(),
              "claim 5 control: caller == grant.principal exactly succeeds");
    }

    // --- claim 6: ctx.capabilities populated ONLY on the grant-supplied path, both call sites --------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};

        g_last_observed_capabilities_was_null = true;
        auto req_sync  = tools_call("c6a", "gated_tool");
        auto resp_sync = server.dispatch(req_sync, kCaller, grant, kNow);
        check(resp_sync.result.has_value() && !resp_sync.result->find("isError")->as_bool() &&
                  !g_last_observed_capabilities_was_null && g_last_observed_capabilities != nullptr,
              "claim 6: synchronous path -- ctx.capabilities is populated when a grant is supplied");

        g_last_observed_capabilities_was_null = true;
        auto req_nogrant  = tools_call("c6b", "gated_tool");
        auto resp_nogrant = server.dispatch(req_nogrant, kCaller);
        check(g_last_observed_capabilities_was_null,
              "claim 6 control: the no-grant path's ctx.capabilities stays null, matching claim 1");
    }

    // --- claim 7: a backgrounded call's detached thread still has a live, usable CapabilitySet -------
    // --- when a per-call grant (not held_) supplied it -----------------------------------------------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};
        g_last_observed_capabilities_was_null = true;

        auto req  = tools_call_as_task("c7a", "gated_backgroundable_tool");
        auto resp = server.dispatch(req, kCaller, grant, kNow);
        check(resp.result.has_value(), "claim 7: backgrounding a gated tool with a real grant succeeds "
                                        "(the task is accepted -- held_ alone would have refused it at "
                                        "the authorize step, before any thread was ever spawned)");

        // The tool body runs on the detached thread ~60ms later, well after dispatch() has returned.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(!g_last_observed_capabilities_was_null && g_last_observed_capabilities != nullptr,
              "claim 7: the detached thread's own ctx.capabilities is populated and non-null, proving "
              "the per-call grant's shared_ptr survived past dispatch()'s own return");

        // Control: an in-flight backgrounded call using only held_ (no grant) behaves identically to
        // before this section -- ctx.capabilities stays null even on the backgrounded path.
        g_last_observed_capabilities_was_null = true;
        auto req2  = tools_call_as_task("c7b", "gated_backgroundable_tool");
        auto resp2 = server.dispatch(req2, kCaller);  // no grant -- held_ grants nothing, denied upfront
        check(!resp2.result.has_value() && resp2.error.has_value(),
              "claim 7 control: no grant, held_ grants nothing -- the backgroundable call is refused "
              "at the authorize step, same as before this section (tool.capability_not_held)");
    }

    if (g_failures == 0) {
        std::printf("test_mcp_capability_grant: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_capability_grant: %d failure(s)\n", g_failures);
    return 1;
}
