// PROVE-PHASE NEGATIVE PROBE. This file MUST FAIL TO COMPILE -- it attempts to construct a
// Principal directly, outside IdentityAuthority, which the design claims is impossible (13.1's
// core forgery fix: "no public constructor... anyone holding a mere copy of a Principal cannot
// mint a child"). A compile error here at the exact construction line is the real proof; a clean
// compile would mean the access-control claim is false.
#include "identity_authority.hpp"

int main() {
    probe::Principal forged(1, "forged");  // <-- THIS LINE MUST FAIL: Principal's constructor is private
    return static_cast<int>(forged.id());
}
