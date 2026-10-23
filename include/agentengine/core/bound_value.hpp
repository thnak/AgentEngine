#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 (header-split plan, 2026-10-23): the compile-time
// spelling of a `Bound`, split out of core/bound.hpp so that header, which nearly every translation unit includes,
// does not change when the CRTP tags' vocabulary does. Only core/agent.hpp (the `MaxTurns<B>` / `TokenBudget<B>`
// tags) and what includes it see this.

#include <concepts>
#include <cstdint>

#include "agentengine/core/bound.hpp"

namespace agentengine {

// ADR-244 §2.1 (slice 4b): the compile-time spelling of a `Bound`, for a CRTP tag's non-type template
// parameter (`MaxTurns<at_most(16)>`, `TokenBudget<unbounded()>`). A structural literal type -- public
// members, no constructors -- so it can be an NTTP; an aggregate has no converting constructor, so a raw
// integer (`MaxTurns<16>`) does not convert and a tag with no argument does not compile. The factories are
// `consteval`, and `at_most(0)` is a compile error there (a `0` is not a limit, `check_bound`'s message),
// so the CRTP path refuses zero at the declaration. `to_bound()` is the one conversion, taken at
// registration.
template <std::unsigned_integral T>
struct BoundValue {  // ae-naming-lint: allow BoundValue — ADR-244 §2.1 new vocabulary (the compile-time spelling of Bound); 027 not yet updated
    T    value;
    bool is_unbounded;

    [[nodiscard]] constexpr Bound<T> to_bound() const noexcept {
        return is_unbounded ? Bound<T>::unbounded() : Bound<T>::at_most(value);
    }
};

// `std::uint64_t` is the one width a tag needs (a turn cap and a token budget are both `Bound<uint64_t>`).
[[nodiscard]] consteval BoundValue<std::uint64_t> at_most(std::uint64_t n) {
    if (n == 0) throw "at_most(0): 0 is not a limit -- state a positive limit, or unbounded() for none";
    return {n, false};
}
[[nodiscard]] consteval BoundValue<std::uint64_t> unbounded() { return {0, true}; }

}  // namespace agentengine
