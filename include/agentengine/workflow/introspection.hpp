#pragma once
// Implements 014-Workflow-and-Orchestration.md §7 (Visualization and introspection): "The graph is
// data: it renders (Mermaid/DOT), it validates [Phase A], it diffs across versions... A workflow
// that cannot be drawn is a workflow nobody can review." Milestone 6 Phase G
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// RENDERING IS A TOTAL FUNCTION over any `Workflow` -- never fails, never throws. §7's own
// falsifiable bar is "a valid workflow that cannot be drawn"; the only way to guarantee that is for
// this code to have no failure path at all, so an author's arbitrary id text (spaces, quotes,
// unicode, whatever a declarative loader (015) or a hand-authored `WorkflowBuilder` call produces)
// can never break Mermaid/DOT syntax. That is why node identifiers on the wire are `n<index>`
// (stable, collision-free, always syntactically valid) rather than a sanitized version of the
// author's own id -- sanitizing (strip/replace special characters) can COLLIDE two distinct ids
// onto the same token, silently merging two different nodes in the rendered graph, which would be
// a worse failure than an ugly label. The author's real id is always the LABEL text instead
// (quoted, with the one escape each format needs), never the identifier a renderer's own syntax
// depends on.
//
// DIFFING is a plain structural comparison over two `Workflow` values -- no version history, no
// three-way merge; 014 §7 says "diffs across versions" and this project's own 025 merge machinery
// is deliberately not reused here (a workflow graph is authored data, not a worktree). `operator==`
// on `Executor`/`Edge`/`EdgeFailurePolicy`/`TerminationBound`/`Workflow` (graph.hpp) is what makes
// this a few lines rather than a hand-rolled field-by-field walk.

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

namespace detail {

// Escapes `"` (the one character both Mermaid's `"..."` label quoting and DOT's `"..."` label
// quoting treat specially) and collapses newlines -- an author's id/label is never trusted to be
// single-line, syntax-safe text.
[[nodiscard]] inline std::string escape_label(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') {
            out += "&quot;";
        } else if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

[[nodiscard]] inline std::unordered_map<std::string, std::size_t> index_by_id(Workflow const& wf) {
    std::unordered_map<std::string, std::size_t> idx;
    idx.reserve(wf.executors.size());
    for (std::size_t i = 0; i < wf.executors.size(); ++i) idx[wf.executors[i].id] = i;
    return idx;
}

[[nodiscard]] inline std::string node_token(std::size_t index) { return "n" + std::to_string(index); }

}  // namespace detail

// -- Mermaid (`flowchart TD`) --------------------------------------------------------------------

[[nodiscard]] inline std::string render_mermaid(Workflow const& wf) {
    auto const idx = detail::index_by_id(wf);

    std::ostringstream out;
    out << "flowchart TD\n";

    // A synthetic entry arrow into the declared start executor -- §7's "start" is not itself an
    // Executor, so it needs a node of its own to point FROM, the common flowchart convention for
    // marking an entry point (rather than styling that could be missed at a glance).
    out << "    start_((\"Start\")) --> " << detail::node_token(idx.at(wf.start)) << "\n";

    for (std::size_t i = 0; i < wf.executors.size(); ++i) {
        Executor const&    e     = wf.executors[i];
        std::string const  token = detail::node_token(i);
        std::string const  label = detail::escape_label(e.id);
        switch (e.kind) {
            case executor_kind::function:
                out << "    " << token << "[\"" << label << "\"]\n";
                break;
            case executor_kind::agent:
                out << "    " << token << "(\"" << label << "\")\n";
                break;
            case executor_kind::sub_workflow:
                out << "    " << token << "[[\"" << label << "\"]]\n";
                break;
            case executor_kind::request_port:
                out << "    " << token << "{{\"" << label << "\"}}\n";
                break;
        }
    }

    for (auto const& edge : wf.edges) {
        std::string const from = detail::node_token(idx.at(edge.from));
        std::string const to   = detail::node_token(idx.at(edge.to));
        switch (edge.kind) {
            case edge_kind::direct:
            case edge_kind::chain:
                out << "    " << from << " --> " << to << "\n";
                break;
            case edge_kind::fan_out:
                out << "    " << from << " -- \"fan-out\" --> " << to << "\n";
                break;
            case edge_kind::fan_in:
                out << "    " << from << " -- \"fan-in\" --> " << to << "\n";
                break;
            case edge_kind::switch_case:
                out << "    " << from << " -- \"" << detail::escape_label(edge.case_label) << "\" --> "
                    << to << "\n";
                break;
            case edge_kind::multi_selection:
                // Dashed, not solid: 014 §1 says switch_case selects EXACTLY one, multi_selection a
                // caller-chosen SUBSET -- the same visual distinction the RFC draws in prose.
                out << "    " << from << " -. \"" << detail::escape_label(edge.case_label) << "\" .-> "
                    << to << "\n";
                break;
        }
        if (edge.on_failure.kind == edge_failure_policy::fallback) {
            out << "    " << from << " -. \"on failure\" .-> " << detail::node_token(idx.at(edge.on_failure.fallback))
                << "\n";
        }
    }

    // 014 §7 style hooks: the start node and every output-selected node are visually distinct
    // without a reader having to trace edges to find them.
    out << "    classDef aeStart fill:#4CAF50,stroke:#2E7D32,color:#ffffff;\n";
    out << "    class start_ aeStart;\n";
    if (!wf.output_selection.empty()) {
        out << "    classDef aeOutput stroke-width:3px;\n";
        out << "    class ";
        for (std::size_t i = 0; i < wf.output_selection.size(); ++i) {
            if (i > 0) out << ",";
            out << detail::node_token(idx.at(wf.output_selection[i]));
        }
        out << " aeOutput;\n";
    }

    return out.str();
}

