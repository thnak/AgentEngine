// Milestone 7 Phase C4 (011-MCP-Conformance.md §3.6/§12, docs/research/2026-mcp-protocol-detail.md
// §12, docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves the
// `io.modelcontextprotocol/tasks` extension end-to-end: `McpClient::call_tool_as_task()` /
// `get_task()` / `cancel_task()` (client.hpp) against a REAL `McpServer` (server.hpp) backgrounding a
// REAL `Backgroundable` tool via `background_task()` (tool_pipeline.hpp, real since Phase B) -- not a
// stub, the same real primitive Phase B's own `test_agent_session_background_task.cpp` proves.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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

struct SlowArgs {
    bool noop;
};
AE_JSON_SCHEMA(SlowArgs, noop)
struct SlowReply {
    bool ok;
};
AE_JSON_SCHEMA(SlowReply, ok)

// Sleeps long enough that a polling test can observe "still working" before it finishes.
struct SlowBackgroundableTool : ae::Tool<SlowBackgroundableTool, ae::Backgroundable> {
    static constexpr std::string_view name        = "slow_backgroundable";
    static constexpr std::string_view description = "Sleeps, then succeeds.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return Reply{true};
    }
};

// Backgroundable but always fails -- proves §12's "failed status MUST NOT represent isError:true;
// that is completed with error details in result" on the ASYNC path, symmetric with C2's own
// synchronous proof of the same isError/JSON-RPC-error split.
struct FailingBackgroundableTool : ae::Tool<FailingBackgroundableTool, ae::Backgroundable> {
    static constexpr std::string_view name        = "failing_backgroundable";
    static constexpr std::string_view description = "Sleeps briefly, then fails.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return std::unexpected(
            ae::error{ae::failure_class::contract, "deliberate failure", "test.deliberate_failure"});
    }
};

// NOT Backgroundable -- proves the extension cannot force a synchronous-only tool into the background.
struct ForegroundOnlyTool : ae::Tool<ForegroundOnlyTool> {
    static constexpr std::string_view name        = "foreground_only";
    static constexpr std::string_view description = "Never declared Backgroundable.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{true}; }
};

json::Value noop_args() { return json::Value::make_object({{"noop", json::Value::make_bool(true)}}); }

}  // namespace

