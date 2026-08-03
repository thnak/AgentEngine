// Correctness claims for decisions/ADR-005-capability-bearer-tokens-cross-process.md Design A
// (trust/capability_token.hpp). T-C5 is a positive control (022 §5): it deliberately supplies the
// wrong key so this test file can prove to itself that AE_CHECK actually catches a failure, not
// just that every real case happens to pass.

#include <chrono>
#include <iostream>
#include <string>

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
    auto key_result = generate_secret_key();
    AE_CHECK(key_result.has_value(), "T-C0: generate_secret_key succeeds");
    SecretKey key = *key_result;

    auto now = Clock::now();

    // T-C1: a freshly minted root token verifies against the same key and matching kind, with no
    // caveats to restrict it.
    {
        auto root = mint_root(key, capability_kind::fs_read, "workspace-mount-1");
        AE_CHECK(root.has_value(), "T-C1: mint_root succeeds");
        auto ok = verify(*root, key, EvaluationRequest{capability_kind::fs_read, "/any/path", now});
        AE_CHECK(ok.has_value(), "T-C1: root token verifies with no caveats");
    }

    // T-C2: attenuating with a PathPrefix caveat narrows the token -- a request under the prefix
    // succeeds, a request outside it fails.
    {
        auto root = mint_root(key, capability_kind::fs_read, "workspace-mount-1");
        AE_CHECK(root.has_value(), "T-C2: mint_root succeeds");
        auto narrowed = attenuate(*root, PathPrefix{"/workspace/reports/"});
        AE_CHECK(narrowed.has_value(), "T-C2: attenuate(PathPrefix) succeeds");

        auto inside = verify(*narrowed, key,
                              EvaluationRequest{capability_kind::fs_read,
                                                "/workspace/reports/q3.csv", now});
        AE_CHECK(inside.has_value(), "T-C2: request under the prefix verifies");

        auto outside = verify(*narrowed, key,
                               EvaluationRequest{capability_kind::fs_read,
                                                 "/workspace/other/secret.csv", now});
        AE_CHECK(!outside.has_value() && outside.error().code == "capability_token.path_denied",
                 "T-C2: request outside the prefix is denied");
    }

    // T-C3: attenuating with an ExpiresAt caveat is honored on both sides of the deadline.
    {
        auto root = mint_root(key, capability_kind::net_out, "egress-allowlist-1");
        AE_CHECK(root.has_value(), "T-C3: mint_root succeeds");
        auto deadline = now + std::chrono::minutes(5);
        auto timeboxed = attenuate(*root, ExpiresAt{deadline});
        AE_CHECK(timeboxed.has_value(), "T-C3: attenuate(ExpiresAt) succeeds");

        auto before = verify(*timeboxed, key,
                              EvaluationRequest{capability_kind::net_out, "", now});
        AE_CHECK(before.has_value(), "T-C3: request before the deadline verifies");

        auto after = verify(*timeboxed, key,
                             EvaluationRequest{capability_kind::net_out, "", deadline + std::chrono::seconds(1)});
        AE_CHECK(!after.has_value() && after.error().code == "capability_token.expired",
                 "T-C3: request after the deadline is denied");
    }

    // T-C4: chained attenuation -- both caveats must independently hold (logical AND, 007 §3
    // rule 2: attenuation only narrows, never widens back).
    {
        auto root = mint_root(key, capability_kind::fs_write, "workspace-mount-1");
        AE_CHECK(root.has_value(), "T-C4: mint_root succeeds");
        auto step1 = attenuate(*root, PathPrefix{"/workspace/out/"});
        AE_CHECK(step1.has_value(), "T-C4: first attenuate succeeds");
        auto step2 = attenuate(*step1, ExpiresAt{now + std::chrono::minutes(1)});
        AE_CHECK(step2.has_value(), "T-C4: second attenuate succeeds");

        auto both_hold = verify(*step2, key,
                                 EvaluationRequest{capability_kind::fs_write,
                                                   "/workspace/out/report.pdf", now});
        AE_CHECK(both_hold.has_value(), "T-C4: request satisfying both caveats verifies");

        auto prefix_fails = verify(*step2, key,
                                    EvaluationRequest{capability_kind::fs_write,
                                                      "/workspace/other/x", now});
        AE_CHECK(!prefix_fails.has_value(), "T-C4: request violating only the prefix caveat is denied");

        auto expiry_fails = verify(*step2, key,
                                    EvaluationRequest{capability_kind::fs_write,
                                                      "/workspace/out/report.pdf",
                                                      now + std::chrono::minutes(2)});
        AE_CHECK(!expiry_fails.has_value(), "T-C4: request violating only the expiry caveat is denied");
    }

    // T-C5 (positive control, 022 §5): verifying against the WRONG key must fail. If this check
    // ever reported success, it would mean AE_CHECK/verify() cannot detect a failure at all --
    // every other "ok" in this file would be meaningless.
    {
        auto root = mint_root(key, capability_kind::fs_read, "workspace-mount-1");
        AE_CHECK(root.has_value(), "T-C5: mint_root succeeds");
        auto wrong_key = generate_secret_key();
        AE_CHECK(wrong_key.has_value(), "T-C5: generate a second, different key");
        auto denied = verify(*root, *wrong_key,
                              EvaluationRequest{capability_kind::fs_read, "/any", now});
        AE_CHECK(!denied.has_value() && denied.error().code == "capability_token.bad_signature",
                 "T-C5 (positive control): verifying with the wrong key is rejected");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All capability_token proof checks passed.\n";
    return 0;
}
