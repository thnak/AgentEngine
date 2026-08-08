#pragma once
// Implements 015-Declarative-Agent-Format.md §2's Agent document, compiling it to
// `core/agent_registry.hpp`'s own `AgentMetadata` -- the SAME compiled-metadata type
// `register_agent<A>()` (the C++ authoring form) produces, the actual I6 property this compiler
// exists to serve. Milestone 7 Phase F3 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// TWO REAL, BLOCKING GAPS confirmed before this file was written (2026-08-08), both named here rather
// than worked around: (1) `spec.tools`/`spec.capabilities` reference tools/capabilities BY NAME
// (015 §1: "the declarative form references tools by name from the registry (native, plugin, MCP,
// A2A)") -- no such NAME-KEYED REGISTRY exists anywhere in this codebase (confirmed by search); (2)
// even given resolved `ToolDescriptor`s, `core/tool_pipeline.hpp`'s own `ToolTable` has NO runtime
// construction API at all -- `from_tools<ToolTs...>()` is a compile-time template over REAL C++ `Tool`
// types, and `descriptors_` is private with no public append/builder surface. Building either is real,
// separate infrastructure work (a tool/capability registry is 006/009's own scope, not 015's; a
// runtime `ToolTable` builder is a `core/tool_pipeline.hpp` API change), not a drive-by inside an
// "Agent document → metadata" compiler.
//
// This compiler therefore produces a REAL `AgentMetadata` for every field NOT blocked by the above:
// `agent_name`, `agent_instructions`, `chat_client_id`, `max_turns`, `token_budget`, `approval`,
// `concurrency`, `telemetry`, `sandbox_profile.is_strict`. `tools` is left as the honestly EMPTY
// default `ToolTable` (default-constructible, real, just empty); `capability_ceiling` stays empty
// too (`spec.capabilities` needs the same missing registry `spec.tools` does, PLUS real per-kind
// `cap::` variant construction from YAML params -- its own scoped follow-up); `output_schema_json`
// stays unset (`spec.output_schema: {$ref: ...}` in 015's own example is a REFERENCE to an external
// schema FILE, and resolving/loading/compiling one is real file-IO work this compiler does not take
// on, distinct from the tools/capabilities registry gap). `output_schema_strategy_chosen` is therefore
// also never set (it depends on `output_schema_json` existing, `agent_registry.hpp`'s own rule).
//
// `metadata.version`/`metadata.description` have no home in `AgentMetadata`'s own current shape --
// read from the document, then honestly dropped, the SAME finding Phase D2 (Agent Card generation)
// and Phase F2 (the Workflow-document compiler) already recorded independently for their own compiled
// targets. Three independent compilers hitting the identical gap is itself a signal `AgentMetadata`
// (002-owned) genuinely needs these fields at some point -- not something this file's own authority
// extends to fixing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine {

namespace agent_yaml_compiler_detail {

[[nodiscard]] inline result<approval_mode> parse_approval_mode(std::string_view s) {
    if (s == "never_require") return approval_mode::never_require;
    if (s == "always_require") return approval_mode::always_require;
    if (s == "policy_driven") return approval_mode::policy_driven;
    return std::unexpected(error{failure_class::contract, "unrecognized approval mode: " + std::string(s),
                                  "agent_yaml_compiler.unknown_approval_mode"});
}

[[nodiscard]] inline result<concurrency_mode> parse_concurrency_mode(std::string_view s) {
    if (s == "sequential") return concurrency_mode::sequential;
    if (s == "parallel") return concurrency_mode::parallel;
    return std::unexpected(error{failure_class::contract,
                                  "unrecognized concurrency mode: " + std::string(s),
                                  "agent_yaml_compiler.unknown_concurrency_mode"});
}

[[nodiscard]] inline result<telemetry_capture> parse_telemetry_capture(std::string_view s) {
    if (s == "none") return telemetry_capture::none;
    if (s == "metadata_only") return telemetry_capture::metadata_only;
    if (s == "full") return telemetry_capture::full;
    return std::unexpected(error{failure_class::contract,
                                  "unrecognized telemetry capture level: " + std::string(s),
                                  "agent_yaml_compiler.unknown_telemetry_capture"});
}

}  // namespace agent_yaml_compiler_detail

