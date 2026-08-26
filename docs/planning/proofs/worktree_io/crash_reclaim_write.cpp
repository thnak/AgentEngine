// PROVE-PHASE PROBE (A7/§34): "Process 1" -- creates a durable branch, commits to it, then exits
// WITHOUT ever calling release_branch()/abandon() -- simulating a real crash (or an ordinary exit
// that simply forgot to clean up; this Ledger cannot tell the difference and does not need to).
// The `branch` handle's own destructor never runs its real body here because the process exits via
// `return` from main with the handle still in scope -- its destructor DOES run during normal stack
// unwind on a clean `return`, calling `queue_pending_abandon()` -- but that queued abandonment is
// itself only IN-MEMORY (`pending_abandons_` is never persisted), so it is lost the instant this
// process exits regardless. This is deliberate: it reproduces the real crash scenario A7 exists
// for, not a contrived one.

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

    std::filesystem::path const root = "ae_crash_reclaim_probe";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    IdentityAuthority& authority = IdentityAuthority::bootstrap(root / "identity");
    Principal owner = authority.adopt("crash-owner", "");

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(root / "objects"), root / "ledger");
    auto branch_r = run(ledger.create_root_branch(owner));
    if (!branch_r.has_value()) { std::fprintf(stderr, "create_root_branch failed\n"); return 1; }
    BranchHandle<FileWorktreeObjectStore> branch = std::move(*branch_r);

    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    if (!quota.has_value()) { std::fprintf(stderr, "quota mint failed\n"); return 1; }

    std::string const content = "content committed before the crash";
    std::vector<std::byte> bytes(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);
    auto d = ledger.put_blob_safe(bytes, owner);
    if (!d.has_value()) { std::fprintf(stderr, "put_blob_safe failed\n"); return 1; }
    agentengine::Tree tree;
    tree.entries.push_back(agentengine::TreeEntry{"a.txt", *d, false});
    auto cp = run(ledger.commit(branch, tree, owner, *quota));
    if (!cp.has_value()) { std::fprintf(stderr, "commit failed\n"); return 1; }

    std::printf("[write] owner id=%llu, branch=%s, committed turn_index=%llu\n",
                static_cast<unsigned long long>(owner.id()), branch.name().c_str(),
                static_cast<unsigned long long>(cp->turn_index));
    std::printf("[write] (this process now exits with `branch` still live and never released -- "
                "simulating a crash; no clean abandon()/release_branch() call is ever made)\n");
    return 0;
}
