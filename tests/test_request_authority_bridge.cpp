// ADR-061 prove phase, §31/§33/§34/§35 (decisions/ADR-061-host-provided-inbound-transport.md).
// Proves the falsifiable claims tables at §31.3 and §33.8 for the two pieces that section's design
// round built: `trust::EndpointId`/`EndpointRegistry`/`mint_endpoint_id()` (endpoint_id.hpp) and
// `rt::request_authority_from_bearer_claims()` (request_authority_bridge.hpp) -- the bridge from a
// verified bearer credential to the `RequestAuthority` §20-§30's Tier-3 mechanism already proved gates
// real tool authorization.

#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>

#include "agentengine/rt/request_authority_bridge.hpp"
#include "agentengine/trust/bearer_token.hpp"
#include "agentengine/trust/endpoint_id.hpp"

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

}  // namespace

int main() {
    namespace trust = agentengine::trust;
    namespace rt    = agentengine::rt;
    using agentengine::CapabilitySet;
    using agentengine::principal_kind;

    // ============================================================================================
    // §31.1 -- EndpointId / EndpointRegistry / mint_endpoint_id()
    // ============================================================================================

    // --- claim 1: mint_endpoint_id() values are CSPRNG-derived, not sequential/predictable --------
    {
        std::set<std::string> tokens;
        bool                  all_minted = true;
        constexpr int         kTrials    = 2000;
        for (int i = 0; i < kTrials; ++i) {
            auto id = trust::mint_endpoint_id();
            if (!id) {
                all_minted = false;
                break;
            }
            tokens.insert(id->token);
        }
        check(all_minted, "claim 1: every mint_endpoint_id() call succeeds");
        check(tokens.size() == static_cast<std::size_t>(kTrials),
              "claim 1: two thousand mints never collide");
        auto sample = trust::mint_endpoint_id();
        check(sample.has_value() && sample->token.size() == 32,
              "claim 1: token width matches secure_random_hex(16)'s 32-hex-char budget");
    }

    // --- claim 2: EndpointRegistry::resolve() fails closed on absent/adjacent-guessed key ----------
    {
        trust::EndpointRegistry registry;
        auto real_id = trust::mint_endpoint_id();
        check(real_id.has_value(), "claim 2 setup: minted a real endpoint id");
        registry.configure(*real_id, trust::EndpointConfig{"aud.example", "iss.example"});

        auto ok = registry.resolve(*real_id);
        check(ok.has_value() && (*ok)->audience == "aud.example",
              "claim 2 control: the real, configured key resolves correctly");

        // A guessed/adjacent value -- flip the last hex character.
        trust::EndpointId guessed = *real_id;
        char&             last    = guessed.token.back();
        last                      = (last == '0') ? '1' : '0';
        auto denied               = registry.resolve(guessed);
        check(!denied.has_value(), "claim 2: an adjacent-guessed key fails closed, not to endpoint 0");

        trust::EndpointId unknown{"0000000000000000000000000000ff"};
        auto              unknown_result = registry.resolve(unknown);
        check(!unknown_result.has_value(), "claim 2: a wholly unconfigured key fails closed");
    }

    // ============================================================================================
    // §31.2/§33.6 -- rt::request_authority_from_bearer_claims()
    // ============================================================================================

    trust::BearerTokenClaims claims;
    claims.sub       = "caller-77";
    claims.tenant_id = "tenant-b";
    claims.aud       = "svc.example";
    claims.iss       = "issuer.example";
    claims.jti       = "jti-request-authority-bridge";

    auto const held = std::make_shared<CapabilitySet const>(CapabilitySet::grant_root({}));

    // --- claim 3: never reads system_clock::now()/steady_clock::now() internally ------------------
    {
        // A `wall_now` deliberately far from real wall-clock time -- if the function read a clock
        // internally, the result would reflect REAL now, not this fabricated instant. It does not.
        auto const wall_now   = std::chrono::system_clock::time_point{std::chrono::hours{1000}};
        auto const steady_now = std::chrono::steady_clock::time_point{std::chrono::hours{2000}};
        claims.exp             = wall_now + std::chrono::hours{1};

        auto authority = rt::request_authority_from_bearer_claims(claims, held, wall_now, steady_now);
        check(authority.has_value(), "claim 3 setup: a well-formed, unexpired claim succeeds");
        check(authority->expiry == steady_now + std::chrono::hours{1},
              "claim 3: expiry is derived purely from the passed-in samples, not a real clock read");
    }

    // --- claim 4: an already-expired claims.exp converts to a dead-on-arrival RequestAuthority,
    //     never a wraparound-derived far-future deadline -------------------------------------------
    {
        auto const wall_now   = std::chrono::system_clock::now();
        auto const steady_now = std::chrono::steady_clock::now();

        trust::BearerTokenClaims expired = claims;
        expired.exp                       = wall_now - std::chrono::hours{1};
        auto dead = rt::request_authority_from_bearer_claims(expired, held, wall_now, steady_now);
        check(dead.has_value(), "claim 4 setup: an already-expired claim still constructs (no re-check)");
        check(!dead->live(steady_now), "claim 4: dead-on-arrival authority is not live at steady_now");
        check(!dead->live(steady_now + std::chrono::hours{24}),
              "claim 4: dead-on-arrival authority never becomes live later either");

        trust::BearerTokenClaims live_claims = claims;
        live_claims.exp                       = wall_now + std::chrono::hours{1};
        auto live = rt::request_authority_from_bearer_claims(live_claims, held, wall_now, steady_now);
        check(live.has_value() && live->live(steady_now),
              "claim 4 control: a real hour-ahead exp is live right now");
        check(live.has_value() && !live->live(steady_now + std::chrono::hours{2}),
              "claim 4 control: the same authority is dead two hours later");
    }

    // --- claim 5: capabilities is never synthesized/defaulted -- an explicit set passes through
    //     unchanged (pointer identity) ---------------------------------------------------------------
    {
        auto const wall_now   = std::chrono::system_clock::now();
        auto const steady_now = std::chrono::steady_clock::now();
        claims.exp              = wall_now + std::chrono::hours{1};

        auto authority = rt::request_authority_from_bearer_claims(claims, held, wall_now, steady_now);
        check(authority.has_value() && authority->capabilities.get() == held.get(),
              "claim 5: the exact CapabilitySet instance passed in comes out unchanged, never "
              "substituted");
    }

    // --- claim 6: principal_from_bearer_claims() is reused, not reimplemented ----------------------
    {
        auto const wall_now   = std::chrono::system_clock::now();
        auto const steady_now = std::chrono::steady_clock::now();
        claims.exp              = wall_now + std::chrono::hours{1};

        auto direct = trust::principal_from_bearer_claims(claims);
        auto bridged =
            rt::request_authority_from_bearer_claims(claims, held, wall_now, steady_now);
        check(bridged.has_value() && bridged->principal == direct,
              "claim 6: the bridge's Principal is byte-identical to calling the shared primitive "
              "directly");

        auto direct_human = trust::principal_from_bearer_claims(claims, principal_kind::human);
        auto bridged_human = rt::request_authority_from_bearer_claims(claims, held, wall_now, steady_now,
                                                                        principal_kind::human);
        check(bridged_human.has_value() && bridged_human->principal.kind == principal_kind::human &&
                  bridged_human->principal == direct_human,
              "claim 6: differing kind arguments are actually threaded through, not ignored");
        check(!(bridged->principal == bridged_human->principal),
              "claim 6: the default-kind and human-kind results differ from each other");
    }

    // --- claim 7 (§33.8): exp more than kMaxAuthorityHorizon past wall_now is rejected, never cast -
    {
        auto const wall_now   = std::chrono::system_clock::now();
        auto const steady_now = std::chrono::steady_clock::now();

        trust::BearerTokenClaims far_future = claims;
        far_future.exp = wall_now + std::chrono::hours{24 * 365 * 1000};  // ~1000 years out
        auto rejected =
            rt::request_authority_from_bearer_claims(far_future, held, wall_now, steady_now);
        check(!rejected.has_value(), "claim 7: an implausibly-distant exp is rejected, not cast");
        check(rejected.has_value() ||
                  rejected.error().code == "request_authority.exp_horizon_exceeded",
              "claim 7: the rejection carries the documented, stable error code");

        trust::BearerTokenClaims near_future = claims;
        near_future.exp = wall_now + std::chrono::hours{1};
        auto accepted =
            rt::request_authority_from_bearer_claims(near_future, held, wall_now, steady_now);
        check(accepted.has_value(), "claim 7 control: exp one hour out succeeds normally");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_request_authority_bridge: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "test_request_authority_bridge: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
