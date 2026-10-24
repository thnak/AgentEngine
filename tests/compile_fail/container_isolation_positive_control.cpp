// Positive control for container_isolation_unstated: the same type with the limits stated, both spellings.

#include "agentengine/sandbox/docker_execution_surface.hpp"

int main() {
    using agentengine::Bound;
    agentengine::ContainerIsolation preset = agentengine::ContainerIsolation::conservative();
    agentengine::ContainerIsolation stated{.memory_bytes = Bound<std::uint64_t>::at_most(1 << 20),
                                           .pids         = Bound<std::uint64_t>::unbounded(),
                                           .cpu_milli    = Bound<std::uint64_t>::at_most(500)};
    return preset.memory_bytes.limit() && stated.memory_bytes.limit() ? 0 : 1;
}
