// PROVE-PHASE NEGATIVE PROBE. Must fail to compile -- attempts to construct a second
// IdentityAuthority directly, bypassing bootstrap(). Proves (or disproves) the singleton claim at
// the type level: if this compiles, ANY code can mint its own independent IdentityAuthority
// instance, which is exactly round 2's original forgery finding recurring.
#include "identity_authority.hpp"

int main() {
    probe::IdentityAuthority rogue;  // <-- THIS LINE MUST FAIL: constructor is private
    return 0;
}
