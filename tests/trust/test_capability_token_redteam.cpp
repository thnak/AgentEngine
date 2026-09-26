// Adversarial claims for decisions/ADR-005-capability-bearer-tokens-cross-process.md — attacks a
// party holding tokens/refs but never the CapabilityToken SecretKey (Design A) or acting as an
// unprivileged remote caller of CapabilityRegistry (Design B) would attempt. Every R-x check's
// "ok" means the attack was rejected; R-A0/R-B0 are positive controls proving a genuinely valid
// use still succeeds under the same harness, so a rejection above isn't just "everything fails."

#include <chrono>
#include <iostream>
#include <string>

#include "agentengine/trust/capability_registry.hpp"
#include "agentengine/trust/capability_token.hpp"

using namespace agentengine;
using namespace agentengine::trust;
using Clock = std::chrono::system_clock;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

} // namespace

int main() {
    auto key = *generate_secret_key();
    auto now = Clock::now();

    auto root = *mint_root(key, capability_kind::fs_read, "workspace-mount-1");
    auto scoped = *attenuate(root, PathPrefix{"/workspace/reports/"});

    // R-A0 (positive control): the untampered, correctly-attenuated token still verifies.
    {
        auto ok = verify(scoped, key,
                          EvaluationRequest{capability_kind::fs_read,
                                            "/workspace/reports/x.csv", now});
        AE_CHECK(ok.has_value(), "R-A0 (positive control): untampered scoped token verifies");
    }

    // R-A1: flip one bit of the signature.
    {
        CapabilityToken forged = scoped;
        forged.signature[0] ^= 0x01;
        auto denied = verify(forged, key,
                              EvaluationRequest{capability_kind::fs_read,
                                                "/workspace/reports/x.csv", now});
        AE_CHECK(!denied.has_value() && denied.error().code == "capability_token.bad_signature",
                 "R-A1: single-bit signature flip is rejected");
    }

    // R-A2: tamper the root `param` after minting, without recomputing the signature.
    {
        CapabilityToken forged = scoped;
        forged.param = "workspace-mount-99";
        auto denied = verify(forged, key,
                              EvaluationRequest{capability_kind::fs_read,
                                                "/workspace/reports/x.csv", now});
        AE_CHECK(!denied.has_value(), "R-A2: tampering the signed param is rejected");
    }

    // R-A3: tamper a caveat's payload post-attenuation (widen the prefix back toward root) without
    // recomputing the signature -- this is the "strip/loosen a caveat" attack the HMAC chain exists
    // to prevent (ADR-005 §3.2, §6.1).
    {
        CapabilityToken forged = scoped;
        auto* pfx = std::get_if<PathPrefix>(&forged.caveats[0]);
        AE_CHECK(pfx != nullptr, "R-A3: fixture has the expected caveat shape");
        pfx->prefix = "/"; // attempt to widen back to root scope
        auto denied = verify(forged, key,
                              EvaluationRequest{capability_kind::fs_read,
                                                "/etc/passwd", now});
        AE_CHECK(!denied.has_value(), "R-A3: widening a caveat's payload is rejected");
    }

    // R-A4: strip the caveat entirely (truncate the vector), presenting the root-looking fields
    // but keeping the *scoped* token's signature.
    {
        CapabilityToken forged = scoped;
        forged.caveats.clear();
        auto denied = verify(forged, key,
                              EvaluationRequest{capability_kind::fs_read, "/etc/passwd", now});
        AE_CHECK(!denied.has_value(), "R-A4: stripping a caveat while keeping the child signature is rejected");
    }

    // R-A5: reorder caveats (only meaningful with two -- add a second, then swap).
    {
        auto two_caveat = *attenuate(scoped, ExpiresAt{now + std::chrono::minutes(5)});
        CapabilityToken reordered = two_caveat;
        std::swap(reordered.caveats[0], reordered.caveats[1]);
        auto denied = verify(reordered, key,
                              EvaluationRequest{capability_kind::fs_read,
                                                "/workspace/reports/x.csv", now});
        AE_CHECK(!denied.has_value(), "R-A5: reordering caveats invalidates the signature");
    }

    // R-A6: a party without `key` cannot mint a fresh, validly-signed root from nothing -- attenuate
    // a fabricated (garbage-signature) token and confirm the result still fails verify(), i.e.
    // attenuate() cannot launder an invalid parent into a valid child.
    {
        CapabilityToken fabricated{};
        fabricated.kind = capability_kind::fs_read;
        fabricated.param = "workspace-mount-1";
        fabricated.signature.fill(0x42); // guessed, not derived from `key`
        auto child = attenuate(fabricated, PathPrefix{"/"});
        AE_CHECK(child.has_value(), "R-A6: attenuate() itself succeeds (it never checks the parent)");
        auto denied = verify(*child, key,
                              EvaluationRequest{capability_kind::fs_read, "/etc/passwd", now});
        AE_CHECK(!denied.has_value(), "R-A6: a token derived from a fabricated parent still fails verify()");
    }

    // ---- Design B (CapabilityRegistry) equivalents ---------------------------------------------
    CapabilityRegistry registry;
    auto ref = *registry.grant(capability_kind::fs_read, "workspace-mount-1",
                                "/workspace/reports/", now + std::chrono::minutes(5));

    // R-B0 (positive control): the genuinely granted ref checks out.
    {
        auto ok = registry.check(ref, capability_kind::fs_read, "/workspace/reports/x.csv", now);
        AE_CHECK(ok.has_value(), "R-B0 (positive control): genuinely granted ref checks out");
    }

    // R-B1: an unknown/guessed ref (never granted) is rejected.
    {
        auto denied = registry.check("00000000000000000000000000000000", capability_kind::fs_read,
                                      "/workspace/reports/x.csv", now);
        AE_CHECK(!denied.has_value() && denied.error().code == "capability_registry.unknown_ref",
                 "R-B1: an unknown ref is rejected");
    }

    // R-B2: a revoked ref is rejected immediately, even though it was valid a moment earlier --
    // this is the property Design A structurally lacks (a minted token verifies until its own
    // caveats expire; it cannot be revoked early without host-side state, ADR-005 §6.2).
    {
        auto revocable = *registry.grant(capability_kind::fs_read, "workspace-mount-1", "",
                                          now + std::chrono::minutes(5));
        auto before = registry.check(revocable, capability_kind::fs_read, "/any", now);
        AE_CHECK(before.has_value(), "R-B2 setup: ref valid before revocation");
        registry.revoke(revocable);
        auto after = registry.check(revocable, capability_kind::fs_read, "/any", now);
        AE_CHECK(!after.has_value() && after.error().code == "capability_registry.unknown_ref",
                 "R-B2: a revoked ref is rejected immediately");
    }

    // R-B3: deriving an attenuated ref that tries to WIDEN past the parent (shorter/unrelated
    // prefix, or a later expiry) is rejected host-side, even though the caller never presents
    // anything but the opaque parent ref.
    {
        auto widen_prefix = registry.derive_attenuated(ref, "/", now);
        AE_CHECK(!widen_prefix.has_value() && widen_prefix.error().code == "capability_registry.widen_rejected",
                 "R-B3a: deriving a wider prefix than the parent is rejected");
        auto widen_expiry = registry.derive_attenuated(ref, "/workspace/reports/deep/",
                                                        now + std::chrono::minutes(999));
        AE_CHECK(!widen_expiry.has_value() && widen_expiry.error().code == "capability_registry.widen_rejected",
                 "R-B3b: deriving a later expiry than the parent is rejected");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All capability_token/registry red-team checks passed.\n";
    return 0;
}
