// This file MUST NOT compile (ADR-102 §3 C1) -- see tests/CMakeLists.txt's try_compile() gate.
// `IdentityAuthority` is the one real singleton for this whole authority model; copying it would
// produce a second, divergent instance with its own independent id/ancestry state, silently
// defeating the single-source-of-truth guarantee `bootstrap()` exists to provide. Its copy
// constructor is explicitly `= delete`d, and this file's only content is exactly such an attempt.

#include "agentengine/trust/identity_authority.hpp"

using namespace agentengine;

int main() {
    IdentityAuthority& original = IdentityAuthority::bootstrap();
    IdentityAuthority copy(original);  // must not compile: copy constructor is deleted
    (void)copy;
    return 0;
}
