// Proof for ADR-037: agentengine::rt::WorkflowSupervisor's time-travel surface
// (include/agentengine/rt/workflow_time_travel.hpp) -- rt::AppendLogStore wired into 014 §5's
// "rewind to any retained checkpoint," closing the gap rt::WorkflowSupervisor's own Slice 2 banner
// named as deferred. Deterministic, offline, single-threaded. Covers:
//   L1 -- a dangling pending checkpoint (staged, never committed) is NOT retained -- decision 7's
//         "not fully committed" rule, ported from the Quark original's own two-phase discipline.
//   L2 -- save_retained_checkpoint() (both phases) makes a checkpoint retrievable.
//   L3 -- retained_checkpoints() returns every fully-committed checkpoint, oldest first, not just
//         the latest.
//   L4 -- load_latest_retained_checkpoint() returns the HIGHEST committed index, not the first one
//         written.
//   L5 -- rewind_workflow() to an index selects exactly that checkpoint's own state, from among
//         several retained ones, not just the latest.
//   L6 -- rewind_workflow() to a non-existent index fails closed with a real, specific error code.
//   L7 -- rewind_workflow()'s audit log records the request -- from/to indices, operator_id, reason,
//         and state_modified -- even when the caller never goes on to apply the returned record.
//   L8 -- a modified_state override replaces the returned record wholesale, and the audit still
//         correctly reports state_modified=true.
//   E1 -- THE END-TO-END POINT: rewinding a REAL WorkflowSupervisor's checkpoint to a point BEFORE a
//         request-port was answered, restoring it into a fresh instance, and resuming with a
//         DIFFERENT answer produces a genuinely DIFFERENT final output than the original run --
//         proof this actually changes the run's course, not just a record round-trip.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_time_travel.hpp"

using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::InMemoryAppendLogStore;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::checkpoint_phase;
using agentengine::rt::load_latest_retained_checkpoint;
using agentengine::rt::read_rewind_audit_log;
using agentengine::rt::retained_checkpoints;
using agentengine::rt::rewind_workflow;
using agentengine::rt::save_retained_checkpoint;
using agentengine::rt::stage_pending_checkpoint;
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

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T"};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::request_port, .input_type = "T", .output_type = "T"};
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in,
                                    agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ">" + name)};
    };
}

// A minimal, single-node RunStateRecord builder for L1-L8 (which don't need a real supervisor).
[[nodiscard]] RunStateRecord fake_record(std::string run_id, std::string tag) {
    RunStateRecord rec;
    rec.run_id          = std::move(run_id);
    rec.rounds          = 1;
    rec.selected_output = text_message(tag);
    return rec;
}

}  // namespace

