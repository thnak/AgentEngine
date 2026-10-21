#pragma once
// Test helper for decisions/ADR-244-required-choices-step-1b.md §2.2 (G-C11, tests only): `AgentSessionOptions`
// now states the session's identity and choices, with no defaults, and the bridge refuses a turn cap or token
// budget wider than the agent declares. Tests of the bridge used to get the declared bounds by omission; this
// states exactly those -- the agent's MaxTurns, its TokenBudget or none -- under a fixed test identity, with the
// session holding its own grants (`single_trusted_caller`).

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "agentengine/core/agent_session_bridge.hpp"

namespace ae_test {

[[nodiscard]] inline agentengine::AgentSessionOptions declared_options(agentengine::AgentMetadata const& meta,
                                                                       std::vector<agentengine::Capability> grants = {}) {
    using B = agentengine::Bound<std::uint64_t>;
    return agentengine::AgentSessionOptions{
        .session_id = "s-agent",
        .principal  = agentengine::Principal{"p-agent", ""},
        .choices    = agentengine::SessionChoices{agentengine::AuthoritySource::single_trusted_caller(),
                                                  meta.token_budget ? B::at_most(*meta.token_budget) : B::unbounded(),
                                                  B::at_most(meta.max_turns)},
        .grants             = std::move(grants),
        .chat_client_grants = {},
        .approve_tools      = std::nullopt,
        .policy             = {},
    };
}

// For a native agent `A`: its compiled declaration's bounds. A declaration that does not register gets
// placeholder bounds: the bridge refuses it at registration, before it reads the options, which is what a test of
// a refused declaration checks.
template <class A>
[[nodiscard]] agentengine::AgentSessionOptions declared_options(std::vector<agentengine::Capability> grants = {}) {
    agentengine::result<agentengine::AgentMetadata> meta = agentengine::register_agent<A>(nullptr, nullptr);
    if (meta) return declared_options(*meta, std::move(grants));
    agentengine::AgentMetadata placeholder;
    placeholder.max_turns = 1;
    return declared_options(placeholder, std::move(grants));
}

}  // namespace ae_test
