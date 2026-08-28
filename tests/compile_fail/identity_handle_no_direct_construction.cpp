// This file MUST NOT compile (ADR-102 §3 C1 -- decisions/ADR-102-identity-native-sandbox-
// implementation-phase-1.md) -- see tests/CMakeLists.txt's try_compile() gate. `IdentityHandle`
// deliberately has no public constructor: construction is friend-gated to `IdentityAuthority` only
// (mint_root()/derive_child()/adopt()), matching the identical "no ambient-authority shortcut"
// discipline `CapabilitySet::grant_root()` already enforces for capabilities. This file's only
// content is exactly such an attempt (direct-constructing an IdentityHandle outside
// IdentityAuthority), and it must fail to compile -- if it ever compiles, a caller could mint its
// own identity out of nothing, closing none of what IdentityAuthority exists to guarantee.

#include "agentengine/trust/identity_authority.hpp"

using namespace agentengine;

int main() {
    IdentityHandle handle(1, "forged");  // must not compile: constructor is private, friend-gated
                                          // to IdentityAuthority only
    (void)handle;
    return 0;
}
