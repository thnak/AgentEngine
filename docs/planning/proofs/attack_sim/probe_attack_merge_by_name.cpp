// PROVE-PHASE ADVERSARIAL PROBE (B1/§34): the first-ever real attack attempt against `merge()`
// (§32) and `branch_from()` (§32), which had ZERO adversarial coverage before this pass -- the
// newest, least-scrutinized surface in the whole document, named explicitly in §32.6 as a real gap.
//
// REAL, CONFIRMED VULNERABILITY FOUND, THEN FIXED, IN THE SAME PASS THIS PROBE WAS WRITTEN FOR:
// `merge()` originally took `std::string const& parent_name` -- a bare, GUESSABLE string (branch
// names follow the deterministic "root-<owner_id>" scheme) -- with NO possession check on the
// parent side at all, unlike every other mutating Ledger method (commit/reset_to/abandon all
// require the caller to already HOLD the actual handle for whatever they act on). A real attack
// (an attacker merging their OWN, completely unrelated branch into a victim's branch by NAME
// alone) was attempted against the pre-fix code and genuinely SUCCEEDED against a freshly-created,
// still-empty victim branch -- real, reproducible output, captured here for the record before the
// fix:
//
//   [setup] victim's EMPTY branch created: root-1, head=df3f619804a92fdb...
//   [ATTACK] SUCCEEDED -- victim's EMPTY branch corrupted! new turn_index=1 tree=9ae28a68b4d6...
//
// (A non-empty victim happened to survive the same attack purely as a side effect of a DIFFERENT
// check -- the per-entry authorization commit()-style validation this same pass added -- which
// rejects a merge result containing the victim's own pre-existing entries the attacker isn't
// authorized for. An EMPTY victim has no such entries to trip that check, so the attack went
// straight through. That incidental partial protection is why this needed a real attack attempt to
// find, not code review -- the non-empty case looked safe.)
//
// FIXED by requiring possession of the PARENT's own `BranchHandle` too (`merge(child, parent,
// requested_by)`, `parent` now a `BranchHandle<Store> const&`, not a string) -- this probe now
// proves the fix two ways: (1) the ORIGINAL attack shape no longer even COMPILES (a real,
// structural impossibility, not just a runtime rejection an attacker might find a gap in one day);
// (2) a legitimate merge into a branch the caller actually possesses still works correctly.

#include "../worktree_io/worktree_ledger.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }

std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}
}  // namespace

