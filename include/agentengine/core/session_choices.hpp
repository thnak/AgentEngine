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

#include "agentengine/core/authority_terms.hpp"
#include "agentengine/core/bound.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

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
    // ADR-244 §2.2 (time limits): how long ONE model call or ONE tool call may take, in milliseconds -- `at_most(n)` or
    // `unbounded()`. The session turns it into `EffectContext::deadline` for each call. It is a boundary check, not
    // preemption: a call that starts past it is refused, a retry that cannot finish inside it is not started, and an
    // egress wait ends at it; a tool body already running is not interrupted (ADR-242 §3a).
    Bound<std::uint64_t>  call_timeout_ms;

    // The narrowing order restore and fork use: the same authority mode, and budgets no wider than `this`.
    [[nodiscard]] bool covers(SessionChoices const& narrower) const noexcept {
        return authority.mode() == narrower.authority.mode() && token_budget.covers(narrower.token_budget) &&
               max_turns.covers(narrower.max_turns) && call_timeout_ms.covers(narrower.call_timeout_ms);
    }
};

// The option table `choice.missing.session` lists.
inline constexpr std::array<std::string_view, 3> kSessionAuthorityOptions{
    "SessionChoices{AuthoritySource::per_request(ceiling), ...}",
    "SessionChoices{AuthoritySource::per_request(CapabilitySet::no_ceiling()), ...}",
    "SessionChoices{AuthoritySource::single_trusted_caller(), ...}",
};

}  // namespace agentengine
