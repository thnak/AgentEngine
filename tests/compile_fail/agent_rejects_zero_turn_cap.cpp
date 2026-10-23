// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.1/§2.2, slice 4b): a MaxTurns of at_most(0).
// A turn cap is a stated `Bound` -- never omitted, never a bare integer, never `0`. If this ever compiles, a limit
// is again a number nobody chose.

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

using namespace agentengine;

struct ZeroTurnsAgent : Agent<ZeroTurnsAgent, ChatClientId<"test:scripted">, MaxTurns<at_most(0)>,
                            TokenBudget<unbounded()>, Approval<approval_mode::policy_driven>,
                            Concurrency<concurrency_mode::sequential>, SandboxProfile<Strict>> {
    static constexpr std::string_view name         = "x";
    static constexpr std::string_view instructions = "x";
};

int main() {
    SandboxBackendRegistry sandboxes;
    return register_agent<ZeroTurnsAgent>(nullptr, sandboxes).has_value() ? 1 : 0;
}
