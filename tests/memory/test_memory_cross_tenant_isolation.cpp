// Milestone 5 Phase I1 (018-Identity-Authorization-and-Secrets.md §6: "Tenant is a first-class
// dimension of principal, session, memory scope, sandbox, quota, and audit -- not a filter applied
// at query time... Cross-tenant access is denied at the actor boundary; a cross-tenant leak is a
// release-blocking defect class"; 029-Memory-System.md §9 G4/G5). This file proves a REAL bug this
// phase found and fixed: through Milestone 4, `memory_ref_name()`/`memory_mount_id()`
// (core/memory.hpp) derived their output from `principal.id` ALONE -- `tenant_id` played no role.
// Two principals in DIFFERENT tenants sharing the same `id` (not a contrived scenario: tenant-scoped
// id spaces are typically allocated independently per tenant -- both tenants can easily have a user
// literally named "admin", or a service literally named "orchestrator") got the IDENTICAL memory
// ref name and mount_id -- meaning their memory worktrees were the SAME worktree: full cross-tenant
// memory leakage, exactly the "release-blocking defect class" 018 §6/029 §9 name.
// `test_memory_worktree.cpp`'s own pre-existing cross-principal proof (Milestone 4 Phase G2) never
// caught this because both of its principals share one tenant ("tenant-1") -- same-tenant,
// different-id collisions were the only case that test exercised; this file is the missing
// different-tenant, SAME-id case. Both functions now fold `tenant_id` into their derivation
// (`memory.hpp`'s own comments on `memory_ref_name`/`memory_mount_id` have the full story), which
// this file proves closes the leak while leaving the pre-existing same-tenant guarantee intact.

#include <iostream>
#include <string>

#include "agentengine/core/memory.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

} // namespace

int main() {
    ae::rt::InMemoryAppendLogStore ref_store;
    ae::InMemoryWorktreeObjectStore object_store;

    // Same `id`, DIFFERENT tenants -- the case Milestone 4's own suite never exercised.
    ae::Principal const tenant_a_admin{"admin", "tenant-a"};
    ae::Principal const tenant_b_admin{"admin", "tenant-b"};

    // --- The fix, directly: distinct tenants now produce distinct derivations -------------------
    AE_CHECK(ae::memory_ref_name(tenant_a_admin) != ae::memory_ref_name(tenant_b_admin),
             "I1-R1: two principals sharing an id but in DIFFERENT tenants get DISTINCT memory ref "
             "names -- before this phase's fix, these were byte-identical ('principal:admin' for "
             "both), meaning both tenants' 'admin' shared the exact same memory worktree");
    AE_CHECK(ae::memory_mount_id(tenant_a_admin) != ae::memory_mount_id(tenant_b_admin),
             "I1-R2: same fix, for mount_id -- the internal capability-matching key mount_read/"
             "mount_write actually compare");
    AE_CHECK(ae::ref_log_id(ae::memory_ref_name(tenant_a_admin)) !=
                 ae::ref_log_id(ae::memory_ref_name(tenant_b_admin)),
             "I1-R3: the distinct ref names resolve to distinct log ids too -- real worktree "
             "isolation, not just distinct strings that happen to still collide downstream");

    // --- End-to-end: each tenant's "admin" gets a genuinely separate, non-colliding worktree -----
    auto ref_a = ae::ensure_memory_worktree(object_store, ref_store, tenant_a_admin);
    AE_CHECK(ref_a.has_value(), "setup: tenant-a's admin worktree bootstraps");
    auto ref_b = ae::ensure_memory_worktree(object_store, ref_store, tenant_b_admin);
    AE_CHECK(ref_b.has_value(), "setup: tenant-b's admin worktree bootstraps independently");

    ae::Mount const mount_a = ae::memory_mount(tenant_a_admin);
    ae::Mount const mount_b = ae::memory_mount(tenant_b_admin);
    ae::cap::FsWrite const write_a{ae::memory_mount_id(tenant_a_admin), "", std::nullopt, std::nullopt};
    ae::cap::FsRead const  read_a{ae::memory_mount_id(tenant_a_admin), "", std::nullopt};
    ae::cap::FsRead const  read_b{ae::memory_mount_id(tenant_b_admin), "", std::nullopt};

    ae::MemoryItem secret{};
    secret.kind    = ae::memory_kind::episodic;
    secret.content = "tenant-a's admin confidential note";
    secret.origin  = ae::MemoryOrigin{ae::memory_source::user_stated, "run-1", "turn-1", tenant_a_admin};
    auto written = ae::write_memory_item(object_store, ref_store, mount_a, write_a, secret);
    AE_CHECK(written.has_value(), "setup: tenant-a's admin writes a memory item");

    // Tenant B's "admin", holding a capability minted for tenant B's OWN mount, can never even
    // reach tenant A's mount -- the capability-mismatch check (worktree.hpp) rejects it before any
    // store access, the same "never reaches the store at all" property test_memory_worktree.cpp
    // proves for the same-tenant case.
    auto crossed = ae::list_memory_items(object_store, ref_store, mount_a, read_b);
    AE_CHECK(!crossed.has_value() && crossed.error().code == "worktree.mount_capability_mismatch",
             "I1-R4: tenant-b's admin capability is rejected outright against tenant-a's mount -- "
             "cross-tenant access denied at the capability-matching boundary, before any store read");

    auto b_listed = ae::list_memory_items(object_store, ref_store, mount_b, read_b);
    AE_CHECK(b_listed.has_value() && b_listed->empty(),
             "I1-R5: tenant-b's admin worktree has NONE of tenant-a's items -- genuinely separate "
             "storage, not merely a rejected read against a worktree that secretly holds them");

    auto a_listed = ae::list_memory_items(object_store, ref_store, mount_a, read_a);
    AE_CHECK(a_listed.has_value() && a_listed->size() == 1,
             "I1-R6: tenant-a's own admin can still read its own item normally -- the fix closes "
             "the cross-tenant leak without breaking legitimate same-tenant access");

    // --- The pre-existing same-tenant guarantee (Milestone 4) is unaffected by this fix ----------
    ae::Principal const tenant_a_other{"other-user", "tenant-a"};
    AE_CHECK(ae::memory_ref_name(tenant_a_admin) != ae::memory_ref_name(tenant_a_other),
             "I1-R7: two DIFFERENT ids within the SAME tenant still get distinct ref names -- "
             "unchanged Milestone 4 behavior, not a regression introduced by folding tenant_id in");

    std::cout << (g_failures == 0 ? "test_memory_cross_tenant_isolation: OK\n"
                                   : "test_memory_cross_tenant_isolation: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
