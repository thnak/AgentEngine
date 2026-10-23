#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.3: the two authority sources a session can state
// (`authority_mode`) and a request authority's `Expiry`. Split out of session_choices.hpp (L2) so the L1
// `EffectContext` can carry the source and expiry a context runs under (ADR-244 §2.4) without depending upward
// on the session's choices.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace agentengine {

class Expiry {  // ae-naming-lint: allow Expiry — ADR-244
public:
    using time_point = std::chrono::steady_clock::time_point;

    Expiry() = delete;
    [[nodiscard]] static constexpr Expiry at(time_point t) noexcept { return Expiry(t, false); }
    [[nodiscard]] static constexpr Expiry never() noexcept { return Expiry(time_point{}, true); }

    [[nodiscard]] constexpr bool live(time_point now) const noexcept { return never_ || now < at_; }
    [[nodiscard]] constexpr bool is_never() const noexcept { return never_; }
    // nullopt == never.
    [[nodiscard]] constexpr std::optional<time_point> deadline() const noexcept {
        if (never_) return std::nullopt;
        return at_;
    }
    // The earlier of the two: a child admitted under a parent's authority never outlives it.
    [[nodiscard]] static constexpr Expiry earlier(Expiry a, Expiry b) noexcept {
        if (a.never_) return b;
        if (b.never_) return a;
        return a.at_ < b.at_ ? a : b;
    }

    friend constexpr bool operator==(Expiry, Expiry) noexcept = default;

private:
    constexpr Expiry(time_point t, bool never) noexcept : at_(t), never_(never) {}

    time_point at_;
    bool       never_;
};

enum class authority_mode : std::uint8_t {  // ae-naming-lint: allow authority_mode — ADR-244
    per_request,
    single_trusted_caller,
};

[[nodiscard]] constexpr std::string_view authority_mode_name(authority_mode m) noexcept {
    return m == authority_mode::per_request ? "per_request" : "single_trusted_caller";
}

}  // namespace agentengine
