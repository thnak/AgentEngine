// PROVE-PHASE NEGATIVE PROBE. Must fail to compile -- attempts to hand-construct a Grant<T>
// directly, naming an arbitrary issued_to Principal. This is round 1 Finding 1's exact original
// attack ("hand-build a Grant<T> naming an observed Principal") -- a compile failure here is the
// real proof the 13.1/17.1 fix actually closes it, not just narrows it in prose.
#include "identity_authority.hpp"

struct Dummy { int x; };

int main() {
    probe::IdentityAuthority& authority = probe::IdentityAuthority::bootstrap();
    probe::Principal victim = authority.mint_root("victim");
    probe::Principal attacker = authority.mint_root("attacker");

    // Attacker tries to forge a grant claiming to be issued to `victim`, self-issued by `attacker`,
    // without ever going through IdentityAuthority::mint_grant():
    probe::Grant<Dummy> forged(Dummy{1}, victim, attacker, 0xdeadbeef);  // <-- THIS LINE MUST FAIL
    return static_cast<int>(forged.grant_id());
}
