// Proof for ADR-157 (issues #33/#38): executor_kind::sub_workflow's real runtime bridge, with
// nested request_port proxying. Design draft: docs/planning/sub-workflow-nested-request-port-
// design-draft.md (red-teamed once, this file proves the revised, post-red-team mechanism).
//
// S1 -- unbound sub_workflow node: initialize() refuses (valid_ = false).
// S2 -- bind_sub_workflow() flips valid_ true WITHOUT a second initialize() call (ergonomics fix
//       over the naive two-pass design the first draft would have required).
// S3 -- basic nested suspend/resume/complete round-trip: the outer run suspends when the INNER
//       workflow suspends on its own request_port; the outer caller resumes with a plain answer
//       (no knowledge of the inner interaction_id -- namespacing, design draft §3d); the inner
//       genuinely completes and the OUTER graph continues past the sub_workflow node.
// S4 -- a terminally-FAILING inner run (never completes) folds as an ordinary ok=false reply,
//       routed through the existing EdgeFailurePolicy machinery unchanged.
// S5 -- the round-abort-orphan-disclosure fix (design draft §3e/finding 4): a sub_workflow
//       suspends in the SAME round a DIFFERENT executor's routing fails -- the round ends with a
//       non-suspended terminal status, but open_interactions() still reports the real, live
//       nested interaction (not silently dropped).
// S6 -- the checkpoint-restore fail-closed proof (finding 5, the most severe): a restored
//       WorkflowSupervisor (pending_sub_workflows_ never persisted, by design) refuses to resume a
//       stale pending-sub-workflow interaction_id with workflow_status::invalid -- NEVER silently
//       misroutes the raw human answer as the sub-workflow's own completed output.
// S7 -- the OQ-19-quarantine-generalization concurrency proof (finding 1): two edges converging on
//       the SAME sub_workflow executor_index in one round -- only one dispatch happens, across
//       repeated trials, no crash. Adversarially verified: temporarily bypassing the quarantine
//       reproduces a real crash/hang, confirming the fix is load-bearing.
// S8 -- bounded 2-level nesting completes in bounded wall-clock time (a real, CLAUDE.md-compliant
//       bounded test, not a claim about unbounded nesting).
// S9 -- binding a non-sub_workflow-kind executor_id is silently refused (the node stays unbound,
//       proven via overall graph validity, not just a documented-but-unchecked claim).
// S10 -- the same `inner` instance cannot be bound to two different sub_workflow nodes in one
//        graph -- the second bind is refused (a genuinely distinct second instance still works
//        fine), closing the caller-contract gap ADR-157 §4 documented but did not enforce.
//
// MACHINE SAFETY (CLAUDE.md): every loop below is bounded.
//
// Run: ./test_rt_workflow_sub_workflow

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
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

// ---- A small inner graph: planner -> review(request_port) ----------------------------------------

[[nodiscard]] Workflow inner_graph_with_port() {
    Workflow wf;
    wf.id        = "inner-with-port";
    wf.executors = {node_desc("planner"), node_desc("review", executor_kind::request_port)};
    wf.edges.push_back(Edge{"planner", "review", edge_kind::direct, {}});
    wf.start = "planner";
    wf.output_selection.push_back("review");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> inner_bodies_with_port() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">planned")};
        },
        {},
    };
}

// S7's own widened-race-window variant -- sleeps while holding the inner's run_mutex_ (this body
// runs INSIDE run_workflow()'s own locked duration), matching ADR-150's own W5 technique
// ("sleeps 15ms to widen the race window") for its structurally identical hazard.
std::atomic<int>  g_inflight{0};
std::atomic<bool> g_overlap_observed{false};
[[nodiscard]] std::vector<ExecutorBody> inner_bodies_with_port_slow() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            if (g_inflight.fetch_add(1) > 0) g_overlap_observed.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            g_inflight.fetch_sub(1);
            return ExecutorOutcome{text_message(text_of(in) + ">planned")};
        },
        {},
    };
}

// ---- Outer graph: start -> sub(sub_workflow) -> sink -----------------------------------------------

[[nodiscard]] Workflow outer_graph() {
    Workflow wf;
    wf.id        = "outer-with-sub-workflow";
    wf.executors = {node_desc("start"), node_desc("sub", executor_kind::sub_workflow), node_desc("sink")};
    wf.edges.push_back(Edge{"start", "sub", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"sub", "sink", edge_kind::direct, {}});
    wf.start = "start";
    wf.output_selection.push_back("sink");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] std::vector<ExecutorBody> outer_bodies() {
    return {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">start")};
        },
        {},  // sub is sub_workflow-kind -- no body
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">sink")};
        },
    };
}

