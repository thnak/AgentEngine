// Milestone 7 Phase F3 (015-Declarative-Agent-Format.md §2, docs/planning/milestone-7-protocol-
// conformance-breakdown.md). Proves `compile_agent_document()` (core/agent_yaml_compiler.hpp)
// against 015's own §2 example document, and -- the actual I6 property this compiler exists to
// serve -- against a REAL, hand-written C++ Agent compiled via `register_agent<A>()`, confirming
// every field this narrow slice covers agrees between the two forms.

#include <cstdio>
#include <string>

#include "agentengine/core/agent_yaml_compiler.hpp"
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

namespace ae   = agentengine;
namespace yaml = agentengine::yaml;

// The C++ authoring form of an agent EQUIVALENT to 015 §2's own example document, for the I6
// cross-check (F-2): same name, same instructions, same chat client id, same max_turns/token_budget/
// approval -- every field this narrow compiler slice actually covers.
struct ResearcherAgent
    : ae::Agent<ResearcherAgent, ae::ChatClientId<"anthropic:claude-opus-5">, ae::MaxTurns<12>,
                ae::TokenBudget<200000>, ae::Approval<ae::approval_mode::policy_driven>> {
    static constexpr std::string_view name         = "researcher";
    static constexpr std::string_view instructions = "Research the question. Cite sources.\n";
    // Gap-2 fix (2026-08-14, decisions/ADR-044-*.md): optional statics, detected via
    // agent_registry.hpp's has_agent_description/has_agent_version concepts.
    static constexpr std::string_view description = "Researches a question and cites sources.";
    static constexpr std::string_view version      = "1.4.0";
};

}  // namespace

