// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.1/§2.2, slice 4b): an Agent that declares no MaxTurns.
// A turn cap is a stated `Bound` -- never omitted, never a bare integer, never `0`. If this ever compiles, a limit
// is again a number nobody chose.

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

using namespace agentengine;

struct NoTurnsAgent : Agent<NoTurnsAgent, ChatClientId<"test:scripted">, TokenBudget<unbounded()>,
                            Approval<approval_mode::policy_driven>, Concurrency<concurrency_mode::sequential>,
                            SandboxProfile<Strict>> {
    static constexpr std::string_view name         = "x";
    static constexpr std::string_view instructions = "x";
};

int main() {
    SandboxBackendRegistry sandboxes;
    return register_agent<NoTurnsAgent>(nullptr, sandboxes).has_value() ? 1 : 0;
}
