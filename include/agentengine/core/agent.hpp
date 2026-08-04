#pragma once
// Implements 002-Agent-Model-and-Authoring.md — the CRTP authoring surface. Policies are template
// parameters resolved to metadata at startup (CONVENTIONS.md — no RTTI, no virtual for policy on
// the hot path). This header fixes the policy *tags* as empty/near-empty types; `register_agent<A>`
// (002 §6, the metadata compiler and its validation) is real logic and is not sketched here.
//
// Naming note: the compile-time tag that selects an agent's model backend is `ChatClientId<"...">`,
// not `ChatClient<"...">` — `ChatClient` is already 004's concept name for the backend interface
// itself, and a concept and a class template cannot share one identifier. Keep these distinct.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/fixed_string.hpp"
#include "agentengine/core/policy_tags.hpp"
#include "agentengine/core/tool.hpp"  // approval_mode / Approval<M> (006 §4 owns this; reused here)
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// -- Policy tags (002 §3) — compile-time configuration, never runtime objects on the hot path --

template <fixed_string Id>
struct ChatClientId {  // "vendor:model", overridable per run and by config (002 §3, 004)  // ae-naming-lint: allow ChatClientId — pre-existing M0 scaffolding, reconcile at owning milestone
    static constexpr std::string_view id = std::string_view{Id};
};

template <class... Ts>
struct Tools {};  // ae-naming-lint: allow Tools — pre-existing M0 scaffolding, reconcile at owning milestone

template <sandbox_profile P>
struct SandboxProfile {};  // ae-naming-lint: allow SandboxProfile — pre-existing M0 scaffolding, reconcile at owning milestone

// `Capabilities<Cs...>` (matching the host/mount-parameterized form 002 §2 and 006 §1's own
// examples show, `Capabilities<NetOut<"api.search.example">>`) now lives in core/policy_tags.hpp,
// shared with Tool's declaration site (006 §1) — not redefined here.

template <std::uint32_t N>
struct MaxTurns {};  // ae-naming-lint: allow MaxTurns — pre-existing M0 scaffolding, reconcile at owning milestone

template <std::uint64_t N>
struct TokenBudget {};  // ae-naming-lint: allow TokenBudget — pre-existing M0 scaffolding, reconcile at owning milestone

// `approval_mode`/`Approval<M>` now live in core/tool.hpp (006 §4 owns Approval's semantics) and
// are reused here rather than redefined — collapses what used to be a duplicate
// `approval_policy_mode` enum with identical enumerators (M2 Phase B breakdown decision 7).

enum class telemetry_capture { none, metadata_only, full };  // ae-naming-lint: allow telemetry_capture — pre-existing M0 scaffolding, reconcile at owning milestone

template <telemetry_capture C>
struct Telemetry {};  // ae-naming-lint: allow Telemetry — pre-existing M0 scaffolding, reconcile at owning milestone

// -- The base (002 §1) --

// Derived provides: static name, static instructions, and turn logic. Policies listed as variadic
// template arguments are compiled into metadata at `register_agent<Derived>()` (002 §6); this base
// carries no state and no virtual dispatch.
template <class Derived, class... Policies>
struct Agent {};

} // namespace agentengine
