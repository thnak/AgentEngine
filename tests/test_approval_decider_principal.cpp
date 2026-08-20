// ADR-061 prove phase, §46 (decisions/ADR-061-host-provided-inbound-transport.md). Proves the
// falsifiable claims table at §46.5: `ApprovalDecider` widened to `bool(Principal const&,
// std::string_view, std::string const&)` closes R27's `approve_` finding -- a decider can now see WHO
// is asking, sourced from the SAME `ctx.principal` `ToolInvocationAudit` already records, at both the
// synchronous (`invoke_tool()`) and backgrounded (`background_task()`) call sites, and reaches
// `McpServer::dispatch()`'s per-request `caller` with no code change to `server.hpp` itself.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "agentengine/core/tool_pipeline.hpp"
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

// Approval<always_require>, no Capabilities<...> needed -- the approve() step is this test's only
// interest, not the capability-bind step §39/§40's own tests already cover.
struct AlwaysApproveTool
    : ae::Tool<AlwaysApproveTool, ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name        = "always_approve_tool";
    static constexpr std::string_view description = "Approval<always_require> -- decider probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{}; }
};

struct AlwaysApproveBackgroundableTool
    : ae::Tool<AlwaysApproveBackgroundableTool, ae::Approval<ae::approval_mode::always_require>,
               ae::Backgroundable> {
    static constexpr std::string_view name        = "always_approve_backgroundable_tool";
    static constexpr std::string_view description = "Approval<always_require>, backgrounded.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        return Reply{};
    }
};

ae::ToolCallRequest make_request(std::string call_id, std::string tool_name) {
    return ae::ToolCallRequest{std::move(call_id), std::move(tool_name),
                                *json::parse(R"({"unused":false})"), /*arguments_tainted=*/false};
}

mcp::JsonRpcRequest tools_call(std::string id, std::string tool_name) {
    return mcp::JsonRpcRequest{
        mcp::RpcId{std::move(id)}, "tools/call",
        json::Value::make_object(
            {{"name", json::Value::make_string(std::move(tool_name))},
             {"arguments", json::Value::make_object({{"unused", json::Value::make_bool(false)}})}})};
}

}  // namespace

