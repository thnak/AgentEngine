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
// `choice_missing()` builds the one error shape for a choice that was never made: class `contract`,
// code `choice.missing.<resource>`, and a message that lists the options. Callers pass the same option
// table their parser reads, so an option added there is in the message too.

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
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

// The error for a choice nobody made. `options` is the table the resource's parser reads.
[[nodiscard]] inline error choice_missing(std::string_view resource, std::span<std::string_view const> options) {
    std::string message = "no choice was made for " + std::string(resource) + "; choose one of: ";
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) message += ", ";
        message += options[i];
    }
    return error{failure_class::contract, std::move(message), "choice.missing." + std::string(resource)};
}

}  // namespace agentengine
