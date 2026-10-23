// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, G-C1). Every `SessionChoices`
// member is required: `max_turns` left out would be copy-initialised from `{}`, which needs `Bound`'s deleted
// default constructor. If this ever compiles, an omitted turn cap can mean "no cap" again.

#include <cstdint>

#include "agentengine/core/session_choices.hpp"

int main() {
    agentengine::SessionChoices const choices{.authority    = agentengine::AuthoritySource::single_trusted_caller(),
                                              .token_budget = agentengine::Bound<std::uint64_t>::unbounded()};
    return choices.token_budget.is_unbounded() ? 0 : 1;
}
