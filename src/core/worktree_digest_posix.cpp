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
// NOTE: this translation unit is Linux/POSIX-only (compiled only under NOT WIN32, CMakeLists.txt)
// and has not been built or run in this session -- no Linux build environment was available. It has
// been carefully self-reviewed against FIPS 180-4's published pseudocode but needs a real Linux
// CI/build pass (and the known-answer test actually executing there) before being trusted.

#include "agentengine/core/worktree.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace agentengine {

namespace {

constexpr std::array<std::uint32_t, 8> kInitialHash{
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

std::uint32_t rotr32(std::uint32_t x, int n) { return std::rotr(x, n); }

// Reads 4 big-endian bytes starting at `p` into a std::uint32_t (FIPS 180-4 represents a message as
// a sequence of big-endian 32-bit words).
std::uint32_t load_be32(std::byte const* p) {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

void store_be32(std::uint32_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>(v >> 24);
    out[1] = static_cast<std::uint8_t>(v >> 16);
    out[2] = static_cast<std::uint8_t>(v >> 8);
    out[3] = static_cast<std::uint8_t>(v);
}

// Processes one 64-byte block, updating `state` in place -- FIPS 180-4 §6.2.2's compression
// function, following the algorithm's own SHA-256 pseudocode step for step (message schedule
// expansion, then 64 working-variable rounds, then feed-forward addition into `state`).
void process_block(std::array<std::uint32_t, 8>& state, std::byte const* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
    for (std::size_t i = 16; i < 64; ++i) {
        auto const s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        auto const s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = state[0], b = state[1], c = state[2], d = state[3];
    auto e = state[4], f = state[5], g = state[6], h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        auto const s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        auto const ch = (e & f) ^ (~e & g);
        auto const temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        auto const s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        auto const maj = (a & b) ^ (a & c) ^ (b & c);
        auto const temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

char hex_digit(unsigned v) {
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + (v - 10));
}

std::string to_hex(std::array<std::uint32_t, 8> const& state) {
    std::string out;
    out.reserve(64);
    for (auto word : state) {
        std::uint8_t bytes[4];
        store_be32(word, bytes);
        for (auto b : bytes) {
            out.push_back(hex_digit(b >> 4));
            out.push_back(hex_digit(b & 0x0F));
        }
    }
    return out;
}

} // namespace

result<Digest> compute_digest(std::span<std::byte const> bytes) {
    std::array<std::uint32_t, 8> state = kInitialHash;

    std::size_t const full_blocks = bytes.size() / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        process_block(state, bytes.data() + i * 64);
    }

    // Final padded block(s) -- FIPS 180-4 §5.1.1: append a single '1' bit (the 0x80 byte, since the
    // message length is always a whole number of bytes here), then zero bits, then the 64-bit
    // big-endian bit length, so the total length is a multiple of 64 bytes. `tail` holds the
    // remaining (< 64) message bytes plus that padding, laid out as one contiguous zero-initialized
    // buffer big enough for the worst case (a 64-byte tail needs a second whole block for the length
    // field): the zero bytes between the 0x80 marker and the length field come for free from the
    // buffer's zero-initialization, so nothing else needs to explicitly zero them.
    std::size_t const tail_len = bytes.size() - full_blocks * 64;
    std::array<std::byte, 128> tail{};
    if (tail_len > 0) std::memcpy(tail.data(), bytes.data() + full_blocks * 64, tail_len);
    tail[tail_len] = std::byte{0x80};

    std::size_t const padded_len = (tail_len + 1 <= 56) ? 64 : 128;
    std::uint64_t const bit_len = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 1 - i] = static_cast<std::byte>((bit_len >> (i * 8)) & 0xFF);
    }

    process_block(state, tail.data());
    if (padded_len == 128) process_block(state, tail.data() + 64);

    return to_hex(state);
}

} // namespace agentengine
