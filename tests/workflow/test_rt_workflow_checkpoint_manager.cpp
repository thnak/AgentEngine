// Proof for ADR-149 (GitHub issue #28 item 4): agentengine::rt::WorkflowCheckpointManager
// (include/agentengine/rt/workflow_checkpoint_manager.hpp) -- a thin wrapper over the ALREADY-real
// save_workflow_checkpoint()/load_workflow_checkpoint() and SessionStore conformers
// (rt/workflow_supervisor.hpp, rt/session_store.hpp), not a new persistence mechanism.
//   C1 -- attach() auto-persists a checkpoint every round via InMemorySessionStore, with zero
//         caller-written set_checkpoint_hook() closure.
//   C2 -- resume_or_start() against an empty store returns false (fresh start), and initialize()
//         still ran (the supervisor is ready to drive run_workflow()).
//   C3 -- resume_or_start() against a REAL mid-flight (suspended-at-a-port) checkpoint resumes a
//         BRAND-NEW supervisor into the exact same open-interaction state, and driving it onward
//         from there produces the same result a never-interrupted run would have.
//   C4 -- fail-closed on an agent-kind executor: resume_or_start() refuses (a contract-class error)
//         to resume a graph containing an agent-kind node unless
//         acknowledge_agent_history_reset=true is explicitly passed (ADR-149 §3 finding 4).
//   C5 -- the same attach()/resume_or_start() round-trip works against FileSessionStore, a REAL
//         on-disk store, not just the in-memory one.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "agentengine/rt/workflow_checkpoint_manager.hpp"

using agentengine::result;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::FileSessionStore;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowCheckpointManager;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::encode_run_state_record;
using agentengine::rt::workflow_status;

using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;

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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .capability_ceiling = {}};
}

[[nodiscard]] ExecutorBody forward_body() {
    return [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{in};
    };
}

// A "mgr -> gate" graph, where "gate" is a request_port with no outgoing edge -- the SAME minimal
// shape used to prove C3's mid-flight resume: round 1 runs mgr, round 2 delivers to (and suspends
// at) gate.
[[nodiscard]] Workflow gate_graph() {
    Workflow wf;
    wf.id        = "checkpoint-mgr-gate";
    wf.executors = {node_desc("mgr"), node_desc("gate", executor_kind::request_port)};
    wf.edges.push_back(Edge{"mgr", "gate", edge_kind::direct, {}});
    wf.start     = "mgr";
    wf.output_selection.push_back("gate");
    wf.bound.max_rounds = 8;
    return wf;
}

#if defined(_WIN32)
[[nodiscard]] int current_pid() noexcept { return ::_getpid(); }
#else
[[nodiscard]] int current_pid() noexcept { return ::getpid(); }
#endif

