// Proves ADR-102 Phase 1 (decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md) --
// IdentityAuthority/IdentityHandle/Grant<T> (trust/identity_authority.hpp) and AsyncQuota<T>
// (rt/async_quota.hpp), ported from docs/planning/proofs/{identity_authority,async_quota}/, actually
// work against the real, agentengine-namespaced code -- re-proving ADR-099 §3's own claims against
// the PORTED code, not assuming a rename/promotion preserved them.
//
//   [1]  IdentityAuthority::bootstrap() is a real singleton -- same instance across calls.
//   [2]  mint_root()/derive_child() ancestry is real and multi-hop (is_ancestor_of()).
//   [3]  adopt(Principal) is idempotent -- the SAME real id always maps to the SAME IdentityHandle.
//   [4]  adopt()'s ancestry bridge: an on_behalf_of pointing at an ALREADY-adopted parent is
//        recognized; pointing at a never-seen parent is NOT (disclosed limitation, proven not
//        assumed).
//   [5]  ADR-102 §3 C2: AsyncQuota::mint_root() fails closed against an IdentityHandle the calling
//        IdentityAuthority never minted.
//   [6]  AsyncQuota::mint_root() succeeds for a real, minted owner; try_consume() by the owner
//        succeeds and decrements remaining().
//   [7]  ADR-102 §3 C3: try_consume() by an unrelated IdentityHandle (no owner/child-share
//        relationship) is refused.
//   [8]  allocate_child_share() + try_consume() by the child IdentityHandle succeeds.
//   [9]  release_child_share() is anti-replay: a second release of the same (child, amount) fails.
//   [10] refund() re-credits remaining().
//   [11] Grant<T>/authorized(): a grant issued to a descendant is authorized for that descendant,
//        never for an unrelated IdentityHandle.

#include "agentengine/rt/async_quota.hpp"
#include "agentengine/trust/identity_authority.hpp"

#include <cstdio>
#include <cstdlib>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct BranchCost {};

}  // namespace

