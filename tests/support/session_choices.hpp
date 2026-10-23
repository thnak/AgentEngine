#pragma once
// The test preset for decisions/ADR-244-required-choices-step-1b.md §2.2 (G-C11): `rt::AgentSessionCore::
// initialize()` takes a stated `SessionChoices`. These helpers state the choices tests used to get by omission --
// the session holds its own grants (`single_trusted_caller`), with no budget or turn cap -- and keep the old
// optional arguments' meaning (unset is none) for the tests that passed them. They live under tests/ only:
// tools/test_preset_lint.py fails CI if include/, src/, tools/ or examples/ reach for them.

#include <cstdint>
#include <optional>

#include "agentengine/core/session_choices.hpp"
#include "agentengine/trust/principal.hpp"

namespace ae_test {

[[nodiscard]] inline agentengine::Bound<std::uint64_t> bound_or_unbounded(std::optional<std::uint64_t> n) {
    return n ? agentengine::Bound<std::uint64_t>::at_most(*n) : agentengine::Bound<std::uint64_t>::unbounded();
}

// A session that holds its own grants, with no budget and no turn cap.
[[nodiscard]] inline agentengine::SessionChoices trusted_unbounded() {
    return {agentengine::AuthoritySource::single_trusted_caller(), agentengine::Bound<std::uint64_t>::unbounded(),
            agentengine::Bound<std::uint64_t>::unbounded()};
}

// The same, with the budget and turn cap a test passed (unset is none).
[[nodiscard]] inline agentengine::SessionChoices trusted(std::optional<std::uint64_t> token_budget,
                                                         std::optional<std::uint64_t> max_turns = std::nullopt) {
    return {agentengine::AuthoritySource::single_trusted_caller(), bound_or_unbounded(token_budget),
            bound_or_unbounded(max_turns)};
}

// A Tier-3 session (formerly `set_require_authority(true)`): every request carries its authority, against no
// ceiling.
[[nodiscard]] inline agentengine::SessionChoices per_request_no_ceiling(
    std::optional<std::uint64_t> token_budget = std::nullopt, std::optional<std::uint64_t> max_turns = std::nullopt) {
    return {agentengine::AuthoritySource::per_request(agentengine::CapabilitySet::no_ceiling()),
            bound_or_unbounded(token_budget), bound_or_unbounded(max_turns)};
}

// The principal a builder test states (ADR-244 slice 4c: the builders have no default identity).
[[nodiscard]] inline agentengine::Principal principal() { return agentengine::Principal{"p-test", ""}; }

}  // namespace ae_test
