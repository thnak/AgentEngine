// Milestone 7 Phase F2 (015-Declarative-Agent-Format.md §3, docs/planning/milestone-7-protocol-
// conformance-breakdown.md). Proves `compile_workflow_document()` (workflow/yaml_compiler.hpp)
// against 015's own §3 example -- both LITERALLY as written (proving the real, honest finding that
// it is underspecified relative to what `validate_workflow()` requires) and EXTENDED with the
// input_type/output_type fields a real document needs, where it produces a genuinely valid `Workflow`
// accepted by the SAME `validate_workflow()` (workflow/graph.hpp, Milestone 6) the C++ authoring form
// uses -- the actual I6 property this compiler exists to serve.

#include <cstdio>
#include <optional>
#include <string>

#include "agentengine/core/yaml_value.hpp"
#include "agentengine/workflow/yaml_compiler.hpp"

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

namespace yaml = agentengine::yaml;
namespace wf   = agentengine::workflow;

}  // namespace

int main() {
    // --- W-1: 015 §3's OWN example document, LITERALLY -- compiles, but validate_workflow() -------
    // --- correctly rejects it as underspecified (no input_type/output_type anywhere).             ---
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: research-and-write, version: 0.3.0 }
spec:
  start: planner
  executors:
    - { id: planner,  agent: researcher }
    - { id: search,   agent: researcher, concurrency: 4 }
    - { id: review,   kind: request_port, prompt: "Approve the outline?" }
    - { id: writer,   agent: writer }
  edges:
    - { from: planner, fan_out_to: [search] }
    - { from: search,  fan_in_to: review }
    - { from: review,  to: writer }
  limits: { max_rounds: 20, deadline: 15m }
  output_from: writer
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "W-1: 015 §3's own example parses (reusing F1's parser)");
        if (parsed.has_value()) {
            auto compiled = wf::compile_workflow_document(*parsed);
            check(compiled.has_value(),
                  "W-1: the document COMPILES -- compile_workflow_document() does not itself require "
                  "port types, only validate_workflow() does");
            if (compiled.has_value()) {
                check(compiled->id == "research-and-write" && compiled->start == "planner" &&
                          compiled->executors.size() == 4 && compiled->edges.size() == 3,
                      "W-1: id/start/executor-count/edge-count are all correct");
                auto validated = wf::validate_workflow(*compiled);
                check(!validated.has_value(),
                      "W-1: validate_workflow() correctly REJECTS it -- 015 §3's own illustrative "
                      "example has no input_type/output_type anywhere, but 014 §1's port-typing rule "
                      "requires both on every executor; this is a real gap in the RFC's own example, "
                      "not a bug in this compiler");
                if (!validated.has_value()) {
                    check(validated.error().code == "workflow.untyped_port",
                          "W-1: rejected with the real workflow.untyped_port code, the SAME validator "
                          "a hand-written C++ workflow with an untyped port would also fail");
                }
            }
        }
    }

    // --- W-2: the SAME document, EXTENDED with input_type/output_type -- a genuinely valid Workflow -
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: research-and-write, version: 0.3.0, description: "Research a topic and write it up." }
spec:
  start: planner
  executors:
    - { id: planner, agent: researcher, input_type: Question, output_type: Outline }
    - { id: search,  agent: researcher, input_type: Outline,  output_type: Findings }
    - { id: review,  kind: request_port, input_type: Findings, output_type: Findings }
    - { id: writer,  agent: writer,     input_type: Findings, output_type: Article }
  edges:
    - { from: planner, fan_out_to: [search] }
    - { from: search,  fan_in_to: review }
    - { from: review,  to: writer }
  limits: { max_rounds: 20, deadline: 15m }
  output_from: writer
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "W-2: the extended document parses");
        if (parsed.has_value()) {
            auto compiled = wf::compile_workflow_document(*parsed);
            check(compiled.has_value(), "W-2: the extended document compiles");
            if (compiled.has_value()) {
                auto validated = wf::validate_workflow(*compiled);
                check(validated.has_value(),
                      "W-2: validate_workflow() ACCEPTS the extended document -- a genuinely valid "
                      "Workflow, produced by the declarative loader, checked by the SAME shared "
                      "validator the C++ WorkflowBuilder form uses (I6)");

                // Gap-2 fix (2026-08-14, decisions/ADR-044-*.md): metadata.description/version now
                // have real slots.
                check(compiled->description == "Research a topic and write it up.",
                      "W-2: description <- metadata.description");
                check(compiled->version == std::optional<std::string>{"0.3.0"},
                      "W-2: version <- metadata.version");

                // Spot-check the compiled shape.
                auto const* planner = compiled->find("planner");
                check(planner && planner->kind == wf::executor_kind::agent &&
                          planner->input_type == "Question" && planner->output_type == "Outline",
                      "W-2: the \"agent:\" field correctly infers executor_kind::agent, and both "
                      "port types round-trip exactly");
                auto const* review = compiled->find("review");
                check(review && review->kind == wf::executor_kind::request_port,
                      "W-2: an explicit \"kind: request_port\" is honoured");

                bool saw_fan_out = false, saw_fan_in = false, saw_direct = false;
                for (auto const& e : compiled->edges) {
                    if (e.from == "planner" && e.to == "search" && e.kind == wf::edge_kind::fan_out)
                        saw_fan_out = true;
                    if (e.from == "search" && e.to == "review" && e.kind == wf::edge_kind::fan_in)
                        saw_fan_in = true;
                    if (e.from == "review" && e.to == "writer" && e.kind == wf::edge_kind::direct)
                        saw_direct = true;
                }
                check(saw_fan_out && saw_fan_in && saw_direct,
                      "W-2: fan_out_to/fan_in_to/to all compile to their real, distinct edge_kind");

                check(compiled->bound.max_rounds == std::optional<std::uint32_t>{20},
                      "W-2: limits.max_rounds compiles correctly");
                check(compiled->bound.deadline_ms == std::optional<std::uint64_t>{15ULL * 60 * 1000},
                      "W-2: limits.deadline (\"15m\") compiles to exactly 900000 ms");
                check(compiled->output_selection.size() == 1 && compiled->output_selection[0] == "writer",
                      "W-2: output_from becomes a one-element output_selection");
            }
        }
    }

    // --- W-3: fan_out_to with MULTIPLE targets produces one Edge per target ------------------------
    {
        std::string const doc = R"YAML(
metadata: { id: w3 }
spec:
  start: a
  executors:
    - { id: a, kind: function, input_type: X, output_type: Y }
    - { id: b, kind: function, input_type: Y, output_type: Z }
    - { id: c, kind: function, input_type: Y, output_type: Z }
  edges:
    - { from: a, fan_out_to: [b, c] }
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "W-3: setup: the document parses");
        if (parsed.has_value()) {
            auto compiled = wf::compile_workflow_document(*parsed);
            check(compiled.has_value() && compiled->edges.size() == 2,
                  "W-3: fan_out_to: [b, c] compiles to exactly TWO Edge records, one per target");
        }
    }

    // --- W-4: an edge with BOTH \"to\" and \"fan_out_to\" is rejected -- ambiguous, never guessed ---
    {
        std::string const doc = R"YAML(
metadata: { id: w4 }
spec:
  start: a
  executors:
    - { id: a, kind: function, input_type: X, output_type: Y }
  edges:
    - { from: a, to: a, fan_out_to: [a] }
)YAML";
        auto parsed = yaml::parse(doc);
        check(parsed.has_value(), "W-4: setup: the document parses");
        if (parsed.has_value()) {
            auto compiled = wf::compile_workflow_document(*parsed);
            check(!compiled.has_value(),
                  "W-4: an edge declaring both \"to\" and \"fan_out_to\" is rejected as ambiguous");
            if (!compiled.has_value()) {
                check(compiled.error().code == "yaml_compiler.ambiguous_edge_form",
                      "W-4: rejected with the real ambiguous_edge_form code");
            }
        }
    }

    // --- W-5: a document with no \"spec\" is rejected -------------------------------------------------
    {
        auto parsed = yaml::parse("metadata: { id: w5 }\n");
        check(parsed.has_value(), "W-5: setup: the document parses");
        if (parsed.has_value()) {
            auto compiled = wf::compile_workflow_document(*parsed);
            check(!compiled.has_value() && compiled.error().code == "yaml_compiler.missing_spec",
                  "W-5: a document with no \"spec\" is rejected with the real missing_spec code");
        }
    }

    // --- W-6: duration parsing -- seconds/minutes/hours/ms, and a bad unit is rejected ----------------
    {
        auto make_doc = [](std::string deadline) {
            return "metadata: { id: w6 }\nspec:\n  start: a\n  executors:\n"
                   "    - { id: a, kind: function, input_type: X, output_type: Y }\n"
                   "  limits: { deadline: " +
                   deadline + " }\n";
        };
        auto s = wf::compile_workflow_document(*yaml::parse(make_doc("30s")));
        auto h = wf::compile_workflow_document(*yaml::parse(make_doc("2h")));
        auto ms = wf::compile_workflow_document(*yaml::parse(make_doc("500ms")));
        check(s.has_value() && s->bound.deadline_ms == std::optional<std::uint64_t>{30000},
              "W-6: \"30s\" compiles to 30000 ms");
        check(h.has_value() && h->bound.deadline_ms == std::optional<std::uint64_t>{2ULL * 3600 * 1000},
              "W-6: \"2h\" compiles to 7200000 ms");
        check(ms.has_value() && ms->bound.deadline_ms == std::optional<std::uint64_t>{500},
              "W-6: \"500ms\" compiles to 500 ms");

        auto bad = wf::compile_workflow_document(*yaml::parse(make_doc("30x")));
        check(!bad.has_value() && bad.error().code == "yaml_compiler.bad_duration_unit",
              "W-6: an unrecognized duration unit (\"30x\") is rejected, never silently ignored");
    }

    if (g_failures == 0) {
        std::printf("test_workflow_yaml_compiler: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_yaml_compiler: %d failure(s)\n", g_failures);
    return 1;
}
