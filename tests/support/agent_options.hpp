#pragma once
// Test helper for decisions/ADR-244-required-choices-step-1b.md §2.2 (G-C11, tests only): `AgentSessionOptions`
// now states the session's identity and choices, with no defaults, and the bridge refuses a turn cap or token
// budget wider than the agent declares. Tests of the bridge used to get the declared bounds by omission; this
// states exactly those -- the agent's MaxTurns, its TokenBudget or none -- under a fixed test identity, with the
// session holding its own grants (`single_trusted_caller`).

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

namespace ae_test {

// ADR-244 slice 4b (tests only): `register_agent`, `bind_agent_session` and `make_agent_session` take the sandbox
// registry as a required argument, and an empty one is `choice.missing.sandbox_backend`. This is the registry a test
// that is not about backends passes: one strict-eligible stub supporting both development platforms, so `Strict`
// resolves. It runs nothing -- every method returns an empty outcome.
struct StubSandboxBackend {
    static constexpr agentengine::ProfileTraits traits{
        10, agentengine::platform_id::windows_x86_64 | agentengine::platform_id::linux_x86_64,
        agentengine::cold_start_class::milliseconds};
    agentengine::task<agentengine::result<agentengine::SandboxHandle>> create(agentengine::SandboxSpec,
                                                                               agentengine::EffectContext&) {
        co_return agentengine::SandboxHandle{};
    }
    agentengine::task<agentengine::result<agentengine::ExecOutcome>> exec(agentengine::SandboxHandle&,
                                                                           agentengine::ExecRequest,
                                                                           agentengine::EffectContext&) {
        co_return agentengine::ExecOutcome{};
    }
    agentengine::task<void> destroy(agentengine::SandboxHandle&) { co_return; }
};

[[nodiscard]] inline agentengine::SandboxBackendRegistry const& sandboxes() {
    static agentengine::SandboxBackendRegistry const registry = [] {
        agentengine::SandboxBackendRegistry r;
        (void)r.register_backend("test-stub", std::make_shared<StubSandboxBackend>());
        return r;
    }();
    return registry;
}

[[nodiscard]] inline agentengine::AgentSessionOptions declared_options(agentengine::AgentMetadata const& meta,
                                                                       std::vector<agentengine::Capability> grants = {}) {
    using B = agentengine::Bound<std::uint64_t>;
    return agentengine::AgentSessionOptions{
        .session_id = "s-agent",
        .principal  = agentengine::Principal{"p-agent", ""},
        .choices    = agentengine::SessionChoices{agentengine::AuthoritySource::single_trusted_caller(),
                                                  meta.token_budget.value_or(B::unbounded()),
                                                  meta.max_turns.value_or(B::at_most(1))},
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
    agentengine::result<agentengine::AgentMetadata> meta = agentengine::register_agent<A>(nullptr, sandboxes());
    if (meta) return declared_options(*meta, std::move(grants));
    agentengine::AgentMetadata placeholder;
    placeholder.max_turns = agentengine::Bound<std::uint64_t>::at_most(1);
    return declared_options(placeholder, std::move(grants));
}

// ADR-244 slice 4b (tests only): an `AgentMetadata` stating each of the five choices 002 §3 used to default --
// 16 turns, no budget, `policy_driven` approval, sequential tools, `Strict` -- so a test that wants "a plain
// agent" says so. Production code never builds this: a host states its own choices.
[[nodiscard]] inline agentengine::AgentMetadata stated_metadata() {
    agentengine::AgentMetadata meta;
    meta.max_turns        = agentengine::Bound<std::uint64_t>::at_most(16);
    meta.token_budget     = agentengine::Bound<std::uint64_t>::unbounded();
    meta.approval         = agentengine::approval_mode::policy_driven;
    meta.concurrency      = agentengine::concurrency_mode::sequential;
    meta.sandbox_profile  = agentengine::SandboxProfileDescriptor{};
    return meta;
}

}  // namespace ae_test
