// AgentEngine "get started" examples, 22 -- Magentic plan sign-off, suspended across a simulated
// process restart (ADR-149, GitHub issue #28 items 3+4, combined).
//
// Mirrors MAF's samples/03-workflows/orchestrations/magentic_human_plan_review.py (a Magentic
// manager pauses for a human to approve its plan before any participant runs) AND
// samples/03-workflows/orchestrations/magentic_checkpoint.py (the exact mechanics of persisting that
// paused request across a restart and resuming with the saved response) -- IN ONE example, because
// that combination is the actual MAF scenario neither `tests/test_workflow_magentic_plan_signoff.cpp`
// (proves the plan-signoff mechanism in-process, no restart) nor `examples/20_workflow_checkpoint_
// resume.cpp` (proves checkpoint/resume, but on a plain function graph with no Magentic manager or
// typed payload) demonstrates on its own.
//
// Nothing new: `MagenticWorkflowBuilder::require_plan_signoff()` wires an ordinary `request_port`
// node exactly like any other (ADR-149 finding 3), and `WorkflowCheckpointManager` is the same
// attach()/resume_or_start() pair 20 already uses. This is composition, not new mechanism.
//
// Run: ./agentengine_example_22_magentic_plan_signoff_checkpoint

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_checkpoint_manager.hpp"
#include "agentengine/workflow/magentic.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::FileSessionStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowCheckpointManager;
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

// The manager: a plain function standing in for a real model-backed planner (17/19's own live
// counterpart shows what that slot looks like against a real model -- the plan-signoff mechanism
// doesn't care which produced the plan). Three states, told apart by what's IN the input, not by
// any hidden counter -- the same "read your own transcript" discipline 17_planner_live.cpp uses:
//   1. Nothing decided yet (the host's own initial task) -> propose a plan, route to plan_review.
//   2. A typed MagenticPlanSignoffResponse came back through the resumed port -> if approved, hand
//      the task to the writer; if rejected, finish immediately with the reviewer's own feedback.
//   3. The writer's report came back -> finish, carrying the report.
[[nodiscard]] ExecutorBody manager_body() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (result<MagenticPlanSignoffResponse> resp = parse_plan_signoff_response(in); resp.has_value()) {
            if (!resp->approved) {
                return ExecutorOutcome{text_message("Plan rejected: " + resp->feedback), {"done"}};
            }
            return ExecutorOutcome{text_message("draft the Q3 roadmap"), {"writer"}};
        }
        std::string const text = text_of(in);
        if (text.rfind("REPORT:", 0) == 0) {
            return ExecutorOutcome{text_message("Final: " + text), {"done"}};
        }
        Message req = make_plan_signoff_request(
            MagenticPlanSignoffRequest{"Step 1: outline the roadmap. Step 2: write it up."});
        return ExecutorOutcome{req, {"plan_review"}};
    };
}

[[nodiscard]] ExecutorBody writer_body() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message("REPORT: " + text_of(in))};
    };
}

[[nodiscard]] result<MagenticGraph> make_graph();

}  // namespace

// Declared at file scope (not inside the anonymous namespace above) so AE_WORKFLOW_MESSAGE's
// message_type<> specialization below targets the SAME type make_graph() uses -- matching
// examples/21_workflow_as_participant.cpp's identical placement.
struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Example22.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Example22.ReportMsg");

namespace {

[[nodiscard]] result<MagenticGraph> make_graph() {
    MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("plan-signoff-checkpoint-demo");
    b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr"});
    b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "writer"});
    b.require_plan_signoff("plan_review");
    b.max_rounds(20);
    return b.build();
}

#if defined(_WIN32)
[[nodiscard]] int current_pid() noexcept { return ::_getpid(); }
#else
[[nodiscard]] int current_pid() noexcept { return ::getpid(); }
#endif

[[nodiscard]] std::filesystem::path make_temp_root() {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ae_example_22_magentic_plan_signoff_checkpoint_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    result<MagenticGraph> built = make_graph();
    check(built.has_value(), "MagenticWorkflowBuilder builds the manager/writer/plan_review graph");
    if (!built) {
        std::fprintf(stderr, "example_22_magentic_plan_signoff_checkpoint: FAIL (build error: %s)\n",
                     built.error().message.c_str());
        return 1;
    }
    check(validate_workflow(built->graph).has_value(), "the produced graph validates");

    // `bodies` is parallel to `built->graph.executors` BY INDEX -- manager, participants in call
    // order (writer), the synthetic "done" sink, then the plan_review request_port LAST
    // (MagenticWorkflowBuilder's own add-order convention -- see
    // tests/test_workflow_magentic_plan_signoff.cpp's identical P4 bodies vector).
    std::vector<ExecutorBody> bodies = {
        manager_body(),
        writer_body(),
        [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{in};  // the builder's synthetic "done" sink
        },
        {},  // plan_review is a request_port -- never dispatched to a body
    };

    std::filesystem::path const root = make_temp_root();
    std::string                 run_id;
    std::string                 interaction_id;

    // ---- "before the restart": the manager proposes a plan and suspends for human sign-off,
    //      auto-checkpointing every round ------------------------------------------------------------
    {
        FileSessionStore store(root);
        WorkflowSupervisor sup;
        sup.initialize(built->graph, bodies, {}, built->manager_id);
        WorkflowCheckpointManager<FileSessionStore> mgr(store);
        mgr.attach(sup);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("kick off the roadmap task")}));
        check(r.status == workflow_status::suspended, "the run suspends at the plan_review port");
        check(r.open_interactions.size() == 1, "exactly one open interaction is waiting on plan review");

        run_id         = sup.run_id();
        interaction_id = r.open_interactions.at(0).interaction_id;
        std::printf("[before restart] suspended run_id=%s waiting on plan sign-off (interaction=%s)\n",
                     run_id.c_str(), interaction_id.c_str());
    }  // sup and store both go out of scope here -- nothing survives in memory across this point

    // ---- "the process restarts": a brand-new store handle, a brand-new supervisor, a brand-new
    //      `bodies` vector -- the human's decision, persisted, is what drives the rest -------------
    {
        FileSessionStore reopened(root);
        std::vector<ExecutorBody> bodies2 = {
            manager_body(), writer_body(),
            [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            },
            {},
        };

        WorkflowSupervisor sup;
        result<bool> resumed = WorkflowCheckpointManager<FileSessionStore>::resume_or_start(
            reopened, run_id, sup, built->graph, bodies2);
        check(resumed.has_value() && *resumed == true,
              "resume_or_start() finds the on-disk checkpoint and resumes instead of starting fresh");

        auto const open = sup.open_interactions();
        check(open.size() == 1 && open[0].interaction_id == interaction_id,
              "the resumed supervisor's open interaction is the SAME plan-review request the "
              "original run left open");

        Message approval = make_plan_signoff_response(MagenticPlanSignoffResponse{true, "go ahead"});
        WorkflowResult r = drive(sup.resume_workflow(ResumeWorkflow{interaction_id, approval, {}}));

        check(r.status == workflow_status::completed,
              "resuming the brand-new supervisor with the human's typed approval drives the run "
              "through the writer and on to completion -- no re-review, no replayed plan_review "
              "round");
        std::printf("[after restart] completed: %s\n", text_of(r.output).c_str());
        check(text_of(r.output).find("REPORT:") != std::string::npos,
              "the completed run's output carries the writer's real report -- the writer genuinely "
              "ran after the resumed approval, not a stub");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::fprintf(stderr, g_failures == 0 ? "example_22_magentic_plan_signoff_checkpoint: OK\n"
                                          : "example_22_magentic_plan_signoff_checkpoint: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
