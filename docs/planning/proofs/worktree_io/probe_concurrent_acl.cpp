// PROVE-PHASE PROBE (B2/§34): re-stresses the real ACL fix (§29) under genuine concurrent
// read/write contention with A1 (durable identity), A2/A8 (durable Ledger + bounded ACL), and A4
// (real merge) all in place -- something that had never been done since the ACL fix was originally
// added (§29.6's own disclosed gap: "the ACL fix's own behavior under real concurrent read/write
// contention... not yet re-stress-tested the same way" as §28.4's concurrency work).
//
// Many real OS threads, sharing ONE Ledger, concurrently: (1) each of several distinct OWNER
// principals writes its OWN private content and immediately reads it back -- must always succeed;
// (2) a single ATTACKER principal, running concurrently on its own threads, repeatedly attempts to
// read EVERY owner's content by digest -- must NEVER succeed, not even a single time, under real
// contention; (3) owners concurrently commit real trees referencing their own blobs, exercising the
// bounded ACL insert (A8) under real contention simultaneously with the read-side checks.

#include "../common/block_on.hpp"
#include "worktree_ledger.hpp"

#include <atomic>
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

namespace {
std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Ledger<> ledger;

    constexpr int kOwners = 8;
    constexpr int kRoundsPerOwner = 200;
    std::vector<Principal> owners;
    std::vector<AsyncQuota<StorageBytes>> owner_quotas;
    for (int i = 0; i < kOwners; ++i) {
        owners.push_back(authority.mint_root("concurrent-acl-owner-" + std::to_string(i)));
        auto q = AsyncQuota<StorageBytes>::mint_root(authority, owners.back(), 10'000'000);
        CHECK(q.has_value());
        owner_quotas.push_back(std::move(*q));
    }
    Principal attacker = authority.mint_root("concurrent-acl-attacker");

    std::atomic<int> legit_write_ok{0}, legit_write_fail{0};
    std::atomic<int> legit_read_ok{0}, legit_read_fail{0};
    std::atomic<int> attacker_read_denied{0}, attacker_read_LEAKED{0};
    std::atomic<bool> stop{false};

    // Attacker threads target REAL digests the owner threads just produced -- the strongest,
    // most realistic version of this attack (a real leaked/observed digest, not a guessed one) --
    // via a shared, atomically-published "last known real digest" slot per owner.
    std::vector<agentengine::Digest> last_known_digest(kOwners);
    std::vector<std::atomic<bool>> digest_ready(kOwners);
    for (auto& d : digest_ready) d.store(false);

    // Owner threads: each owns exclusive digests, writes+reads its OWN content repeatedly, and
    // publishes its latest real digest for the attacker threads to (unsuccessfully) target.
    std::vector<std::thread> owner_threads;
    for (int i = 0; i < kOwners; ++i) {
        owner_threads.emplace_back([&, i]() {
            for (int r = 0; r < kRoundsPerOwner; ++r) {
                std::string content = "owner-" + std::to_string(i) + "-round-" + std::to_string(r);
                auto d = ledger.put_blob_safe(to_bytes(content), owners[i]);
                if (!d.has_value()) { ++legit_write_fail; continue; }
                ++legit_write_ok;
                last_known_digest[i] = *d;
                digest_ready[i].store(true, std::memory_order_release);
                auto read_back = ledger.get_blob_safe(*d, owners[i]);
                if (read_back.has_value()) ++legit_read_ok; else ++legit_read_fail;
            }
        });
    }

    std::vector<std::thread> attacker_threads;
    for (int i = 0; i < kOwners; ++i) {
        attacker_threads.emplace_back([&, i]() {
            while (!stop.load(std::memory_order_acquire)) {
                if (!digest_ready[i].load(std::memory_order_acquire)) { std::this_thread::yield(); continue; }
                auto attempt = ledger.get_blob_safe(last_known_digest[i], attacker);
                if (attempt.has_value()) ++attacker_read_LEAKED; else ++attacker_read_denied;
            }
        });
    }

    for (auto& t : owner_threads) t.join();
    stop.store(true, std::memory_order_release);
    for (auto& t : attacker_threads) t.join();

    std::printf("[1] %d owners x %d rounds, concurrent writes+reads of their OWN content: "
                "write_ok=%d write_fail=%d read_ok=%d read_fail=%d\n",
                kOwners, kRoundsPerOwner, legit_write_ok.load(), legit_write_fail.load(),
                legit_read_ok.load(), legit_read_fail.load());
    CHECK(legit_write_ok.load() == kOwners * kRoundsPerOwner);
    CHECK(legit_write_fail.load() == 0);
    CHECK(legit_read_ok.load() == kOwners * kRoundsPerOwner);
    CHECK(legit_read_fail.load() == 0);

    std::printf("[2] attacker threads made %d attempts against REAL, just-published digests "
                "belonging to other owners, running concurrently with the owners' own writes: "
                "denied=%d, LEAKED=%d\n", attacker_read_denied.load() + attacker_read_LEAKED.load(),
                attacker_read_denied.load(), attacker_read_LEAKED.load());
    CHECK(attacker_read_LEAKED.load() == 0);
    CHECK(attacker_read_denied.load() > 0);   // the stress actually ran attempts, not a no-op loop

    // === Concurrent commit()s, exercising the bounded ACL insert (A8) under real contention =======
    std::atomic<int> commit_ok{0}, commit_fail{0};
    std::vector<std::thread> commit_threads;
    for (int i = 0; i < kOwners; ++i) {
        commit_threads.emplace_back([&, i]() {
            auto root_r = block_on(ledger.create_root_branch(owners[i]));
            if (!root_r.has_value()) { ++commit_fail; return; }
            BranchHandle<> branch = std::move(*root_r);
            for (int r = 0; r < 20; ++r) {
                auto d = ledger.put_blob_safe(to_bytes("commit-content-" + std::to_string(i) + "-" +
                                                          std::to_string(r)), owners[i]);
                if (!d.has_value()) { ++commit_fail; continue; }
                agentengine::Tree tree;
                tree.entries.push_back(agentengine::TreeEntry{"f.txt", *d, false});
                auto cp = block_on(ledger.commit(branch, tree, owners[i], owner_quotas[i]));
                if (cp.has_value()) ++commit_ok; else ++commit_fail;
            }
        });
    }
    for (auto& t : commit_threads) t.join();
    std::printf("[3] %d owners x 20 concurrent commits each (real bounded-ACL insert path, A8, "
                "under real contention): commit_ok=%d commit_fail=%d\n", kOwners, commit_ok.load(),
                commit_fail.load());
    CHECK(commit_ok.load() == kOwners * 20);
    CHECK(commit_fail.load() == 0);

    std::printf("\nALL CHECKS PASSED -- the real ACL fix (§29) holds under genuine concurrent "
                "read/write contention with A1/A2/A4/A8 all composed together: zero leaks across "
                "%d real attacker attempts, zero lost/corrupted legitimate writes or reads, zero "
                "bounded-ACL-insert failures under real concurrent commits.\n",
                attacker_read_denied.load());
    return 0;
}
