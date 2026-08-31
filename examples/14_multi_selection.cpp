// AgentEngine "get started" examples, 14 -- a caller-chosen SUBSET fires (multi_selection).
//
// The sixth edge kind, with no MAF sample of its own to mirror -- and no 014 §3 pattern row of its
// own either (it's the "sixth edge kind" the eight named patterns don't individually cover).
// `edge_kind::multi_selection` sits between the two edge kinds the other examples already show:
// `switch_case` (10_conditional_routing.cpp) selects exactly ONE branch; `fan_out`
// (09_concurrent_workflow.cpp) always fires ALL of them. `multi_selection` selects a caller-chosen
// SUBSET -- more than one is allowed (unlike switch_case), and fewer than all is allowed (unlike
// fan_out). The routing mechanism is identical to switch_case's: the source executor returns the
// case labels it selects in `ExecutorOutcome::routes`; only the edges whose label appears there
// fire.
//
// Scenario: a change classifier picks WHICH reviewers a change needs -- security and docs here,
// skipping performance -- rather than either asking one fixed reviewer or all three every time.
//
// ADR-037: driven through `agentengine::rt::WorkflowSupervisor` -- a plain coroutine substrate, no
// Quark actor engine underneath. `rt::ExecutorBody` bodies are wired straight into
// `WorkflowSupervisor::initialize()` by index (parallel to `wf.executors`); no engine, no router,
// no per-node actor, no placement dance. The old harness's `FunctionExecutor::invocations()`
// counter has no rt:: equivalent (an `ExecutorBody` is a plain `std::function`, not an actor with
// its own state), so `counted()` below wraps a body with its own `std::shared_ptr<int>` tally to
// keep the same "did this reviewer actually run" proof.
//
// Run: ./agentengine_example_14_multi_selection

#include <cstdio>
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

// Picks which reviewers this change needs -- an ordinary function of the input here; a model-backed
// classifier would fill this same slot in a real agent-kind node.
[[nodiscard]] ExecutorBody pick_reviewers(std::vector<std::string> selected) {
    return [selected = std::move(selected)](Message const& in,
                                             EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ">triage"), selected};
    };
}

[[nodiscard]] ExecutorBody reviewer(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        return text_message(text_of(in) + ">" + name);
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
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
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
    auto security_calls    = std::make_shared<int>(0);
    auto performance_calls = std::make_shared<int>(0);
    auto docs_calls        = std::make_shared<int>(0);

    Workflow wf;
    wf.id        = "reviewer-selection";
    wf.executors = {node_desc("triage"), node_desc("security"), node_desc("performance"),
                     node_desc("docs")};
    wf.edges.push_back(Edge{"triage", "security", edge_kind::multi_selection, "security"});
    wf.edges.push_back(Edge{"triage", "performance", edge_kind::multi_selection, "performance"});
    wf.edges.push_back(Edge{"triage", "docs", edge_kind::multi_selection, "docs"});
    wf.start = "triage";
    wf.output_selection.push_back("docs");
    wf.bound.max_rounds = 8;
    check(validate_workflow(wf).has_value(), "the graph validates");

    // Bodies are parallel to wf.executors BY INDEX: triage(0), security(1), performance(2), docs(3).
    std::vector<ExecutorBody> bodies = {
        pick_reviewers({"security", "docs"}),  // skips "performance"
        counted(reviewer("security"), security_calls),
        counted(reviewer("performance"), performance_calls),
        counted(reviewer("docs"), docs_calls),
    };

    WorkflowSupervisor supervisor;
    supervisor.initialize(wf, bodies);

    WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("change #482")}));
    check(r.status == workflow_status::completed, "the graph terminates by running dry");
    std::printf("%s\n", text_of(r.output).c_str());
    check(*security_calls == 1 && *docs_calls == 1,
          "BOTH selected reviewers (security, docs) ran -- more than one, unlike switch_case");
    check(*performance_calls == 0,
          "the unselected reviewer (performance) never ran -- fewer than all, unlike fan_out");

    std::fprintf(stderr, g_failures == 0 ? "example_14_multi_selection: OK\n"
                                          : "example_14_multi_selection: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
