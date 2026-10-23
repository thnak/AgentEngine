#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 (ADR-242 §2 rule 5: a budget or limit
// is required, and "explicitly unbounded" is a named value, never a `0` or a missing field).
//
// `Bound<T>` is "at most n" or "unbounded", with no default constructor and no conversion from an
// integer: an aggregate that leaves a `Bound` member out does not compile (MSVC, gcc and clang agree,
// tests/compile_fail/bound_*), so neither an omission nor `0` can stand for "no limit". `at_most(0)` is
// representable, and every place that checks a choice refuses it through `check_bound()` below, which
// points at `unbounded()` instead; a real "none allowed" has its own mechanism (`NetPolicy::deny_all`).
//
// FROZEN (header-split plan, 2026-10-23): this header sits near the root of the include graph (it reached ~356
// translation units through tool_descriptor.hpp and session_choices.hpp, ~490 once slice 5 puts `Bound` fields on
// the capability caps), so an edit here recompiles most of the tree. `choice_missing()` lives in
// core/choice_error.hpp and the compile-time `BoundValue` in core/bound_value.hpp, so neither can invalidate it.
// Add to `Bound<T>` only when `Bound<T>` itself must change.

#include <algorithm>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine {

template <std::unsigned_integral T>
class Bound {
public:
    Bound() = delete;

    [[nodiscard]] static constexpr Bound at_most(T n) noexcept { return Bound(n, false); }
    [[nodiscard]] static constexpr Bound unbounded() noexcept { return Bound(T{0}, true); }

    // nullopt == unbounded.
    [[nodiscard]] constexpr std::optional<T> limit() const noexcept {
        if (unbounded_) return std::nullopt;
        return value_;
    }
    [[nodiscard]] constexpr bool is_unbounded() const noexcept { return unbounded_; }

    // Narrowing order: unbounded covers everything; a value covers any value no larger than itself.
    [[nodiscard]] constexpr bool covers(Bound child) const noexcept {
        if (unbounded_) return true;
        return !child.unbounded_ && child.value_ <= value_;
    }
    [[nodiscard]] static constexpr Bound tighter(Bound a, Bound b) noexcept {
        if (a.unbounded_) return b;
        if (b.unbounded_) return a;
        return Bound(std::min(a.value_, b.value_), false);
    }

    [[nodiscard]] std::string describe() const {
        return unbounded_ ? std::string("unbounded") : std::to_string(value_);
    }

    friend constexpr bool operator==(Bound, Bound) noexcept = default;

private:
    constexpr Bound(T value, bool unbounded) noexcept : value_(value), unbounded_(unbounded) {}

    T    value_;
    bool unbounded_;
};

// `choice.zero_bound` when `bound` is `at_most(0)`; `what` names the choice ("token_budget",
// "limits.memory_bytes", ...).
template <std::unsigned_integral T>
[[nodiscard]] result<void> check_bound(std::string_view what, Bound<T> bound) {
    if (bound.limit() == std::optional<T>{T{0}}) {
        return std::unexpected(error{
            failure_class::contract,
            std::string(what) + " is at_most(0); 0 is not a limit -- state a positive limit, or "
                                "Bound::unbounded() for none",
            "choice.zero_bound"});
    }
    return {};
}

}  // namespace agentengine
