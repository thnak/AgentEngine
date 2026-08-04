#pragma once
// A structural-type wrapper making string literals usable as class-type non-type template
// parameters (002-Agent-Model-and-Authoring.md §2 / README example `ChatClientId<"vendor:model">`
// binds a string-literal prvalue, which cannot bind to a `std::string_view const&` NTTP — that
// reference form requires a named object with linkage). `fixed_string` is the standard C++20
// structural-NTTP idiom: CTAD deduces `N` from the literal, so `ChatClientId<"...">` deduces
// `fixed_string<N>` and the type is usable directly as written in every RFC example.
//
// Extracted out of core/agent.hpp (its original, M0-era home) because
// decisions/ADR-009-capability-set-enforcement-mechanism.md's compile-time capability declaration
// tags (trust/capability.hpp's `cap::decl::*`) need the same idiom too, and trust/ is a lower tier
// than core/agent.hpp — trust/capability.hpp cannot include core/agent.hpp without an include
// cycle (agent.hpp already includes trust/capability.hpp for `CapabilitySet`).

#include <cstddef>
#include <string_view>

namespace agentengine {

template <std::size_t N>
struct fixed_string {  // ae-naming-lint: allow fixed_string — pre-existing M0 scaffolding, reconcile at owning milestone
    char value[N]{};

    constexpr fixed_string(char const (&str)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) value[i] = str[i];
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return std::string_view{value, N - 1};
    }
};

}  // namespace agentengine
