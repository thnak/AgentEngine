// This file MUST NOT compile (007-Capability-and-Trust-Model.md §9 G1 / decisions/
// ADR-009-capability-set-enforcement-mechanism.md's C6 claim, miniature) — see
// tests/CMakeLists.txt's try_compile() gate. `CapabilitySet` deliberately has no public
// constructor, aggregate-init path, or any other way to smuggle a capability list in besides
// `grant_root()`: this file's only content is exactly such an attempt (list-initializing a
// CapabilitySet directly from a capability), and it must fail to compile -- if it ever compiles, a
// zero-argument-adjacent "give me a set with this in it" shortcut has been reintroduced, which is
// precisely the ambient-authority shape 007 §9 G1 forbids.

#include "agentengine/trust/capability.hpp"

using namespace agentengine;

int main() {
    Capability entropy_cap = cap::Entropy{};
    CapabilitySet set{entropy_cap};  // must not compile: no such constructor, and CapabilitySet is
                                      // not an aggregate (private member + user-declared default
                                      // ctor), so brace-init cannot reach `granted_` either.
    (void)set;
    return 0;
}
