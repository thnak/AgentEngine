// ADR-061 prove phase, §39/§40 (decisions/archive/ADR-061-host-provided-inbound-transport.md), as amended by
// decisions/ADR-244-required-choices-step-1b.md §2.3 (a stated McpAuthority; no construction-time fallback). Proves
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

struct GatedTool : ae::Tool<GatedTool, ae::Capabilities<ae::cap::decl::Entropy>, ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy -- gating probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext& ctx) const {
        g_last_observed_capabilities_was_null = (ctx.capabilities == nullptr);
        g_last_observed_capabilities           = ctx.capabilities;
        co_return Reply{};
    }
};

// Same gate, but Backgroundable too -- claim 7's own target (the harder, detached-thread call site).
struct GatedBackgroundableTool
    : ae::Tool<GatedBackgroundableTool, ae::Capabilities<ae::cap::decl::Entropy>, ae::Backgroundable, ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name        = "gated_backgroundable_tool";
    static constexpr std::string_view description = "Requires cap::Entropy, runs backgrounded.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext& ctx) const {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        g_last_observed_capabilities_was_null = (ctx.capabilities == nullptr);
        g_last_observed_capabilities           = ctx.capabilities;
        co_return Reply{};
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
    auto const table = ae::ToolTable::from(GatedTool{}, GatedBackgroundableTool{});
    // ADR-244 §2.3: the server states its source. `server` is per_request against no ceiling, so every claim
    // below about a grant is about the grant alone; `trusted` is the other option, holding nothing -- it could
    // never pass gated_tool's own capability_ceiling, so "authorized via held" and "authorized via grant" are
    // told apart by construction.
    ae::CapabilitySet const held;
    mcp::McpServer server(table, mcp::McpAuthority::per_request(ae::CapabilitySet::no_ceiling()),
                          ae::ApprovalDecider{}, "capability-grant-test-server");
    mcp::McpServer trusted(table, mcp::McpAuthority::single_trusted_caller(held), ae::ApprovalDecider{},
                           "capability-grant-test-trusted");

    ae::Principal const kCaller = ae::make_local_cli_principal("caller-1", "tenant-a");
    // Background<max_concurrent> is a SEPARATE capability from the tool's own declared ceiling
    // (cap::Entropy here) -- background_task()'s own G9 check needs it independently, for ANY
    // backgrounded call regardless of what the tool itself requires (matching
    // tests/protocol/mcp/test_mcp_tasks_extension.cpp's own precedent). Granted here so claim 7 tests the grant's
    // Entropy-vs-held distinction specifically, not accidentally fail on the unrelated G9 gate.
    auto const kEntropyGrant = std::make_shared<ae::CapabilitySet const>(
        ae::CapabilitySet::grant_root({ae::cap::Entropy{}, ae::cap::Background{4}}));
    auto const kNow         = std::chrono::steady_clock::now();
    auto const kFarFuture   = ae::Expiry::at(kNow + std::chrono::hours{1});
    auto const kAlreadyPast = ae::Expiry::at(kNow - std::chrono::hours{1});

    auto const error_code_of = [](mcp::JsonRpcResponse const& r) -> std::string {
        if (!r.error.has_value() || !r.error->data.is_object()) return {};
        json::Value const* code = r.error->data.find("error_code");
        return code != nullptr && code->is_string() ? code->as_string() : std::string{};
    };

    // --- claim 1: under per_request, a call without a grant is refused, never served under a fallback -----
    {
        auto resp = server.dispatch(tools_call("c1a", "gated_tool"), kCaller);
        check(!resp.result.has_value() && resp.error.has_value() && resp.error->code == mcp::kRpcInvalidRequest &&
                  error_code_of(resp) == "mcp.authority_required",
              "claim 1: per_request -- no grant is refused (mcp.authority_required), a JSON-RPC error");
        auto discover = server.dispatch(
            mcp::JsonRpcRequest{mcp::RpcId{std::string{"c1b"}}, "server/discover", json::Value{}}, kCaller);
        check(error_code_of(discover) == "mcp.authority_required",
              "claim 1: on every method, server/discover included");

        // Control: the trusted option serves the same call under its own `held` (nothing here), so the gated
        // tool's ceiling check denies it -- isError:true, and ctx.capabilities stays null.
        g_last_observed_capabilities_was_null = true;
        auto held_resp = trusted.dispatch(tools_call("c1c", "gated_tool"), kCaller);
        check(held_resp.result.has_value() && held_resp.result->find("isError")->as_bool() &&
                  g_last_observed_capabilities_was_null,
              "claim 1 control: single_trusted_caller serves it under `held` (denied by the tool's ceiling), "
              "ctx.capabilities null");
    }

    // --- claim 2: a live grant is the call's authority; the trusted option refuses one -----------------------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};
        auto resp = server.dispatch(tools_call("c2a", "gated_tool"), kCaller, grant, kNow);
        check(resp.result.has_value() && !resp.result->find("isError")->as_bool(),
              "claim 2: a grant supplying cap::Entropy succeeds");

        // Control: the reverse -- a server holding Entropy itself, handed a grant -- refuses the grant rather
        // than serving under either set.
        ae::CapabilitySet const held_with_entropy = ae::CapabilitySet::grant_root({ae::cap::Entropy{}});
        mcp::McpServer server_with_held(table, mcp::McpAuthority::single_trusted_caller(held_with_entropy),
                                        ae::ApprovalDecider{}, "s2");
        auto resp2 = server_with_held.dispatch(tools_call("c2b", "gated_tool"), kCaller, grant, kNow);
        check(error_code_of(resp2) == "mcp.authority_unexpected",
              "claim 2 control: single_trusted_caller refuses a carried grant (mcp.authority_unexpected)");
        auto resp3 = server_with_held.dispatch(tools_call("c2c", "gated_tool"), kCaller);
        check(resp3.result.has_value() && !resp3.result->find("isError")->as_bool(),
              "claim 2 control: the same server, no grant, serves the call under its own `held`");
    }

    // --- claim 3: an expired grant denies outright, before any method branch, including read-only ---
    {
        mcp::CapabilityGrant const expired{kCaller, kEntropyGrant, kAlreadyPast};

        auto resp = server.dispatch(tools_call("c3a", "gated_tool"), kCaller, expired, kNow);
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
                                                ae::Expiry::at(kNow + std::chrono::milliseconds{1})};
        auto resp2 = server.dispatch(tools_call("c3c", "gated_tool"), kCaller, barely_live, kNow);
        check(resp2.result.has_value() && !resp2.result->find("isError")->as_bool(),
              "claim 3 control: a grant that is still (barely) live succeeds");
    }

    // --- claim 4: capabilities == nullptr is rejected before any dereference, never a crash ---------
    {
        mcp::CapabilityGrant const null_caps{kCaller, nullptr, kFarFuture};
        auto resp = server.dispatch(tools_call("c4", "gated_tool"), kCaller, null_caps, kNow);
        check(!resp.result.has_value() && resp.error.has_value() &&
                  resp.error->code == mcp::kRpcInvalidRequest,
              "claim 4: a grant with null capabilities is rejected, not dereferenced -- no crash");
    }

    // --- claim 5: a grant verified for a DIFFERENT principal than `caller` is refused ---------------
    {
        ae::Principal const attacker = ae::make_local_cli_principal("attacker", "tenant-a");
        ae::Principal const victim   = ae::make_local_cli_principal("victim", "tenant-a");
        mcp::CapabilityGrant const victims_grant{victim, kEntropyGrant, kFarFuture};

        auto resp = server.dispatch(tools_call("c5a", "gated_tool"), attacker, victims_grant, kNow);
        check(!resp.result.has_value() && resp.error.has_value() &&
                  resp.error->code == mcp::kRpcInvalidRequest,
              "claim 5: caller=attacker with a grant verified for victim is refused -- distinct ids, "
              "same tenant, no delegation");

        // Control: caller == grant.principal exactly succeeds.
        mcp::CapabilityGrant const own_grant{victim, kEntropyGrant, kFarFuture};
        auto resp2 = server.dispatch(tools_call("c5b", "gated_tool"), victim, own_grant, kNow);
        check(resp2.result.has_value() && !resp2.result->find("isError")->as_bool(),
              "claim 5 control: caller == grant.principal exactly succeeds");
    }

    // --- claim 6: ctx.capabilities populated ONLY from a grant, never from `held` -------------------------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};

        g_last_observed_capabilities_was_null = true;
        auto resp_sync = server.dispatch(tools_call("c6a", "gated_tool"), kCaller, grant, kNow);
        check(resp_sync.result.has_value() && !resp_sync.result->find("isError")->as_bool() &&
                  !g_last_observed_capabilities_was_null && g_last_observed_capabilities != nullptr,
              "claim 6: synchronous path -- ctx.capabilities is populated when a grant is supplied");

        ae::CapabilitySet const held_with_entropy = ae::CapabilitySet::grant_root({ae::cap::Entropy{}});
        mcp::McpServer held_server(table, mcp::McpAuthority::single_trusted_caller(held_with_entropy),
                                   ae::ApprovalDecider{}, "s6");
        g_last_observed_capabilities_was_null = false;
        auto resp_held = held_server.dispatch(tools_call("c6b", "gated_tool"), kCaller);
        check(resp_held.result.has_value() && !resp_held.result->find("isError")->as_bool() &&
                  g_last_observed_capabilities_was_null,
              "claim 6 control: served under `held`, the tool runs and ctx.capabilities stays null");
    }

    // --- claim 7: a backgrounded call's detached thread still has a live, usable CapabilitySet -------
    // --- when a per-call grant supplied it ------------------------------------------------------------------
    {
        mcp::CapabilityGrant const grant{kCaller, kEntropyGrant, kFarFuture};
        g_last_observed_capabilities_was_null = true;

        auto resp = server.dispatch(tools_call_as_task("c7a", "gated_backgroundable_tool"), kCaller, grant, kNow);
        check(resp.result.has_value(), "claim 7: backgrounding a gated tool with a real grant succeeds "
                                        "(the task is accepted)");

        // The tool body runs on the detached thread ~60ms later, well after dispatch() has returned.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(!g_last_observed_capabilities_was_null && g_last_observed_capabilities != nullptr,
              "claim 7: the detached thread's own ctx.capabilities is populated and non-null, proving "
              "the per-call grant's shared_ptr survived past dispatch()'s own return");

        // Control: the trusted server holding nothing refuses the backgroundable call at the authorize step,
        // before any thread is spawned.
        auto resp2 = trusted.dispatch(tools_call_as_task("c7b", "gated_backgroundable_tool"), kCaller);
        check(!resp2.result.has_value() && resp2.error.has_value(),
              "claim 7 control: under `held` (nothing), the backgroundable call is refused upfront");
    }

    // --- claim 8 (ADR-244 §2.3): a grant outside the server's stated ceiling is refused, not narrowed -------
    {
        mcp::McpServer ceilinged(
            table, mcp::McpAuthority::per_request(ae::CapabilitySet::grant_root({ae::cap::Entropy{}})),
            ae::ApprovalDecider{}, "s8");
        mcp::CapabilityGrant const too_wide{kCaller, kEntropyGrant, kFarFuture};  // Entropy + Background
        auto resp = ceilinged.dispatch(tools_call("c8a", "gated_tool"), kCaller, too_wide, kNow);
        check(error_code_of(resp) == "mcp.authority_exceeds_ceiling",
              "claim 8: a grant holding Background outside an Entropy-only ceiling is refused whole");
        mcp::CapabilityGrant const inside{
            kCaller, std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({ae::cap::Entropy{}})),
            kFarFuture};
        auto resp2 = ceilinged.dispatch(tools_call("c8b", "gated_tool"), kCaller, inside, kNow);
        check(resp2.result.has_value() && !resp2.result->find("isError")->as_bool(),
              "claim 8 control: a grant inside the ceiling is served");
    }

    if (g_failures == 0) {
        std::printf("test_mcp_capability_grant: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_capability_grant: %d failure(s)\n", g_failures);
    return 1;
}