int main() {
    ae::ToolTable const table =
        ae::ToolTable::from_tools<AlwaysApproveTool, AlwaysApproveBackgroundableTool>();
    ae::CapabilitySet const held;  // neither tool declares Capabilities<...> -- nothing to bind

    // --- claim 1: a decider's Principal argument matches the SAME identity the audit record carries
    // for the same call -- proving it's `ctx.principal`, not a cached/independent value ---------------
    {
        ae::Principal const kCallerA = ae::make_local_cli_principal("caller-a", "tenant-1");
        ae::Principal const kCallerB = ae::make_local_cli_principal("caller-b", "tenant-2");

        ae::Principal observed_by_decider;
        ae::ApprovalDecider const capturing = [&](ae::Principal const& caller, std::string_view,
                                                     std::string const&) {
            observed_by_decider = caller;
            return true;
        };

        ae::EffectContext ctx_a;
        ctx_a.principal = kCallerA;
        ae::ToolInvocationAudit audit_a;
        auto result_a = invoke_tool(table, held, make_request("c1", "always_approve_tool"), ctx_a,
                                     capturing, &audit_a);
        check(!result_a.is_error, "claim 1 setup: the call with caller A succeeds");
        check(observed_by_decider.id == "caller-a" && observed_by_decider.tenant_id == "tenant-1",
              "claim 1: the decider observed caller A's own identity");
        check(observed_by_decider.id == audit_a.principal_id &&
                  observed_by_decider.tenant_id == audit_a.principal_tenant_id,
              "claim 1: the decider's Principal is byte-identical to what ToolInvocationAudit records "
              "for the SAME call -- the same ctx.principal, not an independent value");

        ae::EffectContext ctx_b;
        ctx_b.principal = kCallerB;
        ae::ToolInvocationAudit audit_b;
        auto result_b = invoke_tool(table, held, make_request("c2", "always_approve_tool"), ctx_b,
                                     capturing, &audit_b);
        check(!result_b.is_error, "claim 1 control setup: the call with caller B succeeds");
        check(observed_by_decider.id == "caller-b" && observed_by_decider.tenant_id == "tenant-2" &&
                  observed_by_decider.id == audit_b.principal_id,
              "claim 1 control: a second call with a DIFFERENT caller produces a DIFFERENT observed "
              "identity, matching ITS OWN audit record -- not one cached/stale value from claim 1's "
              "first call");
    }

    // --- claim 2: McpServer::dispatch()'s per-request `caller` reaches a principal-aware decider with
    // NO code change to server.hpp -- a tenant-conditional decider, supplied once at construction,
    // denies one tenant and allows another on the SAME server instance -----------------------------
    {
        ae::ApprovalDecider const tenant_gated = [](ae::Principal const& caller, std::string_view,
                                                       std::string const&) {
            return caller.tenant_id == "trusted-tenant";
        };
        ae::ToolTable const sync_table = ae::ToolTable::from_tools<AlwaysApproveTool>();
        mcp::McpServer server(sync_table, held, tenant_gated, "approval-decider-principal-test-server");

        ae::Principal const kTrusted   = ae::make_local_cli_principal("caller-t", "trusted-tenant");
        ae::Principal const kUntrusted = ae::make_local_cli_principal("caller-u", "other-tenant");

        auto denied = server.dispatch(tools_call("d1", "always_approve_tool"), kUntrusted);
        check(denied.result.has_value() && denied.result->find("isError")->as_bool(),
              "claim 2: the SAME decider, SAME server instance, denies a caller from a tenant it does "
              "not trust -- `approve_` is construction-time, but the identity it sees is per-request");

        auto allowed = server.dispatch(tools_call("d2", "always_approve_tool"), kTrusted);
        check(allowed.result.has_value() && !allowed.result->find("isError")->as_bool(),
              "claim 2: the SAME decider, SAME server instance, allows a caller from the trusted "
              "tenant right after denying a different one -- proving the identity is genuinely "
              "per-call, not fixed the moment the server/decider was constructed, with zero code "
              "changes to McpServer/server.hpp itself to make this possible");
    }

    // --- claim 4: background_task()'s approve() call sees the correct Principal for the call it is
    // deciding, sourced from ctx.principal on the CALLING thread before the worker detaches ----------
    {
        // Background<max_concurrent> is a SEPARATE capability from the tool's own declared ceiling
        // (background_task()'s own G9 check needs it independently, for ANY backgrounded call --
        // matching tests/test_mcp_capability_grant.cpp's own precedent), so `held` above (empty) is
        // deliberately not reused here.
        ae::CapabilitySet const held_with_background =
            ae::CapabilitySet::grant_root({ae::cap::Background{4}});

        ae::Principal const kCaller = ae::make_local_cli_principal("caller-bg", "tenant-bg");
        ae::Principal          observed_by_decider;
        bool                    decider_called_on_calling_thread = false;
        std::thread::id const   calling_thread                   = std::this_thread::get_id();

        ae::ApprovalDecider const capturing = [&](ae::Principal const& caller, std::string_view,
                                                     std::string const&) {
            observed_by_decider              = caller;
            decider_called_on_calling_thread = (std::this_thread::get_id() == calling_thread);
            return true;
        };

        ae::EffectContext ctx;
        ctx.principal = kCaller;
        bool                completed = false;
        ae::ToolInvocationAudit final_audit;
        auto started = background_task(
            table, held_with_background, make_request("c3", "always_approve_backgroundable_tool"), ctx,
            capturing,
            /*current_background_count=*/0,
            [&](ae::ToolResult result, ae::ToolInvocationAudit audit) {
                completed    = !result.is_error;
                final_audit  = audit;
            });
        check(started.has_value(), "claim 4 setup: the backgrounded call starts successfully");
        check(decider_called_on_calling_thread,
              "claim 4: approve() ran on the CALLING thread, before the worker detaches -- never "
              "inside the detached background thread itself");
        check(observed_by_decider.id == "caller-bg" && observed_by_decider.tenant_id == "tenant-bg",
              "claim 4: the decider observed the correct caller for the backgrounded call");

        for (int i = 0; i < 100 && !completed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(completed, "claim 4 control: the backgrounded call itself completed successfully");
        check(final_audit.principal_id == "caller-bg",
              "claim 4 control: the audit record the worker thread eventually produces carries the "
              "SAME identity the decider observed on the calling thread");
    }

    if (g_failures == 0) {
        std::printf("test_approval_decider_principal: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_approval_decider_principal: %d failure(s)\n", g_failures);
    return 1;
}