// Compiles a parsed 015 §2 Agent document into a REAL `AgentMetadata`, scoped exactly as this file's
// own top comment names -- `tools`/`capability_ceiling` stay empty, `output_schema_json` stays unset,
// every other field is real. Does NOT itself validate the result against 002 §6's own rules (an empty
// `chat_client_id`, say) -- a caller wanting that runs the SAME checks `register_agent<A>()` does,
// exactly like `compile_workflow_document()` (Phase F2) leaves `validate_workflow()` to its own
// caller rather than folding validation into compilation.
[[nodiscard]] inline result<AgentMetadata> compile_agent_document(json::Value const& doc) {
    namespace ac = agent_yaml_compiler_detail;
    namespace json = agentengine::json;

    json::Value const* metadata = doc.find("metadata");
    json::Value const* spec     = doc.find("spec");
    if (!spec) {
        return std::unexpected(
            error{failure_class::contract, "document has no \"spec\"", "agent_yaml_compiler.missing_spec"});
    }

    AgentMetadata meta;

    if (metadata) {
        if (json::Value const* id = metadata->find("id"); id && id->is_string()) meta.agent_name = id->as_string();
        // metadata.version/description: read, then honestly dropped -- see file-top comment.
    }

    if (json::Value const* instructions = spec->find("instructions"); instructions && instructions->is_string()) {
        meta.agent_instructions = instructions->as_string();
    }

    if (json::Value const* provider = spec->find("provider")) {
        if (json::Value const* model = provider->find("model"); model && model->is_string()) {
            meta.chat_client_id = model->as_string();
        }
        // provider.options (e.g. temperature): no home in AgentMetadata -- these are per-CALL
        // ChatRequest-time parameters, not compile-time agent metadata; the C++ authoring form has
        // no equivalent policy tag for them either, so this is not a declarative-form-only gap.
    }

    if (json::Value const* limits = spec->find("limits")) {
        if (json::Value const* mt = limits->find("max_turns"); mt && mt->is_number()) {
            meta.max_turns = static_cast<std::uint32_t>(mt->as_number());
        }
        if (json::Value const* tb = limits->find("token_budget"); tb && tb->is_number()) {
            meta.token_budget = static_cast<std::uint64_t>(tb->as_number());
        }
    }

    if (json::Value const* approval = spec->find("approval"); approval && approval->is_string()) {
        auto a = ac::parse_approval_mode(approval->as_string());
        if (!a) return std::unexpected(a.error());
        meta.approval = *a;
    }

    if (json::Value const* concurrency = spec->find("concurrency"); concurrency && concurrency->is_string()) {
        auto c = ac::parse_concurrency_mode(concurrency->as_string());
        if (!c) return std::unexpected(c.error());
        meta.concurrency = *c;
    }

    if (json::Value const* telemetry = spec->find("telemetry")) {
        if (json::Value const* capture = telemetry->find("capture"); capture && capture->is_string()) {
            auto t = ac::parse_telemetry_capture(capture->as_string());
            if (!t) return std::unexpected(t.error());
            meta.telemetry = *t;
        }
    }

    if (json::Value const* sandbox = spec->find("sandbox")) {
        if (json::Value const* profile = sandbox->find("profile"); profile && profile->is_string()) {
            // 015 §2's own example uses "strict" -- any other declared profile name maps to
            // is_strict=false, with `traits` left at its own default (015 §2's own example never
            // exercises a non-strict profile's traits; mapping those is real, separate follow-up).
            meta.sandbox_profile.is_strict = (profile->as_string() == "strict");
        }
        // sandbox.fallback: no home in SandboxProfileDescriptor (is_strict + traits only) -- the
        // fallback CHAIN concept has no compiled-metadata slot yet, read here for nothing, honestly.
    }

    // tools / capabilities / output_schema: see file-top comment for exactly why each stays at its
    // honest default rather than a fabricated value.
    return meta;
}

}  // namespace agentengine
