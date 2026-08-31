// PROVE-PHASE PROBE: real concurrent multi-session load on ONE shared Ledger + shared
// InMemoryWorktreeObjectStore -- §26.4/§27.4 named this as not stress-tested. Multiple independent
// branches (simulating multiple concurrent sessions) commit repeatedly and concurrently; each
// branch's own turn sequence must stay internally consistent (no lost commits, no cross-branch
// corruption) even though every commit path shares the SAME Ledger instance and the SAME underlying
// object store.

#include "worktree_ledger.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal root_owner = authority.mint_root("concurrent-ledger-root");
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, root_owner, 100'000'000);
    CHECK(quota.has_value());

    Ledger ledger;

    constexpr int kSessions = 12;
    constexpr int kCommitsPerSession = 100;

    std::vector<std::thread> threads;
    std::vector<bool> session_ok(kSessions, false);

    for (int s = 0; s < kSessions; ++s) {
        threads.emplace_back([&, s]() {
            Principal owner = authority.derive_child(root_owner, "session-" + std::to_string(s));
            // A REAL FINDING a code-review pass caught elsewhere (AsyncQuota::try_consume() used to
            // silently skip its own spender-identity check) means this probe must now do what the
            // design always specified: a derived-child principal spends from a SHARE its parent
            // explicitly allocated (§21/§34's own store-wide-ceiling pattern), never straight from a
            // quota it was never granted a share of.
            auto session_quota = block_on(quota->allocate_child_share(owner, 1'000'000));
            if (!session_quota.has_value()) return;
            auto root_branch = block_on(ledger.create_root_branch(owner));
            if (!root_branch.has_value()) return;
            BranchHandle branch = std::move(*root_branch);

            bool ok = true;
            for (int i = 0; i < kCommitsPerSession; ++i) {
                // §28's ACL fix (from the real internal-attack simulation) requires every entry's
                // digest to already be authorized for the committing principal -- put_blob_safe()
                // first, matching what a real caller (RealIoFileSystem::drain_into_tree()) always
                // does, rather than hand-fabricating an arbitrary "digest" string with no ACL entry.
                std::string content = std::to_string(s) + "-" + std::to_string(i);
                std::vector<std::byte> bytes(content.size());
                for (std::size_t b = 0; b < content.size(); ++b) bytes[b] = static_cast<std::byte>(content[b]);
                auto blob_digest = ledger.put_blob_safe(bytes, owner);
                if (!blob_digest.has_value()) { ok = false; break; }
                agentengine::Tree tree;
                tree.entries.push_back(agentengine::TreeEntry{"file.txt", *blob_digest, false});
                auto cp = block_on(ledger.commit(branch, tree, owner, *quota));
                if (!cp.has_value()) { ok = false; break; }
                // Each session's OWN turn_index must be exactly i+1 -- strictly sequential FOR THIS
                // BRANCH, regardless of how many other sessions are committing concurrently to the
                // SAME shared Ledger/object store at the same moment.
                if (cp->turn_index != static_cast<std::uint64_t>(i + 1)) { ok = false; break; }
            }
            session_ok[s] = ok;
            // Release the branch cleanly.
            (void)block_on(ledger.abandon(std::move(branch)));
        });
    }
    for (auto& th : threads) th.join();

    bool all_ok = true;
    for (bool ok : session_ok) if (!ok) all_ok = false;
    std::printf("[1] %d concurrent sessions x %d commits each on ONE shared Ledger: all sessions "
                "maintained a strictly sequential, uncorrupted turn_index (1..%d) despite sharing "
                "the same Ledger/object store: all_ok=%d\n",
                kSessions, kCommitsPerSession, kCommitsPerSession, (int)all_ok);
    CHECK(all_ok);

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
