// Real, permanent regression test for trust/hmac.hpp's hmac_sha256()/constant_time_equal() --
// decisions/ADR-106-hmac-sha256-linux-parity.md. Portable: this test only calls the two functions
// hmac.hpp declares, never anything platform-specific, so it is wired into tests/CMakeLists.txt for
// BOTH Windows (hmac.cpp, CNG/BCrypt) and Linux (hmac_posix.cpp, a from-scratch RFC 2104
// construction) -- the same source proving both backends agree with the real standard, the same
// posture tests/test_worktree_object_store.cpp already established for compute_digest()'s own
// Windows/Linux SHA-256 backends.
//
// Prior to this ADR, hmac_sha256() had only INDIRECT coverage (test_capability_token_proof.cpp,
// test_capability_token_redteam.cpp, test_secret_quarantine.cpp, test_rt_agent_session_quarantine_
// tool.cpp -- all Windows-only until ADR-106) -- no test exercised the primitive itself against a
// published, independent standard. This file closes that gap directly.
//
// Vectors are RFC 4231's own published known-answer test cases (§4.2-4.8), transcribed from the
// authoritative RFC text (https://www.rfc-editor.org/rfc/rfc4231.txt), not from memory -- each
// vector's byte length was verified (64 hex chars = 32 bytes, or 32 hex chars = 16 bytes for the
// deliberately-truncated Test Case 5) before use. A negative control (temporarily corrupting the
// Linux implementation's ipad constant during this ADR's own development) confirmed every one of
// these checks actually fails when the implementation is wrong -- this project's own standard, "a
// test that cannot fail proves nothing."

#include "agentengine/trust/hmac.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace agentengine::trust;

