// Implements 014-Workflow-and-Orchestration.md §1/§2; Milestone 6 Phase A
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// A validator is only worth as much as its negative controls. A test that builds correct graphs and
// watches them pass would be satisfied by `return {};` -- so every rule below is exercised in BOTH
// directions: a graph that trips it, and the nearest graph that does not. The positive cases are
// what stop the validator from being tightened into uselessness; the negative cases are what prove
// it does anything at all.
//
// The compile-time half of 014 §1 ("an edge that connects incompatible types fails to build -- at
// compile time for the C++ form") cannot be asserted from inside a test that must itself compile.
// It is proven instead by `tests/compile_fail/workflow_edge_type_mismatch.cpp` (registered in
// tests/CMakeLists.txt as a build-must-fail check), which is the only form that claim can honestly
// take. A6 below asserts the runtime consequence that IS observable here.

#include <cstdio>
#include <string>

#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;

namespace {

struct Question {};
struct Draft {};
struct Verdict {};

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Rejected with the expected stable code -- never merely "rejected", which would pass if the
// validator failed for an unrelated reason and would hide a rule that stopped working.
void check_rejected(result<void> const& r, char const* expected_code, char const* what) {
    if (r.has_value()) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (accepted, expected rejection)\n", what);
        return;
    }
    if (r.error().code != expected_code) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (rejected with '%s', expected '%s')\n", what,
                     r.error().code.c_str(), expected_code);
        return;
    }
    if (r.error().klass != failure_class::contract) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (not classified `contract`)\n", what);
        return;
    }
    std::fprintf(stderr, "  ok: %s\n", what);
}

