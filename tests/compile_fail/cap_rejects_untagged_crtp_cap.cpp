// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5, G-C1): a CRTP capability tag
// that states no cap. `FsRead<"work">` was the whole-mount, uncapped grant; the tag now requires `unbounded()` or
// `at_most(n)` for the size cap.

#include "agentengine/trust/capability.hpp"

int main() {
    auto const capability = agentengine::to_capability(agentengine::cap::decl::FsRead<"work">{});
    return agentengine::capability_kind_of(capability) == agentengine::capability_kind::fs_read ? 1 : 0;
}