namespace {

int g_failures = 0;

void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::vector<std::uint8_t> from_hex(std::string const& hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::vector<std::uint8_t> repeat_byte(std::uint8_t b, std::size_t n) {
    return std::vector<std::uint8_t>(n, b);
}

std::vector<std::uint8_t> ascii(std::string const& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string to_hex(HmacSha256 const& mac) {
    static char const* digits = "0123456789abcdef";
    std::string out;
    out.reserve(kHmacSha256Bytes * 2);
    for (auto b : mac) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

// Runs one RFC 4231 vector. `compare_bytes` lets Test Case 5's deliberately-truncated 128-bit MAC
// compare only its own published prefix, matching what RFC 4231 itself publishes for that case.
void check_vector(char const* label, std::vector<std::uint8_t> const& key,
                   std::vector<std::uint8_t> const& data, std::string const& expected_hex,
                   std::size_t compare_bytes = kHmacSha256Bytes) {
    auto result = hmac_sha256(key.empty() ? nullptr : key.data(), key.size(),
                               data.empty() ? nullptr : data.data(), data.size());
    check(result.has_value(), label);
    if (!result.has_value()) return;

    std::string got = to_hex(*result).substr(0, compare_bytes * 2);
    check(got == expected_hex, label);
}

} // namespace

int main() {
    // ---- RFC 4231 §4.2 Test Case 1 ----
    check_vector("RFC4231 Test Case 1 (20-byte key)",
                 from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), ascii("Hi There"),
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // ---- RFC 4231 §4.3 Test Case 2 (key shorter than the HMAC output) ----
    check_vector("RFC4231 Test Case 2 (short key, \"Jefe\")", ascii("Jefe"),
                 ascii("what do ya want for nothing?"),
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // ---- RFC 4231 §4.4 Test Case 3 (key+data combined > block size, 20-byte key) ----
    check_vector("RFC4231 Test Case 3 (20-byte key, 50-byte data)", repeat_byte(0xaa, 20),
                 repeat_byte(0xdd, 50),
                 "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // ---- RFC 4231 §4.5 Test Case 4 (25-byte key) ----
    check_vector("RFC4231 Test Case 4 (25-byte key)",
                 from_hex("0102030405060708090a0b0c0d0e0f10111213141516171819"), repeat_byte(0xcd, 50),
                 "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

    // ---- RFC 4231 §4.6 Test Case 5 (published truncated to 128 bits) ----
    check_vector("RFC4231 Test Case 5 (128-bit truncated MAC)", repeat_byte(0x0c, 20),
                 ascii("Test With Truncation"), "a3b6167473100ee06e0c796c2955552b", 16);

    // ---- RFC 4231 §4.7 Test Case 6 (key = 131 bytes > block size -- exercises K'=H(K)) ----
    check_vector("RFC4231 Test Case 6 (key_len=131 > block size, K'=H(K) branch)", repeat_byte(0xaa, 131),
                 ascii("Test Using Larger Than Block-Size Key - Hash Key First"),
                 "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // ---- RFC 4231 §4.8 Test Case 7 (key AND data both > block size) ----
    check_vector(
        "RFC4231 Test Case 7 (key AND data both > block size)", repeat_byte(0xaa, 131),
        ascii("This is a test using a larger than block-size key and a larger than block-size "
              "data. The key needs to be hashed before being used by the HMAC algorithm."),
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

    // ---- Edge cases beyond RFC 4231's own numbered vectors ----
    {
        auto result = hmac_sha256(nullptr, 0, reinterpret_cast<std::uint8_t const*>("x"), 1);
        check(result.has_value(), "empty key (key_len=0) does not crash and produces a MAC");
    }
    {
        std::string key = "somekey";
        auto result =
            hmac_sha256(reinterpret_cast<std::uint8_t const*>(key.data()), key.size(), nullptr, 0);
        check(result.has_value(), "empty data (data_len=0) does not crash and produces a MAC");
    }
    {
        // Key exactly 64 bytes -- the boundary of the K'=H(K) branch (key_len > 64 vs. <= 64).
        auto key = repeat_byte(0xbb, 64);
        std::string data = "boundary test, key == block size exactly";
        auto result = hmac_sha256(key.data(), key.size(),
                                   reinterpret_cast<std::uint8_t const*>(data.data()), data.size());
        check(result.has_value(), "key exactly 64 bytes (SHA-256 block-size boundary) does not crash");
    }
    {
        // Determinism: same input twice must give the same output.
        auto key = ascii("determinism-key");
        auto data = ascii("determinism-data");
        auto r1 = hmac_sha256(key.data(), key.size(), data.data(), data.size());
        auto r2 = hmac_sha256(key.data(), key.size(), data.data(), data.size());
        check(r1.has_value() && r2.has_value() && to_hex(*r1) == to_hex(*r2),
              "same (key, data) computed twice yields the identical MAC");
    }
    {
        // Different keys must give different MACs -- sanity: the key is not silently ignored.
        auto key1 = ascii("key-one");
        auto key2 = ascii("key-two");
        auto data = ascii("same data");
        auto r1 = hmac_sha256(key1.data(), key1.size(), data.data(), data.size());
        auto r2 = hmac_sha256(key2.data(), key2.size(), data.data(), data.size());
        check(r1.has_value() && r2.has_value() && to_hex(*r1) != to_hex(*r2),
              "different keys over the same data yield different MACs");
    }

    // ---- constant_time_equal() (unchanged by ADR-106, already portable -- proven here too) --------
    {
        auto key = ascii("ct-key");
        auto data = ascii("ct-data");
        auto r1 = hmac_sha256(key.data(), key.size(), data.data(), data.size());
        auto r2 = hmac_sha256(key.data(), key.size(), data.data(), data.size());
        check(r1.has_value() && r2.has_value() && constant_time_equal(*r1, *r2),
              "constant_time_equal() reports equal MACs from identical (key, data) as equal");

        HmacSha256 flipped = *r1;
        flipped[0] ^= 0x01;
        check(!constant_time_equal(*r1, flipped),
              "constant_time_equal() reports a single-bit difference as unequal");
    }

    if (g_failures == 0) {
        std::printf("test_hmac_sha256: all checks passed\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