int main() {
    // ---- [1] bootstrap() is a real singleton. -------------------------------------------------
    IdentityAuthority& authority_a = IdentityAuthority::bootstrap();
    IdentityAuthority& authority_b = IdentityAuthority::bootstrap();
    check(&authority_a == &authority_b, "bootstrap() returns the SAME instance across calls");
    IdentityAuthority& authority = authority_a;

    // ---- [2] mint_root()/derive_child() ancestry is real and multi-hop. -----------------------
    IdentityHandle grandparent = authority.mint_root("grandparent");
    IdentityHandle parent = authority.derive_child(grandparent, "parent");
    IdentityHandle child = authority.derive_child(parent, "child");
    IdentityHandle unrelated = authority.mint_root("unrelated");
    check(authority.is_ancestor_of(grandparent.id(), child.id()),
          "is_ancestor_of() walks a real, multi-hop ancestry chain (grandparent -> parent -> child)");
    check(!authority.is_ancestor_of(unrelated.id(), child.id()),
          "is_ancestor_of() correctly refuses an unrelated subject");
    check(authority.is_known(child.id()), "is_known() recognizes a real, minted subject");

    // ---- [3] adopt(Principal) is idempotent. --------------------------------------------------
    Principal real_p1;
    real_p1.id = "real-principal-1";
    IdentityHandle adopted_1a = authority.adopt(real_p1);
    IdentityHandle adopted_1b = authority.adopt(real_p1);
    check(adopted_1a.id() == adopted_1b.id(),
          "adopt() is idempotent -- the SAME real Principal::id maps to the SAME IdentityHandle id");

    // ---- [4] adopt()'s ancestry bridge: recognized only if the parent was already adopted. -----
    Principal real_child_of_unseen;
    real_child_of_unseen.id = "real-child-of-never-adopted-parent";
    real_child_of_unseen.on_behalf_of = "real-parent-never-adopted";
    IdentityHandle adopted_orphan = authority.adopt(real_child_of_unseen);
    check(!authority.is_ancestor_of(adopted_1a.id(), adopted_orphan.id()),
          "adopt()'s ancestry bridge does NOT fabricate a parent relationship for a never-adopted "
          "real principal -- a disclosed limitation, proven, not assumed");

    Principal real_parent;
    real_parent.id = "real-parent-2";
    IdentityHandle adopted_parent = authority.adopt(real_parent);
    Principal real_child;
    real_child.id = "real-child-2";
    real_child.on_behalf_of = "real-parent-2";
    IdentityHandle adopted_child = authority.adopt(real_child);
    check(authority.is_ancestor_of(adopted_parent.id(), adopted_child.id()),
          "adopt()'s ancestry bridge DOES recognize a real on_behalf_of whose parent was already "
          "adopted");

    // ---- [4b] REAL FIX, proven: two different tenants' principals sharing the SAME real id string --
    // ---- do NOT collapse into one IdentityHandle (an independent red-team pass's own MUST-FIX ------
    // ---- finding -- 018 §6's cross-tenant invariant, applied to adopt()'s own bridging map). --------
    Principal tenant_a_principal;
    tenant_a_principal.tenant_id = "tenant-a";
    tenant_a_principal.id = "shared-username";
    IdentityHandle tenant_a_handle = authority.adopt(tenant_a_principal);

    Principal tenant_b_principal;
    tenant_b_principal.tenant_id = "tenant-b";
    tenant_b_principal.id = "shared-username";  // SAME id string, DIFFERENT tenant
    IdentityHandle tenant_b_handle = authority.adopt(tenant_b_principal);

    check(tenant_a_handle.id() != tenant_b_handle.id(),
          "two different tenants' principals sharing the same real id string get DISTINCT "
          "IdentityHandles -- adopt() is tenant-scoped, not a cross-tenant identity merge");

    // Idempotency still holds WITHIN one tenant, proven independently of the [3] check above (which
    // used a Principal with an empty tenant_id).
    IdentityHandle tenant_a_handle_again = authority.adopt(tenant_a_principal);
    check(tenant_a_handle.id() == tenant_a_handle_again.id(),
          "adopt() is still idempotent WITHIN one tenant after the tenant-scoping fix");

    // Cross-tenant delegation (on_behalf_of pointing at a different tenant's already-adopted real id)
    // is NOT silently recognized either -- ancestry lookup is scoped to the child's own tenant.
    Principal tenant_b_child_claiming_tenant_a_parent;
    tenant_b_child_claiming_tenant_a_parent.tenant_id = "tenant-b";
    tenant_b_child_claiming_tenant_a_parent.id = "tenant-b-child";
    tenant_b_child_claiming_tenant_a_parent.on_behalf_of = "shared-username";  // tenant-a's real id,
                                                                                 // not tenant-b's own
    IdentityHandle cross_tenant_child = authority.adopt(tenant_b_child_claiming_tenant_a_parent);
    check(!authority.is_ancestor_of(tenant_a_handle.id(), cross_tenant_child.id()),
          "adopt()'s ancestry bridge does NOT recognize a cross-tenant on_behalf_of -- delegation "
          "ancestry is scoped to the child's own tenant, never inferred across a tenant boundary");

    // ---- [5] AsyncQuota::mint_root()'s is_known() gate: STRUCTURALLY unreachable-false, not ------
    // ---- exercised as a runtime negative here -- and that is itself the real, verified claim. ----
    // `IdentityHandle` has no public constructor at all (proven by the compile_fail probes this
    // file's own CMake wiring runs alongside this test: identity_handle_no_direct_construction.cpp)
    // -- every real IdentityHandle obtainable through the public API comes from mint_root()/
    // derive_child()/adopt(), each of which ALSO inserts into IdentityAuthority's own ancestry_/
    // adopted_ tables in the same call. Since IdentityAuthority is a true process-wide singleton
    // (check [1] above), there is no way, from within one process's public API, to construct an
    // IdentityHandle NOT known to `authority.is_known()`. AsyncQuota::mint_root()'s runtime check is
    // therefore real defense-in-depth against a hypothetical future where handles could be
    // deserialized/forged from outside this process -- not currently reachable as a live negative
    // test, and this file does not pretend otherwise by fabricating one.

    // ---- [6] mint_root() succeeds for a real, minted owner; try_consume() by the owner works. --
    IdentityHandle owner = authority.mint_root("quota-owner");
    auto quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    check(quota_r.has_value(), "AsyncQuota::mint_root() succeeds for a real, minted owner");
    auto& quota = *quota_r;

    auto consume_r = drive(quota.try_consume(30, owner));
    check(consume_r.has_value(), "try_consume() by the quota's own owner succeeds");
    check(quota.remaining() == 70, "try_consume() actually decrements remaining()");

    // ---- [7] try_consume() by an unrelated subject is refused. ---------------------------------
    auto unrelated_consume_r = drive(quota.try_consume(1, unrelated));
    check(!unrelated_consume_r.has_value(),
          "try_consume() by an unrelated IdentityHandle (no owner/child-share relationship) fails");
    check(unrelated_consume_r.error().code == "async_quota.unauthorized_spender",
          "the refusal is the specific, real unauthorized_spender error, not an unrelated failure");
    check(quota.remaining() == 70,
          "a refused try_consume() does not decrement remaining() -- fails closed, not partially");

    // ---- [8] allocate_child_share() + try_consume() by the child succeeds. ---------------------
    IdentityHandle branch_child = authority.derive_child(owner, "branch-child");
    auto share_r = drive(quota.allocate_child_share(branch_child, 20));
    check(share_r.has_value(), "allocate_child_share() succeeds within remaining budget");
    check(quota.remaining() == 50, "allocate_child_share() decrements the PARENT quota's remaining()");
    auto& child_quota = *share_r;
    auto child_consume_r = drive(quota.try_consume(5, branch_child));
    check(child_consume_r.has_value(),
          "try_consume() by a subject the quota split a share TO succeeds on the parent quota");

    // ---- [9] release_child_share() is anti-replay. ----------------------------------------------
    auto release_r = drive(quota.release_child_share(branch_child, 20));
    check(release_r.has_value(), "release_child_share() succeeds for a live, matching allocation");
    check(quota.remaining() == 65, "release_child_share() re-credits the parent quota");
    auto replay_r = drive(quota.release_child_share(branch_child, 20));
    check(!replay_r.has_value(),
          "a SECOND release_child_share() with the same (child, amount) fails closed -- anti-replay, "
          "does not re-credit a second time");
    check(quota.remaining() == 65, "the replayed release did not double-credit remaining()");

    // ---- [10] refund() re-credits. ---------------------------------------------------------------
    auto refund_r = drive(quota.refund(10));
    check(refund_r.has_value(), "refund() succeeds");
    check(quota.remaining() == 75, "refund() re-credits remaining()");

    (void)child_quota;

    // ---- [11] Grant<T>/authorized(): a grant is authorized for a descendant, not an unrelated. ----
    struct FsScope { std::string mount; };
    Grant<FsScope> grant = authority.mint_grant(FsScope{"work"}, /*issued_to=*/child, /*issued_by=*/owner);
    check(authorized(grant, child, FsScope{"work"}),
          "authorized() accepts the grant's own, real issued_to subject");
    check(!authorized(grant, unrelated, FsScope{"work"}),
          "authorized() refuses an unrelated subject with no ancestry relationship to issued_to");

    if (g_failures == 0) {
        std::printf("test_identity_authority_grant: all checks passed\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
