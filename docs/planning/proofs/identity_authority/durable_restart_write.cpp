// PROVE-PHASE PROBE: "Process 1" of a REAL process restart (a genuinely separate OS process from
// durable_restart_read.cpp, not an in-process simulation) -- proves the §33/§34 fix by re-running
// §33's own exact attack scenario against the now-durable IdentityAuthority and showing it now
// closes cleanly.
//
// Configures a durable directory as the FIRST bootstrap() call in this process (mattering only on
// first call, per the fix's own "first call wins" semantics), adopts "alice" (the durable bridge
// path §33.2/§33.3 showed was the realistic attack surface, not just mint_root()), and mints a
// couple of plain root principals too (to burn some ids, proving the high-water-mark genuinely
// advances and is not reset by an unrelated mint_root() call).

#include "identity_authority.hpp"

#include <cstdio>
#include <filesystem>

int main() {
    using namespace probe;

    std::filesystem::path const durable_dir = "ae_durable_identity_probe";
    std::error_code ec;
    std::filesystem::remove_all(durable_dir, ec);

    IdentityAuthority& authority = IdentityAuthority::bootstrap(durable_dir);

    // Burn a couple of ids via plain mint_root() first, unrelated to Alice -- proves the
    // high-water-mark tracks EVERY allocation, not just adopt()'s.
    Principal noise1 = authority.mint_root("unrelated-noise-1");
    Principal noise2 = authority.mint_root("unrelated-noise-2");

    Principal alice = authority.adopt("alice", "");
    std::printf("[write] noise1 id=%llu, noise2 id=%llu, alice id=%llu (durable dir=%s)\n",
                static_cast<unsigned long long>(noise1.id()),
                static_cast<unsigned long long>(noise2.id()),
                static_cast<unsigned long long>(alice.id()), durable_dir.string().c_str());
    std::printf("[write] (this process now exits -- IdentityAuthority's in-memory state dies with "
                "it; only identity_next_id.txt and identity_adopted.log survive on disk)\n");
    return 0;
}
