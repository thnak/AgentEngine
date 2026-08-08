// ADR-021 prove phase (decisions/ADR-021-inbound-protocol-trust-boundary.md). Proves
// trust/bearer_token.hpp -- the inbound bearer-token mechanism Design A (first-party TLS+auth
// termination) depends on -- against the specific findings ADR-021's own red-team pass named:
// algorithm confusion (closed by construction, no runtime test possible -- see the header's own
// finding 1 comment), confused-deputy/audience validation (finding 2), constant-time comparison
// reuse (finding 3, proven indirectly via correct rejection, not timing), and bounded replay
// rejection (finding 4). A positive control (B1) is included FIRST and deliberately, per
// decisions/README.md's own "a test that cannot fail proves nothing" rule for security claims.

#include <chrono>
#include <cstdio>
#include <string>

#include "agentengine/trust/bearer_token.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace trust = agentengine::trust;
using sys_clock   = std::chrono::system_clock;

trust::BearerTokenClaims make_claims(std::string sub, std::string aud, std::string iss,
                                      std::string jti, sys_clock::time_point exp) {
    trust::BearerTokenClaims c;
    c.sub       = std::move(sub);
    c.tenant_id = "tenant-a";
    c.aud       = std::move(aud);
    c.iss       = std::move(iss);
    c.exp       = exp;
    c.jti       = std::move(jti);
    return c;
}

}  // namespace

