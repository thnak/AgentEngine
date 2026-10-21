#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.2 (the session row) and §2.3 (authority sources):
// what a session runs under is a stated choice -- where its authority comes from, its token budget and its turn
// cap -- with no default for any of them. `rt::AgentSessionCore::initialize()` takes a `SessionChoices`; a session
// never initialised, or cleared, refuses every run with `choice.missing.session`.
//
// `AuthoritySource` is the two options ADR-244 §2.3 names, nothing else:
//   - `per_request(ceiling)`: every `StartRun`/`ResolveInteraction` carries a `RequestAuthority`, checked at
//     admission for liveness and against `ceiling` (a host `CapabilitySet`, or the named
//     `CapabilitySet::no_ceiling()`). An authority holding anything outside the ceiling is refused, never narrowed.
//   - `single_trusted_caller()`: the session's own principal and capabilities are the authority; a request that
//     carries one is refused rather than silently replaced. An honest option, not a safe one (§6).
//
// `Expiry` replaces `RequestAuthority`'s default-constructed `time_point` (which meant "already expired") with
// `Expiry::at(t)` or the named `Expiry::never()`. ADR-242 R3 rule 1: expiry is checked at admission only.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/bound.hpp"
#include "agentengine/trust/capability.hpp"

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

class AuthoritySource {  // ae-naming-lint: allow AuthoritySource — ADR-244
public:
    AuthoritySource() = delete;

    [[nodiscard]] static AuthoritySource per_request(CapabilityCeiling ceiling) {
        return AuthoritySource(authority_mode::per_request, std::move(ceiling));
    }
    [[nodiscard]] static AuthoritySource single_trusted_caller() {
        return AuthoritySource(authority_mode::single_trusted_caller, std::nullopt);
    }

    [[nodiscard]] authority_mode mode() const noexcept { return mode_; }
    // Non-null exactly for `per_request`.
    [[nodiscard]] CapabilityCeiling const* ceiling() const noexcept { return ceiling_ ? &*ceiling_ : nullptr; }

    [[nodiscard]] std::string describe() const {
        if (mode_ == authority_mode::single_trusted_caller) return "single_trusted_caller";
        if (ceiling_->is_none()) return "per_request(no_ceiling)";
        return "per_request(ceiling of " + std::to_string(ceiling_->set()->size()) + ")";
    }

private:
    AuthoritySource(authority_mode mode, std::optional<CapabilityCeiling> ceiling)
        : mode_(mode), ceiling_(std::move(ceiling)) {}

    authority_mode                   mode_;
    std::optional<CapabilityCeiling> ceiling_;
};

// Every member is required: none has a default constructor, so an aggregate that leaves one out does not compile.
struct SessionChoices {  // ae-naming-lint: allow SessionChoices — ADR-244
    AuthoritySource       authority;
    Bound<std::uint64_t>  token_budget;
    Bound<std::uint64_t>  max_turns;

    // The narrowing order restore and fork use: the same authority mode, and budgets no wider than `this`.
    [[nodiscard]] bool covers(SessionChoices const& narrower) const noexcept {
        return authority.mode() == narrower.authority.mode() && token_budget.covers(narrower.token_budget) &&
               max_turns.covers(narrower.max_turns);
    }
};

// The option table `choice.missing.session` lists.
inline constexpr std::array<std::string_view, 3> kSessionAuthorityOptions{
    "SessionChoices{AuthoritySource::per_request(ceiling), ...}",
    "SessionChoices{AuthoritySource::per_request(CapabilitySet::no_ceiling()), ...}",
    "SessionChoices{AuthoritySource::single_trusted_caller(), ...}",
};

}  // namespace agentengine
