// Proof for ADR-149 (GitHub issue #28 item 2): `TerminationBound::max_stalls`/`max_resets`
// (include/agentengine/workflow/graph.hpp) and their enforcement in
// `agentengine::rt::WorkflowSupervisor::execute()` (include/agentengine/rt/workflow_supervisor.hpp).
// MACHINE SAFETY (CLAUDE.md): every graph below is a bounded self-loop with a real `max_rounds`
// fallback in addition to the stall/reset bound under test.
//   S1 -- a stall trip with no max_resets set ends the run via bound_max_stalls, at the expected
//         round, with resets_used()==1.
//   S2 -- with max_resets set, a trip under the ceiling is silently absorbed and the run continues;
//         the SECOND trip (exceeding the ceiling) ends the run via bound_max_resets.
//   S3 -- a non-stalled round resets stall_streak_ to 0 -- the bound needs FRESH consecutive stalls.
//   S4 -- ExecutorOutcome::stalled is INERT unless the reporting executor's id matches
//         designated_stall_reporter -- a perpetually-stalled non-designated executor never trips the
//         bound (ADR-149 §3 finding 1's I2/I3 narrowing, proven, not just asserted).
//   S5 -- TerminationBound::any() recognizes max_stalls/max_resets alone as a valid bound (014 §2's
//         mandatory-bound requirement).
//   S6 -- stall_streak_/resets_used_ round-trip through to_record()/restore_from_record(), so
//         resuming from a checkpoint does NOT silently grant extra stall budget (ADR-149 §3 finding
//         5 -- the bug a red-team pass found in this feature's own safety valve before any of this
//         compiled).

#include <atomic>
#include <cstdio>
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

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Safe here for the same reason test_rt_workflow_supervisor.cpp's own drive() is: every graph below
// is a single-node (or two-node) fan-out with no genuinely suspending co_await anywhere in the chain.
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
using agentengine::workflow::TerminationBound;
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
    return Executor{id, executor_kind::function, "T", "T"};
}

// Always reports stalled -- the pathological case every trip scenario below drives.
[[nodiscard]] ExecutorBody always_stalled_body() {
    return [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        ExecutorOutcome out{text_message(text_of(in) + ".")};
        out.stalled = true;
        return out;
    };
}

[[nodiscard]] ExecutorBody never_stalled_body() {
    return [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ".")};
    };
}

}  // namespace

