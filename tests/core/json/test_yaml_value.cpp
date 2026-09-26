// Milestone 7 Phase F1 (015-Declarative-Agent-Format.md §2/§3, docs/planning/milestone-7-protocol-
// conformance-breakdown.md). Proves the hand-rolled YAML-subset parser (core/yaml_value.hpp) --
// primarily against the RFC's OWN §2 Agent / §3 Workflow example documents, verbatim, since that is
// the concrete bar this subset must clear, plus targeted unit checks for each supported construct
// and a negative suite for what this subset deliberately rejects.

#include <cstdio>
#include <string>

#include "agentengine/core/yaml_value.hpp"

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

}  // namespace

int main() {
    // --- Y-1: a flat mapping -------------------------------------------------------------------------
    {
        auto v = yaml::parse("a: 1\nb: two\nc: true\n");
        check(v.has_value(), "Y-1: a flat mapping parses");
        if (v.has_value()) {
            check(v->find("a")->as_number() == 1 && v->find("b")->as_string() == "two" &&
                      v->find("c")->as_bool() == true,
                  "Y-1: number/string/bool scalars resolve correctly");
        }
    }

    // --- Y-2: a nested mapping --------------------------------------------------------------------------
    {
        auto v = yaml::parse("outer:\n  inner: value\n  another: 5\n");
        check(v.has_value(), "Y-2: a nested block mapping parses");
        if (v.has_value()) {
            auto const* outer = v->find("outer");
            check(outer && outer->is_object() && outer->find("inner")->as_string() == "value" &&
                      outer->find("another")->as_number() == 5,
                  "Y-2: nested mapping values are correct");
        }
    }

    // --- Y-3: a block sequence of plain scalars --------------------------------------------------------
    {
        auto v = yaml::parse("items:\n  - one\n  - two\n  - three\n");
        check(v.has_value(), "Y-3: a block sequence of scalars parses");
        if (v.has_value()) {
            auto const* items = v->find("items");
            check(items && items->is_array() && items->as_array().size() == 3 &&
                      items->as_array()[0].as_string() == "one" &&
                      items->as_array()[2].as_string() == "three",
                  "Y-3: sequence items and order are correct");
        }
    }

    // --- Y-4: a sequence of mappings (015 §3's own executors/edges shape) ------------------------------
    {
        auto v = yaml::parse("executors:\n  - id: planner\n    agent: researcher\n  - id: search\n"
                              "    agent: researcher\n    concurrency: 4\n");
        check(v.has_value(), "Y-4: a sequence of inline mappings parses (\"- key: value\" + continuation)");
        if (v.has_value()) {
            auto const& arr = v->find("executors")->as_array();
            check(arr.size() == 2, "Y-4: two executor entries");
            if (arr.size() == 2) {
                check(arr[0].find("id")->as_string() == "planner" &&
                          arr[0].find("agent")->as_string() == "researcher",
                      "Y-4: the first entry's inline key plus its continuation key are both present");
                check(arr[1].find("id")->as_string() == "search" &&
                          arr[1].find("concurrency")->as_number() == 4,
                      "Y-4: the second entry's THREE keys (inline + two continuations) are all present");
            }
        }
    }

    // --- Y-5: flow-style mapping and sequence (015 §2's own "options: { temperature: 0.2 }" shape) ----
    {
        auto v = yaml::parse("options: { temperature: 0.2, top_p: 0.9 }\nfallback: [native-jail, remote]\n");
        check(v.has_value(), "Y-5: flow-style collections parse");
        if (v.has_value()) {
            auto const* options = v->find("options");
            check(options && options->is_object() && options->find("temperature")->as_number() == 0.2,
                  "Y-5: an UNQUOTED flow-mapping key/value (temperature: 0.2) resolves -- real YAML, "
                  "not strict JSON, which would require quoted keys");
            auto const* fallback = v->find("fallback");
            check(fallback && fallback->is_array() && fallback->as_array().size() == 2 &&
                      fallback->as_array()[0].as_string() == "native-jail",
                  "Y-5: a flow sequence of unquoted strings parses");
        }
    }

    // --- Y-6: block literal scalar (015 §2's own multi-line "instructions: |" shape) -------------------
    {
        auto v = yaml::parse("instructions: |\n  Research the question.\n  Cite sources.\nother: x\n");
        check(v.has_value(), "Y-6: a block literal scalar parses");
        if (v.has_value()) {
            auto const* instr = v->find("instructions");
            check(instr && instr->is_string() &&
                      instr->as_string() == "Research the question.\nCite sources.\n",
                  "Y-6: the block literal's lines are joined with \\n, trailing newline clipped-default, "
                  "and parsing correctly resumes at the next sibling key (\"other\")");
            check(v->find("other")->as_string() == "x", "Y-6: the key after the block literal is reached");
        }
    }

    // --- Y-7: quoted strings, single and double, including escapes --------------------------------------
    {
        auto v = yaml::parse("a: \"line one\\nline two\"\nb: 'it''s here'\n");
        check(v.has_value(), "Y-7: quoted scalars parse");
        if (v.has_value()) {
            check(v->find("a")->as_string() == "line one\nline two",
                  "Y-7: a double-quoted string's \\n escape is a real newline");
            check(v->find("b")->as_string() == "it's here",
                  "Y-7: a single-quoted string's doubled '' is a literal single quote");
        }
    }

    // --- Y-8: comments and blank lines are ignored -------------------------------------------------------
    {
        auto v = yaml::parse("# a leading comment\na: 1  # trailing comment\n\nb: 2\n");
        check(v.has_value(), "Y-8: comments/blank lines parse without affecting structure");
        if (v.has_value()) {
            check(v->find("a")->as_number() == 1 && v->find("b")->as_number() == 2,
                  "Y-8: both keys resolve, comment text never leaks into a value");
        }
    }

    // --- Y-9: null/~ and the RFC's own full §2 Agent document, verbatim ----------------------------------
    {
        std::string const agent_doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Agent
metadata:
  id: researcher
  version: 1.4.0
  description: Researches a question and cites sources.
spec:
  provider:
    model: anthropic:claude-opus-5
    options: { temperature: 0.2 }
  instructions: |
    Research the question. Cite sources.
  tools:
    - web_search
    - code_interpreter
    - handoff: writer
  capabilities:
    net_out: ["api.search.example"]
  sandbox:
    profile: strict
    fallback: [native-jail, remote]
  limits:
    max_turns: 12
    token_budget: 200000
  approval: policy_driven
  memory:
    - kind: semantic
      store: project-docs
  middleware: [redact_pii]
  telemetry: { capture: metadata_only }
  output_schema: { $ref: "./schemas/research-result.json" }
)YAML";
        auto v = yaml::parse(agent_doc);
        check(v.has_value(), "Y-9: 015 §2's own Agent example document parses, verbatim, no modifications");
        if (v.has_value()) {
            check(v->find("apiVersion")->as_string() == "agentengine.dev/v1" &&
                      v->find("kind")->as_string() == "Agent",
                  "Y-9: top-level scalars");
            auto const* metadata = v->find("metadata");
            check(metadata && metadata->find("id")->as_string() == "researcher" &&
                      metadata->find("version")->as_string() == "1.4.0",
                  "Y-9: metadata.id/version");
            auto const* spec = v->find("spec");
            check(spec, "Y-9: spec block present");
            if (spec) {
                check(spec->find("provider")->find("model")->as_string() == "anthropic:claude-opus-5",
                      "Y-9: spec.provider.model (nested block-then-scalar)");
                check(spec->find("provider")->find("options")->find("temperature")->as_number() == 0.2,
                      "Y-9: spec.provider.options.temperature (flow mapping nested in a block mapping)");
                check(spec->find("instructions")->as_string() ==
                          "Research the question. Cite sources.\n",
                      "Y-9: spec.instructions (block literal)");
                auto const& tools = spec->find("tools")->as_array();
                check(tools.size() == 3 && tools[0].as_string() == "web_search" &&
                          tools[2].find("handoff")->as_string() == "writer",
                      "Y-9: spec.tools -- a mixed sequence of plain scalars AND an inline mapping item "
                      "(\"- handoff: writer\")");
                check(spec->find("capabilities")->find("net_out")->as_array()[0].as_string() ==
                          "api.search.example",
                      "Y-9: spec.capabilities.net_out (flow sequence of a quoted string)");
                check(spec->find("sandbox")->find("fallback")->as_array().size() == 2,
                      "Y-9: spec.sandbox.fallback (flow sequence of unquoted strings)");
                check(spec->find("limits")->find("token_budget")->as_number() == 200000,
                      "Y-9: spec.limits.token_budget");
                check(spec->find("memory")->as_array()[0].find("kind")->as_string() == "semantic",
                      "Y-9: spec.memory -- a sequence of inline mapping items");
                check(spec->find("output_schema")->find("$ref")->as_string() ==
                          "./schemas/research-result.json",
                      "Y-9: spec.output_schema (a flow mapping with a $-prefixed key)");
            }
        }
    }

    // --- Y-10: the RFC's own full §3 Workflow document, verbatim ------------------------------------------
    {
        std::string const workflow_doc = R"YAML(apiVersion: agentengine.dev/v1
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
        auto v = yaml::parse(workflow_doc);
        check(v.has_value(), "Y-10: 015 §3's own Workflow example document parses, verbatim");
        if (v.has_value()) {
            auto const* metadata = v->find("metadata");
            check(metadata && metadata->find("id")->as_string() == "research-and-write",
                  "Y-10: metadata (an entirely flow-style mapping, single line)");
            auto const* spec = v->find("spec");
            check(spec && spec->find("start")->as_string() == "planner", "Y-10: spec.start");
            if (spec) {
                auto const& executors = spec->find("executors")->as_array();
                check(executors.size() == 4 && executors[0].find("id")->as_string() == "planner" &&
                          executors[2].find("prompt")->as_string() == "Approve the outline?",
                      "Y-10: spec.executors -- a block sequence of FLOW-style mapping items");
                auto const& edges = spec->find("edges")->as_array();
                check(edges.size() == 3 &&
                          edges[0].find("fan_out_to")->as_array()[0].as_string() == "search",
                      "Y-10: spec.edges -- flow mappings containing a nested flow sequence");
                check(spec->find("limits")->find("max_rounds")->as_number() == 20,
                      "Y-10: spec.limits (flow mapping)");
                check(spec->find("output_from")->as_string() == "writer", "Y-10: spec.output_from");
            }
        }
    }

    // --- Y-11: negative -- a tab in indentation is rejected, not silently accepted -----------------------
    {
        auto v = yaml::parse("a:\n\tb: 1\n");
        check(!v.has_value(), "Y-11: a tab character in indentation is rejected");
        if (!v.has_value()) {
            check(v.error().code == "yaml.parse_error", "Y-11: rejected with the real yaml.parse_error code");
        }
    }

    // --- Y-12: negative -- an unterminated quoted string is rejected -------------------------------------
    {
        auto v = yaml::parse("a: \"unterminated\n");
        check(!v.has_value(), "Y-12: an unterminated double-quoted string is rejected");
    }

    // --- Y-13: negative -- an unterminated flow collection is rejected -----------------------------------
    {
        auto v = yaml::parse("a: [1, 2, 3\n");
        check(!v.has_value(), "Y-13: an unclosed flow sequence is rejected");
    }

    // --- Y-14: null / ~ resolve to a real JSON null --------------------------------------------------------
    {
        auto v = yaml::parse("a: null\nb: ~\n");
        check(v.has_value() && v->find("a")->is_null() && v->find("b")->is_null(),
              "Y-14: both \"null\" and \"~\" resolve to a real JSON null");
    }

    if (g_failures == 0) {
        std::printf("test_yaml_value: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_yaml_value: %d failure(s)\n", g_failures);
    return 1;
}
