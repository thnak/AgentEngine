// Positive control for the 007-Capability-and-Trust-Model.md §9 G1 / ADR-009 compile-fail proof —
// this file MUST compile (see tests/CMakeLists.txt). Without this, the companion
// capability_set_no_direct_construction.cpp failing to compile would be meaningless: it could be
// failing because trust/capability.hpp doesn't compile at all, not because the specific shortcut it
// attempts is correctly rejected. `grant_root()` is the one explicit, named entry point that IS
// supposed to work.

#include "agentengine/trust/capability.hpp"

using namespace agentengine;

int main() {
    Capability entropy_cap = cap::Entropy{};
    CapabilitySet set = CapabilitySet::grant_root({entropy_cap});
    (void)set;
    return 0;
}
