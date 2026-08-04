#pragma once
// Implements 006-Tool-and-Function-Plane.md — one declaration, one invocation path, one approval
// model for tools regardless of source (native, WASM plugin, MCP server, remote agent, sandboxed
// script, composite workflow).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/policy_tags.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// Canonical definition (006 §4 owns Approval's semantics) -- core/agent.hpp reuses this rather
// than defining its own duplicate `Approval<M>` (M2 Phase B breakdown decision 7).
enum class approval_mode { never_require, always_require, policy_driven };  // ae-naming-lint: allow approval_mode — pre-existing M0 scaffolding, reconcile at owning milestone

template <approval_mode M>
struct Approval {};  // ae-naming-lint: allow Approval — pre-existing M0 scaffolding, reconcile at owning milestone

// §5's concurrency claim ("safe to run concurrently with other calls in a batch") -- a declared
// tag only; M2 proves a single native tool call, never a parallel batch (006 §8 G4 is deferred,
// see the M2 breakdown's "what's explicitly deferred" list), so this carries no pipeline logic yet.
struct Parallelizable {};  // ae-naming-lint: allow Parallelizable — pre-existing M0 scaffolding, reconcile at owning milestone

// Milliseconds, not a std::chrono duration NTTP: chrono durations typically keep their `rep` as a
// private data member, which disqualifies them as C++20 structural types (the same constraint
// ADR-009 hit for `cap::decl::*`) -- a plain integer avoids that portability question entirely,
// consistent with this project's other duration-as-integer tags (`MaxTurns<N>`, `TokenBudget<N>`,
// core/agent.hpp).
template <std::uint64_t Ms>
struct Timeout {};  // ae-naming-lint: allow Timeout — pre-existing M0 scaffolding, reconcile at owning milestone

enum class tool_source { native, wasm_plugin, mcp_server, remote_agent, sandboxed_script, composite };  // ae-naming-lint: allow tool_source — pre-existing M0 scaffolding, reconcile at owning milestone

namespace tool_detail {

template <class Policy>
struct policy_capabilities {
    static std::vector<Capability> get() { return {}; }
};
template <class... Cs>
struct policy_capabilities<Capabilities<Cs...>> {
    static std::vector<Capability> get() { return {to_capability(Cs{})...}; }
};

template <class Policy>
struct policy_approval {
    static std::optional<approval_mode> get() { return std::nullopt; }
};
template <approval_mode M>
struct policy_approval<Approval<M>> {
    static std::optional<approval_mode> get() { return M; }
};

}  // namespace tool_detail

// The ten-step invocation pipeline (006 §3) is host machinery, not part of a tool's own shape, and
// is not modeled here (that's core/tool_pipeline.hpp, Phase B2). `Tool` fixes the declaration
// surface: a schema-typed name plus an `invoke` reachable exclusively through that pipeline, plus
// the two accessors (`declared_capabilities()`, `declared_approval()`) the pipeline reads to
// enforce steps 4 and 5 -- reading a tool's OWN declared ceiling, never the other way around
// (I2: the tool never gets to grant itself anything, it only ever states what it needs).
//
// Derived provides: static name, static description, nested Args/Reply types -- each paired with
// an AE_JSON_SCHEMA(Args, ...)/AE_JSON_SCHEMA(Reply, ...) description (json_schema.hpp; JSON
// Schema 2020-12, 006 §1) -- and `static result<Reply> invoke(Args, EffectContext&)`. `invoke` is
// synchronous `result<Reply>`, not `ae::task<result<Reply>>` -- `ae::task<T>` stays deferred for M2
// (decision 2, no gate item this milestone needs real coroutine concurrency to prove).
template <class Derived, class... Policies>
struct Tool {
    [[nodiscard]] static std::string args_schema() {
        return schema::json_schema_of<typename Derived::Args>();
    }
    [[nodiscard]] static std::string reply_schema() {
        return schema::json_schema_of<typename Derived::Reply>();
    }

    // The tool's capability ceiling (from its declared `Capabilities<Cs...>`, if any) -- empty if
    // the tool declared none, matching 007 §3's empty-by-default rule rather than defaulting to
    // "no restriction."
    [[nodiscard]] static std::vector<Capability> declared_capabilities() {
        std::vector<Capability> result;
        auto append_one = [&result]<class P>() {
            auto caps = tool_detail::policy_capabilities<P>::get();
            result.insert(result.end(), caps.begin(), caps.end());
        };
        (append_one.template operator()<Policies>(), ...);
        return result;
    }

    // `never_require` if the tool declared no `Approval<...>` -- an explicit, read default, not a
    // silent one: a tool author who wants approval gating must say so (§4's vocabulary is opt-in).
    [[nodiscard]] static approval_mode declared_approval() {
        approval_mode mode = approval_mode::never_require;
        auto consider = [&mode]<class P>() {
            if (auto m = tool_detail::policy_approval<P>::get(); m.has_value()) mode = *m;
        };
        (consider.template operator()<Policies>(), ...);
        return mode;
    }
};

} // namespace agentengine
