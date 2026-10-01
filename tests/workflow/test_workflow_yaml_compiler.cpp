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

struct Ticket {};

}  // namespace

AE_WORKFLOW_MESSAGE(Ticket, "Ticket");

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

    // --- W-7 (ADR-215, issue #34): a switch with a default case, declaratively -- `case:`/`default:` --
    // --- on a `to` edge -- compiles to EXACTLY the Workflow the C++ builder's connect_case/          ---
    // --- connect_default produce (I6), and malformed switch keys are refused, never ignored.         ---
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: triage }
spec:
  start: classify
  executors:
    - { id: classify, input_type: Ticket, output_type: Ticket }
    - { id: billing,  input_type: Ticket, output_type: Ticket }
    - { id: tech,     input_type: Ticket, output_type: Ticket }
    - { id: human,    input_type: Ticket, output_type: Ticket }
  edges:
    - { from: classify, to: billing, case: billing }
    - { from: classify, to: tech,    case: tech }
    - { from: classify, to: human,   default: true }
  limits: { max_rounds: 8 }
  output_from: human
)YAML";
        auto parsed = yaml::parse(doc);
        std::optional<wf::Workflow> compiled;
        if (parsed) {
            if (auto c = wf::compile_workflow_document(*parsed)) compiled = std::move(*c);
        }
        check(compiled.has_value() && wf::validate_workflow(*compiled).has_value(),
              "W-7: the switch-with-default document parses, compiles and validates");

        using T = wf::TypedExecutor<Ticket, Ticket>;
        T const classify{.id = "classify", .capability_ceiling = {}};
        T const billing{.id = "billing", .capability_ceiling = {}};
        T const tech{.id = "tech", .capability_ceiling = {}};
        T const human{.id = "human", .capability_ceiling = {}};
        auto native = wf::WorkflowBuilder("triage")
                          .add(classify)
                          .add(billing)
                          .add(tech)
                          .add(human)
                          .connect_case(classify, billing, "billing")
                          .connect_case(classify, tech, "tech")
                          .connect_default(classify, human)
                          .start_at("classify")
                          .select_output("human")
                          .max_rounds(8)
                          .build();
        check(native.has_value() && compiled.has_value() && *native == *compiled,
              "W-7 -- CORE CLAIM (I6): the declarative switch-with-default and the native "
              "connect_case/connect_default graph are the SAME Workflow (operator==)");
        if (compiled.has_value()) {
            check(compiled->edges.size() == 3 && compiled->edges[2].kind == wf::edge_kind::switch_default &&
                      compiled->edges[2].case_label.empty() && compiled->edges[0].kind == wf::edge_kind::switch_case &&
                      compiled->edges[0].case_label == "billing",
                  "W-7: `case:` compiles to switch_case with its label, `default: true` to switch_default");
        }

        auto reject = [](char const* edge_line) {
            std::string d = std::string(
                "spec:\n  start: a\n  executors:\n    - { id: a, input_type: T, output_type: T }\n"
                "    - { id: b, input_type: T, output_type: T }\n  edges:\n") + edge_line +
                "\n  limits: { max_rounds: 2 }\n";
            auto p = yaml::parse(d);
            if (!p) return std::string("(did not parse)");
            auto c = wf::compile_workflow_document(*p);
            return c ? std::string("(compiled)") : c.error().code;
        };
        check(reject("    - { from: a, to: b, case: x, default: true }") == "yaml_compiler.case_and_default",
              "W-7: an edge with both `case` and `default` is refused");
        check(reject("    - { from: a, fan_out_to: [b], default: true }") == "yaml_compiler.switch_on_non_direct_edge",
              "W-7: `default` on a fan-out edge is refused, not ignored");
        check(reject("    - { from: a, to: b, default: false }") == "yaml_compiler.bad_edge_default",
              "W-7: `default: false` is refused (omit the key for a non-default edge)");
        check(reject("    - { from: a, to: b, case: [x] }") == "yaml_compiler.bad_edge_case",
              "W-7: a non-string `case` is refused");
    }

    if (g_failures == 0) {
        std::printf("test_workflow_yaml_compiler: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_yaml_compiler: %d failure(s)\n", g_failures);
    return 1;
}