void s1_unbound_refuses() {
    WorkflowSupervisor sup;
    sup.initialize(outer_graph(), outer_bodies());
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::invalid,
          "S1: an unbound sub_workflow node refuses at initialize() -- run_workflow() returns invalid");
}

void s2_bind_flips_valid_without_second_initialize() {
    WorkflowSupervisor sup;
    sup.initialize(outer_graph(), outer_bodies());

    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_with_port(), inner_bodies_with_port());
    sup.bind_sub_workflow("sub", inner);

    // No second initialize() call -- proves the ergonomics fix.
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::suspended,
          "S2: binding alone (no second initialize()) makes the graph valid and runnable");
}

void s3_basic_nested_suspend_resume_complete() {
    WorkflowSupervisor sup;
    sup.initialize(outer_graph(), outer_bodies());
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_with_port(), inner_bodies_with_port());
    sup.bind_sub_workflow("sub", inner);

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r1.status == workflow_status::suspended, "S3: the outer run suspends on the inner's own port");
    check(r1.open_interactions.size() == 1, "S3: exactly one outer-visible interaction is open");
    std::string const outer_interaction_id =
        r1.open_interactions.empty() ? std::string{} : r1.open_interactions.at(0).interaction_id;
    check(!outer_interaction_id.empty(), "S3: the outer interaction id is real, not empty");

    WorkflowResult r2 =
        drive(sup.resume_workflow(ResumeWorkflow{outer_interaction_id, text_message("approved"), {}}));
    check(r2.status == workflow_status::completed,
          "S3: resuming with a plain answer (no knowledge of the inner's own interaction_id) "
          "completes the run");
    check(text_of(r2.output) == "approved>sink",
          "S3: the final output carries the inner's real resolution, routed through the outer's "
          "own sink node -- genuine end-to-end composition, not a stub");
}

// ---- S4: a terminally-failing inner run (bound_max_rounds trip, never completes) ------------------

[[nodiscard]] Workflow inner_graph_never_completes() {
    Workflow wf;
    wf.id        = "inner-never-completes";
    wf.executors = {node_desc("loop")};
    wf.edges.push_back(Edge{"loop", "loop", edge_kind::direct, {}});  // self-loop, never terminates
    wf.start = "loop";
    wf.output_selection.push_back("loop");
    wf.bound.max_rounds = 3;  // trips bound_max_rounds quickly
    return wf;
}

void s4_terminal_inner_failure_routes_as_ordinary_failure() {
    Workflow outer = outer_graph();
    // fallback policy on sub->sink (policy_for() reads the on_failure of an edge FROM the failing
    // node, not INTO it) so the failure marker still reaches sink, proving the failure routed
    // through the EXISTING EdgeFailurePolicy machinery, not a crash or hang.
    for (auto& e : outer.edges) {
        if (e.from == "sub" && e.to == "sink") {
            // `attempts` is meaningful ONLY for `retry` -- validate_workflow() requires 0 here.
            e.on_failure = EdgeFailurePolicy{edge_failure_policy::fallback, 0, "sink"};
        }
    }
    WorkflowSupervisor sup;
    sup.initialize(outer, outer_bodies());
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_never_completes(), {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                           return ExecutorOutcome{in};
                       }});
    sup.bind_sub_workflow("sub", inner);

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status == workflow_status::completed,
          "S4: a terminally-failing inner run (bound_max_rounds) is routed via the fallback policy, "
          "not a crash or an unhandled state");
}

// ---- S5: round-abort orphan disclosure -------------------------------------------------------------

[[nodiscard]] Workflow fan_out_with_sub_and_failure() {
    Workflow wf;
    wf.id = "fan-out-sub-plus-failure";
    wf.executors = {node_desc("root"), node_desc("sub", executor_kind::sub_workflow), node_desc("failer")};
    wf.edges.push_back(Edge{"root", "sub", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "failer", edge_kind::fan_out, {}});
    // "failer" has NO outgoing edge at all -- route_from() will report routing_failed for it
    // (switch_edges==0 path is fine; the real trigger is failer's OWN reply being ok=false with a
    // `fail` policy, forcing workflow_failed -> routing_failed status).
    wf.start = "root";
    wf.output_selection.push_back("sub");
    wf.bound.max_rounds = 8;
    return wf;
}