int main() {
    // ---- L1: a dangling pending checkpoint (never committed) is NOT retained -----------------------
    {
        InMemoryAppendLogStore store;
        auto staged = stage_pending_checkpoint(store, "run-l1", 1, fake_record("run-l1", "v1"));
        check(staged.has_value(), "L1 setup: staging a pending checkpoint succeeds");

        auto latest = load_latest_retained_checkpoint(store, "run-l1");
        check(latest.has_value() && !latest->has_value(),
              "L1: a pending-only checkpoint (never committed) reports as NOT retained -- decision "
              "7's 'not fully committed' rule, matching the Quark original's own two-phase discipline");

        auto all = retained_checkpoints(store, "run-l1");
        check(all.has_value() && all->empty(),
              "L1: retained_checkpoints() also excludes the dangling pending entry");
    }

    // ---- L2: save_retained_checkpoint() (both phases) makes a checkpoint retrievable ----------------
    {
        InMemoryAppendLogStore store;
        auto saved = save_retained_checkpoint(store, "run-l2", 1, fake_record("run-l2", "v1"));
        check(saved.has_value(), "L2: save_retained_checkpoint() (stage + commit) succeeds");

        auto latest = load_latest_retained_checkpoint(store, "run-l2");
        check(latest.has_value() && latest->has_value() &&
                  text_of((*latest)->selected_output) == "v1",
              "L2: the fully-committed checkpoint is retrievable, with its own real state");
    }

    // ---- L3/L4: multiple retained checkpoints, oldest-first listing, latest-by-index lookup --------
    {
        InMemoryAppendLogStore store;
        (void)save_retained_checkpoint(store, "run-l34", 1, fake_record("run-l34", "v1"));
        (void)save_retained_checkpoint(store, "run-l34", 2, fake_record("run-l34", "v2"));
        (void)save_retained_checkpoint(store, "run-l34", 3, fake_record("run-l34", "v3"));

        auto all = retained_checkpoints(store, "run-l34");
        check(all.has_value() && all->size() == 3, "L3: all 3 retained checkpoints are returned");
        if (all.has_value() && all->size() == 3) {
            check((*all)[0].first == 1 && (*all)[1].first == 2 && (*all)[2].first == 3,
                  "L3: they come back oldest-first by checkpoint_index, not insertion order or "
                  "reversed");
        }

        auto latest = load_latest_retained_checkpoint(store, "run-l34");
        check(latest.has_value() && latest->has_value() &&
                  text_of((*latest)->selected_output) == "v3",
              "L4: load_latest_retained_checkpoint() returns index 3's state -- the HIGHEST "
              "committed index, not the first one written");
    }

    // ---- L5: rewind_workflow() to a specific index selects exactly that one's state -----------------
    {
        InMemoryAppendLogStore store;
        (void)save_retained_checkpoint(store, "run-l5", 1, fake_record("run-l5", "v1"));
        (void)save_retained_checkpoint(store, "run-l5", 2, fake_record("run-l5", "v2"));
        (void)save_retained_checkpoint(store, "run-l5", 3, fake_record("run-l5", "v3"));

        auto rewound = rewind_workflow(store, "run-l5", /*to_checkpoint_index=*/2, "op-1", "manual review");
        check(rewound.has_value() && text_of(rewound->selected_output) == "v2",
              "L5: rewinding to index 2 returns EXACTLY index 2's state, not the latest (v3) or the "
              "oldest (v1)");
    }

    // ---- L6: rewinding to a non-existent index fails closed with a specific error -------------------
    {
        InMemoryAppendLogStore store;
        (void)save_retained_checkpoint(store, "run-l6", 1, fake_record("run-l6", "v1"));

        auto rewound = rewind_workflow(store, "run-l6", /*to_checkpoint_index=*/99, "op-1", "typo'd index");
        check(!rewound.has_value(),
              "L6: rewinding to an index that was never retained fails, not silently returns "
              "something else");
        check(!rewound.has_value() && rewound.error().code == "rt.workflow_time_travel.rewind.not_found",
              "L6: the failure is specifically the not-found code, not a generic error");
    }

    // ---- L7/L8: the audit log records every rewind request, including a modified-state override ----
    {
        InMemoryAppendLogStore store;
        (void)save_retained_checkpoint(store, "run-l78", 1, fake_record("run-l78", "v1"));
        (void)save_retained_checkpoint(store, "run-l78", 5, fake_record("run-l78", "v5"));

        auto rewound = rewind_workflow(store, "run-l78", /*to_checkpoint_index=*/1, "operator-x",
                                        "investigating a bad output");
        check(rewound.has_value() && text_of(rewound->selected_output) == "v1",
              "L7 setup: the plain rewind (no override) returns the retained checkpoint's own state");

        auto override_state = fake_record("run-l78", "manually-fixed");
        auto rewound2 =
            rewind_workflow(store, "run-l78", /*to_checkpoint_index=*/5, "operator-y",
                             "patching a known-bad field", override_state);
        check(rewound2.has_value() && text_of(rewound2->selected_output) == "manually-fixed",
              "L8: a modified_state override REPLACES the retained checkpoint's own state wholesale "
              "in the returned record");

        auto audit = read_rewind_audit_log(store, "run-l78");
        check(audit.has_value() && audit->size() == 2,
              "L7: BOTH rewind requests are recorded, even though this call never applied either "
              "returned record anywhere -- the REQUEST itself is the audited event");
        if (audit.has_value() && audit->size() == 2) {
            check((*audit)[0].to_checkpoint_index == 1 && (*audit)[0].operator_id == "operator-x" &&
                      (*audit)[0].reason == "investigating a bad output" && !(*audit)[0].state_modified,
                  "L7: the first entry names the real operator/reason/target-index, and correctly "
                  "reports state_modified=false for the plain rewind");
            check((*audit)[1].to_checkpoint_index == 5 && (*audit)[1].from_checkpoint_index == 5 &&
                      (*audit)[1].operator_id == "operator-y" && (*audit)[1].state_modified,
                  "L8: the second entry's from_checkpoint_index reflects the LATEST retained index at "
                  "the time (5, unaffected by the first rewind's own choice of target), and correctly "
                  "reports state_modified=true for the override");
        }
    }

    // =================================================================================================
    // E1: THE END-TO-END POINT -- rewinding a REAL WorkflowSupervisor's run changes its actual course.
    // =================================================================================================
    {
        Workflow wf;
        wf.id        = "e1-time-travel";
        wf.executors = {node_desc("start"), port_desc("ask"), node_desc("finish")};
        wf.edges.push_back(Edge{"start", "ask", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"ask", "finish", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("finish");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "E1 setup: the graph validates");

        std::vector<ExecutorBody> bodies = {appender("start"), {}, appender("finish")};

        InMemoryAppendLogStore store;
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "E1: the run suspends at the request-port");
        check(r1.open_interactions.size() == 1, "E1 setup: exactly one port is open");
        std::string const interaction_id = r1.open_interactions.front().interaction_id;

        // Checkpoint index 1: the SUSPENDED state, before the port is ever answered.
        auto ckpt1 = save_retained_checkpoint(store, sup.run_id(), 1, sup.to_record());
        check(ckpt1.has_value(), "E1: checkpointing the suspended state succeeds");

        // Answer the port with "original-answer" and let the ORIGINAL run finish.
        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{interaction_id, text_message("original-answer"), {}}));
        check(r2.status == workflow_status::completed, "E1: the original run completes after the port answer");
        std::string const original_output = text_of(r2.output);

        // Checkpoint index 2: the COMPLETED state.
        auto ckpt2 = save_retained_checkpoint(store, sup.run_id(), 2, sup.to_record());
        check(ckpt2.has_value(), "E1 setup: checkpointing the completed state succeeds");

        // ---- TIME TRAVEL: rewind to index 1 (BEFORE the port was answered), on a FRESH instance ----
        auto rewound = rewind_workflow(store, sup.run_id(), /*to_checkpoint_index=*/1,
                                        "operator-time-traveler",
                                        "replaying with a different answer to compare outcomes");
        check(rewound.has_value(), "E1: rewinding to the pre-answer checkpoint succeeds");

        WorkflowSupervisor fresh;
        fresh.initialize(wf, bodies);  // same graph/bodies -- a host redeploys these, restore_from_record()
                                        // only restores the RUN's own dynamic state, matching Slice 2's
                                        // own AgentSessionRecord precedent (see workflow_supervisor.hpp).
        if (rewound.has_value()) fresh.restore_from_record(*rewound);

        check(fresh.open_interactions().size() == 1,
              "E1: the FRESH instance, restored from the pre-answer checkpoint, has the SAME port "
              "open again -- the rewind genuinely recovered the suspended-at-the-port state, not "
              "just the completed record");

        // Resume the REWOUND run with a DIFFERENT answer than the original.
        WorkflowResult r3 = drive(fresh.resume_workflow(
            ResumeWorkflow{fresh.open_interactions().front().interaction_id, text_message("REWOUND-answer"), {}}));
        check(r3.status == workflow_status::completed, "E1: the rewound-and-resumed run also completes");
        std::string const rewound_output = text_of(r3.output);

        check(rewound_output != original_output,
              ("E1: THE REAL POINT -- resuming the rewound run with a different answer produces a "
               "GENUINELY DIFFERENT final output than the original run's own completion (\"" +
               original_output + "\" vs \"" + rewound_output +
               "\") -- this is a real change to the run's course, not merely a record that "
               "round-trips back to itself")
                  .c_str());
        check(rewound_output.find("REWOUND-answer") != std::string::npos &&
                  original_output.find("original-answer") != std::string::npos,
              "E1: each output traces back to its OWN answer text specifically, confirming the "
              "divergence comes from the different port resolution, not an unrelated bug");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_time_travel: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_time_travel: %d failure(s)\n", g_failures);
    return 1;
}
