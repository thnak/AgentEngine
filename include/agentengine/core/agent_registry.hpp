#pragma once
// Implements 002-Agent-Model-and-Authoring.md §6 — register_agent<A>(), the real metadata compiler.
// M2 Phase E tasks E2 and E3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// E2 is register_agent<A>() itself; E3 is invoke_agent_tool() at the bottom of this file, the
// milestone's headline exit-criterion sentence wired to real code (see that function's own comment).
//
// Of §6's 8 named validation checks, four run for real against machinery this milestone actually
// built (tool name collision, capability-ceiling coverage, Stateless<N>-vs-session-state via
// std::is_empty_v, and — since decisions/ADR-012-sandbox-profile-template-parameter-kind.md —
// SandboxProfile's compile-time shape) plus a fifth basic shape check (ChatClientId presence). The
// remaining three are stubbed to always-pass with a comment naming exactly what machinery is
// missing, per this task's own scope note — not silently skipped:
//   - ChatClientId *credentials/endpoint* (needs a real ChatClient registry, 004) — Milestone 2
//     builds no such registry; there is no `Engine` type yet to hold one.
//   - SandboxProfile *tool/profile backend mismatch* (002 §6's second SandboxProfile bullet) — needs
//     a per-tool backend-declaration policy tag that does not exist yet (`Tool<Derived, Policies...>`
//     has no analog of `SandboxProfile<P>`); ADR-012 resolved the *template-parameter-kind* conflict
//     (below) but deliberately did not invent a second, larger policy-tag surface as a drive-by.
//   - OutputSchema enforceability — needs a real ChatClient instance bound to the agent's
//     ChatClientId to query ChatClientCapabilities::structured_output_native against (004).
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
#include "agentengine/core/error.hpp"
#include "agentengine/core/fixed_string.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// The compiled, read-only agent metadata table (002 §1). Built once by `register_agent<A>()`;
// nothing here is a runtime configuration object read on a hot path (CONVENTIONS.md).
struct AgentMetadata {  // ae-naming-lint: allow AgentMetadata — pre-existing M0 scaffolding pattern for this milestone's new types, reconcile at owning milestone (matches ToolDescriptor/ToolTable's own deferred status, core/tool_pipeline.hpp)
    std::string agent_name;
    std::string agent_instructions;
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

template <class... Policies>
[[nodiscard]] std::string_view chat_client_id_of() {
    std::string_view found;
    auto consider = [&found]<class P>() {
        if (auto v = policy_chat_client_id<P>::get(); !v.empty()) found = v;
    };
    (consider.template operator()<Policies>(), ...);
    return found;
}

template <class... Policies>
[[nodiscard]] std::vector<Capability> capability_ceiling_of() {
    std::vector<Capability> result;
    auto append_one = [&result]<class P>() {
        auto caps = tool_detail::policy_capabilities<P>::get();
        result.insert(result.end(), caps.begin(), caps.end());
    };
    (append_one.template operator()<Policies>(), ...);
    return result;
}

template <class... Policies>
[[nodiscard]] std::uint32_t max_turns_of() {
    std::uint32_t value = 16;
    auto consider = [&value]<class P>() {
        if (auto v = policy_max_turns<P>::get(); v.has_value()) value = *v;
    };
    (consider.template operator()<Policies>(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] std::optional<std::uint64_t> token_budget_of() {
    std::optional<std::uint64_t> value;
    auto consider = [&value]<class P>() {
        if (auto v = policy_token_budget<P>::get(); v.has_value()) value = v;
    };
    (consider.template operator()<Policies>(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] approval_mode approval_of() {
    // Same tag, same extraction mechanism as Tool's (tool_detail::policy_approval) -- Agent's own
    // default (policy_driven) differs from Tool's (never_require, core/tool.hpp), a difference in
    // the owning context's default, not a second definition of Approval<M> itself.
    approval_mode mode = approval_mode::policy_driven;
    auto consider = [&mode]<class P>() {
        if (auto m = tool_detail::policy_approval<P>::get(); m.has_value()) mode = *m;
    };
    (consider.template operator()<Policies>(), ...);
    return mode;
}

template <class... Policies>
[[nodiscard]] concurrency_mode concurrency_of() {
    concurrency_mode value = concurrency_mode::sequential;
    auto consider = [&value]<class P>() {
        if (auto v = policy_concurrency<P>::get(); v.has_value()) value = *v;
    };
    (consider.template operator()<Policies>(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] telemetry_capture telemetry_of() {
    telemetry_capture value = telemetry_capture::metadata_only;
    auto consider = [&value]<class P>() {
        if (auto v = policy_telemetry<P>::get(); v.has_value()) value = *v;
    };
    (consider.template operator()<Policies>(), ...);
    return value;
}

template <class... Policies>
[[nodiscard]] std::optional<std::uint32_t> stateless_of() {
    std::optional<std::uint32_t> value;
    auto consider = [&value]<class P>() {
        if (auto v = policy_stateless<P>::get(); v.has_value()) value = v;
    };
    (consider.template operator()<Policies>(), ...);
    return value;
}

// 002 §3 table default (`Strict`) when no `SandboxProfile<P>` tag is declared at all -- absence is a
// real, meaningful default here (unlike `Stateless<N>`'s absence-off, 002 §9 Q3), so this returns a
// value directly rather than an `std::optional` a caller might mistake for "unspecified."
template <class... Policies>
[[nodiscard]] SandboxProfileDescriptor sandbox_profile_of() {
    std::optional<SandboxProfileDescriptor> value;
    auto consider = [&value]<class P>() {
        if (auto v = policy_sandbox_profile<P>::get(); v.has_value()) value = v;
    };
    (consider.template operator()<Policies>(), ...);
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

// Stubbed: needs a real ChatClient registry/credential store (004) -- see this file's top comment.
[[nodiscard]] inline result<void> check_chat_client_credentials(std::string_view) { return {}; }

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

// ADR-012: real for the case it CAN be real about. When `P` named a concrete backend type directly,
// `SandboxProfileArg<P>` already required `SandboxBackend<P>` at `SandboxProfile<P>`'s own
// declaration site (compile time) -- a type that doesn't satisfy the concept, or isn't even
// includable in this build (e.g. a wasm backend type without AGENTENGINE_WITH_WASM), fails to compile
// before `register_agent<A>()` is ever instantiated, so there is no runtime "unavailable" case left
// to check for that branch; `profile.is_strict == false` here is proof enough by construction.
// `Strict` is the branch genuinely deferred: resolving it to a concrete backend needs the real set of
// backends *this deployment* has available, which needs an Engine-level backend registry M2 does not
// build (same shape of gap as `check_chat_client_credentials` above) --
// `resolve_strict()`/`ProfileTraits` (sandbox/sandbox.hpp) are the tested, real ranking logic already
// waiting for that registry (tests/test_sandbox_backend_contract.cpp).
[[nodiscard]] inline result<void> check_sandbox_profile_availability(SandboxProfileDescriptor const&) {
    return {};
}

// Stubbed: needs a per-tool backend-declaration policy tag that does not exist yet
// (`Tool<Derived, Policies...>` has no analog of `SandboxProfile<P>`) -- ADR-012 resolved the
// template-parameter-*kind* conflict this check was originally blocked on, but deliberately did not
// invent a second, larger policy-tag surface as a drive-by (see file top).
[[nodiscard]] inline result<void> check_tool_sandbox_profile_compatibility() { return {}; }

// Stubbed: needs a real ChatClient instance to query capabilities() against (004).
[[nodiscard]] inline result<void> check_output_schema_enforceable() { return {}; }

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

template <class A, class... Policies>
struct compiler<A, Agent<A, Policies...>> {
    [[nodiscard]] static result<AgentMetadata> run() {
        AgentMetadata meta;
        meta.agent_name = std::string(A::name);
        meta.agent_instructions = std::string(A::instructions);
        meta.chat_client_id = std::string(chat_client_id_of<Policies...>());

        if (auto r = check_chat_client_id(meta.chat_client_id); !r) return std::unexpected(r.error());
        if (auto r = check_chat_client_credentials(meta.chat_client_id); !r) return std::unexpected(r.error());

        meta.tools = tools_of<Policies...>::table();
        if (auto r = check_tool_name_collision(meta.tools.descriptors()); !r) return std::unexpected(r.error());

        meta.capability_ceiling = capability_ceiling_of<Policies...>();
        CapabilitySet const ceiling = CapabilitySet::grant_root(meta.capability_ceiling);
        if (auto r = check_capability_ceiling(ceiling, meta.tools.descriptors()); !r) {
            return std::unexpected(r.error());
        }

        meta.sandbox_profile = sandbox_profile_of<Policies...>();
        if (auto r = check_sandbox_profile_availability(meta.sandbox_profile); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = check_tool_sandbox_profile_compatibility(); !r) return std::unexpected(r.error());

        meta.max_turns = max_turns_of<Policies...>();
        meta.token_budget = token_budget_of<Policies...>();
        meta.approval = approval_of<Policies...>();
        meta.concurrency = concurrency_of<Policies...>();
        meta.telemetry = telemetry_of<Policies...>();

        if (auto r = check_output_schema_enforceable(); !r) return std::unexpected(r.error());
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
template <class A>
[[nodiscard]] result<AgentMetadata> register_agent() {
    return agent_detail::compiler<A, agent_detail::agent_base_t<A>>::run();
}

// M2 Phase E task E3: the milestone's own headline exit-criterion sentence, made real -- "an agent
// declares Tools<...>, a capability-gated native tool call is enforced end to end." Pure wiring, no
// new enforcement logic: register_agent<A>() (E2, above) already compiled `meta.tools` and
// `meta.capability_ceiling` from A's declared policies (already validated as mutually consistent --
// E2's own capability-ceiling-mismatch check rejects registration otherwise, so a valid `meta` here
// can never fail this call's own `contains()` check for a tool it declares); core/tool_pipeline.hpp's
// `invoke_tool()` (Phase B, 006 §3's real ten-step pipeline, ADR-009's CapabilitySet::bind/revoke) is
// the actual mechanism -- this is the one glue function connecting an agent's compiled metadata to
// it. `CapabilitySet::grant_root` is called fresh per invocation rather than cached on `AgentMetadata`
// itself: `AgentMetadata` is 002 §1's read-only compiled table (no request-scoped state), and a
// `CapabilitySet` is cheap to construct from the same `capability_ceiling` vector already sitting
// there (ADR-009's own type, not reinvented).
[[nodiscard]] inline ToolResult invoke_agent_tool(AgentMetadata const& meta, ToolCallRequest const& request,
                                                   EffectContext& ctx, ApprovalDecider const& approve = {},
                                                   ToolInvocationAudit* audit_out = nullptr) {
    CapabilitySet const ceiling = CapabilitySet::grant_root(meta.capability_ceiling);
    return invoke_tool(meta.tools, ceiling, request, ctx, approve, audit_out);
}

}  // namespace agentengine
