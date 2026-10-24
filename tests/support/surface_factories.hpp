#pragma once
// ADR-244 §2.5: a Docker or containerd surface has no default isolation, so a provider bound to one is given the factory
// that states it. The conservative preset is what these tests ran under before the limits were required.

#include "agentengine/sandbox/docker_execution_surface.hpp"

namespace ae_test {

[[nodiscard]] inline agentengine::DockerExecutionSurface docker_surface() {
    return agentengine::DockerExecutionSurface("alpine:latest", agentengine::ContainerIsolation::conservative());
}

}  // namespace ae_test