// A minimal VALID two-node workflow, as the baseline every negative case perturbs by exactly one
// thing. Written as a function rather than a constant so each case gets a fresh copy to break.
[[nodiscard]] Workflow baseline() {
    Workflow wf;
    wf.id = "review";
    wf.executors.push_back(Executor{.id = "writer", .kind = executor_kind::agent, .input_type = "Question", .output_type = "Draft",
                                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
    wf.executors.push_back(Executor{.id = "critic", .kind = executor_kind::agent, .input_type = "Draft", .output_type = "Verdict",
                                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
    wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
    wf.start = "writer";
    wf.output_selection.push_back("critic");
    wf.bound.max_rounds = 8;
    return wf;
}

}  // namespace

AE_WORKFLOW_MESSAGE(Question, "Question");
AE_WORKFLOW_MESSAGE(Draft, "Draft");
AE_WORKFLOW_MESSAGE(Verdict, "Verdict");

int main() {
    // ---- A1: the baseline validates, and each rule's near-miss neighbour still validates ---------
    // If this failed, every negative case below would be meaningless -- they would all be rejected
    // for whatever the baseline itself got wrong.
    {
        check(validate_workflow(baseline()).has_value(),
              "A1: a well-formed two-node workflow validates");

        Workflow cyclic = baseline();
        // 014 §9 Q2: cycles are ALLOWED. The loop-closing edge is type-checked exactly like any
        // other, and §2's whole-workflow round counter bounds its iterations. A validator that
        // rejected this would make §3's reflection/critic and group-chat patterns unbuildable.
        cyclic.executors.push_back(Executor{.id = "revise", .kind = executor_kind::agent, .input_type = "Verdict", .output_type = "Draft",
                                             .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
        cyclic.edges.push_back(Edge{"critic", "revise", edge_kind::direct, {}});
        cyclic.edges.push_back(Edge{"revise", "critic", edge_kind::direct, {}});
        check(validate_workflow(cyclic).has_value(),
              "A1: a CYCLIC graph validates -- 014 §9 Q2 allows cycles, bounded globally by §2's "
              "round counter, and the reflection/critic pattern depends on it");
    }

    // ---- A2: 014 §1's type rule, in both directions ---------------------------------------------
    {
        Workflow wf = baseline();
        wf.executors[1].input_type = "Verdict";  // critic now wants what writer does not produce
        check_rejected(validate_workflow(wf), "workflow.edge_type_mismatch",
                       "A2: an edge whose source output type differs from its target input type is "
                       "rejected (014 §1)");

        Workflow untyped = baseline();
        untyped.executors[0].output_type.clear();
        check_rejected(validate_workflow(untyped), "workflow.untyped_port",
                       "A2: an UNTYPED port is rejected rather than compared -- otherwise the type "
                       "check would silently pass by having nothing to compare");
    }

    // ---- A3: 014 §2's required termination bound -------------------------------------------------
    {
        Workflow wf = baseline();
        wf.bound = TerminationBound{};
        check_rejected(validate_workflow(wf), "workflow.unbounded",
                       "A3: a workflow with no bound at all is rejected -- 014 §2's 'an unbounded "
                       "workflow does not run'");

        Workflow zero = baseline();
        zero.bound.max_rounds = 0;
        check_rejected(validate_workflow(zero), "workflow.zero_max_rounds",
                       "A3: max_rounds = 0 is rejected -- it satisfies 'a bound is present' while "
                       "being unable to execute a single round");

        // Each of the three bound kinds independently satisfies §2. A validator that only accepted
        // max_rounds would silently make deadline- and budget-bounded workflows unbuildable.
        for (int which = 0; which < 3; ++which) {
            Workflow b = baseline();
            b.bound = TerminationBound{};
            if (which == 0) b.bound.max_rounds = 4;
            if (which == 1) b.bound.deadline_ms = 30'000;
            if (which == 2) b.bound.token_budget = 100'000;
            check(validate_workflow(b).has_value(),
                  "A3: each of MaxRounds / deadline / budget independently satisfies §2's bound");
        }
    }

    // ---- A4: structural integrity ----------------------------------------------------------------
    {
        Workflow dup = baseline();
        dup.executors.push_back(Executor{.id = "writer", .kind = executor_kind::function, .input_type = "Question", .output_type = "Draft",
                                          .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
        check_rejected(validate_workflow(dup), "workflow.duplicate_executor_id",
                       "A4: a duplicate executor id is reported AS a duplicate, not as whichever "
                       "downstream lookup happened to trip over it first");

        Workflow bad_edge = baseline();
        bad_edge.edges.push_back(Edge{"critic", "nobody", edge_kind::direct, {}});
        check_rejected(validate_workflow(bad_edge), "workflow.unknown_edge_endpoint",
                       "A4: an edge to an undeclared executor is rejected");

        Workflow no_start = baseline();
        no_start.start = "ghost";
        check_rejected(validate_workflow(no_start), "workflow.unknown_start",
                       "A4: an undeclared start executor is rejected");

        Workflow bad_sel = baseline();
        bad_sel.output_selection.push_back("ghost");
        check_rejected(validate_workflow(bad_sel), "workflow.unknown_output_selection",
                       "A4: output_selection naming an undeclared executor is rejected");

        Workflow orphan = baseline();
        orphan.executors.push_back(Executor{.id = "stranded", .kind = executor_kind::function, .input_type = "Draft", .output_type = "Verdict",
                                             .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
        check_rejected(validate_workflow(orphan), "workflow.unreachable_executor",
                       "A4: an executor no edge can reach from start is rejected -- 014 §7 makes the "
                       "graph reviewable, and a node nothing reaches is an authoring slip");

        // Milestone 6 Phase E: `validate_workflow` answers "is this a well-formed graph" and its
        // answer must NOT depend on how much of 014 is implemented -- §7's rendering, diffing, and
        // review all run over graphs this build cannot execute, and this suite's own `baseline()`
        // has declared `agent`-kind nodes since Phase A for exactly that reason. "Can this build
        // execute it" is a separate predicate (`check_workflow_executable`), tested in
        // test_workflow_request_port.cpp where the executor that would misrun it lives.
        Workflow unbuilt = baseline();
        unbuilt.executors.front().kind = executor_kind::agent;
        check(validate_workflow(unbuilt).has_value(),
              "A4: an `agent`-kind executor is a well-formed graph node even though this build "
              "cannot run one -- validity is a property of the graph, not of the implementation");

        // The reachability walk must handle cycles without looping forever. This is the case that
        // would hang rather than fail, so it is asserted explicitly.
        Workflow self_loop = baseline();
        self_loop.executors.push_back(Executor{.id = "revise", .kind = executor_kind::agent, .input_type = "Verdict", .output_type = "Verdict",
                                                .worktree_mode = sharing_mode::branch, .capability_ceiling = {}});
        self_loop.edges.push_back(Edge{"critic", "revise", edge_kind::direct, {}});
        self_loop.edges.push_back(Edge{"revise", "revise", edge_kind::direct, {}});
        check(validate_workflow(self_loop).has_value(),
              "A4: reachability terminates on a self-loop -- the walk marks visited nodes, so a "
              "cyclic graph validates rather than hanging");
    }

    // ---- A5: case labels belong to exactly two edge kinds -----------------------------------------
    {
        Workflow missing = baseline();
        missing.edges[0].kind = edge_kind::switch_case;
        check_rejected(validate_workflow(missing), "workflow.missing_case_label",
                       "A5: a switch/case edge with no label is rejected -- it could never route");

        Workflow unexpected = baseline();
        unexpected.edges[0].case_label = "approved";
        check_rejected(validate_workflow(unexpected), "workflow.unexpected_case_label",
                       "A5: a label on a plain direct edge is rejected rather than silently ignored "
                       "-- a field quietly dropped at runtime is how an author's intent disappears");

        Workflow ok = baseline();
        ok.edges[0].kind = edge_kind::switch_case;
        ok.edges[0].case_label = "needs_review";
        check(validate_workflow(ok).has_value(), "A5: a labelled switch/case edge validates");

        Workflow multi = baseline();
        multi.edges[0].kind = edge_kind::multi_selection;
        multi.edges[0].case_label = "reviewers";
        check(validate_workflow(multi).has_value(),
              "A5: multi-selection carries a label on the same rule as switch/case");
    }

    // ---- A6: the C++ builder emits a description the SHARED validator accepts ---------------------
    // I6's actual content. The builder's `static_assert` covers edge types and nothing else, so if
    // `build()` did not also run `validate_workflow`, the C++ form would accept graphs the
    // declarative form rejects -- two surfaces, two behaviours, which is the drift decision 6 exists
    // to prevent.
    {
        TypedExecutor<Question, Draft>  writer{.id = "writer", .kind = executor_kind::agent, .capability_ceiling = {}};
        TypedExecutor<Draft, Verdict>   critic{.id = "critic", .kind = executor_kind::agent, .capability_ceiling = {}};

        WorkflowBuilder b("review");
        auto built = b.add(writer).add(critic).connect(writer, critic).start_at("writer")
                       .select_output("critic").max_rounds(8).build();
        check(built.has_value(), "A6: the C++ builder produces a workflow that validates");
        if (built) {
            check(built->executors.size() == 2 && built->edges.size() == 1,
                  "A6: the emitted description carries the authored executors and edges");
            check(built->executors[0].input_type == "Question" &&
                      built->executors[0].output_type == "Draft",
                  "A6: port types are the DECLARED portable names -- the same strings a 015 YAML "
                  "file would carry, not a compiler-derived spelling");
        }

        // The builder is not exempt from the shared validator: this graph is type-correct at every
        // edge (so the static_asserts are all satisfied) and still invalid, because it declares no
        // bound. A builder that only trusted its compile-time checks would accept it.
        WorkflowBuilder unbounded("review");
        auto rejected = unbounded.add(writer).add(critic).connect(writer, critic)
                                 .start_at("writer").build();
        check(!rejected.has_value() && rejected.error().code == "workflow.unbounded",
              "A6: a type-correct graph with no bound is still rejected by build() -- the C++ form "
              "runs the SAME validator, it does not substitute its static_asserts for it");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_workflow_graph_validation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_graph_validation: %d FAILURE(S)\n", g_failures);
    return 1;
}
