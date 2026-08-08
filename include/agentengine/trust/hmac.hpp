#pragma once
// A general-purpose HMAC-SHA256 primitive, extracted from `capability_token.hpp`/`.cpp` (ADR-005) so
// a SECOND, unrelated component (ADR-021's bearer-token validator, Milestone 7) can reuse the exact
// same audited implementation rather than re-deriving BCrypt provider setup and its own
// constant-time comparison from scratch -- ADR-021's own red-team pass named this directly: a fresh
// HMAC-comparison implementation is exactly the kind of thing that silently re-introduces a timing
// oracle the first time, even though this project already got it right once (ADR-005 §3.3).
//
// Windows-only for now (021 §2), via Windows CNG/BCrypt -- a system API, not a third-party
// dependency (CONVENTIONS' core tier: "std + Quark only, no third-party dependency, ever"). A Linux
// backend needs only a different provider (OpenSSL EVP or libsodium) behind these same two functions.

#include <array>
#include <cstdint>
#include <cstddef>

#include "agentengine/core/error.hpp"

namespace agentengine::trust {

inline constexpr std::size_t kHmacSha256Bytes = 32;
using HmacSha256 = std::array<std::uint8_t, kHmacSha256Bytes>;

// HMAC-SHA256(key, data) -> 32-byte MAC. One-shot: opens a fresh HMAC provider/hash object per call
// rather than caching a handle (ADR-005 §2's own "small-prove-scope, not fastest possible" choice,
// unchanged by this extraction).
[[nodiscard]] result<HmacSha256> hmac_sha256(std::uint8_t const* key, std::size_t key_len,
                                              std::uint8_t const* data, std::size_t data_len);

// Constant-time comparison (ADR-005 §3.3: "never leak a timing/content oracle back to a party that,
// by construction, never holds the key"). Every caller comparing a computed MAC against an
// attacker-influenced one MUST use this, never `==`/`memcmp`.
[[nodiscard]] bool constant_time_equal(HmacSha256 const& a, HmacSha256 const& b) noexcept;

} // namespace agentengine::trust
