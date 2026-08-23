#pragma once
// Implements 002-Agent-Model-and-Authoring.md §6 — register_agent<A>(), the real metadata compiler.
// M2 Phase E tasks E2 and E3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// E2 is register_agent<A>() itself; E3 is invoke_agent_tool() at the bottom of this file, the
// milestone's headline exit-criterion sentence wired to real code (see that function's own comment).
//
// Of §6's 8 named validation checks, six now run for real: tool name collision, capability-ceiling
// coverage, Stateless<N>-vs-session-state via std::is_empty_v, SandboxProfile's compile-time shape
// (decisions/ADR-012-sandbox-profile-template-parameter-kind.md), a basic shape check (ChatClientId
// presence), and — Milestone 5 Phase B6 (docs/planning/milestone-5-providers-identity-secrets-
// breakdown.md) — ChatClientId credentials/OutputSchema enforceability, ADDITIVELY: an optional
// `ChatClientRegistry const*` parameter on `register_agent<A>()` (default `nullptr`, so every
// existing zero-arg call site is unaffected). With no registry supplied, both checks stay the
// pre-M5 always-pass stub (still an honest "not evaluated," not a silent pass dressed up as
// enforcement); with one supplied, they run for real against the registry's declared capabilities.
// Two remain stubbed to always-pass, comment naming exactly what machinery is missing — not
// silently skipped:
//   - SandboxProfile *tool/profile backend mismatch* (002 §6's second SandboxProfile bullet) — needs
//     a per-tool backend-declaration policy tag that does not exist yet (`Tool<Derived, Policies...>`
//     has no analog of `SandboxProfile<P>`); ADR-012 resolved the *template-parameter-kind* conflict
//     (below) but deliberately did not invent a second, larger policy-tag surface as a drive-by.
//   - Handoff cycle detection — needs 014's workflow/handoff graph, explicitly out of scope for M2.
//
// SandboxProfile's own history: 002 §2's worked example instantiated it with an enum-like value
// (`Profile::Strict`, itself an undefined symbol — no `Profile` type existed anywhere); 008 §2a said
// P is "any type satisfying the SandboxBackend concept" (open extensibility, custom backends); this
// file's existing `SandboxProfile<sandbox_profile P>` (an enum NTTP) picked a reading that couldn't
// represent 008 §2a's custom-backend case at all — a real spec-vs-spec conflict discovered while
// implementing E2, resolved by ADR-012 (`SandboxProfile<P>` now takes any type satisfying the new
// `SandboxProfileArg` concept — a real `SandboxBackend`, or `Strict`, sandbox/sandbox.hpp's renamed,
// now-real resolution selector) rather than picked unilaterally inline, per CLAUDE.md.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/fixed_string.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// The compiled, read-only agent metadata table (002 §1). Built once by `register_agent<A>()`;
// nothing here is a runtime configuration object read on a hot path (CONVENTIONS.md).
struct AgentMetadata {  // ae-naming-lint: allow AgentMetadata — pre-existing M0 scaffolding pattern for this milestone's new types, reconcile at owning milestone (matches ToolDescriptor/ToolTable's own deferred status, core/tool_pipeline.hpp)
    std::string agent_name;
    std::string agent_instructions;
    // Gap-2 fix (2026-08-14, decisions/ADR-044-*.md): 002 §1/§7's own identity list
    // ("stable agent_id, name, description, version") named these two from the start; this struct
    // simply never grew them. `agent_description` defaults empty (matching `agent_name`'s own "not
    // declared" convention); `agent_version` is `optional`, not a defaulted empty string, because
    // "no version declared" and "declared as the empty string" are genuinely different states a
    // consumer (an A2A Agent Card, an MCP server descriptor) needs to tell apart.
    std::string agent_description;
    std::optional<std::string> agent_version;
    std::string chat_client_id;
    ToolTable tools;
    std::vector<Capability> capability_ceiling;
    std::uint32_t max_turns = 16;                  // 002 §3 table default
    std::optional<std::uint64_t> token_budget;      // nullopt = unbounded (002 §3 table default)
    approval_mode approval = approval_mode::policy_driven;      // 002 §3 table default
    concurrency_mode concurrency = concurrency_mode::sequential; // 002 §3 table default
    telemetry_capture telemetry = telemetry_capture::metadata_only; // 002 §3 table default
    std::optional<std::uint32_t> stateless_pool_size;  // nullopt = off (002 §3 table default)
    SandboxProfileDescriptor sandbox_profile;          // is_strict=true default (002 §3 table default)
    // Milestone 5 Phase B5: 003 §4's OutputSchema<T> contract, compiled to JSON Schema text; nullopt
    // when the agent declared no OutputSchema<T>. `output_schema_strategy` is nullopt whenever
    // `output_schema_json` is nullopt (nothing to enforce) OR no `ChatClientRegistry` was supplied
    // to `register_agent<A>()` (nothing to enforce it against yet) — an honest "not evaluated," not
    // a silent default.
    std::optional<std::string> output_schema_json;
    std::optional<output_schema_strategy> output_schema_strategy_chosen;
};

