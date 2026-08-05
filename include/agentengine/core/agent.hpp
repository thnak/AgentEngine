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

// M2 Phase E task E1: the six tags below complete 002 §3's table (`Agent` previously left them
// missing entirely). All six are empty/near-empty per E1's own scope — API surface only, no
// interpretation by `register_agent<A>()` (E2) or any pipeline yet. Each comment names the RFC
// section/milestone that owns the real behavior, matching CLAUDE.md's "don't silently pretend
// complete" rule.

// Tool-batch concurrency (001 §4's second axis: parallel only when every tool in a batch declares
// `Parallelizable`, 006 §5). `sequential`/`parallel` mirror 001 §4's own prose exactly; nothing
// currently reads this tag — 006 §8 G4 (parallel-batch scheduling) is deferred past M2, so
// `Concurrency<parallel>` is representable but has no effect yet, same as `Parallelizable` itself.
enum class concurrency_mode { sequential, parallel };  // ae-naming-lint: allow concurrency_mode — pre-existing M0 scaffolding, reconcile at owning milestone

template <concurrency_mode Mode>
struct Concurrency {};  // ae-naming-lint: allow Concurrency — pre-existing M0 scaffolding, reconcile at owning milestone

// Transient-failure retry shape (001 §6: retry applies to `Transient`-classified failures only,
// bounded exponential with jitter, must respect the remaining deadline) — owned by 004 §4, not built
// this milestone. `Policy` is left an unconstrained type rather than an enum: 004 §4 has not yet
// named a fixed set of retry shapes, and inventing one here would be scope this task doesn't own.
template <class Policy>
struct Retry {};  // ae-naming-lint: allow Retry — pre-existing M0 scaffolding, reconcile at owning milestone (004 §4 owns real retry semantics)

// Memory/context providers (005 §5) — 027 §3 records that 005's originally-named `MemoryProvider`
// seam is superseded: "Memory is a kind of `ContextProvider`, not a parallel concept"
// (core/context_provider.hpp already models that concept). `Ms...` is left unconstrained here
// (no `ContextProvider`-satisfying `static_assert`) because enforcing that is real behavior for 005's
// owning milestone, not API-completeness scaffolding.
template <class... Ms>
struct Memory {};  // ae-naming-lint: allow Memory — pre-existing M0 scaffolding, reconcile at owning milestone (005 owns real semantics; Ms... are expected to satisfy core::ContextProvider once real logic lands)

// Ordered middleware chain (§5 above: four interception points, assembled at compile time, may not
// widen capabilities per I2). `Middleware` already has a canonical-name row in 027 §3 ("An
// interceptor around a run, turn, model call, or tool call") — pre-approved vocabulary, unlike its
// five siblings here, so no `ae-naming-lint: allow` suppression is needed.
template <class... Ms>
struct Middleware {};

enum class telemetry_capture { none, metadata_only, full };  // ae-naming-lint: allow telemetry_capture — pre-existing M0 scaffolding, reconcile at owning milestone

template <telemetry_capture C>
struct Telemetry {};  // ae-naming-lint: allow Telemetry — pre-existing M0 scaffolding, reconcile at owning milestone

// Agent holds no cross-run state, hosted as a Quark pool (§3's table; 001's glossary: "stateless
// agents may be `Stateless<N>` pools"). `N` is the pool size, mirroring `MaxTurns<N>`'s shape. Off by
// default (absence of this tag, not a template default) per 002 §9 Q3's resolution: this is a lint
// suggestion at `register_agent()` time (E2), never an inferred default — an author who never wrote
// `Stateless<N>` must never end up implicitly stateless for cross-run state they wrote expecting to
// persist.
template <std::uint32_t N>
struct Stateless {};  // ae-naming-lint: allow Stateless — pre-existing M0 scaffolding, reconcile at owning milestone

// Structured-output contract (003 §4: native provider JSON-schema/constrained-decoding preferred,
// tool-shaped forced single call, or parse-and-repair bounded re-ask as the last resort) — owned by
// 003, not built this milestone. Not a runtime override axis (002 §9 Q2): compile-time only, like
// `Tools`/`Middleware`/`Stateless<N>`.
template <class T>
struct OutputSchema {};  // ae-naming-lint: allow OutputSchema — pre-existing M0 scaffolding, reconcile at owning milestone (003 §4 owns real schema-enforcement semantics)

// -- The base (002 §1) --

// Derived provides: static name, static instructions, and turn logic. Policies listed as variadic
// template arguments are compiled into metadata at `register_agent<Derived>()` (002 §6); this base
// carries no state and no virtual dispatch.
template <class Derived, class... Policies>
struct Agent {};

} // namespace agentengine