// -- Graphviz DOT ---------------------------------------------------------------------------------

[[nodiscard]] inline std::string render_dot(Workflow const& wf) {
    auto const idx = detail::index_by_id(wf);

    std::ostringstream out;
    out << "digraph \"" << detail::escape_label(wf.id) << "\" {\n";
    out << "  rankdir=TD;\n";

    out << "  start_ [label=\"Start\", shape=circle, style=filled, fillcolor=\"#4CAF50\", "
           "fontcolor=\"#ffffff\"];\n";
    out << "  start_ -> " << detail::node_token(idx.at(wf.start)) << ";\n";

    bool const has_output = !wf.output_selection.empty();
    auto is_output_selected = [&](std::string const& id) {
        return std::find(wf.output_selection.begin(), wf.output_selection.end(), id) !=
               wf.output_selection.end();
    };

    for (std::size_t i = 0; i < wf.executors.size(); ++i) {
        Executor const&    e     = wf.executors[i];
        std::string const  token = detail::node_token(i);
        std::string const  label = detail::escape_label(e.id);
        char const*        shape = "box";
        switch (e.kind) {
            case executor_kind::function: shape = "box"; break;
            case executor_kind::agent: shape = "ellipse"; break;
            case executor_kind::sub_workflow: shape = "box3d"; break;
            case executor_kind::request_port: shape = "hexagon"; break;
        }
        out << "  " << token << " [label=\"" << label << "\", shape=" << shape;
        if (has_output && is_output_selected(e.id)) out << ", peripheries=2";
        out << "];\n";
    }

    for (auto const& edge : wf.edges) {
        std::string const from = detail::node_token(idx.at(edge.from));
        std::string const to   = detail::node_token(idx.at(edge.to));
        out << "  " << from << " -> " << to;
        switch (edge.kind) {
            case edge_kind::direct:
            case edge_kind::chain:
                out << ";\n";
                break;
            case edge_kind::fan_out:
                out << " [label=\"fan-out\"];\n";
                break;
            case edge_kind::fan_in:
                out << " [label=\"fan-in\"];\n";
                break;
            case edge_kind::switch_case:
                out << " [label=\"" << detail::escape_label(edge.case_label) << "\"];\n";
                break;
            case edge_kind::multi_selection:
                out << " [label=\"" << detail::escape_label(edge.case_label) << "\", style=dashed];\n";
                break;
        }
        if (edge.on_failure.kind == edge_failure_policy::fallback) {
            out << "  " << from << " -> " << detail::node_token(idx.at(edge.on_failure.fallback))
                << " [label=\"on failure\", style=dotted];\n";
        }
    }

    out << "}\n";
    return out.str();
}

// -- Diff across versions --------------------------------------------------------------------------

struct WorkflowDiff {  // ae-naming-lint: allow WorkflowDiff — 014 §7 names "diffs across versions" normatively; 027 has not been updated to list this type
    std::vector<std::string> added_executor_ids;
    std::vector<std::string> removed_executor_ids;
    // Present in both graphs under the same id, but some field differs (kind, input/output type).
    std::vector<std::string> changed_executor_ids;
    std::vector<Edge>        added_edges;
    std::vector<Edge>        removed_edges;
    bool                     start_changed            = false;
    bool                     output_selection_changed = false;
    bool                     bound_changed             = false;

    [[nodiscard]] bool empty() const noexcept {
        return added_executor_ids.empty() && removed_executor_ids.empty() &&
               changed_executor_ids.empty() && added_edges.empty() && removed_edges.empty() &&
               !start_changed && !output_selection_changed && !bound_changed;
    }
};

[[nodiscard]] inline WorkflowDiff diff_workflows(Workflow const& before, Workflow const& after) {
    WorkflowDiff diff;

    for (auto const& e : after.executors) {
        Executor const* prior = before.find(e.id);
        if (prior == nullptr) {
            diff.added_executor_ids.push_back(e.id);
        } else if (!(*prior == e)) {
            diff.changed_executor_ids.push_back(e.id);
        }
    }
    for (auto const& e : before.executors) {
        if (after.find(e.id) == nullptr) diff.removed_executor_ids.push_back(e.id);
    }

    for (auto const& edge : after.edges) {
        bool const present_before =
            std::find(before.edges.begin(), before.edges.end(), edge) != before.edges.end();
        if (!present_before) diff.added_edges.push_back(edge);
    }
    for (auto const& edge : before.edges) {
        bool const present_after =
            std::find(after.edges.begin(), after.edges.end(), edge) != after.edges.end();
        if (!present_after) diff.removed_edges.push_back(edge);
    }

    diff.start_changed             = before.start != after.start;
    diff.output_selection_changed  = before.output_selection != after.output_selection;
    diff.bound_changed             = !(before.bound == after.bound);

    return diff;
}

}  // namespace agentengine::workflow
