#pragma once
// Implements 002-Agent-Model-and-Authoring.md — the CRTP authoring surface. Policies are template
// parameters resolved to metadata at startup (CONVENTIONS.md — no RTTI, no virtual for policy on
// the hot path). This header fixes the policy *tags* as empty/near-empty types; `register_agent<A>`
// (002 §6, the metadata compiler and its validation) is real logic and is not sketched here.
//
// Naming note: the compile-time tag that selects an agent's model backend is `ChatClientId<"...">`,
// not `ChatClient<"...">` — `ChatClient` is already 004's concept name for the backend interface
// itself, and a concept and a class template cannot share one identifier. Keep these distinct.

#include <cstdint>
#include <string_view>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// -- Policy tags (002 §3) — compile-time configuration, never runtime objects on the hot path --

template <std::string_view const& Id>
struct ChatClientId {};  // "vendor:model", overridable per run and by config (002 §3, 004)

template <class... Ts>
struct Tools {};

template <sandbox_profile P>
struct SandboxProfile {};

template <capability_kind... Ks>
struct Capabilities {};

template <std::uint32_t N>
struct MaxTurns {};

template <std::uint64_t N>
struct TokenBudget {};

enum class approval_policy_mode { never_require, always_require, policy_driven };

template <approval_policy_mode M>
struct Approval {};

enum class telemetry_capture { none, metadata_only, full };

template <telemetry_capture C>
struct Telemetry {};

// -- The base (002 §1) --

// Derived provides: static name, static instructions, and turn logic. Policies listed as variadic
// template arguments are compiled into metadata at `register_agent<Derived>()` (002 §6); this base
// carries no state and no virtual dispatch.
template <class Derived, class... Policies>
struct Agent {};

} // namespace agentengine