int main() {
    auto key1 = trust::generate_bearer_secret_key();
    check(key1.has_value(), "setup: generate_bearer_secret_key() succeeds");
    auto key2 = trust::generate_bearer_secret_key();
    check(key2.has_value(), "setup: a second, independent key generates successfully");
    if (!key1.has_value() || !key2.has_value()) {
        std::fprintf(stderr, "test_bearer_token_proof: setup failure, aborting\n");
        return 1;
    }

    sys_clock::time_point const now       = sys_clock::now();
    sys_clock::time_point const future    = now + std::chrono::minutes(5);
    sys_clock::time_point const past      = now - std::chrono::minutes(5);

    // --- B1: POSITIVE CONTROL -- a genuinely valid token is accepted --------------------------------
    // Mandatory per decisions/README.md for a security claim: every negative test below would pass
    // vacuously if verify_bearer_token() simply rejected everything.
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-1", "svc-mcp", "issuer-a", "jti-1", future));
        check(token.has_value(), "B1: mint_bearer_token() succeeds");
        if (token.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto verified = trust::verify_bearer_token(*token, *key1, req, guard);
            check(verified.has_value(), "B1: a genuinely valid token is ACCEPTED (positive control)");
            if (verified.has_value()) {
                check(verified->sub == "u-1" && verified->tenant_id == "tenant-a",
                      "B1: the returned claims carry the real sub/tenant_id");
            }
        }
    }

    // --- B2: a tampered signature is rejected --------------------------------------------------------
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-2", "svc-mcp", "issuer-a", "jti-2", future));
        check(token.has_value(), "B2: mint succeeds");
        if (token.has_value()) {
            trust::BearerToken tampered = *token;
            tampered.signature[0] ^= 0xFF;  // flip one bit of the signature
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto verified = trust::verify_bearer_token(tampered, *key1, req, guard);
            check(!verified.has_value(), "B2: a tampered signature is rejected");
            if (!verified.has_value()) {
                check(verified.error().code == "bearer_token.bad_signature",
                      "B2: rejected with the real bad_signature classification");
            }
        }
    }

    // --- B3: a token signed with a DIFFERENT key is rejected -- proves the signature is genuinely --
    // --- keyed, not just structurally well-formed.                                                ---
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-3", "svc-mcp", "issuer-a", "jti-3", future));
        check(token.has_value(), "B3: mint with key1 succeeds");
        if (token.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto verified = trust::verify_bearer_token(*token, *key2, req, guard);  // verified against key2, not key1
            check(!verified.has_value(),
                  "B3: a token verified against the WRONG key is rejected, not accepted by structure alone");
        }
    }

    // --- B4: confused-deputy -- claim 2's own framing: TWO real tokens, distinct aud, cross-------
    // --- presented. Both rejected. expected_aud is caller-supplied config, never token content.  ---
    {
        auto token_for_mcp = trust::mint_bearer_token(*key1, make_claims("u-4", "svc-mcp", "issuer-a", "jti-4a", future));
        auto token_for_a2a = trust::mint_bearer_token(*key1, make_claims("u-4", "svc-a2a", "issuer-a", "jti-4b", future));
        check(token_for_mcp.has_value() && token_for_a2a.has_value(), "B4: both tokens mint successfully");
        if (token_for_mcp.has_value() && token_for_a2a.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest a2a_route_req{"svc-a2a", "issuer-a", now};
            auto cross_presented = trust::verify_bearer_token(*token_for_mcp, *key1, a2a_route_req, guard);
            check(!cross_presented.has_value(),
                  "B4: a token minted for svc-mcp is rejected when presented to the svc-a2a route");
            if (!cross_presented.has_value()) {
                check(cross_presented.error().code == "bearer_token.wrong_audience",
                      "B4: rejected with the real wrong_audience classification");
            }

            trust::BearerVerificationRequest mcp_route_req{"svc-mcp", "issuer-a", now};
            auto reverse_cross = trust::verify_bearer_token(*token_for_a2a, *key1, mcp_route_req, guard);
            check(!reverse_cross.has_value(),
                  "B4: the reverse cross-presentation (svc-a2a token to the svc-mcp route) is ALSO rejected");

            // And each token IS accepted on its own correct route -- proves rejection above was
            // about audience specifically, not a broken validator rejecting everything.
            trust::ReplayGuard guard2;
            auto correct_mcp = trust::verify_bearer_token(*token_for_mcp, *key1, mcp_route_req, guard2);
            check(correct_mcp.has_value(), "B4: the svc-mcp token IS accepted on the svc-mcp route");
        }
    }

    // --- B5: a wrong issuer is rejected ----------------------------------------------------------------
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-5", "svc-mcp", "issuer-a", "jti-5", future));
        check(token.has_value(), "B5: mint succeeds");
        if (token.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-DIFFERENT", now};
            auto verified = trust::verify_bearer_token(*token, *key1, req, guard);
            check(!verified.has_value(), "B5: a wrong issuer is rejected");
            if (!verified.has_value()) {
                check(verified.error().code == "bearer_token.wrong_issuer",
                      "B5: rejected with the real wrong_issuer classification");
            }
        }
    }

    // --- B6: an expired token is rejected ------------------------------------------------------------
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-6", "svc-mcp", "issuer-a", "jti-6", past));
        check(token.has_value(), "B6: mint an already-expired token succeeds (minting itself has no "
                                  "time check -- only verification does)");
        if (token.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto verified = trust::verify_bearer_token(*token, *key1, req, guard);
            check(!verified.has_value(), "B6: an expired token is rejected");
            if (!verified.has_value()) {
                check(verified.error().code == "bearer_token.expired",
                      "B6: rejected with the real expired classification");
            }
        }
    }

    // --- B7: replay -- the SAME jti presented twice against the SAME ReplayGuard: first accepted, --
    // --- second rejected, even though the token itself is still perfectly valid (unexpired,        --
    // --- correctly signed) -- proves this is genuinely REPLAY rejection, not a signature/expiry     --
    // --- side effect.                                                                               ---
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-7", "svc-mcp", "issuer-a", "jti-7-unique", future));
        check(token.has_value(), "B7: mint succeeds");
        if (token.has_value()) {
            trust::ReplayGuard guard;
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto first  = trust::verify_bearer_token(*token, *key1, req, guard);
            auto second = trust::verify_bearer_token(*token, *key1, req, guard);
            check(first.has_value(), "B7: the FIRST presentation of a fresh jti is accepted");
            check(!second.has_value(), "B7: the SECOND presentation of the SAME jti is rejected as a replay");
            if (!second.has_value()) {
                check(second.error().code == "bearer_token.replayed",
                      "B7: rejected with the real replayed classification");
            }
        }
    }

    // --- B8: named residual, proven honestly rather than hidden -- replay defense is SINGLE-------
    // --- INSTANCE. The identical jti presented to a DIFFERENT ReplayGuard (modeling a second       --
    // --- process in a horizontally-scaled deployment) is accepted, because nothing shares state    --
    // --- across instances. This is exactly ADR-021's own named limitation, demonstrated rather than --
    // --- silently true.                                                                             ---
    {
        auto token = trust::mint_bearer_token(*key1, make_claims("u-8", "svc-mcp", "issuer-a", "jti-8-shared", future));
        check(token.has_value(), "B8: mint succeeds");
        if (token.has_value()) {
            trust::ReplayGuard instance_1;
            trust::ReplayGuard instance_2;  // models a SECOND process's own independent guard
            trust::BearerVerificationRequest req{"svc-mcp", "issuer-a", now};
            auto on_instance_1 = trust::verify_bearer_token(*token, *key1, req, instance_1);
            auto on_instance_2 = trust::verify_bearer_token(*token, *key1, req, instance_2);
            check(on_instance_1.has_value() && on_instance_2.has_value(),
                  "B8: the SAME jti is accepted on TWO independent ReplayGuard instances -- "
                  "demonstrates the named single-instance-only residual honestly, not a silent gap");
        }
    }

    // --- B9: ReplayGuard prunes entries once THEIR OWN exp passes -- memory is bounded by ----------
    // --- distinct not-yet-expired jtis, not by every jti ever seen.                                ---
    {
        trust::ReplayGuard guard;
        auto short_exp = now + std::chrono::seconds(1);
        check(guard.check_and_record("jti-prune-1", short_exp, now),
              "B9: recording a fresh jti with a near expiry succeeds");
        check(guard.tracked_count() == 1, "B9: exactly one entry is tracked after one recording");
        // Advance well past short_exp -- the next call must prune it before doing anything else.
        auto much_later = now + std::chrono::minutes(10);
        check(guard.check_and_record("jti-prune-2", much_later, much_later),
              "B9: recording a second, unrelated jti succeeds");
        check(guard.tracked_count() == 1,
              "B9: the FIRST entry was pruned once its own exp passed -- only the still-live second "
              "entry remains tracked, memory does not grow unbounded from expired tokens");
    }

    if (g_failures == 0) {
        std::printf("test_bearer_token_proof: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_bearer_token_proof: %d failure(s)\n", g_failures);
    return 1;
}