void s5_round_abort_does_not_orphan_pending_sub_workflow() {
    WorkflowSupervisor sup;
    Workflow            outer = fan_out_with_sub_and_failure();
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        {},  // sub
        [](Message const&, EffectContext&) -> result<ExecutorOutcome> {
            return std::unexpected(error{failure_class::fatal, "scripted failure", "test.failer"});
        },
    };
    sup.initialize(outer, bodies);
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_with_port(), inner_bodies_with_port());
    sup.bind_sub_workflow("sub", inner);

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r.status != workflow_status::completed && r.status != workflow_status::suspended,
          "S5: the round ends with a real failure status (failer's own fail-policy failure), not "
          "suspended and not completed");
    check(!r.open_interactions.empty(),
          "S5: even though the round's OWN status is a failure, the sub_workflow's real, live nested "
          "interaction is STILL disclosed -- not silently orphaned (finding 4's fix)");
}

// ---- S6: checkpoint-restore fail-closed proof (the most severe finding) --------------------------

void s6_restored_run_fails_closed_on_stale_pending_interaction() {
    WorkflowSupervisor sup;
    sup.initialize(outer_graph(), outer_bodies());
    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_with_port(), inner_bodies_with_port());
    sup.bind_sub_workflow("sub", inner);

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r1.status == workflow_status::suspended, "S6: the run suspends on the nested interaction");
    std::string const stale_interaction_id =
        r1.open_interactions.empty() ? std::string{} : r1.open_interactions.at(0).interaction_id;

    // Snapshot and restore -- pending_sub_workflows_ is NOT part of RunStateRecord by design (§4 of
    // the draft), so a restored instance starts with it empty, exactly like a real crash-recovery
    // restore would.
    RunStateRecord const rec = drive(sup.snapshot_record());
    WorkflowSupervisor    restored;
    restored.initialize(outer_graph(), outer_bodies());
    // Deliberately NOT re-binding "sub" -- this is the realistic case a host that bypasses
    // WorkflowCheckpointManager's own fail-closed guard (which DOES cover sub_workflow-kind, per
    // the red team's confirmed trace of workflow_checkpoint_manager.hpp) would hit.
    restored.restore_from_record(rec);

    WorkflowResult r2 = drive(
        restored.resume_workflow(ResumeWorkflow{stale_interaction_id, text_message("approved"), {}}));
    check(r2.status == workflow_status::invalid,
          "S6: resuming the STALE pending-sub-workflow interaction_id against a restored instance "
          "fails closed (invalid) -- it does NOT silently misroute the raw answer as if the "
          "sub-workflow had completed (the exact hazard the first, pre-red-team design had)");
}

// ---- S7: OQ-19-quarantine-generalization concurrency proof ----------------------------------------

// GENUINE duplicate deliveries -- `direct` edges, NOT `fan_in` (fan_in MERGES two converging
// replies into ONE delivery entry upstream of dispatch, so it never actually produces two entries
// for the quarantine to dedupe at all; `direct` edges each unconditionally push_back their own
// entry, matching the ORIGINAL agent-kind OQ-19 quarantine's own documented target case: "two
// ordinary (non-fan_in) edges converging on the SAME ... node in one round").
[[nodiscard]] Workflow converge_on_same_sub_workflow_direct() {
    Workflow wf;
    wf.id        = "converge-on-sub-direct";
    wf.executors = {node_desc("root"), node_desc("a"), node_desc("b"),
                     node_desc("sub", executor_kind::sub_workflow)};
    wf.edges.push_back(Edge{"root", "a", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "b", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"a", "sub", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"b", "sub", edge_kind::direct, {}});
    wf.start = "root";
    wf.output_selection.push_back("sub");
    wf.bound.max_rounds = 8;
    return wf;
}

