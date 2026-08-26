#pragma once
// PROVE-PHASE PROBE: the REAL SandboxSession (§6/§19.3), this time actually composing the other
// three already-proven primitives instead of standing in for them -- Ledger (§23), MediatedFileSystem
// (§22), AsyncQuota<StorageBytes> (§21). This is the "prove tich hop cac thanh phan do" (prove the
// integration of those components) pass: does the whole stack actually fit together as ONE movable,
// task<result<SandboxSession>>-returning type, or does composing four independently-proven pieces
// surface a NEW seam none of them hit alone?
//
// POST-REVIEW REWIRE: this file originally composed `../ledger/ledger.hpp` (§23's Ledger --
// FNV-1a placeholder digests, no blob storage, no ACL, but real branch_from()/merge()). An
// independent code review confirmed that made this "full stack" demo and §29's attack-simulation
// demo (which hardened `../worktree_io/worktree_ledger.hpp` -- real SHA-256 content-addressing,
// the real §28.4 concurrency fix, the real §29 ACL fix) claims about TWO DIFFERENT, non-overlapping
// artifacts: nothing here was ever actually vulnerable to (or protected against) the attacks §29
// found, because this Ledger had no shared blob store for those attacks to target in the first
// place. Now rewired onto `worktree_io/worktree_ledger.hpp` -- the same Ledger §29 hardened --
// after that file was extended (this same review pass) with the `branch_from()`/`merge()` it was
// previously missing, so there is now exactly ONE Ledger implementation with real storage + the
// concurrency fix + the ACL fix + branch/merge, and this "full stack" demo is actually composing
// it. See probe_full_stack.cpp's own added case proving the ACL fix is live through this exact
// composed path, not just in worktree_ledger.hpp's own standalone probes.

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"
#include "../worktree_io/worktree_ledger.hpp"

namespace probe {

struct StagedWrite {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint64_t author_id = 0;
};

// §22's MediatedFileSystem, rebuilt here composing INTO SandboxSession rather than standing alone --
// see this file's own §26.1 finding for why sync_mutex_ needed the identical unique_ptr fix §21.1
// found for AsyncQuota/SandboxSession's own mutex members.
class MediatedFileSystem {
public:
    MediatedFileSystem()
        : sync_mutex_(std::make_unique<std::mutex>()),
          commit_lock_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    [[nodiscard]] result<void> write(std::string path, std::vector<std::byte> bytes,
                                       std::uint64_t author_id) {
        std::lock_guard<std::mutex> guard(*sync_mutex_);
        staged_writes_.push_back(StagedWrite{std::move(path), std::move(bytes), author_id});
        return result<void>{};
    }

    [[nodiscard]] agentengine::rt::task<result<std::vector<StagedWrite>>> drain_staged_writes() {
        std::vector<StagedWrite> batch;
        {
            std::lock_guard<std::mutex> guard(*sync_mutex_);
            batch = std::move(staged_writes_);
            staged_writes_.clear();
        }
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();
        co_return batch;
    }

private:
    std::unique_ptr<std::mutex> sync_mutex_;
    std::vector<StagedWrite> staged_writes_;
    std::unique_ptr<agentengine::rt::AsyncMutex> commit_lock_;
};

// REAL content-addressed tree building -- replaces the original's fake "sorted concatenation of
// paths" placeholder, which an independent code review caught was never actually sorted (its own
// comment claimed "sorted concatenation" but the loop had no sort call, making the resulting
// fake digest order-dependent rather than content-set-dependent -- unexercised only because every
// prior test turn staged exactly one file). Each staged write is put through the REAL
// content-addressed store via Ledger::put_blob_safe() (real SHA-256, real dedup, real per-writer
// ACL registration), and the resulting agentengine::Tree is handed to Ledger::commit(), which
// itself goes through agentengine::WorktreeObjectStore::put_tree() -- genuinely sorted by name
// before hashing (worktree_types.hpp's own documented precondition), not a hand-rolled string.
[[nodiscard]] inline result<agentengine::Tree> combine_into_tree(std::vector<StagedWrite> const& writes,
                                                                    Ledger<>& ledger, Principal author) {
    agentengine::Tree tree;
    for (auto const& w : writes) {
        auto digest = ledger.put_blob_safe(w.bytes, author);
        if (!digest.has_value()) return std::unexpected(digest.error());
        tree.entries.push_back(agentengine::TreeEntry{w.path, *digest, false});
    }
    return tree;
}

class SandboxSession {
public:
    [[nodiscard]] static agentengine::rt::task<result<SandboxSession>> create(Ledger<>& ledger,
                                                                                 BranchHandle<> branch) {
        co_return SandboxSession(ledger, std::move(branch));
    }

    SandboxSession(SandboxSession const&) = delete;
    SandboxSession& operator=(SandboxSession const&) = delete;
    SandboxSession(SandboxSession&&) = default;
    SandboxSession& operator=(SandboxSession&&) = default;

    [[nodiscard]] MediatedFileSystem& filesystem() noexcept { return fs_; }
    // Exposes the real, composed Ledger -- needed by callers (e.g. RealSandboxReflector's
    // reset_sandbox closure) that must inspect a REAL tree's actual entries after a rollback,
    // now that Checkpoint::tree is an opaque real SHA-256 digest rather than the old fake
    // Ledger's human-readable "sorted path concatenation" string.
    [[nodiscard]] Ledger<>& ledger() noexcept { return *ledger_; }
    [[nodiscard]] std::string const& branch_name() const noexcept { return branch_.name(); }

    // The REAL turn-boundary operation (§6/§15.2): drains staged writes, commits through the REAL
    // Ledger, consuming the REAL AsyncQuota<StorageBytes>, AND runs the real reap_pending_abandons()/
    // collect_garbage() maintenance step §15.2 wired into this exact call.
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> harvest_and_checkpoint(
        Principal turn_owner, AsyncQuota<StorageBytes>& quota) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        if (released_) {
            co_return std::unexpected(error{"SandboxSession used after release_branch()",
                                              "sandbox_session.already_released"});
        }
        auto staged = co_await fs_.drain_staged_writes();
        if (!staged.has_value()) co_return std::unexpected(staged.error());
        auto tree = combine_into_tree(*staged, *ledger_, turn_owner);
        if (!tree.has_value()) co_return std::unexpected(tree.error());
        auto committed = co_await ledger_->commit(branch_, *tree, turn_owner, quota);
        if (!committed.has_value()) co_return std::unexpected(committed.error());
        (void)co_await ledger_->reap_pending_abandons();
        co_return *committed;
    }

    // §7/§17's dynamically-checked rollback -- delegates to the REAL Ledger::reset_to().
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> reset_to_turn(std::uint64_t turn_index,
                                                                              Principal requested_by) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        if (released_) {
            co_return std::unexpected(error{"SandboxSession used after release_branch()",
                                              "sandbox_session.already_released"});
        }
        co_return co_await ledger_->reset_to(branch_, turn_index, requested_by);
    }

    [[nodiscard]] agentengine::rt::task<result<BranchHandle<>>> release_branch() && {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        released_ = true;
        co_return std::move(branch_);
    }

private:
    SandboxSession(Ledger<>& ledger, BranchHandle<> branch)
        : ledger_(&ledger), branch_(std::move(branch)),
          exclusivity_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    Ledger<>* ledger_;
    BranchHandle<> branch_;
    MediatedFileSystem fs_;
    std::unique_ptr<agentengine::rt::AsyncMutex> exclusivity_;
    bool released_ = false;
};

}  // namespace probe