// Matches test_rt_session_store.cpp's own temp-root idiom.
[[nodiscard]] std::filesystem::path make_temp_root() {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                  ("ae_workflow_checkpoint_manager_test_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

}  // namespace

int main() {
    // ---- C1: attach() auto-persists every round --------------------------------------------------
    {
        InMemorySessionStore store;
        Workflow wf = gate_graph();
        std::vector<ExecutorBody> bodies = {forward_body(), {}};

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowCheckpointManager<InMemorySessionStore> mgr(store);
        mgr.attach(sup);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::suspended, "C1 setup: the graph suspends at the gate");
        check(store.exists(sup.run_id()),
              "C1: attach() persisted a checkpoint under run_id() with zero caller-written hook code");
    }

    // ---- C2: resume_or_start() against an empty store starts fresh --------------------------------
    {
        InMemorySessionStore store;
        Workflow wf = gate_graph();
        std::vector<ExecutorBody> bodies = {forward_body(), {}};

        WorkflowSupervisor sup;
        result<bool> resumed = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, "never-checkpointed:run:1", sup, wf, bodies);
        check(resumed.has_value() && *resumed == false,
              "C2: no stored checkpoint for this run_id -> resume_or_start() returns false");

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::suspended,
              "C2: initialize() ran (via resume_or_start()) even on the fresh-start path -- the "
              "supervisor is genuinely usable, not left half-configured");
    }

    // ---- C3: resume_or_start() against a real mid-flight checkpoint -------------------------------
    {
        InMemorySessionStore store;
        Workflow wf = gate_graph();
        std::vector<ExecutorBody> bodies1 = {forward_body(), {}};

        WorkflowSupervisor sup1;
        sup1.initialize(wf, bodies1);
        WorkflowCheckpointManager<InMemorySessionStore> mgr(store);
        mgr.attach(sup1);
        WorkflowResult r1 = drive(sup1.run_workflow(RunWorkflow{text_message("payload")}));
        check(r1.status == workflow_status::suspended, "C3 setup: the original run suspends at the gate");
        std::string const run_id = sup1.run_id();
        std::string const original_interaction_id = r1.open_interactions.at(0).interaction_id;

        // A BRAND-NEW supervisor, standing in for "the process restarted" -- sup1 is never touched
        // again below.
        WorkflowSupervisor sup2;
        std::vector<ExecutorBody> bodies2 = {forward_body(), {}};  // fresh, caller-supplied again
        result<bool> resumed = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, run_id, sup2, wf, bodies2);
        check(resumed.has_value() && *resumed == true,
              "C3: a real stored checkpoint for this run_id -> resume_or_start() returns true");

        auto const sup2_open = sup2.open_interactions();
        check(sup2_open.size() == 1 && sup2_open[0].interaction_id == original_interaction_id,
              "C3: the resumed supervisor's open interaction matches the original exactly");

        WorkflowResult r2 = drive(sup2.resume_workflow(
            ResumeWorkflow{original_interaction_id, text_message("approved"), {}}));
        check(r2.status == workflow_status::completed,
              "C3: driving the RESUMED (brand-new) supervisor onward completes the run");
        check(text_of(r2.output) == "approved",
              "C3: the resumed run produces the same result an uninterrupted run would have");
    }

    // ---- C4: fail-closed on an agent-kind executor without acknowledgment -------------------------
    {
        Workflow wf;
        wf.id        = "agent-guard";
        wf.executors = {node_desc("agent_node", executor_kind::agent)};
        wf.start     = "agent_node";
        wf.output_selection.push_back("agent_node");
        wf.bound.max_rounds = 4;

        InMemorySessionStore store;
        std::string const run_id = "agent-guard:run:1";
        RunStateRecord seed;
        seed.run_id = run_id;
        check(store.save(run_id, encode_run_state_record(seed)).has_value(),
              "C4 setup: seed a (structurally minimal) checkpoint under this run_id");

        std::vector<ExecutorBody> bodies = {ExecutorBody{}};  // no real agent body needed for this gate
        WorkflowSupervisor sup;
        result<bool> resumed = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, run_id, sup, wf, bodies);
        check(!resumed.has_value(),
              "C4: resuming a graph with an agent-kind executor, unacknowledged, fails closed");
        if (!resumed) {
            check(resumed.error().code == "rt.workflow_checkpoint_manager.agent_history_reset_unacknowledged",
                  "C4: the specific error code");
        }

        WorkflowSupervisor sup_ack;
        result<bool> resumed_ack = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, run_id, sup_ack, wf, bodies, {}, {}, /*acknowledge_agent_history_reset=*/true);
        check(resumed_ack.has_value() && *resumed_ack,
              "C4: explicitly acknowledging the history reset proceeds with the resume");
    }

    // ---- C4b: the SAME fail-closed guard also covers sub_workflow-kind executors, not just -------
    //           agent-kind (a residual an independent post-implementation audit named: sub_workflow
    //           cannot execute at all today, but the guard's shape should already be broad enough to
    //           catch it once it can, rather than needing a second fix later).
    {
        Workflow wf;
        wf.id        = "sub-workflow-guard";
        wf.executors = {node_desc("sub_node", executor_kind::sub_workflow)};
        wf.start     = "sub_node";
        wf.output_selection.push_back("sub_node");
        wf.bound.max_rounds = 4;

        InMemorySessionStore store;
        std::string const run_id = "sub-workflow-guard:run:1";
        RunStateRecord seed;
        seed.run_id = run_id;
        check(store.save(run_id, encode_run_state_record(seed)).has_value(),
              "C4b setup: seed a (structurally minimal) checkpoint under this run_id");

        std::vector<ExecutorBody> bodies = {ExecutorBody{}};
        WorkflowSupervisor sup;
        result<bool> resumed = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, run_id, sup, wf, bodies);
        check(!resumed.has_value(),
              "C4b: resuming a graph with a sub_workflow-kind executor, unacknowledged, fails closed");
        if (!resumed) {
            check(resumed.error().code == "rt.workflow_checkpoint_manager.agent_history_reset_unacknowledged",
                  "C4b: the same error code as the agent-kind case");
        }

        WorkflowSupervisor sup_ack;
        result<bool> resumed_ack = WorkflowCheckpointManager<InMemorySessionStore>::resume_or_start(
            store, run_id, sup_ack, wf, bodies, {}, {}, /*acknowledge_agent_history_reset=*/true);
        check(resumed_ack.has_value() && *resumed_ack,
              "C4b: explicitly acknowledging the history reset proceeds with the resume");
    }

    // ---- C5: the same round-trip against a REAL on-disk FileSessionStore --------------------------
    {
        std::filesystem::path root = make_temp_root();
        Workflow wf = gate_graph();
        std::vector<ExecutorBody> bodies1 = {forward_body(), {}};

        std::string run_id;
        std::string interaction_id;
        {
            FileSessionStore store(root);
            WorkflowSupervisor sup1;
            sup1.initialize(wf, bodies1);
            WorkflowCheckpointManager<FileSessionStore> mgr(store);
            mgr.attach(sup1);
            WorkflowResult r1 = drive(sup1.run_workflow(RunWorkflow{text_message("disk-payload")}));
            check(r1.status == workflow_status::suspended, "C5 setup: the original run suspends");
            run_id         = sup1.run_id();
            interaction_id = r1.open_interactions.at(0).interaction_id;
        }  // store goes out of scope -- resume must work from a freshly-reopened one

        FileSessionStore reopened(root);
        WorkflowSupervisor sup2;
        std::vector<ExecutorBody> bodies2 = {forward_body(), {}};
        result<bool> resumed = WorkflowCheckpointManager<FileSessionStore>::resume_or_start(
            reopened, run_id, sup2, wf, bodies2);
        check(resumed.has_value() && *resumed == true,
              "C5: resume_or_start() finds the checkpoint through a freshly-reopened FileSessionStore");

        WorkflowResult r2 =
            drive(sup2.resume_workflow(ResumeWorkflow{interaction_id, text_message("disk-approved"), {}}));
        check(r2.status == workflow_status::completed && text_of(r2.output) == "disk-approved",
              "C5: the disk-backed resume completes correctly, same as the in-memory case");

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_checkpoint_manager: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_checkpoint_manager: %d failure(s)\n", g_failures);
    return 1;
}
