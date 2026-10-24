#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.2 (the time-limit row): a call's deadline is a stated
// choice, never a default-constructed `time_point` that happens to mean "none". `EffectContext::deadline` used to be
// `steady_clock::time_point deadline{}`, where the zero value was the unspoken "no deadline"; every one of the 400-odd
// contexts built that way ran with no time limit and nobody had said so.
//
// `Deadline` has no default constructor, so an aggregate that leaves one out does not compile. It is either
// `Deadline::at(t)`, a point in time, or the named `Deadline::none()`. `Deadline::after(now, timeout)` turns a stated
// `Bound` (a timeout in milliseconds: `at_most(n)` or `unbounded()`) into a deadline from an explicit `now`, so this
// header reads no clock (the caller owns the seam, I5); an `unbounded()` timeout is `none()`.
//
// Same shape as `Expiry` (core/authority_terms.hpp), which says when an AUTHORITY stops being admitted; a `Deadline`
// says when one CALL must be done. They stay separate types so one cannot be passed for the other.
// Includes only bound.hpp (layer V, tools/layers.toml).

#include <chrono>
#include <cstdint>
#include <optional>

#include "agentengine/core/bound.hpp"

namespace agentengine {

class Deadline {  // ae-naming-lint: allow Deadline — ADR-244 §2.2
public:
    using time_point = std::chrono::steady_clock::time_point;

    Deadline() = delete;
    [[nodiscard]] static constexpr Deadline at(time_point t) noexcept { return Deadline(t, false); }
    [[nodiscard]] static constexpr Deadline none() noexcept { return Deadline(time_point{}, true); }
    // `timeout_ms` is a stated limit: `unbounded()` gives `none()`. `at_most(0)` is a zero-length call and is refused
    // by `check_bound` where a choice is validated, so it is not special-cased here.
    [[nodiscard]] static constexpr Deadline after(time_point now, Bound<std::uint64_t> timeout_ms) noexcept {
        auto const n = timeout_ms.limit();
        if (!n) return none();
        return at(now + std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(*n)));
    }

    [[nodiscard]] constexpr bool is_none() const noexcept { return none_; }
    // nullopt == none.
    [[nodiscard]] constexpr std::optional<time_point> time() const noexcept {
        if (none_) return std::nullopt;
        return at_;
    }
    [[nodiscard]] constexpr bool expired(time_point now) const noexcept { return !none_ && now >= at_; }
    // Time left from `now`; nullopt == none. Never negative.
    [[nodiscard]] constexpr std::optional<std::chrono::nanoseconds> remaining(time_point now) const noexcept {
        if (none_) return std::nullopt;
        return now >= at_ ? std::chrono::nanoseconds{0} : std::chrono::duration_cast<std::chrono::nanoseconds>(at_ - now);
    }
    // This deadline as a stated timeout from `now`, for a child that must not outlive it: `unbounded()` when there is
    // none, else `at_most(` the time left, rounded up to a whole millisecond and at least 1 `)`.
    [[nodiscard]] constexpr Bound<std::uint64_t> as_timeout_ms(time_point now) const noexcept {
        if (none_) return Bound<std::uint64_t>::unbounded();
        auto const left = remaining(now).value();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
        if (std::chrono::milliseconds(ms) < left) ++ms;
        return Bound<std::uint64_t>::at_most(ms < 1 ? 1u : static_cast<std::uint64_t>(ms));
    }
    // The earlier of the two: a child call never outlives its parent's deadline.
    [[nodiscard]] static constexpr Deadline earlier(Deadline a, Deadline b) noexcept {
        if (a.none_) return b;
        if (b.none_) return a;
        return a.at_ < b.at_ ? a : b;
    }

    friend constexpr bool operator==(Deadline, Deadline) noexcept = default;

private:
    constexpr Deadline(time_point t, bool none) noexcept : at_(t), none_(none) {}

    time_point at_;
    bool       none_;
};

}  // namespace agentengine
