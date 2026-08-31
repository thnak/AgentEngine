// AgentEngine "get started" examples, 15 -- Group Chat and Planner (Magentic), the SAME graph shape.
//
// 014 §3 gives these two named patterns the identical graph shape -- "a moderator executor cycling
// among participants" -- and distinguishes them by exactly one thing: WHO owns the stopping
// decision. This example builds that one shape twice, changing only the moderator's own body, to
// make that distinction observable rather than asserted:
//
//   - Group Chat: the CALLER's round bound is the termination contract. The moderator just keeps
//     cycling between participants; nothing inside the graph ever decides "we're done" -- the run
//     ends at `bound.max_rounds`, and `workflow_status::bound_max_rounds` says so honestly.
//   - Planner (Magentic): the SAME cyclic shape, but the moderator keeps its own ledger and routes
//     to a terminal `done` node once the goal is met. The round bound is only a safety valve, set
//     far above what the ledger should ever need -- the run ends by running dry
//     (`workflow_status::completed`), well before the bound.
//
// Both use `edge_kind::switch_case` for the moderator's own routing choice (which participant, or
// "done") and plain `edge_kind::direct` edges for each participant's reply back to the moderator --
// the same two edge kinds 10_conditional_routing.cpp and 13_reflection_loop.cpp already introduced,
// just composed into a cycle with a routing decision at one end of it.
//
// ADR-037: driven through `agentengine::rt::WorkflowSupervisor` -- a plain coroutine substrate, no
// Quark actor engine underneath. `rt::ExecutorBody` bodies are wired straight into
// `WorkflowSupervisor::initialize()` by index (parallel to `wf.executors`); no engine, no router,
// no per-node actor, no placement dance. The old harness's `FunctionExecutor::invocations()`
// counter has no rt:: equivalent (an `ExecutorBody` is a plain `std::function`, not an actor with
// its own state), so `counted()` below wraps a body with its own `std::shared_ptr<int>` tally to
// keep the same "both participants actually ran" proof.
//
// Run: ./agentengine_example_15_group_chat_and_planner

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + ">" + name);
    };
}

// A moderator: counts how many times "moderator" already appears in the transcript (i.e. how many
// participant turns have happened) and hands that count to `choose`, an ordinary function of the
// input -- a model-backed moderator would fill this same slot in a real agent-kind node.
[[nodiscard]] ExecutorBody moderator(std::function<std::string(std::size_t)> choose) {
    return [choose = std::move(choose)](Message const& in,
                                         EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        std::size_t       turns = 0;
        for (std::size_t i = 0; i + 1 < text.size(); ++i) {
            if (text[i] == '>' && text[i + 1] == 'm') ++turns;
        }
        return ExecutorOutcome{text_message(text + ">mod"), {choose(turns)}};
    };
}

// Wraps a body with an invocation tally -- the rt:: stand-in for the old harness's
// `FunctionExecutor::invocations()` (see file banner).
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<int> tally) {
    return [body = std::move(body), tally](Message const& in,
                                           EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
        ++*tally;
        return body(in, ctx);
    };
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}

// Safe here: run_workflow()'s only suspension points are the run mutex's uncontended fast path and
// a nested co_await whose own body never suspends either -- see
// tests/test_rt_workflow_supervisor.cpp's own drive<T>() comment for the full reasoning.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // ---- Group Chat: the moderator cycles forever; the CALLER's bound stops it -------------------
    {
        auto p1_calls = std::make_shared<int>(0);
        auto p2_calls = std::make_shared<int>(0);

        Workflow wf;
        wf.id        = "group-chat";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("p1");
        wf.output_selection.push_back("p2");
        wf.bound.max_rounds = 6;
        check(validate_workflow(wf).has_value(), "group chat: the moderator-cycle graph validates");

        std::vector<ExecutorBody> bodies = {
            moderator([](std::size_t turns) { return turns % 2 == 0 ? "p1" : "p2"; }),
            counted(appender("p1"), p1_calls),
            counted(appender("p2"), p2_calls),
        };
        WorkflowSupervisor supervisor;
        supervisor.initialize(wf, bodies);

        WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("discuss")}));
        check(r.status == workflow_status::bound_max_rounds,
              "group chat: the CALLER's round bound is the termination contract -- nothing "
              "inside the graph ever decided to stop");
        check(*p1_calls > 0 && *p2_calls > 0,
              "group chat: the moderator cycled among BOTH participants, not just one");
        std::printf("[group chat] %s\n", text_of(r.output).c_str());
    }

    // ---- Planner (Magentic): the SAME shape, but the moderator's own ledger owns completion ------
    {
        Workflow wf;
        wf.id        = "planner";
        wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2"), node_desc("done")};
        wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
        wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
        wf.edges.push_back(Edge{"moderator", "done", edge_kind::switch_case, "done"});
        wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
        wf.start = "moderator";
        wf.output_selection.push_back("done");
        wf.bound.max_rounds = 20;  // a safety valve, deliberately far above what the ledger needs
        check(validate_workflow(wf).has_value(), "planner: the same cyclic shape validates");

        std::vector<ExecutorBody> bodies = {
            moderator([](std::size_t turns) {
                if (turns >= 2) return std::string("done");  // the ledger: goal met after 2 turns
                return turns % 2 == 0 ? std::string("p1") : std::string("p2");
            }),
            appender("p1"),
            appender("p2"),
            appender("done"),
        };
        WorkflowSupervisor supervisor;
        supervisor.initialize(wf, bodies);

        WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("plan")}));
        check(r.status == workflow_status::completed,
              "planner: the MODERATOR decided completion and routed to the terminal node -- the "
              "run ended by running dry, NOT at its bound (the one observable difference from "
              "group chat above, which shares this graph shape exactly)");
        check(r.rounds < 20, "planner: the round bound was a safety valve, never reached");
        std::printf("[planner]    %s (after %llu rounds, bound was 20)\n",
                    text_of(r.output).c_str(), static_cast<unsigned long long>(r.rounds));
    }

    std::fprintf(stderr, g_failures == 0 ? "example_15_group_chat_and_planner: OK\n"
                                          : "example_15_group_chat_and_planner: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