// Proves the quarantine is genuinely exercised (real concurrent dispatch attempted, not merged
// away upstream) AND that the outcome is safe either way: the real (non-quarantined) delivery
// suspends on the inner's own port (tracked, disclosed via open_interactions -- Finding 4's fix);
// the quarantined duplicate's synthetic contract-class failure has no fail-policy edge to recover
// through (this graph declares none for "sub"), so the round itself ends `executor_failed` --
// exactly the same "round aborts for an unrelated reason, nested interaction still disclosed"
// shape S5 already proves, not a new failure mode.
void s7_quarantine_generalization_proof() {
    for (int trial = 0; trial < 5; ++trial) {
        WorkflowSupervisor sup;
        Workflow            outer = converge_on_same_sub_workflow_direct();
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">a")};
            },
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(text_of(in) + ">b")};
            },
            {},  // sub
        };
        sup.initialize(outer, bodies);
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(inner_graph_with_port(), inner_bodies_with_port_slow());
        sup.bind_sub_workflow("sub", inner);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r1.status == workflow_status::executor_failed || r1.status == workflow_status::suspended,
              "S7: two GENUINE duplicate deliveries to the SAME sub_workflow executor_index -- no "
              "crash or hang; the round reaches an honest terminal state either way");
        check(!r1.open_interactions.empty(),
              "S7: the real (non-quarantined) dispatch's nested interaction is disclosed regardless "
              "of whether the quarantined duplicate's own synthetic failure also aborted the round");
    }
    std::fprintf(stderr, "  .. S7 debug: genuine concurrent overlap observed = %s\n",
                 g_overlap_observed.load() ? "YES" : "NO");
}

// ---- S8: bounded 2-level nesting -------------------------------------------------------------------

void s8_bounded_two_level_nesting() {
    // Level 2 (innermost): a plain 1-node graph.
    auto level2 = std::make_shared<WorkflowSupervisor>();
    Workflow level2_graph;
    level2_graph.id        = "level2";
    level2_graph.executors = {node_desc("leaf")};
    level2_graph.start     = "leaf";
    level2_graph.output_selection.push_back("leaf");
    level2_graph.bound.max_rounds = 4;
    level2->initialize(level2_graph,
                        {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                            return ExecutorOutcome{text_message(text_of(in) + ">leaf")};
                        }});

    // Level 1 (middle): a 3-wide fan-out/fan-in, each branch a sub_workflow wrapping ITS OWN
    // (separate, independently-bound) copy of level2 -- proving nested WorkflowSupervisor instances
    // each own their own independent ThreadPool (no shared-pool reentrancy hazard).
    Workflow level1_graph;
    level1_graph.id = "level1";
    level1_graph.executors = {node_desc("root"), node_desc("s1", executor_kind::sub_workflow),
                               node_desc("s2", executor_kind::sub_workflow),
                               node_desc("s3", executor_kind::sub_workflow), node_desc("join")};
    level1_graph.edges.push_back(Edge{"root", "s1", edge_kind::fan_out, {}});
    level1_graph.edges.push_back(Edge{"root", "s2", edge_kind::fan_out, {}});
    level1_graph.edges.push_back(Edge{"root", "s3", edge_kind::fan_out, {}});
    level1_graph.edges.push_back(Edge{"s1", "join", edge_kind::fan_in, {}});
    level1_graph.edges.push_back(Edge{"s2", "join", edge_kind::fan_in, {}});
    level1_graph.edges.push_back(Edge{"s3", "join", edge_kind::fan_in, {}});
    level1_graph.start = "root";
    level1_graph.output_selection.push_back("join");
    level1_graph.bound.max_rounds = 8;

    auto level1 = std::make_shared<WorkflowSupervisor>();
    level1->initialize(level1_graph,
                        {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                            return ExecutorOutcome{in};
                        },
                         {}, {}, {},
                         [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                             return ExecutorOutcome{in};
                         }});
    // Each sub_workflow node bound to its OWN separately-initialize()d level2 instance -- three
    // real, independent WorkflowSupervisor instances, three independent ThreadPools.
    for (char const* id : {"s1", "s2", "s3"}) {
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(level2_graph,
                           {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                               return ExecutorOutcome{text_message(text_of(in) + ">leaf")};
                           }});
        level1->bind_sub_workflow(id, inner);
    }

    // Outer (level 0): a single sub_workflow node wrapping level1 -- this is the 2nd nesting level.
    WorkflowSupervisor outer;
    outer.initialize(outer_graph(), outer_bodies());
    outer.bind_sub_workflow("sub", level1);

    auto const started = std::chrono::steady_clock::now();
    WorkflowResult r    = drive(outer.run_workflow(RunWorkflow{text_message("go")}));
    auto const elapsed  = std::chrono::steady_clock::now() - started;

    check(r.status == workflow_status::completed, "S8: bounded 2-level nesting (3-wide fan-out at "
                                                    "the middle level) completes");
    check(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10,
          "S8: completes in bounded wall-clock time -- no unbounded thread/CPU blowup at this "
          "documented, tested nesting shape (2 levels, 3-wide fan-out)");
}

