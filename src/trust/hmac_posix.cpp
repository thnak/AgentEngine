// Implements trust/hmac.hpp for Linux/POSIX -- decisions/ADR-107-hmac-sha256-linux-parity.md. The
// Windows sibling (hmac.cpp) uses CNG/BCrypt, a system API deliberately scoped Windows-only by
// ADR-005 §2/§3.3 for the cross-process capability-bearer-token design. This file exists NOT to widen
// that scope, but because a SEPARATE, unrelated consumer -- `trust/secret_quarantine.hpp`'s
// `QuarantineSecretStore::quarantine()`, a core turn-boundary secret-quarantine feature with no
// connection to ADR-005's own cross-process tokens -- also calls `hmac_sha256()`, and having zero
// Linux implementation broke every `AGENTENGINE_WITH_HTTPS`-gated Linux target that touches
// `QuarantineSecretStore` (found and disclosed, not fixed, by decisions/ADR-105-sandbox-tool-
// provider-composed-linux-parity.md §4 point 3 / §7).
//
// A from-scratch RFC 2104 HMAC construction, built on the same SHA-256 primitive
// `worktree_digest_posix.cpp` already established for Linux content-addressed digests
// (`include/agentengine/detail/sha256_posix.hpp`'s `sha256_raw()`, factored out of that file by this
// ADR so both callers share one audited compression-function implementation instead of two
// independent copies) -- no new third-party dependency, matching this project's existing Linux SHA-256
// posture (see worktree_digest_posix.cpp's own top comment for why OpenSSL/mbedTLS/AF_ALG were all
// rejected as the unconditional default path).
//
//   HMAC(K, m) = H((K' xor opad) || H((K' xor ipad) || m))
//
// where H is SHA-256, and K' is K zero-padded to the 64-byte SHA-256 block size (or H(K) zero-padded,
// if K is longer than 64 bytes) -- RFC 2104 §2's construction, verified against RFC 4231's published
// known-answer test vectors (tests/test_hmac_sha256.cpp, wired for both platforms since the vectors
// exercise the portable `hmac_sha256()` interface itself, not either platform's specific backend).
//
// Timing: every operation below -- the key-length comparison that selects K'=K vs. K'=H(K), the
// ipad/opad XOR, and `sha256_raw()`'s own compression function -- branches only on PUBLIC lengths
// (key_len, data_len, the fixed 64-byte block size), never on the SECRET key or message bytes'
// VALUES. `sha256_raw()`'s own top comment makes the same claim for its length-dependent control
// flow. This mirrors `hmac.cpp`'s BCrypt-backed sibling, which has no data-dependent branch either
// (BCrypt's HMAC implementation is opaque here, but this project's own from-scratch code carries the
// same discipline). `constant_time_equal()` (below, unchanged from `hmac.cpp`) remains the caller's
// required tool for comparing a computed MAC against an attacker-influenced one -- this function only
// computes a MAC, it never compares one.

#include "agentengine/trust/hmac.hpp"
#include "agentengine/detail/sha256_posix.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace agentengine::trust {

namespace {

inline constexpr std::size_t kBlockSize = 64; // SHA-256 block size, RFC 2104 §2

// RFC 2104 §2: K' is K zero-padded to the block size, or H(K) zero-padded, if K is longer than the
// block size. The `key_len > kBlockSize` branch here depends only on the (public) key LENGTH, never
// on the key's own byte values, so it introduces no content-based timing oracle.
std::array<std::uint8_t, kBlockSize> derive_key_block(std::uint8_t const* key, std::size_t key_len) {
    std::array<std::uint8_t, kBlockSize> k_prime{};
    if (key_len > kBlockSize) {
        auto const hashed = agentengine::detail::sha256_raw(
            std::as_bytes(std::span<std::uint8_t const>(key, key_len)));
        std::memcpy(k_prime.data(), hashed.data(), hashed.size());
    } else if (key_len > 0) {
        std::memcpy(k_prime.data(), key, key_len);
    }
    return k_prime;
}

std::array<std::byte, kBlockSize> xor_pad(std::array<std::uint8_t, kBlockSize> const& k_prime,
                                           std::uint8_t pad_byte) {
    std::array<std::byte, kBlockSize> out{};
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(k_prime[i] ^ pad_byte));
    }
    return out;
}

} // namespace

result<HmacSha256> hmac_sha256(std::uint8_t const* key, std::size_t key_len,
                                std::uint8_t const* data, std::size_t data_len) {
    // Guard `kBlockSize + data_len` (used below to size `inner_buf`) against overflowing
    // std::size_t. An unguarded overflow would silently undersize `inner_buf` relative to the
    // `data_len`-sized memcpy that follows -- a heap-buffer-overflow class defect (found via a
    // -Wstringop-overflow diagnostic at -O2 that plain -O0 does not surface, so it is easy to miss on
    // an unoptimized dev build). `data_len` this large can never correspond to a real,
    // dereferenceable buffer on any machine that exists (SIZE_MAX-64 bytes exceeds any real address
    // space), so this cannot be reached in practice -- but failing closed here costs nothing and
    // removes the latent UB rather than relying on that physical impossibility.
    if (data_len > std::numeric_limits<std::size_t>::max() - kBlockSize) {
        return std::unexpected(ae::error{
            failure_class::contract,
            "hmac_sha256: data_len too large (would overflow internal buffer size computation)",
            "hmac.data_len_overflow",
        });
    }

    auto const k_prime = derive_key_block(key, key_len);

    // inner = H((K' xor ipad) || data)
    auto const ipad_block = xor_pad(k_prime, 0x36);
    std::vector<std::byte> inner_buf(kBlockSize + data_len);
    std::memcpy(inner_buf.data(), ipad_block.data(), kBlockSize);
    if (data_len > 0) std::memcpy(inner_buf.data() + kBlockSize, data, data_len);
    auto const inner_hash = agentengine::detail::sha256_raw(inner_buf);

    // outer = H((K' xor opad) || inner)
    auto const opad_block = xor_pad(k_prime, 0x5c);
    std::array<std::byte, kBlockSize + kHmacSha256Bytes> outer_buf{};
    std::memcpy(outer_buf.data(), opad_block.data(), kBlockSize);
    std::memcpy(outer_buf.data() + kBlockSize, inner_hash.data(), inner_hash.size());
    auto const outer_hash = agentengine::detail::sha256_raw(outer_buf);

    HmacSha256 out{};
    std::memcpy(out.data(), outer_hash.data(), out.size());
    return out;
}

bool constant_time_equal(HmacSha256 const& a, HmacSha256 const& b) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < kHmacSha256Bytes; ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace agentengine::trust
