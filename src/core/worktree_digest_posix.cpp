// Implements core/worktree.hpp's `compute_digest` -- SHA-256 for Linux/POSIX, via a self-contained,
// dependency-free FIPS 180-4 implementation (this project's own code, not a third-party library).
// The Windows sibling (worktree_digest.cpp) uses CNG/BCrypt -- a system API, not a third-party
// dependency, the same posture ADR-005's capability_token.cpp established for HMAC-SHA256. Linux has
// no equivalent zero-cost system API available unconditionally:
//   - OpenSSL is not vendored or find_package'd anywhere in this project's CMakeLists.txt; pulling it
//     in here would add a brand-new mandatory third-party dependency to every Linux build (this
//     target, agentengine_worktree_store, is built unconditionally -- no CMake option gates it, on
//     either WIN32 or NOT WIN32).
//   - mbedTLS IS already vendored (decisions/ADR-013-https-egress-tls-client.md), but strictly behind
//     `AGENTENGINE_WITH_HTTPS`, which defaults OFF (CMakeLists.txt) and is scoped to the egress
//     proxy's tier-2 "one heavy dependency per backend, behind a CMake option" posture
//     (CONVENTIONS.md "Dependency posture -- the three tiers"). compute_digest is called
//     unconditionally from put_blob/put_tree (worktree.hpp) -- making the worktree object store's
//     basic operation depend on an off-by-default flag would silently break it on a default Linux
//     build, and forcing the flag on would violate the Core tier's "std only, zero third-party
//     dependency, ever" for a target that content-addressing depends on everywhere.
//   - The Linux kernel crypto API (AF_ALG sockets, <linux/if_alg.h>) was considered as the closest
//     "system API" analog to BCrypt, and rejected: it needs the algif_hash kernel module (not
//     guaranteed loaded, especially in minimal containers), and this project's own sandboxing
//     posture (mediated_shell/native_jail's syscall-level restriction of raw socket use, 008) could
//     plausibly deny a bare socket() call, which would make content-addressing randomly fail inside
//     exactly the jailed environments this engine targets.
// A from-scratch, standard SHA-256 (FIPS 180-4 §6.2) avoids all three problems and carries no timing
// side-channel concern here: compute_digest hashes public content bytes for content-addressed
// storage, never a secret. Verified against the FIPS 180-4 / RFC 6234 published known-answer test
// vectors for the empty string and "abc" in tests/test_worktree_object_store.cpp -- a portable test
// that also runs against the Windows BCrypt implementation, proving both platforms agree with the
// real standard, not just with each other.
//
// The actual compression-function/padding logic (`sha256_raw`) has been factored out into
// `include/agentengine/detail/sha256_posix.hpp` (decisions/ADR-106-hmac-sha256-linux-parity.md) so
// `src/trust/hmac_posix.cpp`'s HMAC-SHA256 construction can reuse the identical, already-verified
// primitive rather than a second copy of SHA-256. This file now owns only the hex-encoding this
// digest store's own `Digest` type needs -- verified NOT to have changed `compute_digest`'s output by
// re-running the exact same FIPS 180-4/RFC 6234 known-answer vectors this file's own tests already
// covered (tests/test_worktree_object_store.cpp), unchanged, against a real Linux build.
//
// This translation unit has been built and run for real on Linux (WSL2 Ubuntu) as of ADR-106 --
// previously carried a note that it had only been self-reviewed, never built; that residual is now
// closed.

#include "agentengine/core/worktree.hpp"
#include "agentengine/detail/sha256_posix.hpp"

#include <array>
#include <cstdint>

namespace agentengine {

namespace {

char hex_digit(unsigned v) {
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + (v - 10));
}

std::string to_hex(std::array<std::uint8_t, 32> const& raw) {
    std::string out;
    out.reserve(64);
    for (auto b : raw) {
        out.push_back(hex_digit(b >> 4));
        out.push_back(hex_digit(b & 0x0F));
    }
    return out;
}

} // namespace

result<Digest> compute_digest(std::span<std::byte const> bytes) {
    return to_hex(agentengine::detail::sha256_raw(bytes));
}

} // namespace agentengine
