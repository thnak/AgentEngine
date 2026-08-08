// Implements 014-Workflow-and-Orchestration.md §7 (Visualization and introspection): render
// (Mermaid/DOT) and diff across versions. Milestone 6 Phase G
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// G0 -- the falsifiable bar itself: every valid workflow renders, in both formats, without
// exception, for a graph that exercises all four executor kinds, all six edge kinds, and a
// fallback branch -- not just the simple chains other Phase tests use. G1 -- author ids containing
// characters that would break naive Mermaid/DOT syntax (quotes, spaces) still render, because node
// identifiers are index-derived, never sanitized author text. G2-G5 -- diff across versions:
// add/remove/change an executor, add/remove an edge, and an unchanged graph diffing to empty.

#include <cstdio>
#include <string>

#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/introspection.hpp"

using namespace agentengine::workflow;

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

[[nodiscard]] bool contains(std::string const& haystack, std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] Executor node(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{id, kind, "T", "T"};
}

}  // namespace

int main() {
    // =========================================================================================
    // G0/G1 -- a graph exercising every executor kind, every edge kind, and a fallback branch.
    // Deliberately NOT run through `check_workflow_executable` -- §7's rendering/diffing work over
    // graphs THIS BUILD CANNOT EXECUTE (agent/sub_workflow), which is exactly graph.hpp's own
    // stated reason the two questions are separate.
    // =========================================================================================
    Workflow wf;
    wf.id        = "intro test \"quoted\"";  // deliberately awkward: quotes in the workflow's own id
    wf.executors = {
        node("plan", executor_kind::agent),
        node("sub", executor_kind::sub_workflow),
        node("gate", executor_kind::request_port),
        node("router", executor_kind::function),
        node("a"),
        node("b"),
        node("merge"),
        node("recover"),
        node("weird \"id\" here"),  // an author id that would break naive quoted-string syntax
    };
    wf.edges = {
        Edge{"plan", "sub", edge_kind::direct, {}, {}},
        Edge{"sub", "gate", edge_kind::chain, {}, {}},
        Edge{"gate", "router", edge_kind::direct, {}, {}},
        Edge{"router", "a", edge_kind::switch_case, "go_a", {}},
        Edge{"router", "b", edge_kind::multi_selection, "go_b", {}},
        Edge{"a", "merge", edge_kind::fan_in, {}, {}},
        Edge{"b", "merge", edge_kind::fan_in, {}, {}},
        Edge{"merge", "weird \"id\" here", edge_kind::fan_out, {},
             EdgeFailurePolicy{edge_failure_policy::fallback, 0, "recover"}},
    };
    wf.start            = "plan";
    wf.output_selection = {"weird \"id\" here", "recover"};
    wf.bound.max_rounds = 10;

    check(validate_workflow(wf).has_value(), "G0 setup: the exercising graph validates");

    std::string const mermaid = render_mermaid(wf);
    std::string const dot     = render_dot(wf);

    check(!mermaid.empty() && !dot.empty(), "G0: both renderers produce non-empty output");

    // Every executor's own id text appears as a label somewhere, INCLUDING the one with embedded
    // quotes -- escaped, not dropped or truncated.
    for (auto const& e : wf.executors) {
        std::string const escaped_mermaid = e.id == "weird \"id\" here" ? "weird &quot;id&quot; here" : e.id;
        check(contains(mermaid, escaped_mermaid),
              ("G1: Mermaid output contains executor '" + e.id + "'s (escaped) label").c_str());
        check(contains(dot, escaped_mermaid),
              ("G1: DOT output contains executor '" + e.id + "'s (escaped) label").c_str());
    }

    check(contains(mermaid, "flowchart TD") && contains(mermaid, "start_((\"Start\"))"),
          "G0: Mermaid declares a flowchart and a synthetic Start node");
    check(contains(dot, "digraph") && contains(dot, "start_ ["),
          "G0: DOT declares a digraph and a synthetic Start node");

    check(contains(mermaid, "{{\"gate\"}}"), "G0: the request_port node gets its own shape (hexagon)");
    check(contains(mermaid, "[[\"sub\"]]"), "G0: the sub_workflow node gets its own shape (subroutine)");
    check(contains(mermaid, "(\"plan\")"), "G0: the agent node gets its own shape (rounded)");

    check(contains(mermaid, "\"go_a\" -->") || contains(mermaid, "-- \"go_a\" -->"),
          "G0: the switch_case edge carries its case label");
    check(contains(mermaid, "-. \"go_b\" .->"),
          "G0: the multi_selection edge is dashed and carries its case label (distinct from switch_case)");
    check(contains(mermaid, "\"fan-in\""), "G0: fan_in edges are labelled");
    check(contains(mermaid, "\"fan-out\""), "G0: fan_out edges are labelled");
    check(contains(mermaid, "\"on failure\""),
          "G0: the fallback branch renders as its own labelled edge to the recovery executor");

    check(contains(dot, "shape=hexagon") && contains(dot, "shape=box3d") && contains(dot, "shape=ellipse"),
          "G0 (DOT): the same four executor kinds map to four distinct Graphviz shapes");
    check(contains(dot, "style=dashed") && contains(dot, "style=dotted"),
          "G0 (DOT): multi_selection (dashed) and the fallback branch (dotted) are visually distinct");

    check(contains(mermaid, "class start_ aeStart") && contains(mermaid, "aeOutput"),
          "G0: the start node and output-selected nodes get their own Mermaid style classes");
    check(contains(dot, "peripheries=2"),
          "G0 (DOT): an output-selected node is visually marked (double border)");

    // =========================================================================================
    // A totally empty-ish but still-valid single-node workflow -- the degenerate case a renderer
    // with an off-by-one on "no edges" or "no output_selection" would trip on.
    // =========================================================================================
    {
        Workflow solo;
        solo.id                = "solo";
        solo.executors          = {node("only")};
        solo.start              = "only";
        solo.bound.max_rounds   = 1;
        check(validate_workflow(solo).has_value(), "G0b setup: a single-node graph with no edges validates");
        std::string const m = render_mermaid(solo);
        std::string const d = render_dot(solo);
        check(contains(m, "\"only\"") && contains(d, "\"only\""),
              "G0b: a graph with zero edges and zero output_selection still renders without incident");
    }

    // =========================================================================================
    // G2-G5 -- diff across versions.
    // =========================================================================================
    {
        Workflow same_a = wf;
        Workflow same_b = wf;
        check(diff_workflows(same_a, same_b).empty(),
              "G2: an unchanged graph diffs to empty -- no spurious changes reported");
    }
    {
        Workflow added = wf;
        added.executors.push_back(node("brand_new"));
        added.edges.push_back(Edge{"merge", "brand_new", edge_kind::direct, {}, {}});
        auto d = diff_workflows(wf, added);
        check(d.added_executor_ids.size() == 1 && d.added_executor_ids[0] == "brand_new",
              "G3: a newly added executor is reported as added, and only that one");
        check(d.added_edges.size() == 1 && d.added_edges[0].from == "merge" &&
                  d.added_edges[0].to == "brand_new",
              "G3: the new edge into it is reported as added");
        check(d.removed_executor_ids.empty() && d.removed_edges.empty() && !d.start_changed,
              "G3: nothing else is disturbed by a pure addition");

        // The reverse direction: removing is the mirror of adding.
        auto rd = diff_workflows(added, wf);
        check(rd.removed_executor_ids.size() == 1 && rd.removed_executor_ids[0] == "brand_new",
              "G4: diffing in the other direction reports the same node as REMOVED, not added");
    }
    {
        Workflow changed = wf;
        for (auto& e : changed.executors) {
            if (e.id == "router") e.kind = executor_kind::agent;
        }
        auto d = diff_workflows(wf, changed);
        check(d.changed_executor_ids.size() == 1 && d.changed_executor_ids[0] == "router",
              "G5: an executor present in both graphs but with a changed field (kind) is CHANGED, "
              "not reported as both added and removed");
        check(d.added_executor_ids.empty() && d.removed_executor_ids.empty(),
              "G5: a changed executor is not ALSO counted as added/removed");
    }
    {
        Workflow retargeted = wf;
        retargeted.start = "router";
        auto d = diff_workflows(wf, retargeted);
        check(d.start_changed && d.added_executor_ids.empty() && d.removed_executor_ids.empty(),
              "G5b: changing the start executor is reported distinctly from a node add/remove/change");

        Workflow rebounded = wf;
        rebounded.bound.max_rounds = *wf.bound.max_rounds + 1;
        auto d2 = diff_workflows(wf, rebounded);
        check(d2.bound_changed && !d2.start_changed,
              "G5c: changing the termination bound is reported distinctly from start/executor changes");
    }

    if (g_failures == 0) {
        std::printf("test_workflow_introspection: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_introspection: %d failure(s)\n", g_failures);
    return 1;
}