int main() {
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

    // --- F-1: 015 §2's own example compiles, every covered field is real and correct ----------------
    ae::AgentMetadata compiled_meta;
    {
        auto parsed = yaml::parse(agent_doc);
        check(parsed.has_value(), "F-1: 015 §2's own example parses (reusing F1's parser)");
        if (parsed.has_value()) {
            auto compiled = ae::compile_agent_document(*parsed);
            check(compiled.has_value(), "F-1: the document compiles to a real AgentMetadata");
            if (compiled.has_value()) {
                compiled_meta = *compiled;
                check(compiled_meta.agent_name == "researcher", "F-1: agent_name <- metadata.id");
                check(compiled_meta.agent_instructions == "Research the question. Cite sources.\n",
                      "F-1: agent_instructions <- spec.instructions (block literal)");
                check(compiled_meta.chat_client_id == "anthropic:claude-opus-5",
                      "F-1: chat_client_id <- spec.provider.model");
                check(compiled_meta.max_turns == 12, "F-1: max_turns <- spec.limits.max_turns");
                check(compiled_meta.token_budget == std::optional<std::uint64_t>{200000},
                      "F-1: token_budget <- spec.limits.token_budget");
                check(compiled_meta.approval == ae::approval_mode::policy_driven,
                      "F-1: approval <- spec.approval");
                check(compiled_meta.telemetry == ae::telemetry_capture::metadata_only,
                      "F-1: telemetry <- spec.telemetry.capture");
                check(compiled_meta.sandbox_profile.is_strict == true,
                      "F-1: sandbox_profile.is_strict <- spec.sandbox.profile == \"strict\"");
                check(compiled_meta.tools.descriptors().empty(),
                      "F-1: tools is HONESTLY empty -- no tool-name registry and no runtime ToolTable "
                      "builder exist yet, named not fabricated");
                check(compiled_meta.capability_ceiling.empty(),
                      "F-1: capability_ceiling is HONESTLY empty for the identical reason");
                check(!compiled_meta.output_schema_json.has_value(),
                      "F-1: output_schema_json stays unset -- the document only supplies a $ref to an "
                      "external file, which this compiler does not resolve");
                check(compiled_meta.agent_description == "Researches a question and cites sources.",
                      "F-1: agent_description <- metadata.description");
                check(compiled_meta.agent_version == std::optional<std::string>{"1.4.0"},
                      "F-1: agent_version <- metadata.version");
            }
        }
    }

    // --- F-2: I6 IN ACTION -- the SAME fields, compiled from the REAL C++ authoring form via -------
    // --- register_agent<A>(), agree with the YAML-compiled metadata.                              ---
    {
        auto cpp_meta = ae::register_agent<ResearcherAgent>();
        check(cpp_meta.has_value(), "F-2: the equivalent hand-written C++ agent registers cleanly");
        if (cpp_meta.has_value()) {
            check(cpp_meta->agent_name == compiled_meta.agent_name,
                  "F-2: agent_name agrees between the YAML and C++ forms");
            check(cpp_meta->agent_instructions == compiled_meta.agent_instructions,
                  "F-2: agent_instructions agrees");
            check(cpp_meta->chat_client_id == compiled_meta.chat_client_id,
                  "F-2: chat_client_id agrees");
            check(cpp_meta->max_turns == compiled_meta.max_turns, "F-2: max_turns agrees");
            check(cpp_meta->token_budget == compiled_meta.token_budget, "F-2: token_budget agrees");
            check(cpp_meta->approval == compiled_meta.approval,
                  "F-2: approval agrees -- I6: two independently-compiled forms of the same agent "
                  "produce identical metadata for every field this compiler covers");
            check(cpp_meta->agent_description == compiled_meta.agent_description,
                  "F-2: agent_description agrees");
            check(cpp_meta->agent_version == compiled_meta.agent_version, "F-2: agent_version agrees");
        }
    }

    // --- F-3: enum mappings -- every value, not just the one 015 §2's own example happens to use ----
    {
        auto make_doc = [](std::string field, std::string value) {
            return "spec:\n  " + field + ": " + value + "\n";
        };
        auto never = ae::compile_agent_document(*yaml::parse(make_doc("approval", "never_require")));
        auto always = ae::compile_agent_document(*yaml::parse(make_doc("approval", "always_require")));
        check(never.has_value() && never->approval == ae::approval_mode::never_require,
              "F-3: approval: never_require compiles correctly");
        check(always.has_value() && always->approval == ae::approval_mode::always_require,
              "F-3: approval: always_require compiles correctly");

        auto parallel = ae::compile_agent_document(*yaml::parse(make_doc("concurrency", "parallel")));
        check(parallel.has_value() && parallel->concurrency == ae::concurrency_mode::parallel,
              "F-3: concurrency: parallel compiles correctly");

        auto bad = ae::compile_agent_document(*yaml::parse(make_doc("approval", "not_a_real_mode")));
        check(!bad.has_value() && bad.error().code == "agent_yaml_compiler.unknown_approval_mode",
              "F-3: an unrecognized approval mode is rejected, never silently coerced to a default");
    }

    // --- F-4: absent fields leave AgentMetadata's own real defaults untouched -----------------------
    {
        auto minimal = ae::compile_agent_document(*yaml::parse("spec: {}\n"));
        check(minimal.has_value(), "F-4: a minimal document with an empty spec still compiles");
        if (minimal.has_value()) {
            ae::AgentMetadata const defaults;  // the struct's own real defaults, not asserted values
            check(minimal->max_turns == defaults.max_turns && minimal->approval == defaults.approval &&
                      minimal->concurrency == defaults.concurrency &&
                      minimal->telemetry == defaults.telemetry,
                  "F-4: every unset field carries AgentMetadata's own real default, never an "
                  "independently-invented one");
        }
    }

    // --- F-5: a document with no \"spec\" is rejected -------------------------------------------------
    {
        auto no_spec = ae::compile_agent_document(*yaml::parse("metadata: { id: x }\n"));
        check(!no_spec.has_value() && no_spec.error().code == "agent_yaml_compiler.missing_spec",
              "F-5: a document with no \"spec\" is rejected with the real missing_spec code");
    }

    if (g_failures == 0) {
        std::printf("test_agent_yaml_compiler: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_yaml_compiler: %d failure(s)\n", g_failures);
    return 1;
}
