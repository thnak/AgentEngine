#pragma once
// Test helper for decisions/ADR-244-required-choices-step-1b.md §2.4: `MediatedPythonRunner::initialize`
// takes an explicit host grant, and `run()` serves only contexts carrying the one session nonce it bound
// first. Tests are the host here: they grant exactly what the runner lists (`required_mounts`) -- no more --
// and stamp each context with a fixed test nonce. A real session mints its own nonce
// (`rt::AgentSessionCore`); nothing in engine code may use this one.

#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

namespace ae_test {

inline constexpr agentengine::SessionNonce kTestSessionNonce{0xAE24'4000'0000'0001ull, 1};
// The principal test contexts run under. A standing bridge approval for the empty principal approves nobody.
inline agentengine::Principal const kTestPrincipal{"ae-test-host", ""};

[[nodiscard]] inline agentengine::CapabilitySet python_runner_host_grant(
        agentengine::native_jail::MediatedPythonConfig const& config) {
    std::vector<agentengine::Capability> grants;
    for (auto const& m : agentengine::native_jail::MediatedPythonRunner::required_mounts(config)) {
        grants.push_back(m);
    }
    return agentengine::CapabilitySet::grant_root(std::move(grants));
}

// Creates the runner for the session `nonce` names (the runner binds it at creation).
[[nodiscard]] inline agentengine::result<void> initialize_runner(agentengine::native_jail::MediatedPythonRunner& runner,
                                                                 agentengine::SessionNonce nonce = kTestSessionNonce) {
    agentengine::EffectContext ctx;
    ctx.session_nonce = nonce;
    ctx.principal = kTestPrincipal;
    return runner.initialize(python_runner_host_grant(runner.config()), ctx);
}

}  // namespace ae_test
