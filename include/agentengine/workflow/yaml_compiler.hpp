#pragma once
// Implements 015-Declarative-Agent-Format.md §3's Workflow document, compiling it to
// `workflow/graph.hpp`'s own layer-1 `Workflow` -- the SAME struct `WorkflowBuilder` (the C++
// authoring form) emits, checked by the SAME `validate_workflow()`, exactly the "declarative loader
// calls the same validator rather than reimplementing it" design `graph.hpp`'s own file-top comment
// states this milestone exists to fulfil. Milestone 7 Phase F2
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Input is a parsed `json::Value` (from `core/yaml_value.hpp`, Phase F1) shaped like 015 §3's own
// example -- `{apiVersion, kind, metadata: {id, version}, spec: {start, executors[], edges[], limits,
// output_from}}` -- not raw YAML text; this file owns the DOCUMENT-SHAPE compilation, text parsing is
// F1's own already-proven job.
//
// A REAL, HONEST FINDING from building this compiler, not papered over: 015 §3's own illustrative
// example document has NO `input_type`/`output_type` per executor -- but 014 §1's port-typing rule
// (enforced by `validate_workflow()`'s own `workflow.untyped_port` check) requires every executor to
// have both. The RFC's own example is illustrative shorthand, not a document `validate_workflow()`
// can accept as written. This compiler therefore reads `input_type`/`output_type` as EXTRA per-
// executor fields beyond what §3 shows -- a real document needs them, and this compiler faithfully
// surfaces `workflow.untyped_port` (via `validate_workflow()` itself, unchanged) for a document that
// omits them, rather than inventing a placeholder type to make an underspecified example "pass."
//
// Scope, matching what 015 §3's own example edges actually use: `to` (a direct edge), `fan_out_to`
// (a list -- one `edge_kind::fan_out` `Edge` per target, since `graph.hpp`'s own `Edge` struct is
// single-target; an N-target fan-out is N separate `Edge` records sharing one `from`, by design of
// that struct, not this compiler's own invention), `fan_in_to` (single target, `edge_kind::fan_in`).
// `switch_case`/`multi_selection`/`chain`/per-edge `on_failure` policy are NOT built -- 015 §3's own
// example never uses them, and extending the YAML shape to cover them is real, separate follow-up
// work, not a drive-by here.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

namespace yaml_compiler_detail {

namespace json = agentengine::json;

[[nodiscard]] inline result<void> fail(std::string message, std::string code) {
    return std::unexpected(error{failure_class::contract, std::move(message), std::move(code)});
}

[[nodiscard]] inline result<executor_kind> parse_executor_kind(std::string_view s) {
    if (s == "agent") return executor_kind::agent;
    if (s == "function") return executor_kind::function;
    if (s == "sub_workflow") return executor_kind::sub_workflow;
    if (s == "request_port") return executor_kind::request_port;
    return std::unexpected(
        error{failure_class::contract, "unrecognized executor kind: " + std::string(s),
              "yaml_compiler.unknown_executor_kind"});
}

[[nodiscard]] inline result<Executor> compile_executor(json::Value const& node) {
    json::Value const* id_field = node.find("id");
    if (!id_field || !id_field->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "an executor entry is missing a string \"id\"",
                  "yaml_compiler.missing_executor_id"});
    }
    Executor ex;
    ex.id = id_field->as_string();

    // §3's own example: an `agent:` reference implies executor_kind::agent when `kind` is not given
    // explicitly. An explicit `kind:` always wins (015 §4's own "strict" validation posture -- an
    // author's explicit statement is never silently overridden by an inference).
    if (json::Value const* kind_field = node.find("kind"); kind_field && kind_field->is_string()) {
        auto k = parse_executor_kind(kind_field->as_string());
        if (!k) return std::unexpected(k.error());
        ex.kind = *k;
    } else if (node.find("agent") != nullptr) {
        ex.kind = executor_kind::agent;
    }
    // `agent`/`prompt`/`concurrency` and similar §3 example fields are execution-layer configuration
    // (which agent to bind, a request-port's prompt text, fan-out width) -- `Executor` (graph.hpp) is
    // pure GRAPH SHAPE with no field for any of them; read here only for `kind` inference, otherwise
    // honestly dropped, not fabricated into a field the struct does not have.

    if (json::Value const* it = node.find("input_type"); it && it->is_string()) ex.input_type = it->as_string();
    if (json::Value const* ot = node.find("output_type"); ot && ot->is_string()) ex.output_type = ot->as_string();

    return ex;
}

// §3's own `deadline: 15m` shape -- a small, real duration parser: an integer followed by one of
// `ms`/`s`/`m`/`h`. Not a general ISO-8601 duration parser (out of scope, named not built).
[[nodiscard]] inline result<std::uint64_t> parse_duration_ms(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == 0) {
        return std::unexpected(error{failure_class::contract, "malformed duration: " + std::string(s),
                                      "yaml_compiler.bad_duration"});
    }
    std::uint64_t const n = std::stoull(std::string(s.substr(0, i)));
    std::string_view unit = s.substr(i);
    if (unit == "ms") return n;
    if (unit == "s") return n * 1000ULL;
    if (unit == "m") return n * 60ULL * 1000ULL;
    if (unit == "h") return n * 60ULL * 60ULL * 1000ULL;
    return std::unexpected(error{failure_class::contract,
                                  "unrecognized duration unit in: " + std::string(s),
                                  "yaml_compiler.bad_duration_unit"});
}

