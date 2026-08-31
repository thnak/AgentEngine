// PROVE-PHASE PROBE: "Process 2" -- a genuinely separate, later-launched OS process, configured
// against the SAME durable directory `durable_restart_write.cpp` used. Two things must both be
// true for the §33/§34 fix to actually be correct, not just "different" from before:
//
//   (1) An entirely UNRELATED brand-new principal ("mallory," never seen before, minted FIRST in
//       this fresh process, exactly mirroring §33's own attack setup) must NOT receive any id
//       "alice" or the noise principals held before the restart -- closing the real, reproduced
//       leak.
//   (2) Alice herself, re-`adopt()`-ed with the SAME real id string, MUST get back the EXACT SAME
//       internal id she held before the restart -- otherwise the fix would trade a security hole
//       for a usability regression (every legitimate session losing access to its own durable
//       content on every restart), which this design's own §33.4 fix direction explicitly did not
//       intend ("durable identity precedes durable authorization" -- not "identity is discarded on
//       every restart").

#include "identity_authority.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    std::filesystem::path const durable_dir = "ae_durable_identity_probe";
    CHECK(std::filesystem::exists(durable_dir / "identity_next_id.txt"));

    IdentityAuthority& authority = IdentityAuthority::bootstrap(durable_dir);

    // (1) Mallory: brand-new, unrelated, minted FIRST in this fresh process -- the exact §33 setup.
    Principal mallory = authority.mint_root("session-for-mallory");
    std::printf("[read]  mallory id=%llu\n", static_cast<unsigned long long>(mallory.id()));
    CHECK(mallory.id() > 3);   // process 1 handed out ids 1 (noise1), 2 (noise2), 3 (alice) -- a
                                 // FIXED authority must start process 2 at 4, never recycling any
                                 // of those three. This is the literal negation of §33's own
                                 // demonstrated failure ("is Mallory's freshly-minted id the SAME
                                 // numeric id Alice held? YES").
    std::printf("[read]  mallory.id() > 3 (no recycling across the restart): PASS\n");

    // (2) Alice: re-adopting the SAME real id string must return her OWN original id, id=3 --
    // not a new one, and not mallory's.
    Principal alice_again = authority.adopt("alice", "");
    std::printf("[read]  alice re-adopted id=%llu (must be 3, her original id from process 1)\n",
                static_cast<unsigned long long>(alice_again.id()));
    CHECK(alice_again.id() == 3);
    CHECK(alice_again.id() != mallory.id());
    std::printf("[read]  alice_again.id() == 3 and != mallory.id() (durable re-adoption preserves "
                "her own legitimate identity across the restart, without granting it to anyone "
                "else): PASS\n");

    // Reproduce §33's own exact ACL-shaped check, now showing it correctly REJECTS mallory.
    bool const mallory_is_alice = (mallory.id() == 3);
    std::printf("[read]  is Mallory's id the SAME as Alice's persisted id 3? %s\n",
                mallory_is_alice ? "YES -- STILL LEAKING" : "no -- fix holds");
    CHECK(!mallory_is_alice);

    std::error_code ec;
    std::filesystem::remove_all(durable_dir, ec);
    std::printf("\nALL CHECKS PASSED -- the real, twice-reproduced §33 leak (id recycling across a "
                "genuine process restart) no longer occurs, and the legitimate principal's own "
                "continued access to her durably-owned content is preserved, not sacrificed to fix "
                "it.\n");
    return 0;
}
