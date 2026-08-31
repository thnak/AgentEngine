// AgentEngine "get started" examples, 21 -- a whole Workflow, reused as ONE participant inside an
// OUTER MagenticWorkflowBuilder graph (adjacent to GitHub issue #35; design draft:
// docs/planning/workflow-as-executor-body-adapter-design-draft.md).
//
// Mirrors MAF's samples/03-workflows/agents/sequential_workflow_as_agent.py in spirit --
// `workflow.as_agent()` there, `agentengine::rt::workflow_as_executor_body()` here
// (rt/workflow_as_executor.hpp). The "researcher" participant below is not an ordinary function or a
// live model call -- it is an entire two-step INNER Workflow ("fetch" -> "digest"), wrapped once and
// handed into the OUTER MagenticWorkflowBuilder's `bodies` vector exactly like any other participant.
// Proves the adapter is genuinely composable with this session's own convenience builder (ADR-149),
// not just unit-tested standalone (see tests/test_rt_workflow_as_executor.cpp's own W7 for the
// minimal-graph version of the same proof).
//
// Also demonstrates a real design-draft disclosure directly: the OUTER graph declares this
// participant as `TypedExecutor<TaskMsg, ReportMsg>` (the outer manager's own message types) while
// the WRAPPED inner workflow's two nodes use plain untyped `Message` internally and have no
// relationship to `TaskMsg`/`ReportMsg` at all -- nothing checks that correspondence at compile time
// or at `initialize()`; it is the author's responsibility, exactly like any hand-written
// `ExecutorBody`.
//
// Fully offline -- no live model needed, matching examples 10/13/14/20's own style.
//
// Run: ./agentengine_example_21_workflow_as_participant

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_as_executor.hpp"
#include "agentengine/workflow/magentic.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_as_executor_body;
using agentengine::rt::workflow_status;

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

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

// A synthetic moderator, matching examples/15's own non-live decision style: routes to "researcher"
// until it sees the inner workflow's own two-step signature in the transcript, then to "writer", then
// finishes.
[[nodiscard]] ExecutorBody moderator() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const transcript = text_of(in);
        std::string        decision;
        if (transcript.find(">digest") == std::string::npos) decision = "researcher";
        else if (transcript.find("Writer:") == std::string::npos) decision = "writer";
        else decision = "done";
        std::printf("[moderator] %s\n", decision.c_str());
        return ExecutorOutcome{text_message(transcript), {decision}};
    };
}

[[nodiscard]] Workflow inner_research_graph() {
    Workflow wf;
    wf.id        = "inner-research";
    wf.executors = {Executor{.id = "fetch", .kind = executor_kind::function, .input_type = "T",
                              .output_type = "T", .worktree_mode = sharing_mode::branch,
                              .capability_ceiling = {}},
                     Executor{.id = "digest", .kind = executor_kind::function, .input_type = "T",
                              .output_type = "T", .worktree_mode = sharing_mode::branch,
                              .capability_ceiling = {}}};
    wf.edges.push_back(Edge{"fetch", "digest", edge_kind::direct, {}});
    wf.start = "fetch";
    wf.output_selection.push_back("digest");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> inner_research_bodies() {
    return {
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">fetch")};
        },
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">digest")};
        },
    };
}

}  // namespace

// The outer graph's own message types -- unrelated to the wrapped inner workflow's plain `Message`
// nodes, exactly as this file's own top comment describes.
struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.WorkflowAsParticipant.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.WorkflowAsParticipant.ReportMsg");

int main() {
    // The inner workflow, wrapped ONCE via the owning (shared_ptr) overload -- safe to hand into the
    // outer `bodies` vector and call from wherever the outer supervisor dispatches it.
    auto inner_sup = std::make_shared<WorkflowSupervisor>();
    inner_sup->initialize(inner_research_graph(), inner_research_bodies());
    result<ExecutorBody> researcher_body = workflow_as_executor_body(inner_sup);
    check(researcher_body.has_value(), "the inner research workflow wraps successfully (no request_port)");
    if (!researcher_body) {
        std::fprintf(stderr, "example_21_workflow_as_participant: FAIL (wrap error: %s)\n",
                     researcher_body.error().message.c_str());
        return 1;
    }

    MagenticWorkflowBuilder<TaskMsg, ReportMsg> builder("research-writer-magentic");
    builder.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "moderator", .capability_ceiling = {}});
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "researcher", .capability_ceiling = {}});  // the WRAPPED workflow
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "writer", .capability_ceiling = {}});      // an ordinary function
    builder.max_rounds(10);

    result<MagenticGraph> built = builder.build();
    check(built.has_value(), "the MagenticWorkflowBuilder builds a valid graph around the wrapped participant");
    if (!built) {
        std::fprintf(stderr, "example_21_workflow_as_participant: FAIL (build error: %s)\n",
                     built.error().message.c_str());
        return 1;
    }
    check(validate_workflow(built->graph).has_value(), "the produced graph validates");

    // `bodies` is parallel to `built->graph.executors` BY INDEX -- manager, participants in call
    // order (researcher, writer), then the synthetic "done" sink (MagenticWorkflowBuilder's own
    // add-order convention, matching examples/17/19).
    std::vector<ExecutorBody> bodies = {
        moderator(),
        *researcher_body,  // <-- the whole inner Workflow, reused as an ORDINARY participant
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + "\n\nWriter: summary complete.")};
        },
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{in};  // the builder's synthetic "done" sink
        },
    };

    WorkflowSupervisor outer_sup;
    outer_sup.initialize(built->graph, bodies, {}, built->manager_id);

    WorkflowResult r = drive(outer_sup.run_workflow(RunWorkflow{text_message("Task: research and write a summary.")}));
    check(r.status == workflow_status::completed, "the outer run completes");
    std::printf("\n--- status: %d, rounds: %u ------------------------------\n",
                static_cast<int>(r.status), r.rounds);
    std::printf("%s\n", text_of(r.output).c_str());
    check(text_of(r.output).find(">fetch>digest") != std::string::npos,
          "the final transcript carries the WRAPPED inner workflow's real two-step output "
          "(>fetch>digest), not a stub -- the composition genuinely ran end to end");
    check(text_of(r.output).find("Writer: summary complete.") != std::string::npos,
          "the ordinary function participant also ran, alongside the wrapped workflow participant, "
          "in the same outer graph");

    std::fprintf(stderr, g_failures == 0 ? "example_21_workflow_as_participant: OK\n"
                                          : "example_21_workflow_as_participant: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
