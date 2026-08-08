#pragma once
// ADR-021's prove phase (decisions/ADR-021-inbound-protocol-trust-boundary.md): the inbound-identity
// bearer token mechanism Design A (first-party TLS+auth termination) depends on -- 018 §1's "OAuth
// 2.1 bearer" row, scoped down (ADR-021 §3) to a self-contained HMAC-SHA256 scheme with `exp`/`aud`/
// `iss`/`jti` claims, explicitly NOT a full OAuth 2.1 authorization-code/PKCE flow against a live
// external Authorization Server (that integration is a separate, later decision).
//
// Deliberately separate from `trust/capability_token.hpp` (ADR-005): that is a cross-process
// CAPABILITY delegation macaroon (proves "may this call use this specific attenuated authority");
// this is an inbound IDENTITY credential (proves "who is this caller"), 018 §2's "admission" half,
// upstream of any capability derivation. Different concept, same underlying HMAC-SHA256 primitive
// (trust/hmac.hpp), reused rather than re-derived -- ADR-021's own red-team pass named this directly.
//
// Closes four specific findings from ADR-021's red-team pass, each named at its point of enforcement
// below:
//   1. Algorithm confusion is impossible BY CONSTRUCTION, not by validation logic: `BearerToken`'s
//      wire shape has no algorithm-selector field at all. HMAC-SHA256 is the only algorithm this
//      type can express; there is nothing for an attacker to redirect a verifier's algorithm choice
//      to, because there is no algorithm choice. This constraint is BINDING on any future extension
//      of this mechanism (e.g. toward accepting real external-AS-issued tokens): the day this
//      verifier ever reads an algorithm/key identifier FROM the token to select how it's checked,
//      this property is gone and the classic alg-confusion/JWKS-substitution class becomes live.
//   2. Confused-deputy / audience validation: `expected_aud`/`expected_iss` are caller-supplied by
//      `verify_bearer_token()`'s own signature -- from static, per-route server configuration, never
//      derived from the token or the surrounding request. A verifier that instead read the expected
//      audience from request content would be validating a claim against itself.
//   3. Constant-time comparison: reuses `trust::constant_time_equal` (trust/hmac.hpp), the same
//      audited primitive ADR-005's capability token already uses, not a fresh `==`/`memcmp`.
//   4. Replay-within-validity-window: `ReplayGuard` rejects a reused `jti` while its `exp` is still
//      in the future, with a bound: an entry is pruned once its own `exp` passes, so memory is
//      bounded by the number of DISTINCT not-yet-expired tokens outstanding, never unbounded.
//      Explicitly named as a residual, not closed here: `ReplayGuard` is SINGLE-INSTANCE (an
//      in-process map) -- a token replayed against a DIFFERENT process (any horizontally-scaled
//      deployment) is not caught by this component. Externalizing the replay store is future work.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/hmac.hpp"

namespace agentengine::trust {

// Generated once per issuing host; never serialized, never transmitted (same posture as
// `capability_token.hpp`'s `SecretKey` -- possession of a token never implies possession of this).
// A distinct TYPE from `capability_token.hpp`'s `SecretKey`, even though the underlying bytes are the
// same shape, so a caller cannot accidentally use one seam's key to mint the other seam's tokens.
struct BearerSecretKey {
    std::array<std::uint8_t, 32> bytes{};
};

[[nodiscard]] result<BearerSecretKey> generate_bearer_secret_key();

struct BearerTokenClaims {
    std::string sub;         // the principal id this token asserts (018 SS1's "who is this caller")
    std::string tenant_id;   // 018 SS6: tenant is first-class, carried on the credential itself
    std::string aud;         // which service this token is valid for (confused-deputy control)
    std::string iss;         // which issuer minted it
    std::chrono::system_clock::time_point exp;
    std::string jti;         // unique id, for replay rejection (ReplayGuard)
};

struct BearerToken {
    BearerTokenClaims claims;
    HmacSha256          signature;  // HMAC-SHA256 over the canonical encoding of `claims`
};

// Only ever called by the issuing host (dev-mode self-issuance, or a future bridge that mints these
// after validating a real external AS's own token -- not built here, see file-top comment).
[[nodiscard]] result<BearerToken> mint_bearer_token(BearerSecretKey const& key, BearerTokenClaims claims);

// Bounded, single-instance replay cache -- see finding 4 above. Thread-safe (a real listener's
// per-connection handlers would call this concurrently).
class ReplayGuard {
public:
    // Returns true (and records `jti`) the FIRST time this exact `jti` is seen before its own `exp`;
    // returns false (a replay) if `jti` was already recorded and its recorded `exp` has not yet
    // passed. Also prunes every entry whose OWN `exp` is now in the past, so the map never grows
    // without bound from tokens that have long since expired.
    [[nodiscard]] bool check_and_record(std::string const& jti, std::chrono::system_clock::time_point exp,
                                         std::chrono::system_clock::time_point now);

    [[nodiscard]] std::size_t tracked_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point> seen_;
};

struct BearerVerificationRequest {
    // Both from STATIC, per-route server configuration -- see finding 2 above. Never populate these
    // from the token being verified, or from any other part of the inbound request.
    std::string                              expected_aud;
    std::string                              expected_iss;
    std::chrono::system_clock::time_point    now;
};

// Recomputes the expected signature from `key` and compares it against `token.signature` in constant
// time (finding 3), THEN checks `aud`/`iss`/`exp`/replay (finding 4) against `request`/`replay_guard`.
// All must hold. The returned error identifies which check failed at the class level only, never a
// byte-level mismatch detail -- the same "no timing/content oracle" discipline `capability_token.hpp`'s
// own `verify()` already documents.
[[nodiscard]] result<BearerTokenClaims> verify_bearer_token(BearerToken const& token, BearerSecretKey const& key,
                                                              BearerVerificationRequest const& request,
                                                              ReplayGuard& replay_guard);

} // namespace agentengine::trust
