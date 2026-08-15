// Proves decisions/ADR-061-host-provided-inbound-transport.md §7's R1/R2/R3 fixes: server-minted
// task handles are bound to the principal that created them (011 §8a), a non-owner cannot
// distinguish someone else's task from one that never existed (012 §4), and an inbound A2A message
// is subject to a real 018 §2 admission check rather than the `StartRun::caller == nullopt`
// fail-open it previously relied on.
//
// Structured per decisions/README.md's mandatory-positive-control rule for security claims: every
// negative case below is preceded by the positive control that the SAME operation succeeds for the
// rightful owner. A test that only ever observes rejection cannot distinguish "correctly denied"
// from "broken for everyone", and would pass against a handler that returns an error
// unconditionally.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "agentengine/protocol/a2a/server.hpp"
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
namespace a2a  = agentengine::a2a;
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

// Sleeps long enough that a task is still "working" when the ownership checks below run, so the
// cancel path is exercised against a live task rather than an already-terminal one.
struct SlowBackgroundableTool : ae::Tool<SlowBackgroundableTool, ae::Backgroundable> {
    static constexpr std::string_view name        = "slow_backgroundable";
    static constexpr std::string_view description = "Sleeps, then succeeds.";
    using Args  = SlowArgs;
    using Reply = SlowReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return Reply{true};
    }
};

[[nodiscard]] mcp::JsonRpcRequest task_call_request(std::string const& rpc_id) {
    json::Value params = json::Value::make_object(
        {{"name", json::Value::make_string("slow_backgroundable")},
         {"arguments", json::Value::make_object({{"noop", json::Value::make_bool(true)}})},
         {"extensions",
          json::Value::make_array({json::Value::make_string("io.modelcontextprotocol/tasks")})}});
    return mcp::JsonRpcRequest{mcp::RpcId{rpc_id}, "tools/call", params};
}

[[nodiscard]] mcp::JsonRpcRequest tasks_request(std::string const& rpc_id, char const* method,
                                                 std::string const& task_id) {
    json::Value params = json::Value::make_object({{"taskId", json::Value::make_string(task_id)}});
    return mcp::JsonRpcRequest{mcp::RpcId{rpc_id}, method, params};
}

[[nodiscard]] std::string task_id_of(mcp::JsonRpcResponse const& resp) {
    if (!resp.result.has_value()) return {};
    auto const* task = resp.result->find("task");
    if (!task) return {};
    auto const* id = task->find("taskId");
    if (!id || !id->is_string()) return {};
    return id->as_string();
}

}  // namespace