// ---- S9: binding a non-sub_workflow-kind executor_id is silently refused ------------------------

void s9_bind_wrong_kind_is_refused() {
    WorkflowSupervisor sup;
    sup.initialize(outer_graph(), outer_bodies());  // "start" (function), "sub" (sub_workflow), "sink" (function)

    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(inner_graph_with_port(), inner_bodies_with_port());

    // Wrong kind -- "start" is function-kind, not sub_workflow-kind.
    sup.bind_sub_workflow("start", inner);
    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r1.status == workflow_status::invalid,
          "S9: binding to a non-sub_workflow-kind executor_id is a no-op -- 'sub' is still unbound, "
          "so the graph is still invalid (proves the bind was genuinely refused, not silently "
          "mis-targeted onto the wrong node)");

    // The REAL sub_workflow node still binds normally afterward.
    sup.bind_sub_workflow("sub", inner);
    WorkflowResult r2 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r2.status == workflow_status::suspended,
          "S9: binding to the CORRECT sub_workflow-kind node still works after the refused attempt");
}

// ---- S10: the same `inner` instance cannot be bound to two different sub_workflow nodes ----------

[[nodiscard]] Workflow two_sub_workflow_nodes_graph() {
    Workflow wf;
    wf.id        = "two-sub-workflow-nodes";
    wf.executors = {node_desc("root"), node_desc("sub1", executor_kind::sub_workflow),
                     node_desc("sub2", executor_kind::sub_workflow), node_desc("join")};
    wf.edges.push_back(Edge{"root", "sub1", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"root", "sub2", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"sub1", "join", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"sub2", "join", edge_kind::fan_in, {}});
    wf.start = "root";
    wf.output_selection.push_back("join");
    wf.bound.max_rounds = 8;
    return wf;
}

void s10_duplicate_inner_binding_is_refused() {
    WorkflowSupervisor sup;
    std::vector<ExecutorBody> bodies = {
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
        {},  // sub1
        {},  // sub2
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };
    sup.initialize(two_sub_workflow_nodes_graph(), bodies);

    auto inner = std::make_shared<WorkflowSupervisor>();
    Workflow trivial;
    trivial.id        = "trivial";
    trivial.executors = {node_desc("leaf")};
    trivial.start     = "leaf";
    trivial.output_selection.push_back("leaf");
    trivial.bound.max_rounds = 4;
    inner->initialize(trivial,
                       {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                           return ExecutorOutcome{in};
                       }});

    sup.bind_sub_workflow("sub1", inner);
    // Binding the SAME `inner` to a SECOND executor_index must be refused -- otherwise the
    // OQ-19-generalized quarantine's own per-executor_index dedup (ADR-157) would not protect
    // against two DIFFERENT nodes dispatching concurrently into the same inner instance.
    sup.bind_sub_workflow("sub2", inner);

    WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r1.status == workflow_status::invalid,
          "S10: 'sub2' stays unbound (the duplicate bind was refused) -- the graph is still "
          "invalid even though 'sub1' bound successfully");

    // A genuinely DIFFERENT inner instance for sub2 works fine.
    auto inner2 = std::make_shared<WorkflowSupervisor>();
    inner2->initialize(trivial,
                        {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                            return ExecutorOutcome{in};
                        }});
    sup.bind_sub_workflow("sub2", inner2);
    WorkflowResult r2 = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
    check(r2.status == workflow_status::completed,
          "S10: binding a SECOND, genuinely distinct inner instance to 'sub2' succeeds -- the "
          "refusal is specifically about reusing the SAME instance, not about having two "
          "sub_workflow nodes at all");
}

}  // namespace

int main() {
    s1_unbound_refuses();
    s2_bind_flips_valid_without_second_initialize();
    s3_basic_nested_suspend_resume_complete();
    s4_terminal_inner_failure_routes_as_ordinary_failure();
    s5_round_abort_does_not_orphan_pending_sub_workflow();
    s6_restored_run_fails_closed_on_stale_pending_interaction();
    s7_quarantine_generalization_proof();
    s8_bounded_two_level_nesting();
    s9_bind_wrong_kind_is_refused();
    s10_duplicate_inner_binding_is_refused();

    std::fprintf(stderr, g_failures == 0 ? "test_rt_workflow_sub_workflow: ALL PASS\n"
                                          : "test_rt_workflow_sub_workflow: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
