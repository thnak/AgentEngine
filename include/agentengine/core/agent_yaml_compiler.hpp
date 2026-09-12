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
// `metadata.version`/`metadata.description` -- CLOSED (2026-08-14, decisions/ADR-044-*.md):
// `AgentMetadata` gained real `agent_description`/`agent_version` fields, closing the identical gap
// Phase D2 (Agent Card generation) and Phase F2 (the Workflow-document compiler) independently
// recorded for their own compiled targets. Three independent compilers hitting the same gap was the
// signal that `AgentMetadata` (002-owned) genuinely needed these fields -- it now does.
//
// `spec.tools` -- CLOSED for the name-keyed half (2026-08-14, decisions/ADR-054-*.md,
// docs/planning/tool-capability-registry-design-draft.md): an OPTIONAL `ToolRegistry const*` param,
// defaulting `nullptr` (the identical additive shape `register_agent<A>()`'s own `ChatClientRegistry
// const*` parameter established, Milestone 5 Phase B6) -- every existing single-argument call site
// keeps compiling and keeps its EXACT pre-existing behavior (`tools`/`capability_ceiling` stay
// honestly empty). With a registry supplied, each plain-string `spec.tools` entry resolves via
// `ToolTable::from_names()`, failing closed (compile-time, matching 002 §6's own fail-fast bar) on a
// name the registry doesn't have. `{handoff: ...}` entries (015 §2's own worked example has one) are
// NOT tool names -- 014's workflow/handoff graph owns that vocabulary, out of this compiler's scope,
// same as before this change; only plain-string entries are read. `spec.capabilities` (YAML capability
// parsing into real `cap::` variants) is a separate, still-open gap the design draft itself names
// (§4) -- `capability_ceiling` stays empty even with a registry supplied; only tool NAME resolution is
// closed here.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_registry.hpp"

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
[[nodiscard]] inline result<AgentMetadata> compile_agent_document(json::Value const& doc,
                                                                    ToolRegistry const* registry = nullptr) {
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
        // Gap-2 fix (2026-08-14, decisions/ADR-044-*.md): AgentMetadata now has real slots for both --
        // see that struct's own comment.
        if (json::Value const* description = metadata->find("description");
            description && description->is_string()) {
            meta.agent_description = description->as_string();
        }
        if (json::Value const* version = metadata->find("version"); version && version->is_string()) {
            meta.agent_version = version->as_string();
        }
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

    // I8 (budgets are enforced) is decided HERE for every declarative agent, so a bad value must be
    // refused rather than coerced -- the same stance this file already takes for an unrecognized
    // approval, concurrency or telemetry mode a few lines up.
    //
    // Both of these used to be a bare `static_cast` guarded only by `is_number()`. Every JSON number
    // is a double, and `static_cast<std::uint32_t>(-1.0)` is undefined ([conv.fpint]); on these
    // compilers it lands on 4294967295. So `max_turns: -1` -- which reads like "no limit" to anyone
    // writing YAML, and is the natural thing to try -- compiled to the LARGEST representable limit,
    // which `run_rounds()`'s `turn_index < *max_turns_` bound then treats as unbounded in practice.
    // A declared budget silently became no budget. `token_budget: -1` did the same at 64 bits.
    //
    // It broke I6 as well: the native surface takes `std::optional<std::uint64_t>`, in which a
    // negative limit cannot be spelled at all, so the declarative form accepted something its
    // supposedly equivalent C++ counterpart rejects at compile time.
    //
    // A present-but-non-numeric value is refused too, rather than silently left at the default. A
    // `limits:` block someone actually wrote and that quietly does nothing is the same failure in a
    // quieter register.
    if (json::Value const* limits = spec->find("limits")) {
        if (json::Value const* mt = limits->find("max_turns")) {
            auto bounded = json::as_bounded_integer(*mt, 0xFFFFFFFFull);
            if (!bounded.has_value()) {
                return std::unexpected(error{
                    failure_class::contract,
                    "limits.max_turns must be a whole number between 0 and 4294967295",
                    "agent_yaml_compiler.invalid_limit"});
            }
            meta.max_turns = static_cast<std::uint32_t>(*bounded);
        }
        if (json::Value const* tb = limits->find("token_budget")) {
            auto bounded = json::as_bounded_integer(*tb);
            if (!bounded.has_value()) {
                return std::unexpected(error{
                    failure_class::contract,
                    "limits.token_budget must be a whole, non-negative number",
                    "agent_yaml_compiler.invalid_limit"});
            }
            meta.token_budget = *bounded;
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

    // tools: honestly empty (unchanged) when no registry is supplied, matching every pre-existing
    // call site's exact behavior (F-1's own already-passing assertion). With one supplied, resolve
    // every plain-string spec.tools entry via ToolTable::from_names() -- fails closed (compile time)
    // the moment ANY named tool isn't in the registry, per 002 §6's own fail-fast bar.
    if (registry != nullptr) {
        if (json::Value const* tools = spec->find("tools"); tools && tools->is_array()) {
            std::vector<std::string> names;
            for (json::Value const& entry : tools->as_array()) {
                // `{handoff: ...}` entries are not tool names (014's workflow/handoff vocabulary, out
                // of scope here, unchanged from before this registry parameter existed) -- only a
                // plain string is a tool-name reference this compiler resolves.
                if (entry.is_string()) names.push_back(entry.as_string());
            }
            auto resolved = ToolTable::from_names(names, *registry);
            if (!resolved) return std::unexpected(resolved.error());
            meta.tools = std::move(*resolved);
        }
    }

    // capabilities / output_schema: see file-top comment for exactly why each stays at its honest
    // default rather than a fabricated value.
    return meta;
}

}  // namespace agentengine
