// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.5): a `ContainerIsolation` with no stated
// limits. Its memory, pids and cpu limits used to default to 512 MiB / 128 / one CPU without anyone saying so; they are
// `Bound`s with no default, so `ContainerIsolation::conservative()` (a named preset) or a stated value is required.

#include "agentengine/sandbox/docker_execution_surface.hpp"

int main() {
    agentengine::ContainerIsolation iso{};  // no limits stated
    (void)iso;
    return 0;
}
