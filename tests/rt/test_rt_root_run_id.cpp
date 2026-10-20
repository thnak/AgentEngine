// Proves decisions/ADR-241-egress-proxy-async-path-on-rt-http.md §8.4 (S2 step 1): `EffectContext::root_run_id`
// plumbing -- the run identity the egress proxy's admission buckets are keyed on. No admission exists yet at this
// step; what is proven here is that every place a run starts hands its calls the right ROOT run id, host-derived
// and never client- or model-chosen (I3).
//
//   S1  a top-level session: each run is its own root (root == that run's run_id)
//   S2  a child/node session with an inherited root: every run carries the INHERITED root, not its own id
//   S3  fork_from() copies the inheritance with principal_ (G13h2); a mutant that does not copy it fails here
//   S4  clear_in_process_state() does NOT reset the inheritance (G13h2); a mutant that resets it fails here
//   S5  the session record round-trips root_run_id + delegation_root; an older record (neither field) still loads;
//       a present-but-not-a-string field is malformed (R14(b) closed)
//   S6  run_child_agent_session(): the child's calls carry ChildSpawnRequest::root_run_id (agent.spawn row)
//   S7  a workflow's agent nodes share ONE root (the workflow run's id), and a host-stamped root is inherited
//   S8  the MCP server mints a per-request run id from its own counter -- never the client's JSON-RPC id --
//       on both the synchronous and the task-augmented path
//
// BackgroundJobOwner::root_run_id -> the job's context is asserted in test_rt_background_job_runner.cpp (R11).
// Resource-capped: a handful of in-process sessions, no sockets, one short-lived background thread (S8).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/protocol/mcp/client.hpp"
#include "agentengine/protocol/mcp/server.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_spawn_child_run.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace ae   = agentengine;
namespace mcp  = agentengine::mcp;
namespace json = agentengine::json;
using namespace agentengine;

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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    return agentengine::rt::block_on(std::move(t));
}

struct Seen {
    std::string run_id;
    std::string root_run_id;
};

// A scripted backend that records, from INSIDE chat()'s own EffectContext, the run identity every call carried.
class RecordingClient {
public:
    RecordingClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Seen> seen;
    };
    [[nodiscard]] std::shared_ptr<State> const& shared_state() const { return state_; }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        state_->seen.push_back({ctx.run_id, ctx.root_run_id});
        Message m;
        m.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"done"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{}};
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};
static_assert(ChatClient<RecordingClient>);

using Session = rt::AgentSession<RecordingClient>;

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Principal delegated_principal() {
    Principal root = make_embedded_principal("root-principal", "tenant-a");
    auto derived = derive_on_behalf_of(root, "child-principal");
    return *derived;
}

// ---- S8's probe tool: records the identity a tools/call reached it with ---------------------------------------
std::mutex           g_probe_m;
std::vector<Seen>    g_probe_seen;
std::atomic<int>     g_probe_count{0};

struct ProbeArgs {
    bool noop;
};
AE_JSON_SCHEMA(ProbeArgs, noop)
struct ProbeReply {
    bool ok;
};
AE_JSON_SCHEMA(ProbeReply, ok)

struct ProbeTool : Tool<ProbeTool, Backgroundable> {
    static constexpr std::string_view name        = "probe";
    static constexpr std::string_view description = "Records the run identity it was called with.";
    using Args  = ProbeArgs;
    using Reply = ProbeReply;
    task<result<Reply>> invoke(Args, EffectContext& ctx) const {
        {
            std::lock_guard<std::mutex> lock(g_probe_m);
            g_probe_seen.push_back({ctx.run_id, ctx.root_run_id});
        }
        g_probe_count.fetch_add(1);
        co_return Reply{true};
    }
};