int main() {
    // ---- S1: no max_resets -- the FIRST trip ends the run --------------------------------------
    {
        Workflow wf;
        wf.id        = "stall-s1";
        wf.executors = {node_desc("mgr")};
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.start     = "mgr";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 50;  // fallback so a design bug fails loud, not hangs
        wf.bound.max_stalls = 3;
        check(validate_workflow(wf).has_value(), "S1 setup: self-loop graph with only max_stalls set validates");

        std::vector<ExecutorBody> bodies = {always_stalled_body()};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(r.status == workflow_status::bound_max_stalls,
              "S1: 3 consecutive stalled rounds with no max_resets ends via bound_max_stalls");
        check(r.rounds == 3, "S1: ends exactly at the 3rd consecutive stalled round");
        check(sup.resets_used() == 1, "S1: exactly one reset counted at the trip");
        check(sup.stall_streak() == 0, "S1: the streak is cleared when a reset is counted");
    }

    // ---- S2: max_resets set -- a trip under the ceiling is absorbed, the run continues ----------
    {
        Workflow wf;
        wf.id        = "stall-s2";
        wf.executors = {node_desc("mgr")};
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.start     = "mgr";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 50;
        wf.bound.max_stalls = 2;
        wf.bound.max_resets = 1;

        std::vector<ExecutorBody> bodies = {always_stalled_body()};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(r.status == workflow_status::bound_max_resets,
              "S2: the run survives the first trip (absorbed under max_resets) and ends via "
              "bound_max_resets once a second trip exceeds the ceiling");
        check(r.rounds == 4,
              "S2: round 2 trips reset #1 (absorbed), round 4 trips reset #2 (exceeds ceiling of 1)");
        check(sup.resets_used() == 2, "S2: two resets counted total (one absorbed, one terminal)");
    }

    // ---- S3: a non-stalled round resets the streak -- the bound needs FRESH consecutive stalls ---
    {
        Workflow wf;
        wf.id        = "stall-s3";
        wf.executors = {node_desc("mgr")};
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.start     = "mgr";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 50;
        wf.bound.max_stalls = 2;

        std::atomic<int> round_ctr{0};
        std::vector<ExecutorBody> bodies = {
            [&round_ctr](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                int const n = round_ctr.fetch_add(1);
                ExecutorOutcome out{text_message(text_of(in) + ".")};
                out.stalled = (n != 1);  // rounds (0-indexed): stall, PROGRESS, stall, stall
                return out;
            }};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(r.status == workflow_status::bound_max_stalls,
              "S3: the run still trips once two FRESH consecutive stalls follow the progress round");
        check(r.rounds == 4,
              "S3: round 2's progress reset the streak, so the trip needs rounds 3+4, not round 2");
    }

    // ---- S4: stalled is INERT unless the reporter is the designated one --------------------------
    {
        Workflow wf;
        wf.id        = "stall-s4";
        wf.executors = {node_desc("root"), node_desc("mgr"), node_desc("noise")};
        wf.edges.push_back(Edge{"root", "mgr", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"root", "noise", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"noise", "noise", edge_kind::direct, {}});
        wf.start     = "root";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 6;
        wf.bound.max_stalls = 3;
        check(validate_workflow(wf).has_value(), "S4 setup: fan-out-then-self-loop graph validates");

        std::vector<ExecutorBody> bodies = {never_stalled_body(),  // root -- runs once, irrelevant
                                             never_stalled_body(),  // mgr -- the designated reporter,
                                                                     // NEVER stalls (healthy)
                                             always_stalled_body()};  // noise -- perpetually "stalled",
                                                                       // but NOT the designated reporter
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(r.status == workflow_status::bound_max_rounds,
              "S4: noise's perpetual stalled=true is inert -- the run ends via the ordinary "
              "max_rounds bound, not bound_max_stalls, because mgr (the designated reporter) never "
              "self-reports a stall");
        check(sup.stall_streak() == 0, "S4: stall_streak_ never moves off zero");
        check(sup.resets_used() == 0, "S4: no reset is ever counted");
    }

    // ---- S5: TerminationBound::any() recognizes max_stalls/max_resets alone ----------------------
    {
        TerminationBound b;
        check(!b.any(), "S5 setup: an empty TerminationBound reports no bound");
        b.max_stalls = 5;
        check(b.any(), "S5: max_stalls alone satisfies TerminationBound::any()");

        Workflow wf;
        wf.id        = "stall-s5";
        wf.executors = {node_desc("mgr")};
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.start     = "mgr";
        wf.output_selection.push_back("mgr");
        wf.bound.max_stalls = 5;  // deliberately no max_rounds/deadline_ms/token_budget
        check(validate_workflow(wf).has_value(),
              "S5: a graph whose ONLY declared bound is max_stalls passes 014 §2's mandatory-bound "
              "validation (ADR-149 §3 finding 6)");
    }

    // ---- S6: checkpoint/resume carries stall_streak_/resets_used_, not silently resetting them ---
    {
        Workflow wf;
        wf.id        = "stall-s6";
        wf.executors = {node_desc("mgr")};
        wf.edges.push_back(Edge{"mgr", "mgr", edge_kind::direct, {}});
        wf.start     = "mgr";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 50;
        wf.bound.max_stalls = 3;

        std::vector<ExecutorBody> bodies = {always_stalled_body()};
        std::vector<RunStateRecord> checkpoints;
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        sup.set_checkpoint_hook(
            [&checkpoints](std::uint32_t, RunStateRecord const& rec) { checkpoints.push_back(rec); });
        WorkflowResult baseline = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(baseline.status == workflow_status::bound_max_stalls, "S6 setup: baseline run trips at round 3");
        check(baseline.rounds == 3, "S6 setup: baseline trips at round 3");
        // The checkpoint hook fires only on rounds that DON'T end the run (ADR-149's stall check is
        // positioned before it, mirroring the broke/merge_failed terminal paths) -- so only rounds 1
        // and 2 are captured, not the tripping round 3.
        check(checkpoints.size() == 2, "S6 setup: exactly 2 checkpoints captured (rounds 1 and 2)");
        if (checkpoints.size() == 2) {
            check(checkpoints[0].stall_streak == 1, "S6: round 1's record carries stall_streak == 1");

            WorkflowSupervisor resumed;
            resumed.initialize(wf, bodies, {}, "mgr");
            resumed.restore_from_record(checkpoints[0]);
            check(resumed.stall_streak() == 1,
                  "S6: restore_from_record() restores stall_streak_ from the checkpoint, not 0");

            WorkflowResult r2 = drive(resumed.continue_workflow(ContinueWorkflow{}));
            check(r2.status == workflow_status::bound_max_stalls,
                  "S6: resuming from round 1's streak still trips at the correct TOTAL round count");
            check(r2.rounds == 3,
                  "S6: total rounds across the checkpoint/resume boundary is 3, matching the "
                  "un-resumed baseline -- NOT 4, which is what a silently-reset streak would need "
                  "(1 restored round + 3 fresh stalls) -- proving the checkpoint/resume boundary "
                  "grants no extra stall budget");
        }
    }

    // ---- S7: two CONCURRENT deliveries to the designated reporter in one round -- both must count
    // -----     (a real bug found by an independent post-implementation audit: an earlier version
    //           took only the FIRST matching delivery's `stalled` value and discarded the second) --
    {
        Workflow wf;
        wf.id        = "stall-s7-concurrent-reporter-deliveries";
        wf.executors = {node_desc("root"), node_desc("srcA"), node_desc("srcB"), node_desc("mgr")};
        wf.edges.push_back(Edge{"root", "srcA", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"root", "srcB", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"srcA", "mgr", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"srcB", "mgr", edge_kind::direct, {}});
        wf.start     = "root";
        wf.output_selection.push_back("mgr");
        wf.bound.max_rounds = 10;
        wf.bound.max_stalls = 1;
        check(validate_workflow(wf).has_value(),
              "S7 setup: fan-out-to-two-sources-converging-on-mgr graph validates");

        std::vector<ExecutorBody> bodies = {
            never_stalled_body(),  // root
            [](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message("fromA")};
            },
            [](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message("fromB")};
            },
            // mgr is dispatched TWICE in the round both srcA's and srcB's deliveries arrive --
            // once per delivery, distinguishing by its own incoming payload. Only the fromB
            // invocation reports stalled=true; the fromA invocation reports stalled=false.
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                std::string const text = text_of(in);
                ExecutorOutcome   out{text_message(text + ".mgr")};
                out.stalled = (text == "fromB");
                return out;
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies, {}, "mgr");
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));

        check(r.status == workflow_status::bound_max_stalls,
              "S7: mgr's SECOND concurrent delivery this round (fromB) genuinely self-reported "
              "stalled=true and is NOT silently discarded just because the fromA delivery, "
              "reporting stalled=false, happened to be dispatched first");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_stall_reset_bounds: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_stall_reset_bounds: %d failure(s)\n", g_failures);
    return 1;
}
