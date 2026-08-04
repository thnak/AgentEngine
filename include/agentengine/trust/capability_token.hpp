#pragma once
// Design A of decisions/ADR-005-capability-bearer-tokens-cross-process.md, resolving OQ-3
// (OpenQuestions.md), 007-Capability-and-Trust-Model.md §10 Q1, 008-Sandbox-and-Isolation.md §9 Q4,
// and 018-Identity-Authorization-and-Secrets.md §Q2: a capability that must cross a process
// boundary (the `remote` sandbox profile, delegated A2A calls, a remote plugin) represented as a
// self-verifying, macaroon-style bearer token with caveats, so the boundary needs no bespoke
// authority protocol of its own.
//
// Deliberately separate from trust/capability.hpp's in-process Capability/CapabilitySet, which stay
// unforgeable by construction (private handle types that never leave the process) and need no
// cryptography at all. A CapabilityToken exists ONLY where "unforgeable by the type system" is not
// available to the receiving side -- it must instead be unforgeable by cryptography. Only the
// minting host ever holds the SecretKey; every other party holds tokens only.
//
// Windows-only for now (021 §2: Windows is the only implementation target; HMAC-SHA256 is provided
// by Windows CNG/BCrypt, a system API, not a third-party dependency -- same posture as
// src/backends/native_jail/job_object_limits.hpp's direct use of Job Objects). The token *shape* and
// chaining algorithm are platform-independent; a Linux backend needs only a different HMAC provider
// (e.g. OpenSSL EVP or libsodium) behind the same three free functions below.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

inline constexpr std::size_t kMacBytes = 32; // HMAC-SHA256 output size

using Mac = std::array<std::uint8_t, kMacBytes>;

// The root secret a single minting host holds. Generated once per session/run (ADR-005 §3.1);
// never serialized, never transmitted -- possession of a token never implies possession of this.
struct SecretKey {
    std::array<std::uint8_t, 32> bytes{};
};

result<SecretKey> generate_secret_key();

// A caveat narrows what a token authorizes. Attenuation-only (007 §3 rule 2): every caveat is a
// restriction, evaluated with logical AND against every other caveat and against the root grant --
// there is no caveat that widens. Deliberately minimal set for this ADR's small-prove scope
// (ADR-005 §2); a real deployment's caveat vocabulary is a superset, not a different mechanism.
struct ExpiresAt {
    std::chrono::system_clock::time_point time;
};
struct PathPrefix {
    // Must already be canonicalized by the caller (021 §4: "paths are a type, not a string") --
    // this caveat does its own prefix check only, and does not itself canonicalize. Out of scope
    // for this ADR; ADR-005 §9 names it as a residual risk if this caveat is used before a real
    // Path type (025) exists.
    std::string prefix;
};

using Caveat = std::variant<ExpiresAt, PathPrefix>;

struct CapabilityToken {
    capability_kind kind;
    std::string param;           // e.g. a mount id -- opaque, kind-specific (007 §3)
    std::vector<Caveat> caveats; // applied in mint order; verify() replays this exact order
    Mac signature;               // HMAC chain over (kind, param, caveats) -- ADR-005 §3.2
};

// What a party evaluating a token is actually trying to do -- the "requested effect" checked
// against the token's kind and every caveat.
struct EvaluationRequest {
    capability_kind kind;
    std::string path;                              // checked against every PathPrefix caveat
    std::chrono::system_clock::time_point now;      // checked against every ExpiresAt caveat
};

// Mints a fresh root token. Only ever called by the host holding `key`.
result<CapabilityToken> mint_root(SecretKey const& key, capability_kind kind, std::string param);

// Produces a strictly narrower token by appending one caveat. Does NOT require `key` -- this is the
// operation a party that holds a token but never the key can perform itself when delegating a
// sub-scope onward, which is the entire point of a self-verifying bearer token (007 Q1's "without a
// bespoke protocol"). The new signature folds the caveat in via HMAC chaining, so a party without
// `key` can compute it, and cannot invert the chain to strip a caveat back off (ADR-005 §3.2, §6.1).
result<CapabilityToken> attenuate(CapabilityToken const& parent, Caveat caveat);

// Recomputes the expected signature chain from `key` and compares it against `token.signature` in
// constant time (ADR-005 §3.3), THEN checks every caveat against `request`. Both must hold. The
// returned error identifies which check failed at the class level only (signature vs. a specific
// caveat kind), never which byte of the signature mismatched -- that would leak a timing/content
// oracle back to a party that, by construction, never holds `key`.
result<void> verify(CapabilityToken const& token, SecretKey const& key,
                     EvaluationRequest const& request);

} // namespace agentengine::trust
