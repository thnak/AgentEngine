// Implements trust/bearer_token.hpp. See that header for the ADR-021 red-team findings this closes.

#include "agentengine/trust/bearer_token.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <iterator>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace agentengine::trust {

namespace {

std::unexpected<ae::error> bcrypt_error(char const* what, NTSTATUS status) {
    return std::unexpected(ae::error{
        failure_class::fatal,
        std::string(what) + " failed: NTSTATUS 0x" + std::to_string(static_cast<unsigned long>(status)),
        "bearer_token.bcrypt_failure",
    });
}

// ---- Canonical encoding ------------------------------------------------------------------------
// Same length-prefixed discipline `capability_token.cpp`'s own `encode_root`/`encode_caveat` use
// (ADR-005 §3.2): every variable-length field is length-prefixed (u32, little-endian) so no two
// distinct claim sets can ever encode to the same byte string.

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_str(std::vector<std::uint8_t>& out, std::string const& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<std::uint8_t> encode_claims(BearerTokenClaims const& c) {
    std::vector<std::uint8_t> out;
    put_str(out, c.sub);
    put_str(out, c.tenant_id);
    put_str(out, c.aud);
    put_str(out, c.iss);
    auto ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(c.exp.time_since_epoch()).count());
    put_u64(out, ticks);
    put_str(out, c.jti);
    // Deliberately NOT length-prefixed with a leading algorithm/version byte -- see the header's own
    // finding 1: there is nothing in this encoding a verifier could read to select a DIFFERENT
    // algorithm than HMAC-SHA256, because the encoding never carries one.
    return out;
}

} // namespace

result<BearerSecretKey> generate_bearer_secret_key() {
    BearerSecretKey key{};
    NTSTATUS status = BCryptGenRandom(nullptr, key.bytes.data(),
                                       static_cast<ULONG>(key.bytes.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return bcrypt_error("BCryptGenRandom", status);
    }
    return key;
}

result<BearerToken> mint_bearer_token(BearerSecretKey const& key, BearerTokenClaims claims) {
    auto encoded = encode_claims(claims);
    auto sig = hmac_sha256(key.bytes.data(), key.bytes.size(), encoded.data(), encoded.size());
    if (!sig.has_value()) {
        return std::unexpected(sig.error());
    }
    BearerToken token{};
    token.claims    = std::move(claims);
    token.signature = *sig;
    return token;
}

bool ReplayGuard::check_and_record(std::string const& jti, std::chrono::system_clock::time_point exp,
                                    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Prune every entry whose OWN exp has passed -- bounds memory by the number of DISTINCT
    // not-yet-expired jtis outstanding (header's own finding 4), not by total tokens ever seen.
    for (auto it = seen_.begin(); it != seen_.end();) {
        it = (it->second <= now) ? seen_.erase(it) : std::next(it);
    }
    auto it = seen_.find(jti);
    if (it != seen_.end()) {
        return false;  // a jti already tracked (and, by the prune above, still un-expired) is a replay
    }
    seen_.emplace(jti, exp);
    return true;
}

std::size_t ReplayGuard::tracked_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_.size();
}

result<BearerTokenClaims> verify_bearer_token(BearerToken const& token, BearerSecretKey const& key,
                                               BearerVerificationRequest const& request,
                                               ReplayGuard& replay_guard) {
    auto encoded = encode_claims(token.claims);
    auto expected = hmac_sha256(key.bytes.data(), key.bytes.size(), encoded.data(), encoded.size());
    if (!expected.has_value()) {
        return std::unexpected(expected.error());
    }
    if (!constant_time_equal(*expected, token.signature)) {
        return std::unexpected(ae::error{failure_class::policy, "bearer token signature mismatch",
                                          "bearer_token.bad_signature"});
    }

    if (token.claims.exp <= request.now) {
        return std::unexpected(
            ae::error{failure_class::policy, "bearer token expired", "bearer_token.expired"});
    }
    if (token.claims.aud != request.expected_aud) {
        return std::unexpected(ae::error{failure_class::policy, "bearer token audience mismatch",
                                          "bearer_token.wrong_audience"});
    }
    if (token.claims.iss != request.expected_iss) {
        return std::unexpected(
            ae::error{failure_class::policy, "bearer token issuer mismatch", "bearer_token.wrong_issuer"});
    }
    if (!replay_guard.check_and_record(token.claims.jti, token.claims.exp, request.now)) {
        return std::unexpected(
            ae::error{failure_class::policy, "bearer token jti already used (replay)",
                      "bearer_token.replayed"});
    }

    return token.claims;
}

} // namespace agentengine::trust
