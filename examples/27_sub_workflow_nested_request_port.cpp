// AgentEngine "get started" examples, 27 -- executor_kind::sub_workflow's real runtime bridge, with
// nested request_port proxying (ADR-157, GitHub issues #33/#38).
//
// Mirrors MAF's samples/03-workflows/composition/sub_workflow_request_interception.py in spirit: a
// sub-workflow (here, an approval pipeline: draft -> review[request_port]) is wrapped as ONE node
// inside an outer workflow. When the INNER workflow suspends on its own request_port, the OUTER
// graph itself suspends -- the outer caller sees a single, ordinary Interaction (no knowledge of
// the inner workflow's own interaction_id -- namespacing is handled entirely inside
// WorkflowSupervisor, see docs/planning/sub-workflow-nested-request-port-design-draft.md §3d) and
// resumes it exactly like any other request_port. The inner workflow then genuinely completes, and
// the OUTER graph continues past the sub_workflow node with the inner's real result.
//
// Fully offline -- no live model needed, matching examples 10/13/20/23/25's own style.
//
// Run: ./agentengine_example_27_sub_workflow_nested_request_port

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
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

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// ---- The inner "approval pipeline" workflow: draft -> review(request_port) -----------------------

[[nodiscard]] Workflow approval_pipeline_graph() {
    Workflow wf;
    wf.id        = "approval-pipeline";
    wf.executors = {node_desc("draft"), node_desc("review", executor_kind::request_port)};
    wf.edges.push_back(Edge{"draft", "review", edge_kind::direct, {}});
    wf.start = "draft";
    wf.output_selection.push_back("review");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> approval_pipeline_bodies() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message("DRAFT: " + text_of(in))};
        },
        {},  // review is a request_port -- no body
    };
}

// ---- The outer workflow: intake -> approval_pipeline(sub_workflow) -> publish ----------------------

[[nodiscard]] Workflow outer_graph() {
    Workflow wf;
    wf.id        = "publishing-workflow";
    wf.executors = {node_desc("intake"), node_desc("pipeline", executor_kind::sub_workflow),
                     node_desc("publish")};
    wf.edges.push_back(Edge{"intake", "pipeline", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"pipeline", "publish", edge_kind::direct, {}});
    wf.start = "intake";
    wf.output_selection.push_back("publish");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> outer_bodies() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message("Intake: " + text_of(in))};
        },
        {},  // pipeline is sub_workflow-kind -- bound via bind_sub_workflow(), not a body
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message("PUBLISHED -- " + text_of(in))};
        },
    };
}

}  // namespace

int main() {
    Workflow const outer = outer_graph();
    check(validate_workflow(outer).has_value(), "the outer graph validates");

    WorkflowSupervisor sup;
    sup.initialize(outer, outer_bodies());

    // Bind BEFORE the graph is treated as runnable -- an unbound sub_workflow node refuses
    // (check_workflow_executable() no longer unconditionally refuses the KIND, but
    // WorkflowSupervisor's own structural check does refuse an UNBOUND node).
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(approval_pipeline_graph(), approval_pipeline_bodies());
    sup.bind_sub_workflow("pipeline", inner);  // flips valid_ true -- no second initialize() needed

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("Q3 roadmap")}));
    check(r1.status == workflow_status::suspended,
          "the outer run suspends -- the INNER workflow's own request_port surfaced through the "
          "outer graph's ordinary Interaction/open_interactions surface");
    check(r1.open_interactions.size() == 1, "exactly one outer-visible interaction is open");
    std::printf("[outer] suspended, waiting on interaction=%s\n",
                r1.open_interactions.empty() ? "?" : r1.open_interactions.at(0).interaction_id.c_str());

    // The outer caller resumes with a PLAIN answer -- it has no knowledge of, and never needs to
    // know, the inner workflow's own interaction_id. Namespacing is entirely internal.
    std::string const outer_interaction_id =
        r1.open_interactions.empty() ? std::string{} : r1.open_interactions.at(0).interaction_id;
    WorkflowResult r2 = drive(
        sup.resume_workflow(ResumeWorkflow{outer_interaction_id, text_message("approved by reviewer"), {}}));

    check(r2.status == workflow_status::completed,
          "resuming the outer's own interaction completes the whole run -- the inner workflow "
          "genuinely finished, and the OUTER graph continued past the sub_workflow node");
    std::printf("[outer] completed: %s\n", text_of(r2.output).c_str());
    check(text_of(r2.output) == "PUBLISHED -- approved by reviewer",
          "the final output carries the inner's real resolution ('approved by reviewer'), routed "
          "through the outer's own publish node -- genuine end-to-end nested composition, not a stub");

    std::fprintf(stderr, g_failures == 0 ? "example_27_sub_workflow_nested_request_port: OK\n"
                                          : "example_27_sub_workflow_nested_request_port: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
