// PROVE-PHASE PROBE (A7/§34): "Process 2" -- a genuinely separate, later-launched OS process
// reopening the SAME durable directories `crash_reclaim_write.cpp` used, after that process
// exited with its one live BranchHandle never cleanly released. Proves, for real:
//   (1) the branch shows up in orphaned_branches() -- the Ledger correctly recognizes it has no
//       live handle anywhere;
//   (2) an UNRELATED principal cannot reclaim it merely by knowing its name -- the ACL gate on
//       reclaim_orphaned_branch() actually holds, not just in prose;
//   (3) the LEGITIMATE owner (re-adopted via A1's durable identity, so it gets its OWN id back)
//       CAN reclaim it, receiving a genuinely fresh, live BranchHandle;
//   (4) that reclaimed handle is fully functional -- a further real commit succeeds through it;
//   (5) once reclaimed, the SAME orphan cannot be reclaimed a second time (no double-reclaim).

#include "file_object_store.hpp"
#include "worktree_ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
}  // namespace

int main() {
    using namespace probe;

    std::filesystem::path const root = "ae_crash_reclaim_probe";
    CHECK(std::filesystem::exists(root / "ledger" / "ledger_state.snapshot"));

    IdentityAuthority& authority = IdentityAuthority::bootstrap(root / "identity");
    Principal owner = authority.adopt("crash-owner", "");
    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(root / "objects"), root / "ledger");

    std::string const branch_name = "root-" + std::to_string(owner.id());

    // (1) The branch is recognized as orphaned.
    auto orphans = ledger.orphaned_branches();
    bool found = false;
    for (auto const& name : orphans) if (name == branch_name) found = true;
    CHECK(found);
    std::printf("[read] (1) branch '%s' correctly appears in orphaned_branches() after the "
                "restart -- PASS\n", branch_name.c_str());

    // (2) An unrelated principal cannot reclaim it.
    Principal outsider = authority.mint_root("crash-reclaim-outsider");
    auto outsider_attempt = ledger.reclaim_orphaned_branch(branch_name, outsider);
    CHECK(!outsider_attempt.has_value());
    CHECK(outsider_attempt.error().code == "ledger.reclaim_unauthorized");
    std::printf("[read] (2) an UNRELATED principal's reclaim attempt was REJECTED (%s) -- knowing "
                "the branch's name is not enough -- PASS\n", outsider_attempt.error().code.c_str());

    // (3) The legitimate owner reclaims it -- a genuinely fresh, live handle.
    auto reclaimed = ledger.reclaim_orphaned_branch(branch_name, owner);
    CHECK(reclaimed.has_value());
    BranchHandle<FileWorktreeObjectStore> branch = std::move(*reclaimed);
    CHECK(branch.name() == branch_name);
    std::printf("[read] (3) the legitimate owner successfully reclaimed the orphaned branch -- a "
                "real, fresh, live BranchHandle was minted -- PASS\n");

    // (4) The reclaimed handle is fully functional -- commit a real second turn through it.
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    CHECK(quota.has_value());
    std::string const content2 = "content committed AFTER reclaiming from the crash";
    std::vector<std::byte> bytes2(content2.size());
    for (std::size_t i = 0; i < content2.size(); ++i) bytes2[i] = static_cast<std::byte>(content2[i]);
    auto d2 = ledger.put_blob_safe(bytes2, owner);
    CHECK(d2.has_value());
    agentengine::Tree tree2;
    tree2.entries.push_back(agentengine::TreeEntry{"a.txt", *d2, false});
    auto cp2 = run(ledger.commit(branch, tree2, owner, *quota));
    CHECK(cp2.has_value());
    CHECK(cp2->turn_index == 2);   // turn 1 was committed pre-crash; this is genuinely turn 2,
                                      // continuing the SAME branch's real history, not a fresh start
    std::printf("[read] (4) a real commit through the reclaimed handle succeeded (turn_index=2, "
                "continuing the branch's real pre-crash history) -- PASS\n");

    // (5) The same orphan cannot be reclaimed a second time.
    auto double_reclaim = ledger.reclaim_orphaned_branch(branch_name, owner);
    CHECK(!double_reclaim.has_value());
    CHECK(double_reclaim.error().code == "ledger.not_an_orphan");
    std::printf("[read] (5) a second reclaim attempt on the SAME (now-live) branch was correctly "
                "REJECTED (%s) -- no double-reclaim -- PASS\n", double_reclaim.error().code.c_str());

    auto abandoned = run(ledger.abandon(std::move(branch)));
    CHECK(abandoned.has_value());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("\nALL CHECKS PASSED -- a branch orphaned by a real process crash/exit is neither "
                "lost nor silently discarded: it is recognized, gated by the same ACL every other "
                "read already uses, explicitly reclaimable exactly once by its legitimate owner, "
                "and fully functional afterward.\n");
    return 0;
}
