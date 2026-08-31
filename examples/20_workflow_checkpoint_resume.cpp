// AgentEngine "get started" examples, 20 -- workflow-level checkpoint/resume (ADR-149, GitHub issue
// #28 item 4), the workflow counterpart to 12_session_checkpoint.cpp's AgentSession version.
//
// Mirrors MAF's samples/03-workflows/checkpoint/checkpoint_with_resume.py: a workflow suspends
// mid-run, the process is treated as gone, and a BRAND-NEW WorkflowSupervisor -- built from nothing
// but a freshly-reopened on-disk store and the run's own id -- picks the run back up exactly where
// it left off. Like 12's own discipline, this example proves that by actually throwing the original
// supervisor away rather than re-using the same in-memory object.
//
// The mechanism is `agentengine::rt::WorkflowCheckpointManager<StoreT>`
// (rt/workflow_checkpoint_manager.hpp) -- a thin wrapper over ALREADY-real
// save_workflow_checkpoint()/load_workflow_checkpoint() and the `SessionStore` concept
// (rt/session_store.hpp), not a new persistence mechanism:
//   - `attach(sup)` installs a checkpoint hook that auto-persists after every round, so the caller
//     never hand-writes a `set_checkpoint_hook()` closure.
//   - `resume_or_start(store, run_id, sup, graph, bodies)` is "resume if a checkpoint exists for
//     run_id, else start fresh" as one call, returning which one happened.
// See tests/test_rt_workflow_checkpoint_manager.cpp for the full proof (including the fail-closed
// guard on agent-kind/sub_workflow-kind executors, not exercised by this deliberately simple
// function-only graph).
//
// Run: ./agentengine_example_20_workflow_checkpoint_resume

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

// The planner: an ordinary function node, standing in for whatever real work happens before a plan
// needs sign-off. A model-backed planner would fill this same slot -- checkpoint/resume doesn't care
// what produced the plan.
[[nodiscard]] ExecutorBody planner_body() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + " > planned")};
    };
}

[[nodiscard]] Workflow make_graph() {
    Workflow wf;
    wf.id        = "checkpoint-resume-demo";
    wf.executors = {Executor{"planner", executor_kind::function, "T", "T"},
                     Executor{"review", executor_kind::request_port, "T", "T"}};
    wf.edges.push_back(Edge{"planner", "review", edge_kind::direct, {}});
    wf.start = "planner";
    wf.output_selection.push_back("review");
    wf.bound.max_rounds = 8;
    return wf;
}

#if defined(_WIN32)
[[nodiscard]] int current_pid() noexcept { return ::_getpid(); }
#else
[[nodiscard]] int current_pid() noexcept { return ::getpid(); }
#endif

[[nodiscard]] std::filesystem::path make_temp_root() {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ae_example_20_workflow_checkpoint_resume_" + std::to_string(current_pid()));
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
    std::filesystem::path const root = make_temp_root();
    Workflow const               wf  = make_graph();
    check(validate_workflow(wf).has_value(), "the graph validates");

    std::string run_id;
    std::string interaction_id;

    // ---- "before the restart": run to suspension, auto-checkpointing every round -------------------
    {
        FileSessionStore store(root);
        std::vector<ExecutorBody> bodies = {planner_body(), {}};  // review is a request_port -- no body

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowCheckpointManager<FileSessionStore> mgr(store);
        mgr.attach(sup);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("draft the quarterly report")}));
        check(r.status == workflow_status::suspended, "the run suspends at the review gate");
        check(r.open_interactions.size() == 1, "exactly one open interaction is waiting on review");

        run_id         = sup.run_id();
        interaction_id = r.open_interactions.at(0).interaction_id;
        std::printf("[before restart] suspended run_id=%s waiting on interaction=%s\n", run_id.c_str(),
                     interaction_id.c_str());
    }  // sup and store both go out of scope here -- nothing survives in memory across this point

    // ---- "the process restarts": a brand-new store handle, a brand-new supervisor -------------------
    {
        FileSessionStore reopened(root);  // a FRESH handle onto the same on-disk directory
        std::vector<ExecutorBody> bodies = {planner_body(), {}};  // caller-supplied again, fresh objects

        WorkflowSupervisor sup;  // a FRESH supervisor -- the original from above is gone
        result<bool> resumed = WorkflowCheckpointManager<FileSessionStore>::resume_or_start(
            reopened, run_id, sup, wf, bodies);
        check(resumed.has_value() && *resumed == true,
              "resume_or_start() finds the on-disk checkpoint and resumes instead of starting fresh");

        auto const open = sup.open_interactions();
        check(open.size() == 1 && open[0].interaction_id == interaction_id,
              "the resumed supervisor's open interaction is the SAME one the original run left open");

        WorkflowResult r = drive(
            sup.resume_workflow(ResumeWorkflow{interaction_id, text_message("approved by reviewer"), {}}));
        check(r.status == workflow_status::completed,
              "resuming the brand-new supervisor with the reviewer's response completes the run");
        std::printf("[after restart] completed: %s\n", text_of(r.output).c_str());
        check(text_of(r.output) == "approved by reviewer",
              "the completed run reflects the resumed response, not anything replayed from before the "
              "restart");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::fprintf(stderr, g_failures == 0 ? "example_20_workflow_checkpoint_resume: OK\n"
                                          : "example_20_workflow_checkpoint_resume: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
