#pragma once
// ADR-244 §2.5: a Docker or containerd surface has no default isolation, so a provider bound to one is given the factory
// that states it. The conservative preset is what these tests ran under before the limits were required.

#include "agentengine/sandbox/docker_execution_surface.hpp"

namespace ae_test {

[[nodiscard]] inline agentengine::DockerExecutionSurface docker_surface() {
    return agentengine::DockerExecutionSurface("alpine:latest", agentengine::ContainerIsolation::conservative());
}

}  // namespace ae_test

#include <memory>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"

namespace ae_test {

// ADR-244 §2.5: an EffectContext holding the one grant an unmediated surface needs.
[[nodiscard]] inline agentengine::EffectContext unmediated_ctx(std::string surface, bool network) {
    agentengine::EffectContext ctx{.deadline = agentengine::Deadline::none()};
    ctx.capabilities = std::make_shared<agentengine::CapabilitySet const>(agentengine::CapabilitySet::grant_root(
        {agentengine::Capability{agentengine::cap::UnmediatedShell{std::move(surface), network}}}));
    return ctx;
}

// For `SandboxRuntime::run(..., executed, &ctx)` on a Docker surface in its default (no network) isolation.
[[nodiscard]] inline agentengine::EffectContext const& docker_ctx() {
    static agentengine::EffectContext const ctx = unmediated_ctx("docker", false);
    return ctx;
}

}  // namespace ae_test
