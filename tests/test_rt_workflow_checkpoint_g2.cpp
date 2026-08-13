// Implements 014-Workflow-and-Orchestration.md §8 G2 (the promotion gate): "checkpoint/resume: kill
// at each superstep boundary of a 20-node workflow; every resume completes with output identical
// to the uninterrupted control." ADR-037 task #60 -- SUPERSEDES the old Quark-actor-based
// test_workflow_checkpoint_g2.cpp, reproven against agentengine::rt::WorkflowSupervisor.
//
// A 20-node sequential chain, run once as the CONTROL (capturing every superstep-boundary
// checkpoint along the way via set_checkpoint_hook(), ported onto rt:: for exactly this purpose --
// see workflow_supervisor.hpp's own "Checkpoint hook" banner paragraph), then for EVERY one of
// those 20 checkpoints, a genuinely NEW WorkflowSupervisor instance is restore_from_record()'d from
// exactly that checkpoint and driven to completion via continue_workflow() -- simulating "the
// process died right after this superstep's checkpoint was written, and something else picked the
// run back up." G2's own bar is that EVERY one of those 20 restarts reaches the SAME final output
// as the control, not just one or two convenient points -- this is the test that would catch a
// design that only happened to survive a restart at ONE boundary (e.g. only the very last one)
// while silently corrupting the rest.
//
// Ported off quark::Engine/Actor/ActorRef entirely: rt::WorkflowSupervisor is a plain host-held
// object (no activation/router/actor-id plumbing), driven via the same drive<T>() single-threaded
// coroutine-draining idiom test_rt_workflow_supervisor.cpp already established -- run_workflow()'s
// own execute() never suspends on anything but ThreadPool::submit()'s blocking std::future::get()
// (see that file's own comment above its drive<T>()), so one resume() call per ask resolves each
// run/restart in full, no Engine/Activation stack needed to stand it up.
//
// MACHINE SAFETY (CLAUDE.md): single-threaded driver, 21 independent WorkflowSupervisor instances in
// sequence (never concurrent), bounded rounds, no sleeps in this file. Each instance's own round-loop
// still fans out through rt::ThreadPool internally, but this graph is a pure chain (width 1 per
// round), so no real parallel fan-out ever occurs here.

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

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

// Same shape test_rt_workflow_supervisor.cpp's own drive<T>() establishes -- see that file's
// comment for exactly why one resume() call is sufficient (execute() never internally suspends;
// concurrent fan-out happens through std::future::get(), an ordinary blocking call, not a coroutine
// suspension point).
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::role;
using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += ">";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in,
                                     agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

constexpr std::size_t kNodeCount = 20;

[[nodiscard]] Workflow build_chain_graph() {
    Workflow wf;
    wf.id = "g2-chain";
    for (std::size_t i = 0; i < kNodeCount; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "n%02zu", i);
        wf.executors.push_back(Executor{buf, executor_kind::function, "T", "T"});
    }
    for (std::size_t i = 0; i + 1 < kNodeCount; ++i) {
        wf.edges.push_back(Edge{wf.executors[i].id, wf.executors[i + 1].id, edge_kind::direct, {}});
    }
    wf.start            = wf.executors.front().id;
    wf.output_selection = {wf.executors.back().id};
    wf.bound.max_rounds = static_cast<std::uint32_t>(kNodeCount) + 5;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> build_bodies() {
    std::vector<ExecutorBody> bodies;
    bodies.reserve(kNodeCount);
    for (std::size_t i = 0; i < kNodeCount; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "n%02zu", i);
        bodies.push_back(appender(buf));
    }
    return bodies;
}

}  // namespace

int main() {
    Workflow const wf = build_chain_graph();
    check(validate_workflow(wf).has_value(), "G2 setup: the 20-node chain validates");

    // =========================================================================================
    // The CONTROL: one uninterrupted run, capturing every superstep-boundary checkpoint along
    // the way (workflow_supervisor.hpp's own RunStateRecord, via set_checkpoint_hook()/to_record()).
    // =========================================================================================
    std::vector<RunStateRecord> checkpoints;
    WorkflowResult               control_result;
    {
        WorkflowSupervisor sup;
        sup.initialize(wf, build_bodies());
        sup.set_checkpoint_hook(
            [&](std::uint32_t /*round*/, RunStateRecord const& rec) { checkpoints.push_back(rec); });

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "G2 control: the run completes");
        control_result = r;
    }

    check(control_result.status == workflow_status::completed,
          "G2 control: the chain runs dry and completes, not bounded/failed");
    check(checkpoints.size() == kNodeCount,
          "G2 control: exactly one checkpoint per node/round -- a 20-node chain crosses 20 "
          "superstep boundaries");
    std::string const control_output = all_text_of(control_result.output);
    std::fprintf(stderr, "  .. G2 control output = %s\n", control_output.c_str());
    check(control_output == "in>n00>n01>n02>n03>n04>n05>n06>n07>n08>n09>n10>n11>n12>n13>n14>n15>"
                            "n16>n17>n18>n19",
          "G2 control: the chain threaded the payload through every node in graph order (sanity)");

    // =========================================================================================
    // G2 ITSELF: for EVERY one of the 20 checkpoints, a genuinely NEW instance restores from
    // exactly that one and continues -- simulating a kill right after that superstep's checkpoint
    // was durable, and something else picking the run back up from nothing but the record.
    // =========================================================================================
    std::size_t matched = 0;
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        WorkflowSupervisor sup;
        sup.initialize(wf, build_bodies());
        sup.restore_from_record(checkpoints[i]);

        WorkflowResult r = drive(sup.continue_workflow(ContinueWorkflow{}));
        bool const ok = r.status == workflow_status::completed && all_text_of(r.output) == control_output;
        if (ok) ++matched;
        if (!ok) {
            std::fprintf(stderr, "  .. G2 mismatch at checkpoint %zu (round=%u): status=%d output=%s\n",
                        i, checkpoints[i].rounds, static_cast<int>(r.status), all_text_of(r.output).c_str());
        }
    }
    check(matched == checkpoints.size(),
          "G2 (014 §8 G2): killed at EVERY one of the 20 superstep boundaries, every single resume "
          "completed with output IDENTICAL to the uninterrupted control -- not just the convenient "
          "first or last one");
    std::fprintf(stderr, "  .. G2: %zu/%zu checkpoints resumed to the identical control output\n",
                matched, checkpoints.size());

    if (g_failures == 0) {
        std::printf("test_rt_workflow_checkpoint_g2: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_checkpoint_g2: %d failure(s)\n", g_failures);
    return 1;
}
