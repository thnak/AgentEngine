// PROVE-PHASE PROBE (A2/§34): "Process 1" of a REAL process restart -- proves the durable
// `Ledger<FileWorktreeObjectStore>` (branches_/checkpoints/ACL now persisted atop the
// already-durable object-store content, §28.2) actually survives a genuine process exit, composed
// with A1's durable identity so the branch owner reliably gets its OWN id back on the read side.
//
// Real separate OS process from durable_ledger_read.cpp -- not an in-process simulation.

#include "file_object_store.hpp"
#include "worktree_ledger.hpp"

#include <cstdio>
#include <filesystem>

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }
}  // namespace

int main() {
    using namespace probe;

    std::filesystem::path const root = "ae_durable_ledger_probe";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    // A1's durable identity, reused here so "owner" reliably gets the same internal id back on the
    // read side -- a durable Ledger composed with a NON-durable identity would be exactly §33's
    // leak all over again, just one layer up. This IS the "process/lifetime independence" assumption
    // §34 states explicitly: both durable stores live under the SAME real durable root.
    IdentityAuthority& authority = IdentityAuthority::bootstrap(root / "identity");
    Principal owner = authority.adopt("owner", "");

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(root / "objects"), root / "ledger");

    auto branch_r = run(ledger.create_root_branch(owner));
    if (!branch_r.has_value()) { std::fprintf(stderr, "create_root_branch failed\n"); return 1; }
    BranchHandle<FileWorktreeObjectStore> branch = std::move(*branch_r);

    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    if (!quota.has_value()) { std::fprintf(stderr, "quota mint failed\n"); return 1; }

    std::string const content = "durable ledger content, turn 1";
    std::vector<std::byte> bytes(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);
    auto blob_digest = ledger.put_blob_safe(bytes, owner);
    if (!blob_digest.has_value()) { std::fprintf(stderr, "put_blob_safe failed\n"); return 1; }

    agentengine::Tree tree;
    tree.entries.push_back(agentengine::TreeEntry{"a.txt", *blob_digest, false});
    auto cp1 = run(ledger.commit(branch, tree, owner, *quota));
    if (!cp1.has_value()) { std::fprintf(stderr, "commit turn 1 failed\n"); return 1; }

    std::string const content2 = "durable ledger content, turn 2";
    std::vector<std::byte> bytes2(content2.size());
    for (std::size_t i = 0; i < content2.size(); ++i) bytes2[i] = static_cast<std::byte>(content2[i]);
    auto blob_digest2 = ledger.put_blob_safe(bytes2, owner);
    if (!blob_digest2.has_value()) { std::fprintf(stderr, "put_blob_safe 2 failed\n"); return 1; }
    agentengine::Tree tree2;
    tree2.entries.push_back(agentengine::TreeEntry{"a.txt", *blob_digest, false});
    tree2.entries.push_back(agentengine::TreeEntry{"b.txt", *blob_digest2, false});
    auto cp2 = run(ledger.commit(branch, tree2, owner, *quota));
    if (!cp2.has_value()) { std::fprintf(stderr, "commit turn 2 failed\n"); return 1; }

    std::printf("[write] owner id=%llu\n", static_cast<unsigned long long>(owner.id()));
    std::printf("[write] branch=%s turn1_tree=%s turn2_tree=%s\n", branch.name().c_str(),
                cp1->tree.c_str(), cp2->tree.c_str());
    std::printf("[write] (this process now exits -- IdentityAuthority AND Ledger in-memory state "
                "both die with it; only the durable directories under %s survive on disk)\n",
                root.string().c_str());

    // Leave `branch` unresolved deliberately (no release/abandon) -- this is exactly the real,
    // still-open A7 scenario (§11 item 6): a process that exits without a clean handle resolution.
    // Not this probe's job to fix that (A7 is its own later step); just don't let the destructor's
    // own synchronous queue_pending_abandon() call touch a Ledger that's about to be destroyed
    // anyway in the same stack unwind -- which is exactly what happens here, harmlessly, since nothing
    // ever reads pending_abandons_ across a restart today (also A7's job, not A2's).
    return 0;
}
