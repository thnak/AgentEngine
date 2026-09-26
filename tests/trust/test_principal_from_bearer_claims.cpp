// ADR-039 prove phase (decisions/ADR-039-inbound-transport-host-pluggable.md). Proves
// trust::principal_from_bearer_claims() -- the canonical BearerTokenClaims -> Principal bridge every
// host-pluggable transport adapter is expected to call, per ADR-039's red-team finding that this
// mapping did not exist anywhere and every adapter would otherwise hand-roll its own, inconsistently.

#include <cstdio>
#include <string>

#include "agentengine/trust/bearer_token.hpp"
#include "agentengine/trust/principal.hpp"

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
    using agentengine::Principal;
    using agentengine::principal_kind;

    trust::BearerTokenClaims claims;
    claims.sub       = "caller-42";
    claims.tenant_id = "tenant-a";
    claims.aud       = "mcp.example";
    claims.iss       = "issuer.example";
    claims.jti       = "jti-1";
    // exp deliberately left default -- not part of what this bridge copies (see P3 below).

    // --- P1: sub/tenant_id map straight through, kind defaults to service ---------------------------
    {
        Principal const p = trust::principal_from_bearer_claims(claims);
        check(p.id == "caller-42", "P1: Principal::id == claims.sub");
        check(p.tenant_id == "tenant-a", "P1: Principal::tenant_id == claims.tenant_id");
        check(p.kind == principal_kind::service, "P1: kind defaults to service");
        check(p.on_behalf_of.empty(), "P1: a bearer-authenticated caller is never pre-delegated");
        check(p.delegation_depth == 0, "P1: delegation_depth starts at 0");
    }

    // --- P2: kind is an explicit, caller-supplied override, never read from the token ---------------
    {
        Principal const p = trust::principal_from_bearer_claims(claims, principal_kind::human);
        check(p.kind == principal_kind::human, "P2: explicit kind override is honored");
        check(p.id == "caller-42", "P2: id still maps correctly under the override");
    }

    // --- P3: aud/iss/exp/jti never leak into Principal -- only sub/tenant_id cross the boundary ------
    // (Principal has no fields for them at all; this check exists so a future field addition to
    // Principal doesn't silently start leaking token-internal claims without a deliberate decision.)
    {
        Principal const p = trust::principal_from_bearer_claims(claims);
        Principal        p2{};
        p2.id        = p.id;
        p2.tenant_id = p.tenant_id;
        p2.kind      = p.kind;
        check(p == p2, "P3: Principal carries nothing beyond id/tenant_id/kind from the claims");
    }

    // --- P4: two distinct subjects never collide ------------------------------------------------------
    {
        trust::BearerTokenClaims other = claims;
        other.sub                      = "caller-99";
        Principal const p1             = trust::principal_from_bearer_claims(claims);
        Principal const p2             = trust::principal_from_bearer_claims(other);
        check(!(p1 == p2), "P4: distinct claims.sub produce distinct Principal::id");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_principal_from_bearer_claims: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "test_principal_from_bearer_claims: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
