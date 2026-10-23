#pragma once
// Test helper for decisions/ADR-244-required-choices-step-1b.md slice 4b (tests only; header-split plan, 2026-10-23):
// the stub sandbox registry and the metadata that states the five agent choices, split out of agent_options.hpp so a
// test that only needs them does not include the session bridge (and everything behind it).

#include <cstdint>
#include <memory>

#include "agentengine/core/agent_registry.hpp"
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
