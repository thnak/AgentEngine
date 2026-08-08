// Implements trust/capability_token.hpp. See that header for the spec/ADR citations this satisfies.
//
// HMAC-SHA256 via Windows CNG/BCrypt (system API, not a third-party dependency -- 021 §2/CONVENTIONS
// tier posture, same as job_object_limits.cpp's direct use of Job Objects). The signature chain is
// the macaroon construction: sig_0 = HMAC(root_key, encode(kind, param)); sig_i = HMAC(sig_{i-1},
// encode(caveat_i)) -- each step's output becomes the next step's HMAC key, so a party that only
// ever holds sig_i can extend the chain (attenuate) but cannot invert it to recover sig_{i-1} or the
// root key (ADR-005 §3.2, §6.1).

#include "agentengine/trust/capability_token.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <cstring>
#include <utility>

#include "agentengine/trust/hmac.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace agentengine::trust {

namespace {

std::unexpected<ae::error> bcrypt_error(char const* what, NTSTATUS status) {
    return std::unexpected(ae::error{
        failure_class::fatal,
        std::string(what) + " failed: NTSTATUS 0x" + std::to_string(static_cast<unsigned long>(status)),
        "capability_token.bcrypt_failure",
    });
}

// Milestone 7 (ADR-021): `hmac_sha256`/`constant_time_equal` moved to trust/hmac.hpp so a second,
// unrelated component can reuse the exact same audited implementation. This file keeps its own
// `bcrypt_error` (used by `generate_secret_key()`'s own direct `BCryptGenRandom` call below, which
// hmac.hpp has no reason to know about) rather than sharing a five-line formatter across an
// unrelated concern.

// ---- Canonical encoding (ADR-005 §3.2) --------------------------------------------------------
// Every variable-length field is length-prefixed (u32, little-endian) so no two distinct
// (kind, param, caveats) tuples can ever encode to the same byte string -- the property that makes
// "sign the encoding" actually bind to the whole logical value rather than to an ambiguous
// concatenation (e.g. without length prefixes, param="ab",caveat_prefix="c" would collide with
// param="a",caveat_prefix="bc").

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

std::vector<std::uint8_t> encode_root(capability_kind kind, std::string const& param) {
    std::vector<std::uint8_t> out;
    put_u32(out, static_cast<std::uint32_t>(kind));
    put_str(out, param);
    return out;
}

std::vector<std::uint8_t> encode_caveat(Caveat const& caveat) {
    std::vector<std::uint8_t> out;
    if (auto const* exp = std::get_if<ExpiresAt>(&caveat)) {
        out.push_back(0); // type tag
        auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(exp->time.time_since_epoch())
                .count());
        put_u64(out, ticks);
    } else if (auto const* pfx = std::get_if<PathPrefix>(&caveat)) {
        out.push_back(1);
        put_str(out, pfx->prefix);
    }
    return out;
}

} // namespace

result<SecretKey> generate_secret_key() {
    SecretKey key{};
    NTSTATUS status = BCryptGenRandom(nullptr, key.bytes.data(),
                                       static_cast<ULONG>(key.bytes.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return bcrypt_error("BCryptGenRandom", status);
    }
    return key;
}

result<CapabilityToken> mint_root(SecretKey const& key, capability_kind kind, std::string param) {
    auto encoded = encode_root(kind, param);
    auto sig = hmac_sha256(key.bytes.data(), key.bytes.size(), encoded.data(), encoded.size());
    if (!sig.has_value()) {
        return std::unexpected(sig.error());
    }
    CapabilityToken token{};
    token.kind = kind;
    token.param = std::move(param);
    token.signature = *sig;
    return token;
}

result<CapabilityToken> attenuate(CapabilityToken const& parent, Caveat caveat) {
    auto encoded = encode_caveat(caveat);
    // Folding with the PARENT's signature as the HMAC key is the macaroon step: only someone who
    // already holds a valid signature for the parent scope can produce a valid signature for any
    // narrowing of it, and holding the child's signature never yields the parent's (HMAC is not
    // invertible) -- ADR-005 §3.2. The only way this can fail is a BCrypt provider outage, which is
    // surfaced rather than swallowed into a token that looks well-formed but isn't.
    auto sig = hmac_sha256(parent.signature.data(), parent.signature.size(),
                            encoded.data(), encoded.size());
    if (!sig.has_value()) {
        return std::unexpected(sig.error());
    }
    CapabilityToken child = parent;
    child.caveats.push_back(std::move(caveat));
    child.signature = *sig;
    return child;
}

result<void> verify(CapabilityToken const& token, SecretKey const& key,
                     EvaluationRequest const& request) {
    auto encoded = encode_root(token.kind, token.param);
    auto running = hmac_sha256(key.bytes.data(), key.bytes.size(), encoded.data(), encoded.size());
    if (!running.has_value()) {
        return std::unexpected(running.error());
    }
    for (auto const& caveat : token.caveats) {
        auto step_encoded = encode_caveat(caveat);
        running = hmac_sha256(running->data(), running->size(),
                               step_encoded.data(), step_encoded.size());
        if (!running.has_value()) {
            return std::unexpected(running.error());
        }
    }

    if (!constant_time_equal(*running, token.signature)) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability token signature mismatch",
                                          "capability_token.bad_signature"});
    }

    if (token.kind != request.kind) {
        return std::unexpected(ae::error{failure_class::policy,
                                          "capability token kind does not match requested effect",
                                          "capability_token.kind_mismatch"});
    }

    for (auto const& caveat : token.caveats) {
        if (auto const* exp = std::get_if<ExpiresAt>(&caveat)) {
            if (request.now >= exp->time) {
                return std::unexpected(ae::error{failure_class::policy,
                                                  "capability token expired",
                                                  "capability_token.expired"});
            }
        } else if (auto const* pfx = std::get_if<PathPrefix>(&caveat)) {
            if (request.path.compare(0, pfx->prefix.size(), pfx->prefix) != 0) {
                return std::unexpected(ae::error{failure_class::policy,
                                                  "capability token path-prefix caveat not satisfied",
                                                  "capability_token.path_denied"});
            }
        }
    }

    return {};
}

} // namespace agentengine::trust