int main() {
    // ================================================================================
    // Part 1 -- MCP: tasks/get and tasks/cancel are principal-bound (ADR-061 §7 R3).
    // ================================================================================
    {
        auto const              table = ae::ToolTable::from_tools<SlowBackgroundableTool>();
        ae::CapabilitySet const held  = ae::CapabilitySet::grant_root({ae::cap::Background{8}});
        mcp::McpServer          server(table, held, ae::ApprovalDecider{}, "binding-test-server");

        ae::Principal const alice = ae::make_local_cli_principal("alice", "tenant-a");
        ae::Principal const bob   = ae::make_local_cli_principal("bob", "tenant-a");
        // Same id as alice, DIFFERENT tenant -- 018 §6: a cross-tenant id collision is not ownership.
        ae::Principal const alice_other_tenant = ae::make_local_cli_principal("alice", "tenant-b");

        mcp::JsonRpcResponse created  = server.dispatch(task_call_request("c1"), alice);
        std::string const    task_id = task_id_of(created);
        check(created.result.has_value() && !task_id.empty(),
              "R3-POS-1: alice can create a backgrounded task, and it has a real taskId");

        // --- POSITIVE CONTROLS. Without these, every negative case below would also pass against
        // --- a tasks/get that is simply broken for everyone.
        {
            mcp::JsonRpcResponse own = server.dispatch(tasks_request("g1", "tasks/get", task_id), alice);
            check(own.result.has_value() && !own.error.has_value(),
                  "R3-POS-2 (positive control): the OWNING principal can tasks/get its own task");
        }

        // --- NEGATIVE: a different principal in the same tenant.
        mcp::JsonRpcResponse other =
            server.dispatch(tasks_request("g2", "tasks/get", task_id), bob);
        check(other.error.has_value() && !other.result.has_value(),
              "R3-NEG-1: a NON-owning principal cannot tasks/get another principal's task");

        // 012 §4 / 011 §8a: indistinguishable from a task that never existed. Compared against the
        // REAL unknown-id response rather than a hardcoded string, so the two cannot drift apart
        // without this failing.
        mcp::JsonRpcResponse absent =
            server.dispatch(tasks_request("g3", "tasks/get", "no-such-task-at-all"), bob);
        check(other.error.has_value() && absent.error.has_value() &&
                  absent.error->message == other.error->message &&
                  absent.error->code == other.error->code,
              "R3-NEG-2: not-authorized is BYTE-IDENTICAL to not-found -- existence is not leaked "
              "(012 §4: never distinguish not-found from not-authorized)");

        // --- NEGATIVE: same principal id, different tenant (018 §6).
        {
            mcp::JsonRpcResponse cross =
                server.dispatch(tasks_request("g4", "tasks/get", task_id), alice_other_tenant);
            check(cross.error.has_value() && !cross.result.has_value(),
                  "R3-NEG-3: a matching principal id in a DIFFERENT tenant is not ownership "
                  "(018 §6)");
        }

        // --- NEGATIVE: cancel is bound too, and ownership is checked BEFORE task state, so a
        // --- stranger never learns the task exists via a state-specific error.
        {
            mcp::JsonRpcResponse cancel_other =
                server.dispatch(tasks_request("x2", "tasks/cancel", task_id), bob);
            check(cancel_other.error.has_value() &&
                      cancel_other.error->message == "unknown taskId",
                  "R3-NEG-4: a non-owner's tasks/cancel is refused as NOT-FOUND, never with a "
                  "state-specific error that would confirm the task exists");
        }

        // --- POSITIVE CONTROL for cancel: the owner's cancel really does reach the real handler.
        {
            mcp::JsonRpcResponse own_cancel =
                server.dispatch(tasks_request("x1", "tasks/cancel", task_id), alice);
            bool reached_real_handler =
                own_cancel.result.has_value() ||
                (own_cancel.error.has_value() && own_cancel.error->message != "unknown taskId");
            check(reached_real_handler,
                  "R3-POS-3 (positive control): the owner's tasks/cancel reaches the real handler "
                  "-- it is never refused on ownership grounds");
        }

        // --- ADR-061 §7 R13: `Principal{}` has an empty id, and two empty ids must NOT be the same
        // --- principal, or every unauthenticated caller would own every other one's tasks.
        {
            mcp::JsonRpcResponse anon_created = server.dispatch(task_call_request("c2"), ae::Principal{});
            std::string const    anon_id      = task_id_of(anon_created);
            if (!anon_id.empty()) {
                mcp::JsonRpcResponse anon_read =
                    server.dispatch(tasks_request("g5", "tasks/get", anon_id), ae::Principal{});
                check(anon_read.error.has_value(),
                      "R13-NEG-1: an empty-id principal does not own an empty-id principal's task "
                      "-- two anonymous callers are not the same principal");
            }
        }

        // --- Handle entropy (ADR-061 §7 R3). Not a statistical test: it checks the structural
        // --- properties that actually failed before -- ids derived from nothing caller-visible,
        // --- and not repeating.
        {
            std::string const id_a = task_id_of(server.dispatch(task_call_request("c3"), alice));
            std::string const id_b = task_id_of(server.dispatch(task_call_request("c4"), alice));
            check(!id_a.empty() && !id_b.empty() && id_a != id_b,
                  "R3-ENT-1: two tasks created with identical arguments get distinct handles");
            check(id_a.find("alice") == std::string::npos &&
                      id_a.find("tenant-a") == std::string::npos &&
                      id_a.find("slow_backgroundable") == std::string::npos,
                  "R3-ENT-2: a handle embeds no caller-supplied or caller-visible identifier");
            check(id_a.size() == 32,
                  "R3-ENT-3: the handle is a full 128-bit CSPRNG value (32 hex chars), not two "
                  "mt19937 draws");
        }

        // Let the backgrounded workers finish before `server` (captured by their completion lambdas)
        // goes out of scope -- server.hpp's own documented `this`-capture lifetime rule.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    // ================================================================================
    // Part 2 -- A2A: admission is really checked, and GetTask is principal-bound
    // (ADR-061 §7 R1/R2).
    // ================================================================================
    // Deliberately NOT re-testing the happy path (test_a2a_server.cpp already proves it end to end
    // against a real Engine); what this adds is the DENIAL half, which no test covered.
    {
        // A `RunStarter` standing in for a session owning `p-owner`, applying the exact predicate
        // `AgentSession::handle()` applies -- reused rather than re-derived, so it cannot drift.
        ae::Principal const session_owner{"p-owner", ""};
        auto starter = [session_owner](ae::StartRun req) -> ae::result<a2a::RunOutcome> {
            // R2's whole point: `caller` is now always present on the protocol path, so this branch
            // is reachable and meaningful. Previously `A2aServer` sent none and the real
            // `AgentSession` skipped admission entirely.
            if (!req.caller.has_value()) {
                return std::unexpected(
                    ae::error{ae::failure_class::policy, "no caller asserted", "test.no_caller"});
            }
            ae::Principal const caller{req.caller->id, req.caller->tenant_id};
            if (!ae::principal_admitted_for(caller, session_owner)) {
                return std::unexpected(
                    ae::error{ae::failure_class::policy, "admission denied", "test.admission_denied"});
            }
            return a2a::RunOutcome{"s-bind:run:1", ae::AgentResponse{}};
        };

        a2a::A2aServer server(starter, "ctx-bind");

        a2a::Message inbound;
        inbound.message_id = "m1";
        inbound.role        = a2a::a2a_role::user;
        a2a::Part p;
        p.value = a2a::TextPart{"hello"};
        inbound.parts.push_back(std::move(p));

        ae::SessionCaller const owner{"p-owner", ""};
        ae::SessionCaller const stranger{"mallory", ""};

        // --- POSITIVE CONTROL: the owner's message really is admitted and produces a task.
        auto owned = server.send_message(inbound, owner);
        check(owned.has_value() && owned->status.state == a2a::task_state::completed,
              "R2-POS-1 (positive control): the owning caller's message IS admitted and completes");
        std::string const a2a_task_id = owned.has_value() ? owned->id : std::string{};

        // --- NEGATIVE: a stranger is denied by the real admission check. Before this fix
        // --- `A2aServer` passed no caller at all, so this ran with admission SKIPPED.
        {
            auto denied = server.send_message(inbound, stranger);
            check(!denied.has_value() || denied->status.state == a2a::task_state::failed,
                  "R2-NEG-1: a non-owning caller's inbound message does NOT execute as the session "
                  "owner -- 018 §2 admission is really applied on the A2A path");
        }

        if (!a2a_task_id.empty()) {
            auto own_fetch = server.get_task(a2a_task_id, owner);
            check(own_fetch.has_value(),
                  "R1-POS-1 (positive control): the owner can GetTask its own task");

            auto stranger_fetch = server.get_task(a2a_task_id, stranger);
            check(!stranger_fetch.has_value(),
                  "R1-NEG-1: a stranger cannot GetTask another principal's task -- with Task.id "
                  "being the structured, enumerable run_id, this was an unauthenticated read of "
                  "the full conversation history before ADR-061 §7 R1");

            auto absent = server.get_task("no-such-task-at-all", stranger);
            check(!absent.has_value() && !stranger_fetch.has_value() &&
                      absent.error().code == stranger_fetch.error().code &&
                      absent.error().message == stranger_fetch.error().message,
                  "R1-NEG-2: not-authorized is byte-identical to not-found for A2A too (012 §4)");

            auto stranger_cancel = server.cancel_task(a2a_task_id, stranger);
            check(!stranger_cancel.has_value() &&
                      stranger_cancel.error().code == absent.error().code,
                  "R1-NEG-3: CancelTask checks ownership BEFORE task state, so a stranger gets "
                  "not-found rather than the terminal-state error that would confirm existence");

            // --- POSITIVE CONTROL for the ordering above: the OWNER does get the state-specific
            // --- error, proving the not-found the stranger saw was an ownership decision and not
            // --- cancel being broken outright.
            auto owner_cancel = server.cancel_task(a2a_task_id, owner);
            check(!owner_cancel.has_value() &&
                      owner_cancel.error().code != absent.error().code,
                  "R1-POS-2 (positive control): the OWNER's cancel gets the real terminal-state "
                  "rejection, a DIFFERENT error than not-found");
        }

        // --- 018 §4 / ADR-061 §7 R15: under a host-owned transport a credential can ride an id or
        // --- a path, and an error string is a logging/telemetry surface.
        {
            auto absent = server.get_task("SECRET-LOOKING-VALUE", owner);
            check(!absent.has_value() &&
                      absent.error().message.find("SECRET-LOOKING-VALUE") == std::string::npos,
                  "R15-NEG-1: the not-found error does not echo the caller-supplied taskId back "
                  "into a string destined for logs and telemetry");
        }
    }

    if (g_failures == 0) {
        std::printf("test_task_principal_binding: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_task_principal_binding: %d failure(s)\n", g_failures);
    return 1;
}
