// PROVE-PHASE NEGATIVE PROBE. Must fail to compile -- attempts to copy the singleton
// IdentityAuthority, which would let code silently create a second, independently-mutable
// authority sharing the first's ancestry snapshot but diverging from then on (the exact C2-shaped
// non-copyability requirement carried over from ADR-096's own real MSVC-proven precedent).
#include "identity_authority.hpp"

int main() {
    probe::IdentityAuthority& real = probe::IdentityAuthority::bootstrap();
    probe::IdentityAuthority copy = real;  // <-- THIS LINE MUST FAIL: copy constructor is deleted
    (void)copy;
    return 0;
}