[[nodiscard]] Seen probe_seen(std::size_t i) {
    std::lock_guard<std::mutex> lock(g_probe_m);
    return i < g_probe_seen.size() ? g_probe_seen[i] : Seen{"<missing>", "<missing>"};
}
bool wait_probe(int n) {
    auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        if (g_probe_count.load() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return g_probe_count.load() >= n;
}

[[nodiscard]] workflow::Executor agent_desc(char const* id) {
    return workflow::Executor{.id = id, .kind = workflow::executor_kind::agent, .input_type = "T", .output_type = "T",
                              .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}
[[nodiscard]] workflow::Executor func_desc(char const* id) {
    return workflow::Executor{.id = id, .kind = workflow::executor_kind::function, .input_type = "T",
                              .output_type = "T", .worktree_mode = sharing_mode::branch,
                              .capability_ceiling = {}};
}

}  // namespace

int main() {
    // ---- S1: a top-level session: every run is its own root ------------------------------------------------
    {
        Session s;
        s.initialize("top", make_embedded_principal("p1", "t1"));
        auto seen = s.emplace_chat_client().shared_state();
        (void)drive(s.start_run(rt::StartRun{user_message("one")}));
        (void)drive(s.start_run(rt::StartRun{user_message("two")}));
        check(seen->seen.size() == 2, "S1 setup: two runs reached the model");
        if (seen->seen.size() == 2) {
            check(seen->seen[0].run_id == "top:run:1" && seen->seen[0].root_run_id == "top:run:1",
                  "S1: the first run of a top-level session is its own root");
            check(seen->seen[1].run_id == "top:run:2" && seen->seen[1].root_run_id == "top:run:2",
                  "S1: the second run is a NEW root (its own id), not the first run's");
        }
        check(s.inherited_root_run_id().empty(), "S1: a top-level session inherits nothing");
    }

    // ---- S2: a child/node session carries the INHERITED root on every run -----------------------------------
    Session inheriting;
    {
        inheriting.initialize("child", delegated_principal());
        inheriting.set_inherited_root_run_id("ROOT:run:7");
        auto seen = inheriting.emplace_chat_client().shared_state();
        (void)drive(inheriting.start_run(rt::StartRun{user_message("a")}));
        (void)drive(inheriting.start_run(rt::StartRun{user_message("b")}));
        check(seen->seen.size() == 2, "S2 setup: two runs reached the model");
        if (seen->seen.size() == 2) {
            check(seen->seen[0].run_id == "child:run:1" && seen->seen[0].root_run_id == "ROOT:run:7",
                  "S2: the child's first run is charged to the inherited root, not to its own run id");
            check(seen->seen[1].run_id == "child:run:2" && seen->seen[1].root_run_id == "ROOT:run:7",
                  "S2: so is its second run (a child never becomes a root of its own)");
        }
    }

    // ---- S3: fork_from() copies the inheritance (G13h2) ------------------------------------------------------
    {
        Session forked;
        forked.fork_from(inheriting, "forked");
        check(forked.inherited_root_run_id() == "ROOT:run:7",
              "S3: fork_from() copies inherited_root_run_id_ together with principal_ (G13h2)");
        auto seen = forked.emplace_chat_client().shared_state();
        (void)drive(forked.start_run(rt::StartRun{user_message("f")}));
        check(seen->seen.size() == 1 && seen->seen[0].root_run_id == "ROOT:run:7",
              "S3: a run of the fork is still charged to the root");
    }

    // ---- S4: clear_in_process_state() keeps the inheritance (G13h2) ------------------------------------------
    {
        Session child2;
        child2.initialize("child2", delegated_principal());
        child2.set_inherited_root_run_id("ROOT:run:9");
        child2.clear_in_process_state();
        check(child2.inherited_root_run_id() == "ROOT:run:9",
              "S4: clear_in_process_state() does NOT reset the inheritance (a reset child would become its own root)");
        child2.initialize("child2b", delegated_principal());
        auto seen = child2.emplace_chat_client().shared_state();
        (void)drive(child2.start_run(rt::StartRun{user_message("c")}));
        check(seen->seen.size() == 1 && seen->seen[0].root_run_id == "ROOT:run:9",
              "S4: after the clear a run is still charged to the root, not to its own id");
    }

    // ---- S5: the session record carries root_run_id and delegation_root ------------------------------------
    {
        rt::AgentSessionRecord const rec = inheriting.to_record();
        check(rec.root_run_id == "ROOT:run:7", "S5: to_record() carries the inherited root run id");
        check(rec.principal_delegation_root == "root-principal",
              "S5: to_record() carries the principal's delegation_root (R14(b))");
        auto decoded = rt::decode_agent_session_record(rt::encode_agent_session_record(rec));
        check(decoded.has_value() && decoded->root_run_id == "ROOT:run:7" &&
                  decoded->principal_delegation_root == "root-principal",
              "S5: both fields survive the JSON round trip");
        Session restored;
        if (decoded) restored.restore_from_record(*decoded);
        check(restored.inherited_root_run_id() == "ROOT:run:7",
              "S5: restore_from_record() restores the inherited root (a restored child is not a fresh root)");
        check(restored.principal().root_id() == "root-principal",
              "S5: restore_from_record() keeps delegation_root, so the principal level still keys on the root");

        // An older record: neither field. It still loads, as an empty (top-level, non-delegated) session.
        json::Value old = rt::agent_session_record_to_json(rec);
        std::vector<std::pair<std::string, json::Value>> stripped;
        for (auto const& [k, v] : old.as_object()) {
            if (k != "root_run_id" && k != "principal_delegation_root") stripped.emplace_back(k, v);
        }
        auto old_decoded = rt::agent_session_record_from_json(json::Value::make_object(std::move(stripped)));
        check(old_decoded.has_value() && old_decoded->root_run_id.empty() &&
                  old_decoded->principal_delegation_root.empty(),
              "S5: an older record without the two fields still loads (additive)");
        // A present-but-wrong-type field is malformed, never silently ignored.
        std::vector<std::pair<std::string, json::Value>> wrong;
        for (auto const& [k, v] : old.as_object()) {
            wrong.emplace_back(k, k == "root_run_id" ? json::Value::make_number(1) : v);
        }
        auto bad = rt::agent_session_record_from_json(json::Value::make_object(std::move(wrong)));
        check(!bad.has_value(), "S5: a non-string root_run_id is a malformed record");
    }

    // ---- S6: run_child_agent_session(): ChildSpawnRequest::root_run_id reaches the child's calls --------------
    {
        rt::ChildSpawnRequest req;
        req.input        = user_message("go");
        req.principal    = make_embedded_principal("parent", "tenant-a");
        req.root_run_id  = "ROOT:run:3";
        std::shared_ptr<RecordingClient::State> child_state;
        auto reply = drive(rt::run_child_agent_session<RecordingClient>(
            "spawned-1", std::move(req), [&child_state](RecordingClient& c) { child_state = c.shared_state(); }));
        check(reply.has_value(), "S6 setup: the child run completed");
        check(child_state && child_state->seen.size() == 1,
              "S6 setup: the child's model call was observed through the client's shared state");
        if (child_state && child_state->seen.size() == 1) {
            check(child_state->seen[0].root_run_id == "ROOT:run:3" && child_state->seen[0].run_id == "spawned-1:run:1",
                  "S6: the child's calls carry ChildSpawnRequest::root_run_id (the ROOT), not the child's own run id");
        }
    }

    // ---- S7: a workflow's agent nodes share ONE root --------------------------------------------------------
    {
        Session a1, a2;
        a1.initialize("n1", make_embedded_principal("wf-owner", "t1"));
        a2.initialize("n2", make_embedded_principal("wf-owner", "t1"));
        auto seen1 = a1.emplace_chat_client().shared_state();
        auto seen2 = a2.emplace_chat_client().shared_state();
        std::vector<Seen> fn_seen;
        rt::ExecutorBody fn = [&fn_seen](Message const& in, EffectContext& ctx) -> task<result<rt::ExecutorOutcome>> {
            fn_seen.push_back({ctx.run_id, ctx.root_run_id});
            co_return rt::ExecutorOutcome{in};
        };

        workflow::Workflow wf;
        wf.id        = "wf-1";
        wf.executors = {agent_desc("a"), func_desc("f"), agent_desc("b")};
        wf.edges.push_back(workflow::Edge{"a", "f", workflow::edge_kind::direct, {}});
        wf.edges.push_back(workflow::Edge{"f", "b", workflow::edge_kind::direct, {}});
        wf.start = "a";
        wf.output_selection.push_back("b");
        wf.bound.max_rounds = 5;

        rt::WorkflowSupervisor sup;
        sup.initialize(wf, {rt::agent_session_as_executor_body(a1), fn, rt::agent_session_as_executor_body(a2)});
        auto result_ = drive(sup.run_workflow(rt::RunWorkflow{user_message("start")}));
        check(result_.status == rt::workflow_status::completed, "S7 setup: the workflow completed");
        std::string const wf_run = sup.run_id();
        check(!wf_run.empty(), "S7 setup: the workflow run has an id");
        check(seen1->seen.size() == 1 && seen2->seen.size() == 1 && fn_seen.size() == 1,
              "S7 setup: both agent nodes and the function node ran once");
        if (seen1->seen.size() == 1 && seen2->seen.size() == 1 && fn_seen.size() == 1) {
            check(seen1->seen[0].root_run_id == wf_run && seen2->seen[0].root_run_id == wf_run,
                  "S7: both agent nodes are charged to the workflow run (ONE shared root, G13h)");
            check(seen1->seen[0].run_id != seen2->seen[0].run_id,
                  "S7: ...although each node session has its own run id (so the roots are shared by inheritance)");
            check(fn_seen[0].root_run_id == wf_run, "S7: a function node's context carries the same stamped root");
        }

        // A host-stamped root (a workflow driven from a tool of an outer run) is inherited, not overwritten.
        Session a3;
        a3.initialize("n3", make_embedded_principal("wf-owner", "t1"));
        auto seen3 = a3.emplace_chat_client().shared_state();
        workflow::Workflow wf2;
        wf2.id        = "wf-2";
        wf2.executors = {agent_desc("a")};
        wf2.start     = "a";
        wf2.output_selection.push_back("a");
        wf2.bound.max_rounds = 2;
        EffectContext stamped;
        stamped.root_run_id = "OUTER:run:1";
        rt::WorkflowSupervisor sup2;
        sup2.initialize(wf2, {rt::agent_session_as_executor_body(a3)}, {stamped});
        (void)drive(sup2.run_workflow(rt::RunWorkflow{user_message("start")}));
        check(seen3->seen.size() == 1 && seen3->seen[0].root_run_id == "OUTER:run:1",
              "S7: a root the host stamped on the node context is inherited, never replaced by the workflow's own id");
    }

    // ---- S8: the MCP server mints a per-request run id ------------------------------------------------------
    {
        auto const table = ToolTable::from(ProbeTool{});
        CapabilitySet const held = CapabilitySet::grant_root({cap::Background{4}});
        mcp::McpServer server(table, held, ApprovalDecider{}, "root-run-id-test-server");
        Principal const caller = make_local_cli_principal("mcp-caller", "tenant-a");
        auto call = [&](std::string const& rpc_id) {
            mcp::JsonRpcRequest req{mcp::RpcId{rpc_id}, "tools/call",
                                    json::Value::make_object(
                                        {{"name", json::Value::make_string("probe")},
                                         {"arguments", json::Value::make_object({{"noop", json::Value::make_bool(true)}})}})};
            return server.dispatch(req, caller);
        };
        (void)call("client-chosen-id");
        (void)call("client-chosen-id");  // the SAME JSON-RPC id twice: must still be two distinct runs
        Seen const first  = probe_seen(0);
        Seen const second = probe_seen(1);
        check(first.run_id.rfind("mcp:", 0) == 0 && first.root_run_id == first.run_id,
              "S8: a synchronous tools/call carries a server-minted run id as its root");
        check(first.run_id != second.run_id,
              "S8: two requests with the SAME client-chosen JSON-RPC id get distinct server-minted run ids");
        check(first.run_id.find("client-chosen-id") == std::string::npos,
              "S8: the client's JSON-RPC id never appears in the run identity (I3: not client-chosen)");

        mcp::McpClient client([&server, &caller](mcp::JsonRpcRequest const& r) -> task<mcp::JsonRpcResponse> {
                                  co_return server.dispatch(r, caller);
                              },
                              "root-run-id-client");
        auto handle = drive(client.call_tool_as_task("probe", json::Value::make_object({{"noop", json::Value::make_bool(true)}})));
        check(handle.has_value(), "S8 setup: the task-augmented call was accepted");
        check(wait_probe(3), "S8 setup: the backgrounded probe ran");
        Seen const third = probe_seen(2);
        check(third.run_id.rfind("mcp:", 0) == 0 && third.root_run_id == third.run_id &&
                  third.run_id != first.run_id && third.run_id != second.run_id,
              "S8: the task-augmented path (server.hpp, the second start site) mints its own run id too");

        // red-team r5 m6: a SECOND server instance's first request must not mint the id the first instance's first request
        // did (the counter alone restarts at 1; a per-instance host-side nonce keeps them apart).
        mcp::McpServer server2(table, held, ApprovalDecider{}, "root-run-id-test-server");
        mcp::JsonRpcRequest req2{mcp::RpcId{std::string{"client-chosen-id"}}, "tools/call",
                                 json::Value::make_object(
                                     {{"name", json::Value::make_string("probe")},
                                      {"arguments", json::Value::make_object({{"noop", json::Value::make_bool(true)}})}})};
        (void)server2.dispatch(req2, caller);
        Seen const other_instance_first = probe_seen(3);
        check(other_instance_first.run_id.rfind("mcp:", 0) == 0 && other_instance_first.run_id != first.run_id &&
                  other_instance_first.root_run_id == other_instance_first.run_id,
              "S8/m6: two McpServer instances mint different run ids for their first request");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_rt_root_run_id: ALL PASS\n" : "test_rt_root_run_id: %d FAILURE(S)\n",
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