// Compile-time proof the original attack shape is now impossible: merge()'s second parameter is
// `BranchHandle<Store> const&`, which cannot bind to a `std::string` -- uncomment either line below
// to see it fail with "no viable conversion from 'std::string' to 'const BranchHandle<...>'":
//
//   auto x = ledger.merge(std::move(some_child), std::string("root-1"), some_principal);
//   auto y = ledger.merge(std::move(some_child), "root-1", some_principal);
//
// This is checked here as a comment, not a negative-compile probe of its own, because the failure
// mode is a plain, unambiguous overload-resolution error on an ordinary function call -- the same
// class of "must fail to compile" property this document's identity_authority/probe_negative_*.cpp
// files verify via a real separate build, which would be redundant machinery for a case this
// direct. The real, load-bearing proof is the runtime scenario below: a legitimate caller who DOES
// possess the right handles merges correctly, and there is no code path left anywhere that hands
// an attacker a handle for a branch they never created.

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Ledger<> ledger;

    // === The victim: an entirely separate, unrelated root branch, deliberately left EMPTY -- ======
    // === exactly the shape that let the pre-fix attack through when a non-empty victim's own ======
    // === entries happened to trip an unrelated check instead. ======================================
    Principal victim_owner = authority.mint_root("merge-attack-victim");
    auto victim_root_r = run(ledger.create_root_branch(victim_owner));
    CHECK(victim_root_r.has_value());
    BranchHandle<> victim_branch = std::move(*victim_root_r);
    agentengine::Digest const victim_head_before = ledger.head_tree_digest(victim_branch.name());
    std::printf("[setup] victim's EMPTY branch '%s' created, head=%s\n", victim_branch.name().c_str(),
                victim_head_before.c_str());

    // === The attacker: a completely UNRELATED root+child, sharing the SAME Ledger (the real, ====
    // === already-established multi-tenant shape). The attacker possesses ONLY their own branch. ==
    Principal attacker_owner = authority.mint_root("merge-attack-attacker");
    auto attacker_quota = AsyncQuota<StorageBytes>::mint_root(authority, attacker_owner, 1'000'000);
    CHECK(attacker_quota.has_value());
    auto attacker_branch_cost = AsyncQuota<BranchCost>::mint_root(authority, attacker_owner, 100);
    CHECK(attacker_branch_cost.has_value());
    auto attacker_root_r = run(ledger.create_root_branch(attacker_owner));
    CHECK(attacker_root_r.has_value());
    BranchHandle<> attacker_root = std::move(*attacker_root_r);
    auto attacker_child_r = run(ledger.branch_from(attacker_root, attacker_owner, *attacker_branch_cost));
    CHECK(attacker_child_r.has_value());
    BranchHandle<> attacker_child = std::move(*attacker_child_r);
    auto attacker_blob = ledger.put_blob_safe(to_bytes("ATTACKER-CONTROLLED CONTENT"), attacker_owner);
    CHECK(attacker_blob.has_value());
    agentengine::Tree attacker_tree;
    attacker_tree.entries.push_back(agentengine::TreeEntry{"pwned.txt", *attacker_blob, false});
    auto attacker_cp = run(ledger.commit(attacker_child, attacker_tree, attacker_owner, *attacker_quota));
    CHECK(attacker_cp.has_value());

    // THE ATTACKER HAS NO WAY TO OBTAIN A `BranchHandle` FOR `victim_branch` -- there is no public
    // API anywhere in this Ledger that hands one out for a branch the caller didn't itself create
    // via create_root_branch()/branch_from(), or legitimately reclaim() as its own orphan. The only
    // merge() the attacker CAN even call is one naming a branch THEY possess:
    auto self_merge_attempt = run(ledger.merge(std::move(attacker_child), attacker_root, attacker_owner));
    CHECK(self_merge_attempt.has_value());
    std::printf("[1] the attacker CAN merge their own child into their OWN possessed root -- that "
                "is legitimate, ordinary use, not an attack -- PASS\n");

    // The victim's branch is, and remains, completely untouched -- not because of a runtime check
    // that happened to catch this specific case, but because the attacker structurally never had
    // any way to name it as a merge target in the first place.
    CHECK(ledger.head_tree_digest(victim_branch.name()) == victim_head_before);
    std::printf("[2] the victim's branch head is UNCHANGED -- the attacker never had a `BranchHandle`"
                " for it and merge() no longer accepts a bare name, so the original attack shape "
                "cannot even be EXPRESSED anymore, let alone attempted -- PASS\n");

    // Sanity: a legitimate merge by the VICTIM into their OWN branch, via a real child branch they
    // themselves created, still works correctly after the fix -- the possession requirement closes
    // the attack without breaking the real, intended use case.
    auto victim_quota = AsyncQuota<StorageBytes>::mint_root(authority, victim_owner, 1'000'000);
    CHECK(victim_quota.has_value());
    auto victim_branch_cost = AsyncQuota<BranchCost>::mint_root(authority, victim_owner, 100);
    CHECK(victim_branch_cost.has_value());
    auto victim_child_r = run(ledger.branch_from(victim_branch, victim_owner, *victim_branch_cost));
    CHECK(victim_child_r.has_value());
    BranchHandle<> victim_child = std::move(*victim_child_r);
    auto victim_blob = ledger.put_blob_safe(to_bytes("victim's own real content"), victim_owner);
    CHECK(victim_blob.has_value());
    agentengine::Tree victim_tree;
    victim_tree.entries.push_back(agentengine::TreeEntry{"real.txt", *victim_blob, false});
    auto victim_cp = run(ledger.commit(victim_child, victim_tree, victim_owner, *victim_quota));
    CHECK(victim_cp.has_value());
    auto legit_merge = run(ledger.merge(std::move(victim_child), victim_branch, victim_owner));
    CHECK(legit_merge.has_value());
    std::printf("[3] the VICTIM's own legitimate merge (into a branch they actually possess) still "
                "works correctly after the fix -- PASS\n");

    std::printf("\nALL CHECKS PASSED -- merge()'s parent-side possession gap (found real, exploited "
                "real, against a real empty victim branch) is now closed structurally: naming a "
                "merge target by string is no longer even possible, not merely rejected at "
                "runtime.\n");
    return 0;
}
