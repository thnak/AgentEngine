// ADR-061 prove phase, §42/§43 (decisions/ADR-061-host-provided-inbound-transport.md). Proves the
// shared trust::steady_deadline_from() primitive's use by mcp::capability_grant_from_bearer_claims()
// -- the falsifiable claims table at §42.3 -- and, end to end, that the resulting CapabilityGrant is
// accepted by a REAL McpServer::dispatch() call. trust::steady_deadline_from()'s extraction itself
// (claim 1) is proven by tests/test_request_authority_bridge.cpp continuing to pass unmodified after
// the rt-side bridge's refactor (§43.2's own claim 1 rewording).

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/protocol/mcp/capability_grant_bridge.hpp"
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

namespace ae    = agentengine;
namespace mcp   = agentengine::mcp;
namespace trust = agentengine::trust;
namespace json  = agentengine::json;

struct GatedArgs {
    bool unused = false;
};
AE_JSON_SCHEMA(GatedArgs, unused)
struct GatedReply {
    bool unused = false;
};
AE_JSON_SCHEMA(GatedReply, unused)

struct GatedTool : ae::Tool<GatedTool, ae::Capabilities<ae::cap::decl::Entropy>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy -- gating probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{}; }
};

trust::BearerTokenClaims make_claims(std::string sub, std::chrono::system_clock::time_point exp) {
    trust::BearerTokenClaims c;
    c.sub       = std::move(sub);
    c.tenant_id = "tenant-a";
    c.aud       = "mcp-server";
    c.iss       = "issuer-a";
    c.jti       = "jti-capability-grant-bridge";
    c.exp       = exp;
    return c;
}

}  // namespace

int main() {
    auto const kWallNow   = std::chrono::system_clock::now();
    auto const kSteadyNow = std::chrono::steady_clock::now();
    auto const kHeld      = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({}));

    // --- claim 2: expiry matches trust::steady_deadline_from()'s own output exactly -----------------
    {
        auto claims = make_claims("caller-1", kWallNow + std::chrono::hours{1});
        auto grant  = mcp::capability_grant_from_bearer_claims(claims, kHeld, kWallNow, kSteadyNow);
        check(grant.has_value(), "claim 2 setup: a well-formed, unexpired claim succeeds");
        auto expected =
            trust::steady_deadline_from(claims.exp, kWallNow, kSteadyNow, trust::kMaxAuthorityHorizon);
        check(expected.has_value() && grant.has_value() && grant->expiry == *expected,
              "claim 2: CapabilityGrant.expiry matches steady_deadline_from()'s own output exactly");

        // Control: a differing wall_now produces a differing expiry -- not a fixed/ignored value.
        auto grant2 = mcp::capability_grant_from_bearer_claims(claims, kHeld,
                                                                kWallNow - std::chrono::minutes{30},
                                                                kSteadyNow);
        check(grant2.has_value() && !(grant2->expiry == grant->expiry),
              "claim 2 control: differing wall_now inputs produce differing expiry");
    }

    // --- claim 3: an exp beyond kMaxAuthorityHorizon is rejected, with the bridge's OWN code ---------
    {
        auto far_future_claims = make_claims("caller-2", kWallNow + std::chrono::hours{24 * 365 * 1000});
        auto rejected = mcp::capability_grant_from_bearer_claims(far_future_claims, kHeld, kWallNow,
                                                                   kSteadyNow);
        check(!rejected.has_value(), "claim 3: an implausibly-distant exp is rejected");
        check(!rejected.has_value() && rejected.error().code == "capability_grant.exp_horizon_exceeded",
              "claim 3: the rejection carries the bridge's OWN namespaced code -- never the shared "
              "primitive's generic \"steady_deadline.horizon_exceeded\", and never the rt-side "
              "bridge's differently-namespaced code");

        auto near_future_claims = make_claims("caller-2", kWallNow + std::chrono::hours{1});
        auto accepted = mcp::capability_grant_from_bearer_claims(near_future_claims, kHeld, kWallNow,
                                                                    kSteadyNow);
        check(accepted.has_value(), "claim 3 control: exp one hour out succeeds normally");
    }

    // --- claim 4: end to end -- a REAL verified bearer token bridges to a CapabilityGrant that a ------
    // --- REAL McpServer::dispatch() accepts, gating a tool held_ alone could never authorize --------
    {
        auto key = trust::generate_bearer_secret_key();
        check(key.has_value(), "claim 4 setup: generate_bearer_secret_key() succeeds");

        auto const table = ae::ToolTable::from_tools<GatedTool>();
        ae::CapabilitySet const held;  // grants nothing -- held_ alone could never pass gated_tool
        mcp::McpServer server(table, held, ae::ApprovalDecider{}, "capability-grant-bridge-test-server");

        auto token = trust::mint_bearer_token(
            *key, make_claims("caller-3", kWallNow + std::chrono::hours{1}));
        check(token.has_value(), "claim 4 setup: mint_bearer_token() succeeds");

        trust::ReplayGuard guard;
        trust::BearerVerificationRequest verify_req{"mcp-server", "issuer-a", kWallNow};
        auto verified = trust::verify_bearer_token(*token, *key, verify_req, guard);
        check(verified.has_value(), "claim 4 setup: verify_bearer_token() accepts the real token");

        auto const kEntropyCaps =
            std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({ae::cap::Entropy{}}));
        auto grant = mcp::capability_grant_from_bearer_claims(*verified, kEntropyCaps, kWallNow,
                                                                kSteadyNow);
        check(grant.has_value(), "claim 4: the verified claims bridge to a real CapabilityGrant");

        ae::Principal const caller = trust::principal_from_bearer_claims(*verified);
        auto req = mcp::JsonRpcRequest{
            mcp::RpcId{std::string{"c4"}}, "tools/call",
            json::Value::make_object(
                {{"name", json::Value::make_string("gated_tool")},
                 {"arguments", json::Value::make_object({{"unused", json::Value::make_bool(false)}})}})};
        auto resp = server.dispatch(req, caller, grant.has_value() ? std::optional{*grant} : std::nullopt,
                                     kSteadyNow);
        check(resp.result.has_value() && !resp.result->find("isError")->as_bool(),
              "claim 4: a REAL McpServer::dispatch() accepts the bridged grant end to end, authorizing "
              "a tool held_ alone (empty) could never have permitted");

        // kind threading control: human kind produces a Principal reflecting it.
        auto human_grant = mcp::capability_grant_from_bearer_claims(*verified, kEntropyCaps, kWallNow,
                                                                       kSteadyNow, ae::principal_kind::human);
        check(human_grant.has_value() && human_grant->principal.kind == ae::principal_kind::human,
              "claim 4 control: kind=human is threaded through, producing a Principal with "
              "principal_kind::human");
    }

    if (g_failures == 0) {
        std::printf("test_capability_grant_bridge: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_capability_grant_bridge: %d failure(s)\n", g_failures);
    return 1;
}