[[nodiscard]] inline result<std::vector<Edge>> compile_edge_entry(json::Value const& node) {
    json::Value const* from_field = node.find("from");
    if (!from_field || !from_field->is_string()) {
        return std::unexpected(error{failure_class::contract, "an edge entry is missing a string \"from\"",
                                      "yaml_compiler.missing_edge_from"});
    }
    std::string const from = from_field->as_string();

    std::vector<Edge> out;
    json::Value const* to_field       = node.find("to");
    json::Value const* fan_out_field  = node.find("fan_out_to");
    json::Value const* fan_in_field   = node.find("fan_in_to");
    int const forms_present = (to_field != nullptr) + (fan_out_field != nullptr) + (fan_in_field != nullptr);
    if (forms_present != 1) {
        return std::unexpected(
            error{failure_class::contract,
                  "edge from '" + from + "' must declare EXACTLY ONE of \"to\"/\"fan_out_to\"/\"fan_in_to\"",
                  "yaml_compiler.ambiguous_edge_form"});
    }

    if (to_field) {
        if (!to_field->is_string()) {
            return std::unexpected(error{failure_class::contract, "edge \"to\" must be a string",
                                          "yaml_compiler.bad_edge_to"});
        }
        Edge e;
        e.from = from;
        e.to   = to_field->as_string();
        e.kind = edge_kind::direct;
        out.push_back(std::move(e));
    } else if (fan_out_field) {
        if (!fan_out_field->is_array()) {
            return std::unexpected(error{failure_class::contract, "edge \"fan_out_to\" must be an array",
                                          "yaml_compiler.bad_fan_out_to"});
        }
        for (json::Value const& target : fan_out_field->as_array()) {
            if (!target.is_string()) {
                return std::unexpected(error{failure_class::contract,
                                              "a \"fan_out_to\" entry must be a string",
                                              "yaml_compiler.bad_fan_out_entry"});
            }
            Edge e;
            e.from = from;
            e.to   = target.as_string();
            e.kind = edge_kind::fan_out;
            out.push_back(std::move(e));
        }
    } else {
        if (!fan_in_field->is_string()) {
            return std::unexpected(error{failure_class::contract, "edge \"fan_in_to\" must be a string",
                                          "yaml_compiler.bad_fan_in_to"});
        }
        Edge e;
        e.from = from;
        e.to   = fan_in_field->as_string();
        e.kind = edge_kind::fan_in;
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace yaml_compiler_detail

// Compiles a parsed 015 §3 Workflow document into `graph.hpp`'s own `Workflow`, WITHOUT validating it
// -- call `validate_workflow()` (graph.hpp) on the result yourself, exactly as `WorkflowBuilder`'s own
// callers already do, so a compiled-but-invalid document surfaces the identical diagnostics a
// hand-written C++ workflow's own mistakes would (this compiler's whole reason to exist, I6).
[[nodiscard]] inline result<Workflow> compile_workflow_document(json::Value const& doc) {
    namespace jc = yaml_compiler_detail;
    namespace json = agentengine::json;

    json::Value const* metadata = doc.find("metadata");
    json::Value const* spec     = doc.find("spec");
    if (!spec) return std::unexpected(error{failure_class::contract, "document has no \"spec\"",
                                             "yaml_compiler.missing_spec"});

    Workflow wf;
    if (metadata) {
        if (json::Value const* id = metadata->find("id"); id && id->is_string()) wf.id = id->as_string();
        // §3's own metadata.version has no home in graph.hpp's Workflow struct (pure graph shape, no
        // document-identity/versioning field) -- read here for completeness, intentionally dropped,
        // the same honest "the struct doesn't have a slot for this yet" finding D2 already recorded
        // for AgentMetadata's own missing description/version fields.
    }

    if (json::Value const* start = spec->find("start"); start && start->is_string()) wf.start = start->as_string();

    if (json::Value const* executors = spec->find("executors"); executors && executors->is_array()) {
        for (json::Value const& node : executors->as_array()) {
            auto ex = jc::compile_executor(node);
            if (!ex) return std::unexpected(ex.error());
            wf.executors.push_back(std::move(*ex));
        }
    }

    if (json::Value const* edges = spec->find("edges"); edges && edges->is_array()) {
        for (json::Value const& node : edges->as_array()) {
            auto compiled = jc::compile_edge_entry(node);
            if (!compiled) return std::unexpected(compiled.error());
            for (auto& e : *compiled) wf.edges.push_back(std::move(e));
        }
    }

    if (json::Value const* limits = spec->find("limits")) {
        if (json::Value const* mr = limits->find("max_rounds"); mr && mr->is_number()) {
            wf.bound.max_rounds = static_cast<std::uint32_t>(mr->as_number());
        }
        if (json::Value const* tb = limits->find("token_budget"); tb && tb->is_number()) {
            wf.bound.token_budget = static_cast<std::uint64_t>(tb->as_number());
        }
        if (json::Value const* dl = limits->find("deadline"); dl && dl->is_string()) {
            auto ms = jc::parse_duration_ms(dl->as_string());
            if (!ms) return std::unexpected(ms.error());
            wf.bound.deadline_ms = *ms;
        }
    }

    if (json::Value const* output_from = spec->find("output_from"); output_from && output_from->is_string()) {
        wf.output_selection.push_back(output_from->as_string());
    }

    return wf;
}

}  // namespace agentengine::workflow