namespace agent_detail {

// -- per-tag extraction (mirrors core/tool.hpp's tool_detail pattern: primary default + one
// specialization per tag, folded over the Policies pack) --------------------------------------

template <class Policy>
struct policy_chat_client_id {
    static std::string_view get() { return {}; }
};
template <fixed_string Id>
struct policy_chat_client_id<ChatClientId<Id>> {
    static std::string_view get() { return ChatClientId<Id>::id; }
};

template <class Policy>
struct policy_max_turns {
    static std::optional<std::uint32_t> get() { return std::nullopt; }
};
template <std::uint32_t N>
struct policy_max_turns<MaxTurns<N>> {
    static std::optional<std::uint32_t> get() { return N; }
};

template <class Policy>
struct policy_token_budget {
    static std::optional<std::uint64_t> get() { return std::nullopt; }
};
template <std::uint64_t N>
struct policy_token_budget<TokenBudget<N>> {
    static std::optional<std::uint64_t> get() { return N; }
};

template <class Policy>
struct policy_concurrency {
    static std::optional<concurrency_mode> get() { return std::nullopt; }
};
template <concurrency_mode M>
struct policy_concurrency<Concurrency<M>> {
    static std::optional<concurrency_mode> get() { return M; }
};

template <class Policy>
struct policy_telemetry {
    static std::optional<telemetry_capture> get() { return std::nullopt; }
};
template <telemetry_capture C>
struct policy_telemetry<Telemetry<C>> {
    static std::optional<telemetry_capture> get() { return C; }
};

template <class Policy>
struct policy_stateless {
    static std::optional<std::uint32_t> get() { return std::nullopt; }
};
template <std::uint32_t N>
struct policy_stateless<Stateless<N>> {
    static std::optional<std::uint32_t> get() { return N; }
};

// Milestone 5 Phase B5: OutputSchema<T> (core/agent.hpp) compiled to its JSON Schema text, the same
// mechanism ToolDescriptor::args_schema_json already uses (core/tool_pipeline.hpp) — only
// instantiated when an agent actually declares OutputSchema<T>, so a T missing its own
// AE_JSON_SCHEMA(...) description fails to compile exactly there, the same enforcement 006's own
// tool-argument types already get.
template <class Policy>
struct policy_output_schema {
    static std::optional<std::string> get() { return std::nullopt; }
};
template <class T>
struct policy_output_schema<OutputSchema<T>> {
    static std::optional<std::string> get() { return schema::json_schema_of<T>(); }
};

// ADR-012: `SandboxProfile<P>` compiles `P` (a real `SandboxBackend`, or `Strict`) into a runtime-
// readable `SandboxProfileDescriptor` -- `P` itself cannot survive past compile time (it is a type,
// not a value), so this is where that information gets turned into data.
template <class Policy>
struct policy_sandbox_profile {
    static std::optional<SandboxProfileDescriptor> get() { return std::nullopt; }
};
template <SandboxProfileArg P>
struct policy_sandbox_profile<SandboxProfile<P>> {
    static std::optional<SandboxProfileDescriptor> get() {
        if constexpr (std::same_as<P, Strict>) {
            return SandboxProfileDescriptor{.is_strict = true, .traits = {}};
        } else {
            return SandboxProfileDescriptor{.is_strict = false, .traits = P::traits};
        }
    }
};

// `Tools<Ts...>` needs the pack itself, not just a runtime-readable value -- ToolTable::from_tools
// is a template. Standard recursive pack search: the head-matches specialization wins when it's
// `Tools<Ts...>`, otherwise recurse into the tail; the primary template is the empty/not-found base
// case.
template <class... Policies>
struct tools_of {
    [[nodiscard]] static ToolTable table() { return ToolTable::from_tools<>(); }
};
template <class... Ts, class... Rest>
struct tools_of<Tools<Ts...>, Rest...> {
    [[nodiscard]] static ToolTable table() { return ToolTable::from_tools<Ts...>(); }
};
template <class P, class... Rest>
struct tools_of<P, Rest...> {
    [[nodiscard]] static ToolTable table() { return tools_of<Rest...>::table(); }
};

// Every `*_of<Policies...>()` below shares one idiom: a lambda invoked once per policy by a comma
// fold, written INSIDE the fold pattern rather than declared first and invoked after. The two forms
// run identically for a non-empty pack; the difference is the EMPTY one. `Agent<>` with no policy
// tags expands the fold to nothing, which left the previously-named `consider`/`append_one` local
// declared and never called -- gcc's -Wunused-but-set-variable, 14 times across this file and
// tool.hpp. Naming nothing means there is nothing to be unused, so the warning is gone by
// construction instead of by annotation. The empty-pack instantiation still returns the declared
// default, and for a multi-policy pack the last tag that supplies a value still wins.
template <class... Policies>
[[nodiscard]] std::string_view chat_client_id_of() {
    std::string_view found;
    ([&found] {
        if (auto v = policy_chat_client_id<Policies>::get(); !v.empty()) found = v;
    }(), ...);
    return found;
}

template <class... Policies>
[[nodiscard]] std::vector<Capability> capability_ceiling_of() {
    std::vector<Capability> result;
    ([&result] {
        auto caps = tool_detail::policy_capabilities<Policies>::get();
        result.insert(result.end(), caps.begin(), caps.end());
    }(), ...);
    return result;
}

template <class... Policies>
[[nodiscard]] std::uint32_t max_turns_of() {
    std::uint32_t value = 16;
    ([&value] {
        if (auto v = policy_max_turns<Policies>::get(); v.has_value()) value = *v;
    }(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] std::optional<std::uint64_t> token_budget_of() {
    std::optional<std::uint64_t> value;
    ([&value] {
        if (auto v = policy_token_budget<Policies>::get(); v.has_value()) value = v;
    }(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] approval_mode approval_of() {
    // Same tag, same extraction mechanism as Tool's (tool_detail::policy_approval) -- Agent's own
    // default (policy_driven) differs from Tool's (never_require, core/tool.hpp), a difference in
    // the owning context's default, not a second definition of Approval<M> itself.
    approval_mode mode = approval_mode::policy_driven;
    ([&mode] {
        if (auto m = tool_detail::policy_approval<Policies>::get(); m.has_value()) mode = *m;
    }(), ...);
    return mode;
}

template <class... Policies>
[[nodiscard]] concurrency_mode concurrency_of() {
    concurrency_mode value = concurrency_mode::sequential;
    ([&value] {
        if (auto v = policy_concurrency<Policies>::get(); v.has_value()) value = *v;
    }(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] telemetry_capture telemetry_of() {
    telemetry_capture value = telemetry_capture::metadata_only;
    ([&value] {
        if (auto v = policy_telemetry<Policies>::get(); v.has_value()) value = *v;
    }(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] std::optional<std::uint32_t> stateless_of() {
    std::optional<std::uint32_t> value;
    ([&value] {
        if (auto v = policy_stateless<Policies>::get(); v.has_value()) value = v;
    }(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] std::optional<std::string> output_schema_of() {
    std::optional<std::string> value;
    ([&value] {
        if (auto v = policy_output_schema<Policies>::get(); v.has_value()) value = v;
    }(), ...);
    return value;
}

// 002 §3 table default (`Strict`) when no `SandboxProfile<P>` tag is declared at all -- absence is a
// real, meaningful default here (unlike `Stateless<N>`'s absence-off, 002 §9 Q3), so this returns a
// value directly rather than an `std::optional` a caller might mistake for "unspecified."
template <class... Policies>
[[nodiscard]] SandboxProfileDescriptor sandbox_profile_of() {
    std::optional<SandboxProfileDescriptor> value;
    ([&value] {
        if (auto v = policy_sandbox_profile<Policies>::get(); v.has_value()) value = v;
    }(), ...);
    return value.value_or(SandboxProfileDescriptor{});
}

// -- 002 §6 validation checks ---------------------------------------------------------------------

[[nodiscard]] inline result<void> check_chat_client_id(std::string_view id) {
    if (id.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "agent declares no ChatClientId<\"vendor:model\"> binding "
                                      "(002 §3: required, no default)",
                                      "agent.chat_client_id_missing"});
    }
    return {};
}

// Milestone 5 Phase B6: real when `registry` is supplied (an entry must exist for `id`); the pre-M5
// always-pass stub when it isn't (no registry to check against yet — an honest "not evaluated," see
// file-top comment).
[[nodiscard]] inline result<void> check_chat_client_credentials(std::string_view id,
                                                                  ChatClientRegistry const* registry) {
    if (registry == nullptr) return {};
    if (!registry->find(id).has_value()) {
        return std::unexpected(error{failure_class::contract,
                                      "ChatClientId '" + std::string(id) +
                                          "' has no bound backend in the supplied ChatClientRegistry",
                                      "agent.chat_client_id_unregistered"});
    }
    return {};
}

[[nodiscard]] inline result<void> check_tool_name_collision(std::vector<ToolDescriptor> const& tools) {
    for (std::size_t i = 0; i < tools.size(); ++i) {
        for (std::size_t j = i + 1; j < tools.size(); ++j) {
            if (tools[i].name == tools[j].name) {
                return std::unexpected(
                    error{failure_class::contract,
                          "two declared tools share the name '" + tools[i].name + "'",
                          "agent.tool_name_collision"});
            }
        }
    }
    return {};
}

[[nodiscard]] inline result<void> check_capability_ceiling(CapabilitySet const& ceiling,
                                                             std::vector<ToolDescriptor> const& tools) {
    for (ToolDescriptor const& tool : tools) {
        for (Capability const& requirement : tool.capability_ceiling) {
            if (!ceiling.contains(requirement)) {
                return std::unexpected(error{
                    failure_class::policy,
                    "tool '" + tool.name + "' declares a capability the agent's ceiling does not cover",
                    "agent.capability_ceiling_exceeded"});
            }
        }
    }
    return {};
}

// ADR-012, real per docs/planning/sandbox-backend-registry-design-draft.md (Revision 2). When `P`
// named a concrete backend type directly, `SandboxProfileArg<P>` already required
// `SandboxBackend<P>` at `SandboxProfile<P>`'s own declaration site (compile time) -- a type that
// doesn't satisfy the concept, or isn't even includable in this build (e.g. a wasm backend type
// without AGENTENGINE_WITH_WASM), fails to compile before `register_agent<A>()` is ever
// instantiated, so there is no runtime "unavailable" case left to check for that branch;
// `desc.is_strict == false` here is proof enough by construction. `Strict` is the branch genuinely
// deferred until a registry is supplied: with none (`registry == nullptr`), this stays the pre-M2
// always-pass stub -- an honest "not evaluated," not a silent pass dressed up as enforcement, the
// same shape `check_chat_client_credentials` above already uses. With one supplied, `Strict`
// resolves against the registry's real, registered candidates (`SandboxBackendRegistry::
// resolve_strict()`, sandbox/sandbox_backend_registry.hpp) -- `nullopt`/error there becomes 008 §3's
// "no fallback -> startup fails" case here, for real.
[[nodiscard]] inline result<void> check_sandbox_profile_availability(
        SandboxProfileDescriptor const& desc, SandboxBackendRegistry const* registry) {
    if (registry == nullptr) return {};
    if (!desc.is_strict) return {};
    return registry->resolve_strict(current_platform()).transform([](auto*) { return; });
}

// Stubbed: needs a per-tool backend-declaration policy tag that does not exist yet
// (`Tool<Derived, Policies...>` has no analog of `SandboxProfile<P>`) -- ADR-012 resolved the
// template-parameter-*kind* conflict this check was originally blocked on, but deliberately did not
// invent a second, larger policy-tag surface as a drive-by (see file top).
[[nodiscard]] inline result<void> check_tool_sandbox_profile_compatibility() { return {}; }

// Milestone 5 Phase B5/B6: real when both an OutputSchema<T> is declared AND a registry is
// supplied; a no-op (correctly, not by omission) whenever either is absent -- no schema declared
// means nothing to enforce, no registry means no bound capabilities to check against yet (same
// "not evaluated" honesty as check_chat_client_credentials above). `select_output_schema_strategy`
// (core/chat_client.hpp) always produces a strategy under today's three-tier design (parse-and-
// repair is the universal last resort) -- that function's own comment names why 004 §2's "if no
// fallback exists, fail at startup" clause is therefore currently unreachable from here rather than
// faked with an artificial rejection.
[[nodiscard]] inline result<void> check_output_schema_enforceable(
    std::optional<std::string> const& output_schema_json, ChatClientRegistry const* registry,
    std::string_view chat_client_id) {
    if (!output_schema_json.has_value() || registry == nullptr) return {};
    if (!registry->find(chat_client_id).has_value()) {
        // Already caught by check_chat_client_credentials, which the compiler runs first --
        // defensive redundancy in case call order ever changes, not a second, divergent code path.
        return std::unexpected(error{failure_class::contract,
                                      "ChatClientId '" + std::string(chat_client_id) +
                                          "' has no bound backend to check OutputSchema enforceability against",
                                      "agent.chat_client_id_unregistered"});
    }
    return {};
}

// Stubbed: needs 014's handoff/workflow graph (explicitly out of scope for M2).
[[nodiscard]] inline result<void> check_handoff_cycle() { return {}; }

template <class Derived>
[[nodiscard]] result<void> check_stateless_session_state(std::optional<std::uint32_t> stateless_pool_size) {
    // `Stateless<N>` (absence-off, 002 §9 Q3) claims the agent holds no cross-run state; a
    // conforming Derived is therefore a pure CRTP tag type with no instance data members (only the
    // static `name`/`instructions` and hook member functions Agent<Derived,...> expects) --
    // `std::is_empty_v` is exactly "no non-static data members, no virtual functions", the real,
    // mechanical shape of "carries no session state" this milestone can check without inventing a
    // static-analysis pass over hook bodies.
    if (stateless_pool_size.has_value() && !std::is_empty_v<Derived>) {
        return std::unexpected(
            error{failure_class::contract,
                  "Stateless<N> declared but the agent type carries instance data (session state)",
                  "agent.stateless_session_state"});
    }
    return {};
}

// -- the compiler itself: pattern-matches A's unique `Agent<A, Policies...>` base to recover the
// Policies pack (the only way to get it back from `A` alone -- `A` converts to its Agent base by
// public inheritance, and overload resolution + decltype extracts the template arguments) ---------

template <class Derived, class... Policies>
Agent<Derived, Policies...> agent_base_of(Agent<Derived, Policies...> const&);  // decltype-only, never defined

template <class A>
using agent_base_t = decltype(agent_base_of(std::declval<A const&>()));

template <class A, class Base>
struct compiler;  // primary intentionally undefined -- Base must be A's own Agent<A, Policies...>

// Gap-2 fix: `description`/`version` are optional statics, unlike the required `name`/`instructions`
// (`A::name`/`A::instructions` are used directly above with no detection -- a type missing either
// already fails to compile at `register_agent<A>()`, by design, per this file's own §6 comment).
// Making these two REQUIRED as well would break every existing `Agent<Derived,...>` conformer that
// predates this fix; `requires`-detected optional statics, defaulting to "not declared," extend the
// identity 002 §1 always specified without an invasive, breaking migration.
//
// Naming note, not a compile hazard: `Tool<Derived, Policies...>` (core/tool.hpp) also requires a
// CRTP-provided `static description` -- same member name, different meaning (a tool's schema text
// shown to a model vs. an agent's identity text for a card/listing). No actual collision is possible
// (`Agent<Derived,...>` and `Tool<Derived,...>` are unrelated CRTP bases, never satisfied by the same
// type), but an author skimming both conventions could reasonably expect them to mean the same thing.
// Flagged here rather than silently left for someone to discover the hard way.
template <class A>
concept has_agent_description = requires { { A::description } -> std::convertible_to<std::string_view>; };
template <class A>
concept has_agent_version = requires { { A::version } -> std::convertible_to<std::string_view>; };

template <class A, class... Policies>
struct compiler<A, Agent<A, Policies...>> {
    [[nodiscard]] static result<AgentMetadata> run(ChatClientRegistry const* registry,
                                                     SandboxBackendRegistry const* sandbox_registry) {
        AgentMetadata meta;
        meta.agent_name = std::string(A::name);
        meta.agent_instructions = std::string(A::instructions);
        if constexpr (has_agent_description<A>) meta.agent_description = std::string(A::description);
        if constexpr (has_agent_version<A>) meta.agent_version = std::string(A::version);
        meta.chat_client_id = std::string(chat_client_id_of<Policies...>());

        if (auto r = check_chat_client_id(meta.chat_client_id); !r) return std::unexpected(r.error());
        if (auto r = check_chat_client_credentials(meta.chat_client_id, registry); !r) {
            return std::unexpected(r.error());
        }

        meta.tools = tools_of<Policies...>::table();
        if (auto r = check_tool_name_collision(meta.tools.descriptors()); !r) return std::unexpected(r.error());

        meta.capability_ceiling = capability_ceiling_of<Policies...>();
        CapabilitySet const ceiling = CapabilitySet::grant_root(meta.capability_ceiling);
        if (auto r = check_capability_ceiling(ceiling, meta.tools.descriptors()); !r) {
            return std::unexpected(r.error());
        }

        meta.sandbox_profile = sandbox_profile_of<Policies...>();
        if (auto r = check_sandbox_profile_availability(meta.sandbox_profile, sandbox_registry); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = check_tool_sandbox_profile_compatibility(); !r) return std::unexpected(r.error());

        meta.max_turns = max_turns_of<Policies...>();
        meta.token_budget = token_budget_of<Policies...>();
        meta.approval = approval_of<Policies...>();
        meta.concurrency = concurrency_of<Policies...>();
        meta.telemetry = telemetry_of<Policies...>();

        meta.output_schema_json = output_schema_of<Policies...>();
        if (auto r = check_output_schema_enforceable(meta.output_schema_json, registry, meta.chat_client_id);
            !r) {
            return std::unexpected(r.error());
        }
        if (meta.output_schema_json.has_value() && registry != nullptr) {
            if (auto caps = registry->find(meta.chat_client_id); caps.has_value()) {
                meta.output_schema_strategy_chosen = select_output_schema_strategy(*caps);
            }
        }
        if (auto r = check_handoff_cycle(); !r) return std::unexpected(r.error());

        meta.stateless_pool_size = stateless_of<Policies...>();
        if (auto r = check_stateless_session_state<A>(meta.stateless_pool_size); !r) {
            return std::unexpected(r.error());
        }

        return meta;
    }
};

}  // namespace agent_detail

// 002 §6's entry point: "at register_agent<A>() the engine compiles metadata and validates, failing
// fast" -- `A` must derive from `Agent<A, Policies...>` for some Policies pack (any other type fails
// to compile here, not at some later use site).
//
// Milestone 5 Phase B6: `registry` is additive and defaults to `nullptr`, so every pre-M5 call site
// (`register_agent<A>()`, no arguments) keeps compiling and keeps its pre-M5 stubbed-check
// behavior unchanged -- passing a real `ChatClientRegistry` opts a caller into the real
// credential/OutputSchema-enforceability checks (agent_detail::compiler::run, above).
//
// docs/planning/sandbox-backend-registry-design-draft.md (Revision 2): `sandbox_registry` is a
// second, independent additive parameter, same "opts a caller into the real check, unaffected
// otherwise" contract as `registry` above -- every existing one-arg or zero-arg call site is
// unaffected; passing a real `SandboxBackendRegistry` opts a caller into `Strict` actually
// resolving against real, registered backends (check_sandbox_profile_availability, above) instead
// of the pre-registry always-pass stub.
template <class A>
[[nodiscard]] result<AgentMetadata> register_agent(ChatClientRegistry const* registry = nullptr,
                                                     SandboxBackendRegistry const* sandbox_registry = nullptr) {
    return agent_detail::compiler<A, agent_detail::agent_base_t<A>>::run(registry, sandbox_registry);
}

// M2 Phase E task E3: the milestone's own headline exit-criterion sentence, made real -- "an agent
// declares Tools<...>, a capability-gated native tool call is enforced end to end." Pure wiring, no
// new enforcement logic: register_agent<A>() (E2, above) already compiled `meta.tools` and
// `meta.capability_ceiling` from A's declared policies (already validated as mutually consistent --
// E2's own capability-ceiling-mismatch check rejects registration otherwise, so a valid `meta` here
// can never fail this call's own `contains()` check for a tool it declares); core/tool_pipeline.hpp's
// `invoke_tool()` (Phase B, 006 §3's real ten-step pipeline, ADR-009's CapabilitySet::bind/revoke) is
// the actual mechanism -- this is the one glue function connecting an agent's compiled metadata to
// it.
//
// decisions/ADR-059-invoke-agent-tool-capability-attenuation.md (design -> red-team -> prove ->
// judge, required per CLAUDE.md/I2): the target agent's own compiled `meta.capability_ceiling` is
// NEVER granted directly (that was the bug this ADR fixes -- `grant_root(meta.capability_ceiling)`
// minted the TARGET's own full ceiling unconditionally, ignoring what the CALLER invoking this
// function actually holds, a textbook I2 ambient-authority hole). Instead, `ctx.capabilities` (the
// caller's own actually-held set, `effect_context.hpp:18`) is attenuated DOWN to the target's
// declared ceiling via `CapabilitySet::attenuate()` (ADR-009, already proven): the target's ceiling
// becomes the requested narrower set, checked against the caller's held set as parent. The result is
// bounded on BOTH sides at once -- never wider than what the caller holds, and never wider than what
// the target itself declares (`attenuate()`'s own `grant_root(narrower)` return means the derived set
// is exactly the target's ceiling, not the caller's possibly-larger held set, ADR-009 C2). A caller
// with no held capabilities at all (`ctx.capabilities == nullptr`) fails closed rather than being
// silently read as "everything" -- there is nothing to attenuate from.
[[nodiscard]] inline ToolResult invoke_agent_tool(AgentMetadata const& meta, ToolCallRequest const& request,
                                                   EffectContext& ctx, ApprovalDecider const& approve = {},
                                                   ToolInvocationAudit* audit_out = nullptr,
                                                   // ADR-070: appended last, default `{}`, threaded
                                                   // straight through to `invoke_tool()` below --
                                                   // same seam, same fail-closed-when-unset contract.
                                                   PolicyDecider const& policy = {}) {
    auto fail_closed = [&](error const& e) -> ToolResult {
        if (audit_out) {
            audit_out->call_id = request.call_id;
            audit_out->tool_name = request.tool_name;
            audit_out->ok = false;
            audit_out->error_code = e.code;
            audit_out->result_bytes = 0;
            audit_out->duration = std::chrono::steady_clock::duration{};
        }
        return tool_pipeline_detail::make_error_result(request.call_id, e);
    };

    if (!ctx.capabilities) {
        return fail_closed(error{failure_class::policy,
                                  "invoke_agent_tool: caller has no capabilities to attenuate from",
                                  "agent_call.no_caller_capabilities"});
    }
    result<CapabilitySet> attenuated = ctx.capabilities->attenuate(meta.capability_ceiling);
    if (!attenuated) {
        return fail_closed(attenuated.error());
    }
    return invoke_tool(meta.tools, *attenuated, request, ctx, approve, audit_out, policy);
}

}  // namespace agentengine
