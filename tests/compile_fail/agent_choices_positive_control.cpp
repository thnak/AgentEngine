// Positive control for agent_rejects_missing_max_turns.cpp, agent_rejects_raw_integer_max_turns.cpp and
// agent_rejects_zero_turn_cap.cpp (decisions/ADR-244-required-choices-step-1b.md §2.1/§2.2, slice 4b): an agent
// stating all five choices with `BoundValue` arguments compiles and registers, so the negatives fail for the
// rule they name and not for some unrelated reason.

#include "agentengine/core/agent_registry.hpp"

using namespace agentengine;

struct StatedAgent : Agent<StatedAgent, ChatClientId<"test:scripted">, MaxTurns<at_most(8)>,
                            TokenBudget<unbounded()>, Approval<approval_mode::policy_driven>,
                            Concurrency<concurrency_mode::sequential>, SandboxProfile<Strict>> {
    static constexpr std::string_view name         = "stated";
    static constexpr std::string_view instructions = "States every choice.";
};

int main() {
    SandboxBackendRegistry sandboxes;
    auto meta = register_agent<StatedAgent>(nullptr, sandboxes);
    (void)meta;
    return 0;
}