int main() {
    auto const table =
        ae::ToolTable::from_tools<SlowBackgroundableTool, FailingBackgroundableTool, ForegroundOnlyTool>();
    // Background<1>: exactly one concurrent backgrounded call, so C4-6 can prove capacity enforcement
    // reused through the MCP path (G9, Phase B).
    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({ae::cap::Background{1}});
    mcp::McpServer server(table, held, ae::ApprovalDecider{}, "agentengine-test-tasks-server");
    // ADR-061 §7 R3: one principal for this whole client, so every task it creates is owned by
    // the same identity that later polls it -- the ordinary case. The cross-principal case gets
    // its own dedicated test (test_task_principal_binding.cpp).
    ae::Principal const kCaller = ae::make_local_cli_principal("tasks-caller", "test-tenant");
    mcp::McpClient client([&server, &kCaller](mcp::JsonRpcRequest const& r) {
                              return server.dispatch(r, kCaller);
                          },
                          "tasks-client");

    // --- C4-1: the extension cannot background a non-Backgroundable tool ---------------------------
    {
        auto handle = client.call_tool_as_task("foreground_only", noop_args());
        check(!handle.has_value(),
              "C4-1: call_tool_as_task() on a non-Backgroundable tool is rejected");
    }

    // --- C4-2/3: a Backgroundable call returns a task handle immediately, status "working" ---------
    mcp::McpTaskHandle handle1;
    {
        auto h = client.call_tool_as_task("slow_backgroundable", noop_args());
        check(h.has_value(), "C4-2: call_tool_as_task() on a Backgroundable tool succeeds");
        if (h.has_value()) {
            handle1 = *h;
            check(!handle1.task_id.empty(), "C4-2: the returned taskId is non-empty");
            check(handle1.status == "working", "C4-3: the initial status is \"working\"");
        }
    }

    // --- C4-4: an immediate poll (before the sleep finishes) still reports "working", no result ----
    {
        auto poll = client.get_task(handle1.task_id);
        check(poll.has_value() && poll->status == "working" && !poll->has_result,
              "C4-4: polling immediately after start still shows \"working\", no result attached yet");
    }

    // --- C4-5: after it finishes, tasks/get reports "completed" with the real reply -----------------
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto poll = client.get_task(handle1.task_id);
        check(poll.has_value() && poll->status == "completed",
              "C4-5: once finished, status is \"completed\"");
        if (poll.has_value()) {
            check(poll->has_result, "C4-5: a completed task carries a real result");
            check(!poll->outcome.is_error, "C4-5: the successful call's isError is false");
        }
    }

    // --- C4-6: a failing Backgroundable call is STILL status "completed" -- never "failed" ----------
    // --- (§12's own rule), with isError:true faithfully carried in the result.                    ---
    {
        auto h = client.call_tool_as_task("failing_backgroundable", noop_args());
        check(h.has_value(), "C4-6: call_tool_as_task() on the failing tool still starts (the tool "
                              "hasn't RUN yet -- only resolution/authorize/approve are checked before "
                              "backgrounding)");
        if (h.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            auto poll = client.get_task(h->task_id);
            check(poll.has_value() && poll->status == "completed",
                  "C4-6: a tool that ran and FAILED is still task status \"completed\", never "
                  "\"failed\" -- §12: \"The failed status MUST NOT represent a tool result with "
                  "isError:true\"");
            if (poll.has_value() && poll->has_result) {
                check(poll->outcome.is_error,
                      "C4-6: isError is true, faithfully carried into the completed task's result");
            }
        }
    }

    // --- C4-7: Background<1> capacity is enforced through the MCP tasks path too (G9, Phase B) ------
    {
        auto h1 = client.call_tool_as_task("slow_backgroundable", noop_args());
        check(h1.has_value(), "C4-7: the first call under a fresh Background<1> grant succeeds");
        auto h2 = client.call_tool_as_task("slow_backgroundable", noop_args());
        check(!h2.has_value(),
              "C4-7: a second concurrent call while the first is still \"working\" is rejected -- "
              "Background<1>'s own ceiling, reused unchanged from Phase B's background_task()");
        if (h1.has_value()) {
            // Let it actually finish so it stops counting as "working" for later checks/cleanliness.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    // --- C4-8: tasks/get on an unknown taskId is rejected --------------------------------------------
    {
        auto poll = client.get_task("no-such-task-id");
        check(!poll.has_value(), "C4-8: get_task() on an unknown taskId is rejected");
    }

    // --- C4-9: tasks/cancel on a still-running task marks it "cancelled", and it STAYS "cancelled" --
    // --- even once the underlying (uncancellable) worker thread actually finishes (§12: no revert). -
    {
        auto h = client.call_tool_as_task("slow_backgroundable", noop_args());
        check(h.has_value(), "C4-9: the call to be cancelled starts successfully");
        if (h.has_value()) {
            auto cancelled = client.cancel_task(h->task_id);
            check(cancelled.has_value(), "C4-9: cancel_task() on a running task succeeds");
            auto poll_now = client.get_task(h->task_id);
            check(poll_now.has_value() && poll_now->status == "cancelled",
                  "C4-9: an immediate poll after cancelling already reports \"cancelled\"");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            auto poll_later = client.get_task(h->task_id);
            check(poll_later.has_value() && poll_later->status == "cancelled",
                  "C4-9: the status STAYS \"cancelled\" once the (uncancellable) worker thread "
                  "actually completes -- its result is discarded, never silently reviving the task "
                  "to \"completed\"");
        }
    }

    // --- C4-10: tasks/cancel on an already-completed task is rejected, not silently accepted --------
    {
        auto cancelled = client.cancel_task(handle1.task_id);  // handle1 finished back in C4-5
        check(!cancelled.has_value(),
              "C4-10: cancel_task() on an already-completed task is rejected");
    }

    if (g_failures == 0) {
        std::printf("test_mcp_tasks_extension: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mcp_tasks_extension: %d failure(s)\n", g_failures);
    return 1;
}
