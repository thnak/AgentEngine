#pragma once
// ADR-207 (#120 S7): the member function bodies of `Ledger<Store>` (core/ledger.hpp), moved verbatim out of that
// header. ledger.hpp keeps the class, every declaration and comment, the constructor, the small accessors and the
// member templates; the design and the authorization rule each body implements are documented at its declaration
// there.
//
// Nothing needs to include this file to USE a Ledger: src/core/ledger.cpp explicitly instantiates
// `Ledger<InMemoryWorktreeObjectStore>` and `Ledger<FileWorktreeObjectStore>`, the only two stores the codebase has,
// and every file that includes ledger.hpp links against those (agentengine::worktree_store). Code that brings a
// THIRD `WorktreeObjectStore` conformer includes this file in one of its own translation units and instantiates
// `template class agentengine::Ledger<ItsStore>;` there, the way src/core/ledger.cpp does -- Ledger stays generic over
// any conformer without anyone editing ledger.hpp.

#include "agentengine/core/ledger.hpp"

namespace agentengine {

template <class Store>
agentengine::result<void> Ledger<Store>::mark_digest_shared(agentengine::Digest digest, bool is_tree,
                                                               agentengine::IdentityHandle requested_by) {
    std::lock_guard<std::mutex> g(mutex_);
    auto& acl = is_tree ? tree_acl_ : blob_acl_;
    if (!authorized_for(acl, digest, requested_by)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "requester is not authorized for this digest -- only a principal already authorized "
            "for it may mark it publicly shared",
            "ledger.mark_shared_unauthorized"});
    }
    // Direct insertion, deliberately bypassing insert_acl_root_bounded()'s own cap check -- this
    // IS the cap's escape hatch, so it must never itself be subject to the cap it exists to let
    // an owner opt out of.
    acl[digest].insert(kPubliclySharedSentinelRootId);
    persist_snapshot_locked();
    return {};
}

template <class Store>
agentengine::result<std::vector<std::byte>> Ledger<Store>::get_blob_safe(
        agentengine::Digest const& digest, agentengine::IdentityHandle caller) {
    std::lock_guard<std::mutex> g(mutex_);
    if (!authorized_for(blob_acl_, digest, caller)) {
        return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                      "caller is not authorized to read this blob digest",
                                                      "ledger.blob_access_denied"});
    }
    auto b = store_.get_blob(digest);
    if (!b) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, b.error().message,
                                                      "ledger.get_blob_failed"});
    }
    return *b;
}

template <class Store>
agentengine::result<agentengine::Tree> Ledger<Store>::get_tree_safe(agentengine::Digest const& digest,
                                                                       agentengine::IdentityHandle caller) {
    std::lock_guard<std::mutex> g(mutex_);
    if (!authorized_for(tree_acl_, digest, caller)) {
        return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                      "caller is not authorized to read this tree digest",
                                                      "ledger.tree_access_denied"});
    }
    auto t = store_.get_tree(digest);
    if (!t) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, t.error().message,
                                                      "ledger.get_tree_failed"});
    }
    return *t;
}

template <class Store>
bool Ledger<Store>::would_accept_blob_write(std::span<std::byte const> bytes,
                                              agentengine::IdentityHandle writer) {
    auto digest = agentengine::compute_digest(bytes);
    if (!digest) return false;
    std::lock_guard<std::mutex> g(mutex_);
    auto it = blob_acl_.find(*digest);
    static std::set<std::uint64_t> const kNoRoots;
    return acl_root_admissible(it == blob_acl_.end() ? kNoRoots : it->second, writer.id(),
                               max_acl_roots_per_digest_);
}

template <class Store>
agentengine::rt::task<agentengine::result<BranchHandle<Store>>> Ledger<Store>::create_root_branch(
    agentengine::IdentityHandle owner, std::string disambiguator) {
    std::string name = "root-" + std::to_string(owner.id());
    if (!disambiguator.empty()) name += "-" + disambiguator;
    agentengine::Digest empty_tree_digest;
    {
        // Every store_ access MUST be serialized by THIS Ledger's own mutex_, the same one
        // guarding branches_ -- InMemoryWorktreeObjectStore has no internal synchronization of
        // its own.
        std::lock_guard<std::mutex> g(mutex_);
        auto put = store_.put_tree(agentengine::Tree{});
        if (!put) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                             put.error().message,
                                                             "ledger.put_tree_failed"});
        }
        empty_tree_digest = *put;
        auto acl_ok = insert_acl_root_bounded(tree_acl_, empty_tree_digest, owner.id(),
                                                 max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) co_return std::unexpected(acl_ok.error());
        branches_.insert_or_assign(
            name, BranchState{owner.id(), {}, empty_tree_digest, 0, {}, empty_tree_digest});
        persist_snapshot_locked();
    }
    co_return BranchHandle<Store>(this, name, owner.id(), empty_tree_digest);
}

template <class Store>
agentengine::rt::task<agentengine::result<Checkpoint>> Ledger<Store>::commit(
        BranchHandle<Store> const& branch, agentengine::Tree tree,
        agentengine::IdentityHandle authored_by, agentengine::rt::AsyncQuota<StorageBytes>& quota) {
    std::size_t const approx_bytes = agentengine::canonical_tree_bytes(tree).size();
    auto consumed = co_await quota.try_consume(approx_bytes, authored_by);
    if (!consumed.has_value()) co_return std::unexpected(consumed.error());

    auto folding_check = check_case_folding_collision(tree);
    if (!folding_check.has_value()) {
        (void)co_await quota.refund(approx_bytes);
        co_return std::unexpected(folding_check.error());
    }

    // Computed with `mutex_` released before any `co_await` (a lock held across a coroutine
    // suspension point is a real correctness hazard: the coroutine may resume on a different
    // thread than it suspended on, and unlocking a std::mutex from a different thread than locked
    // it is UB) -- and any failure refunds exactly what was consumed above, rather than burning
    // quota for a rejected commit.
    agentengine::result<Checkpoint> outcome = [&]() -> agentengine::result<Checkpoint> {
        std::lock_guard<std::mutex> g(mutex_);
        for (auto const& entry : tree.entries) {
            auto const& acl = entry.is_tree ? tree_acl_ : blob_acl_;
            if (!authorized_for(acl, entry.digest, authored_by)) {
                return std::unexpected(agentengine::error{
                    agentengine::failure_class::policy,
                    "commit references digest '" + entry.digest.substr(0, 12) + "...' (path '" +
                        entry.name +
                        "') that the committing principal is not authorized for -- every entry a "
                        "commit references must already be legitimately accessible to the "
                        "committing principal",
                    "ledger.commit_unauthorized_reference"});
            }
        }
        auto tree_digest = store_.put_tree(std::move(tree));
        if (!tree_digest) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          tree_digest.error().message,
                                                          "ledger.put_tree_failed"});
        }
        auto acl_ok = insert_acl_root_bounded(tree_acl_, *tree_digest, authored_by.id(),
                                                 max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());

        auto it = branches_.find(branch.name());
        if (it == branches_.end()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "unknown branch", "ledger.unknown_branch"});
        }
        auto& state = it->second;
        std::uint64_t const new_turn = state.head_turn_index + 1;
        auto self = compute_self_digest(*tree_digest, state.head_self_digest, authored_by.id(), new_turn);
        if (!self.has_value()) return std::unexpected(self.error());
        Checkpoint cp{*self, *tree_digest, state.head_self_digest, authored_by.id(), new_turn};
        state.head_self_digest = *self;
        state.head_tree_digest = *tree_digest;
        state.head_turn_index = new_turn;
        state.checkpoints.insert_or_assign(new_turn, cp);
        persist_snapshot_locked();
        return cp;
    }();

    if (!outcome.has_value()) {
        (void)co_await quota.refund(approx_bytes);
        co_return std::unexpected(outcome.error());
    }
    co_return *outcome;
}

template <class Store>
agentengine::rt::task<agentengine::result<Checkpoint>> Ledger<Store>::reset_to(
        BranchHandle<Store> const& branch, std::uint64_t target_turn_index,
        agentengine::IdentityHandle requested_by) {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = branches_.find(branch.name());
    if (it == branches_.end()) {
        co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                         "unknown branch", "ledger.unknown_branch"});
    }
    auto& state = it->second;
    auto cp_it = state.checkpoints.find(target_turn_index);
    if (cp_it == state.checkpoints.end()) {
        co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                         "no such checkpoint",
                                                         "ledger.no_such_checkpoint"});
    }

    agentengine::Digest const target_tree = cp_it->second.tree;
    std::uint64_t const new_turn = state.head_turn_index + 1;
    auto self = compute_self_digest(target_tree, state.head_self_digest, requested_by.id(), new_turn);
    if (!self.has_value()) co_return std::unexpected(self.error());
    Checkpoint cp{*self, target_tree, state.head_self_digest, requested_by.id(), new_turn};
    state.head_self_digest = *self;
    state.head_tree_digest = target_tree;
    state.head_turn_index = new_turn;
    state.checkpoints.insert_or_assign(new_turn, cp);
    persist_snapshot_locked();
    co_return cp;
}

template <class Store>
agentengine::rt::task<agentengine::result<BranchHandle<Store>>> Ledger<Store>::branch_from(
    BranchHandle<Store> const& parent, agentengine::IdentityHandle created_by,
    agentengine::rt::AsyncQuota<BranchCost>& quota) {
    auto consumed = co_await quota.try_consume(1, created_by);
    if (!consumed.has_value()) co_return std::unexpected(consumed.error());

    agentengine::result<BranchHandle<Store>> outcome = [&]() -> agentengine::result<BranchHandle<Store>> {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(parent.name());
        if (it == branches_.end()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "unknown parent branch",
                                                          "ledger.unknown_branch"});
        }
        BranchState const& parent_state = it->second;
        std::string child_name =
            parent.name() + "/child-" + std::to_string(created_by.id()) + "-" +
            std::to_string(branch_seq_++);
        auto acl_ok = insert_acl_root_bounded(tree_acl_, parent_state.head_tree_digest,
                                                 created_by.id(), max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());
        BranchState child_state = parent_state;
        child_state.created_by_id = created_by.id();
        // The child's merge `base` is the PARENT's tree AT THIS EXACT MOMENT.
        child_state.base_tree_digest = parent_state.head_tree_digest;
        branches_.insert_or_assign(child_name, child_state);
        persist_snapshot_locked();
        return BranchHandle<Store>(this, child_name, created_by.id(), parent_state.head_tree_digest);
    }();

    if (!outcome.has_value()) {
        (void)co_await quota.refund(1);
        co_return std::unexpected(outcome.error());
    }
    co_return std::move(*outcome);
}

template <class Store>
agentengine::rt::task<agentengine::result<Checkpoint>> Ledger<Store>::merge(
        BranchHandle<Store> child, BranchHandle<Store> const& parent,
        agentengine::IdentityHandle requested_by, agentengine::rt::AsyncQuota<MergeCost>& quota) {
    auto consumed = co_await quota.try_consume(1, requested_by);
    if (!consumed.has_value()) {
        // MUST-FIX, found by an independent red-team pass on this exact fix (2026-08-29): without
        // this block, `child` reaches its own destructor still `resolved_ == false` (nothing below
        // ever ran), so `BranchHandle::~BranchHandle()`'s `maybe_queue_abandon()` queues a REAL
        // abandon -- the next unrelated `reap_pending_abandons()` call would genuinely ERASE this
        // branch, destroying the caller's real work, not merely leave it "untouched" or
        // "reclaimable" the way every other rejection path below does. Matches every other
        // rejection path's own contract exactly: register `child` as a reclaimable orphan (only if
        // it still genuinely exists) and mark it resolved, so a quota-refused merge loses no more
        // than any other refused merge does, and the caller keeps the same real
        // `reclaim_orphaned_branch()` recovery path. No `co_await` inside this scope, so taking
        // `mutex_` here briefly does not reopen the lock-across-suspension hazard this method's own
        // header comment names.
        {
            std::lock_guard<std::mutex> g(mutex_);
            orphan_child_locked(child);
        }
        co_return std::unexpected(consumed.error());
    }

    agentengine::result<Checkpoint> outcome = [&]() -> agentengine::result<Checkpoint> {
        std::lock_guard<std::mutex> g(mutex_);
        auto participants = load_merge_participants_locked(child, parent, requested_by);
        if (!participants.has_value()) return std::unexpected(participants.error());
        BranchState& parent_state = *participants->parent_state;

        auto merge_result = perform_three_way_merge_locked(
            participants->base_digest, participants->ours_digest, participants->theirs_digest, child);
        if (!merge_result.has_value()) return std::unexpected(merge_result.error());

        auto committed = commit_merged_tree_locked(std::move(*merge_result), requested_by,
                                                      parent_state, child);
        if (!committed.has_value()) return std::unexpected(committed.error());

        auto granted = grant_parent_owner_access_locked(committed->merged_tree_digest,
                                                            committed->entries_for_owner_grant,
                                                            parent_state, requested_by, child);
        if (!granted.has_value()) return std::unexpected(granted.error());

        std::uint64_t const new_turn = parent_state.head_turn_index + 1;
        auto self = compute_self_digest(committed->merged_tree_digest, parent_state.head_self_digest,
                                          requested_by.id(), new_turn);
        if (!self.has_value()) {
            orphan_child_locked(child);
            return std::unexpected(self.error());
        }
        Checkpoint cp{*self, committed->merged_tree_digest, parent_state.head_self_digest,
                         requested_by.id(), new_turn};
        parent_state.head_self_digest = *self;
        parent_state.head_tree_digest = committed->merged_tree_digest;
        parent_state.head_turn_index = new_turn;
        parent_state.checkpoints.insert_or_assign(new_turn, cp);
        branches_.erase(child.name());
        child.resolved_ = true;
        persist_snapshot_locked();
        return cp;
    }();

    if (!outcome.has_value()) {
        (void)co_await quota.refund(1);
        co_return std::unexpected(outcome.error());
    }
    co_return *outcome;
}

template <class Store>
agentengine::rt::task<agentengine::result<void>> Ledger<Store>::abandon(BranchHandle<Store> child) {
    std::lock_guard<std::mutex> g(mutex_);
    branches_.erase(child.name());
    child.resolved_ = true;
    persist_snapshot_locked();
    co_return agentengine::result<void>{};
}

template <class Store>
agentengine::rt::task<std::size_t> Ledger<Store>::reap_pending_abandons() {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> g(mutex_);
        pending = std::move(pending_abandons_);
        pending_abandons_.clear();
    }
    std::size_t processed = 0;
    for (auto& name : pending) {
        std::optional<std::uint64_t> creator_id;
        agentengine::Digest base;
        {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = branches_.find(name);
            if (it != branches_.end()) {
                creator_id = it->second.created_by_id;
                base = it->second.head_tree_digest;
            }
        }
        if (!creator_id.has_value()) continue;
        BranchHandle<Store> handle(this, name, *creator_id, base);
        auto r = co_await abandon(std::move(handle));
        if (r.has_value()) ++processed;
    }
    co_return processed;
}

template <class Store>
agentengine::result<agentengine::Digest> Ledger<Store>::head_tree_digest(
        std::string const& branch_name, agentengine::IdentityHandle caller) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = branches_.find(branch_name);
    if (it == branches_.end()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "unknown branch", "ledger.unknown_branch"});
    }
    if (!authorized_for(tree_acl_, it->second.head_tree_digest, caller)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "caller is not authorized to read this branch's head tree digest",
            "ledger.tree_access_denied"});
    }
    return it->second.head_tree_digest;
}

template <class Store>
agentengine::result<Checkpoint> Ledger<Store>::head_checkpoint(std::string const& branch_name,
                                                                 agentengine::IdentityHandle caller) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = branches_.find(branch_name);
    if (it == branches_.end()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "unknown branch", "ledger.unknown_branch"});
    }
    BranchState const& state = it->second;
    if (!authorized_for(tree_acl_, state.head_tree_digest, caller)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "caller is not authorized to read this branch's head tree digest",
            "ledger.tree_access_denied"});
    }
    auto cp_it = state.checkpoints.find(state.head_turn_index);
    if (cp_it != state.checkpoints.end() && cp_it->second.self_digest == state.head_self_digest) {
        return cp_it->second;
    }
    // A fresh root (never committed), or a child whose head was inherited from its parent by
    // `branch_from()` and so has no checkpoint entry of its own yet: report the head as it stands.
    return Checkpoint{state.head_self_digest, state.head_tree_digest, {}, state.created_by_id,
                      state.head_turn_index};
}

template <class Store>
agentengine::result<Checkpoint> Ledger<Store>::checkpoint_at(std::string const& branch_name,
                                                                std::uint64_t turn_index,
                                                                agentengine::IdentityHandle caller) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = branches_.find(branch_name);
    if (it == branches_.end()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "unknown branch", "ledger.unknown_branch"});
    }
    auto cp_it = it->second.checkpoints.find(turn_index);
    if (cp_it == it->second.checkpoints.end()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "no such checkpoint",
                                                      "ledger.no_such_checkpoint"});
    }
    if (!authorized_for(tree_acl_, cp_it->second.tree, caller)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "caller is not authorized to read this checkpoint's tree digest",
            "ledger.tree_access_denied"});
    }
    return cp_it->second;
}

template <class Store>
agentengine::result<BranchHandle<Store>> Ledger<Store>::reclaim_orphaned_branch(
        std::string const& branch_name, agentengine::IdentityHandle requested_by) {
    std::lock_guard<std::mutex> g(mutex_);
    if (!orphaned_from_restart_.contains(branch_name)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "branch is not a recognized orphan (either it never existed, is still live, or was "
            "already reclaimed once)",
            "ledger.not_an_orphan"});
    }
    auto it = branches_.find(branch_name);
    if (it == branches_.end()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "unknown branch", "ledger.unknown_branch"});
    }
    if (!is_branch_owner(it->second, requested_by)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "requester is not this orphaned branch's own creator (or an ancestor of it)",
            "ledger.reclaim_unauthorized"});
    }
    orphaned_from_restart_.erase(branch_name);
    return BranchHandle<Store>(this, branch_name, requested_by.id(), it->second.base_tree_digest);
}

template <class Store>
agentengine::rt::task<agentengine::result<void>> Ledger<Store>::abandon_orphaned_branch(
        std::string const& branch_name, agentengine::IdentityHandle requested_by) {
    std::optional<std::uint64_t> base_owner_check;
    agentengine::Digest base;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (!orphaned_from_restart_.contains(branch_name)) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                             "branch is not a recognized orphan",
                                                             "ledger.not_an_orphan"});
        }
        auto it = branches_.find(branch_name);
        if (it == branches_.end()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                             "unknown branch",
                                                             "ledger.unknown_branch"});
        }
        if (!is_branch_owner(it->second, requested_by)) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "requester is not this orphaned branch's own creator (or an ancestor of it)",
                "ledger.reclaim_unauthorized"});
        }
        base = it->second.base_tree_digest;
        base_owner_check = requested_by.id();
        orphaned_from_restart_.erase(branch_name);
    }
    BranchHandle<Store> handle(this, branch_name, *base_owner_check, base);
    co_return co_await abandon(std::move(handle));
}

template <class Store>
agentengine::result<agentengine::Digest> Ledger<Store>::put_blob_locked(std::span<std::byte const> bytes,
                                                                           agentengine::IdentityHandle const& writer) {
    auto d = store_.put_blob(bytes);
    if (!d) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, d.error().message,
                                                      "ledger.put_blob_failed"});
    }
    auto acl_ok = insert_acl_root_bounded(blob_acl_, *d, writer.id(), max_acl_roots_per_digest_);
    if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());
    return *d;
}

template <class Store>
bool Ledger<Store>::authorized_for(
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> const& acl,
    agentengine::Digest const& digest, agentengine::IdentityHandle const& caller) {
    auto it = acl.find(digest);
    if (it == acl.end()) return false;
    if (it->second.contains(kPubliclySharedSentinelRootId)) return true;
    for (std::uint64_t allowed_root : it->second) {
        if (allowed_root == caller.id() ||
            agentengine::IdentityAuthority::bootstrap().is_ancestor_of(allowed_root, caller.id())) {
            return true;
        }
    }
    return false;
}

template <class Store>
agentengine::result<void> Ledger<Store>::insert_acl_root_bounded(
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>>& acl,
    agentengine::Digest const& digest, std::uint64_t root_id, std::size_t cap) {
    auto& set = acl[digest];
    if (set.contains(root_id)) return agentengine::result<void>{};
    if (set.contains(kPubliclySharedSentinelRootId)) return agentengine::result<void>{};
    if (!acl_root_admissible(set, root_id, cap)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::resource,
            "digest '" + digest.substr(0, 12) + "...' has reached its maximum of " +
                std::to_string(cap) +
                " distinct authorized root principals; a new, unrelated root cannot be added (a "
                "deliberate, disclosed bound, not an accidental limit -- an already-authorized "
                "principal may call mark_digest_shared() to exempt this digest from the bound "
                "entirely, if it is genuinely meant to be read by anyone)",
            "ledger.acl_root_cap_exceeded"});
    }
    set.insert(root_id);
    return agentengine::result<void>{};
}

template <class Store>
agentengine::result<void> Ledger<Store>::check_case_folding_collision(agentengine::Tree const& tree) {
    auto const collision = ledger_detail::find_case_folding_collision(tree);
    if (!collision.has_value()) return {};
    return std::unexpected(agentengine::error{
        agentengine::failure_class::contract,
        "tree contains two entries that case-fold to the same real path ('" +
            tree.entries[collision->first].name + "' and '" + tree.entries[collision->second].name +
            "') -- rejected before materialize() could silently drop one of them on a "
            "case-insensitive filesystem, matching git's own real CVE-2014-9390 fix "
            "direction",
        "ledger.case_folding_collision"});
}

template <class Store>
agentengine::result<typename Ledger<Store>::MergeParticipants> Ledger<Store>::load_merge_participants_locked(
        BranchHandle<Store>& child, BranchHandle<Store> const& parent,
        agentengine::IdentityHandle requested_by) {
    auto child_it = branches_.find(child.name());
    auto parent_it = branches_.find(parent.name());
    if (child_it == branches_.end() || parent_it == branches_.end()) {
        // The child branch itself may still be unknown too (a stale/already-consumed handle) --
        // only register it as a reclaimable orphan if it genuinely still exists.
        orphan_child_locked(child);
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                         "unknown branch in merge()",
                                                         "ledger.unknown_branch"});
    }
    BranchState const& child_state = child_it->second;
    BranchState& parent_state = parent_it->second;
    agentengine::Digest const theirs_digest = child_state.head_tree_digest;
    agentengine::Digest const ours_digest = parent_state.head_tree_digest;
    agentengine::Digest const base_digest = child_state.base_tree_digest;

    if (!authorized_for(tree_acl_, theirs_digest, requested_by)) {
        orphan_child_locked(child);
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "merge requester is not authorized for the child branch's head tree digest",
            "ledger.merge_unauthorized_reference"});
    }
    return MergeParticipants{&parent_state, theirs_digest, ours_digest, base_digest};
}

template <class Store>
agentengine::result<LedgerMergeResult> Ledger<Store>::perform_three_way_merge_locked(
        agentengine::Digest const& base_digest, agentengine::Digest const& ours_digest,
        agentengine::Digest const& theirs_digest, BranchHandle<Store>& child) {
    auto base_tree = store_.get_tree(base_digest);
    auto ours_tree = store_.get_tree(ours_digest);
    auto theirs_tree = store_.get_tree(theirs_digest);
    if (!base_tree.has_value() || !ours_tree.has_value() || !theirs_tree.has_value()) {
        orphan_child_locked(child);
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "merge could not load base/ours/theirs from the object store",
            "ledger.merge_tree_load_failed"});
    }

    LedgerMergeResult merged = merge_trees(*base_tree, *ours_tree, *theirs_tree);
    if (!merged.conflicts.empty()) {
        orphan_child_locked(child);
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "merge produced " + std::to_string(merged.conflicts.size()) +
                " real conflicting path(s) (first: '" + merged.conflicts.front().path +
                "') -- automatic conflict resolution is explicitly out of this design's scope; "
                "the merge is rejected rather than silently picking a side",
            "ledger.merge_conflict"});
    }
    return merged;
}

template <class Store>
agentengine::result<typename Ledger<Store>::CommittedMergeTree> Ledger<Store>::commit_merged_tree_locked(
        LedgerMergeResult merged, agentengine::IdentityHandle requested_by,
        BranchState const& parent_state, BranchHandle<Store>& child) {
    for (auto const& entry : merged.merged.entries) {
        auto const& acl = entry.is_tree ? tree_acl_ : blob_acl_;
        if (!authorized_for(acl, entry.digest, requested_by)) {
            orphan_child_locked(child);
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "merge result references digest '" + entry.digest.substr(0, 12) + "...' (path '" +
                    entry.name + "') that the merge requester is not authorized for",
                "ledger.merge_unauthorized_reference"});
        }
    }

    std::vector<TreeEntry> merged_entries_for_owner_grant;
    if (parent_state.created_by_id != requested_by.id()) {
        merged_entries_for_owner_grant = merged.merged.entries;
    }

    auto merged_tree_digest = store_.put_tree(std::move(merged.merged));
    if (!merged_tree_digest.has_value()) {
        orphan_child_locked(child);
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                         merged_tree_digest.error().message,
                                                         "ledger.put_tree_failed"});
    }
    auto acl_ok = insert_acl_root_bounded(tree_acl_, *merged_tree_digest, requested_by.id(),
                                             max_acl_roots_per_digest_);
    if (!acl_ok.has_value()) {
        orphan_child_locked(child);
        return std::unexpected(acl_ok.error());
    }
    return CommittedMergeTree{*merged_tree_digest, std::move(merged_entries_for_owner_grant)};
}

template <class Store>
agentengine::result<void> Ledger<Store>::grant_parent_owner_access_locked(
        agentengine::Digest const& merged_tree_digest,
        std::vector<TreeEntry> const& merged_entries_for_owner_grant, BranchState const& parent_state,
        agentengine::IdentityHandle requested_by, BranchHandle<Store>& child) {
    if (parent_state.created_by_id != requested_by.id()) {
        auto owner_acl_ok = insert_acl_root_bounded(tree_acl_, merged_tree_digest,
                                                        parent_state.created_by_id,
                                                        max_acl_roots_per_digest_);
        if (!owner_acl_ok.has_value()) {
            orphan_child_locked(child);
            return std::unexpected(owner_acl_ok.error());
        }
        // ADR-112: closes this method's own long-disclosed "structural, not content-wise" scope
        // limit below -- the grant above is TREE-DIGEST-LEVEL only (`tree_acl_[merged_tree_digest]`),
        // so `parent_state`'s own owner could list the merged tree's structure
        // (`get_tree_safe()`/`head_tree_digest()`) but not fetch the actual bytes of a blob (or
        // read a nested subtree) the child alone contributed (`get_blob_safe()`/`get_tree_safe()`
        // on it still failed `ledger.blob_access_denied`/`ledger.tree_access_denied`). Same
        // reasoning as the tree-level grant above: `parent_state.created_by_id` already owns the
        // branch this merge lands on, so granting them content-level access to what just became
        // their own branch's new state is not widening authority to a stranger -- it completes
        // the identical category of grant already made one level up.
        //
        // Deliberately BEST-EFFORT, not fail-closed: a per-entry grant hitting
        // `ledger.acl_root_cap_exceeded` (a digest already at its configured cap of distinct
        // roots) does NOT reject an otherwise-successful merge -- the tree-level grant above, the
        // one property this method's own callers actually depend on structurally, has already
        // succeeded, and unwinding that decision here to fail the whole merge over one capped
        // blob would trade a narrow, pre-existing content-access residual for a much more
        // surprising full-merge rejection. A capped entry is left exactly as inaccessible to the
        // parent owner as it already was before this fix -- no regression, not a new hazard,
        // just not fully closed for that one digest.
        for (auto const& entry : merged_entries_for_owner_grant) {
            auto& entry_acl = entry.is_tree ? tree_acl_ : blob_acl_;
            (void)insert_acl_root_bounded(entry_acl, entry.digest, parent_state.created_by_id,
                                             max_acl_roots_per_digest_);
        }
    }
    // SCOPE LIMIT, since NARROWED (ADR-112): the tree-level grant above only ever covered
    // `merged_tree_digest` itself; the per-entry loop just above extends the SAME grant to every
    // blob/subtree the merged tree references, best-effort. What remains open: a digest already
    // at its ACL root cap (`ledger.acl_root_cap_exceeded`, rare in practice) stays inaccessible
    // to the parent owner unless an already-authorized principal calls `mark_digest_shared()`
    // instead; and this grant only ever covers digests reachable from THIS merge's own resulting
    // tree, never digests the child wrote but which did not survive into the final merged result
    // (an intentional, not accidental, boundary -- content that isn't part of the branch's new
    // state was never meant to become reachable through it).
    return {};
}

template <class Store>
void Ledger<Store>::persist_snapshot_locked() const {
    if (!durable_dir_) return;
    std::filesystem::path const final_path = *durable_dir_ / "ledger_state.snapshot";
    std::filesystem::path const temp_path = *durable_dir_ / "ledger_state.snapshot.tmp";
    {
        std::ofstream out(temp_path, std::ios::trunc);
        out << "SEQ\t" << branch_seq_ << '\n';
        for (auto const& [name, state] : branches_) {
            out << "BRANCH\t" << name << '\t' << state.created_by_id << '\t' << state.head_self_digest
                << '\t' << state.head_tree_digest << '\t' << state.head_turn_index << '\t'
                << state.base_tree_digest << '\n';
            for (auto const& [turn, cp] : state.checkpoints) {
                out << "CHECKPOINT\t" << name << '\t' << turn << '\t' << cp.self_digest << '\t'
                    << cp.tree << '\t' << cp.parent << '\t' << cp.authored_by_id << '\n';
            }
        }
        for (auto const& [digest, roots] : blob_acl_) {
            out << "BLOB_ACL\t" << digest;
            for (auto id : roots) out << '\t' << id;
            out << '\n';
        }
        for (auto const& [digest, roots] : tree_acl_) {
            out << "TREE_ACL\t" << digest;
            for (auto id : roots) out << '\t' << id;
            out << '\n';
        }
        out.flush();
    }
    ++snapshot_writes_;
    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    // A rename failure here is intentionally not escalated -- durability is a best-effort
    // addition on top of an already-successful in-memory mutation.
}

template <class Store>
void Ledger<Store>::load_durable_state() {
    std::filesystem::path const snapshot_path = *durable_dir_ / "ledger_state.snapshot";
    std::ifstream in(snapshot_path);
    if (!in) return;   // first-ever run in this directory -- nothing to restore
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        std::getline(fields, tag, '\t');
        if (tag == "SEQ") {
            std::string v;
            std::getline(fields, v, '\t');
            try { branch_seq_ = std::stoull(v); } catch (...) {}
        } else if (tag == "BRANCH") {
            std::string name, created_by_id_s, self_d, tree_d, turn_s, base_d;
            std::getline(fields, name, '\t');
            std::getline(fields, created_by_id_s, '\t');
            std::getline(fields, self_d, '\t');
            std::getline(fields, tree_d, '\t');
            std::getline(fields, turn_s, '\t');
            std::getline(fields, base_d, '\t');   // absent on an older snapshot -- getline simply
                                                      // yields an empty string, handled the same
                                                      // as "no known base" below
            try {
                BranchState state;
                state.created_by_id = std::stoull(created_by_id_s);
                state.head_self_digest = self_d;
                state.head_tree_digest = tree_d;
                state.head_turn_index = std::stoull(turn_s);
                state.base_tree_digest = base_d;
                branches_.insert_or_assign(name, std::move(state));
                orphaned_from_restart_.insert(name);   // every restored branch has no live handle
                                                          // in this new process, by construction
            } catch (...) { continue; }
        } else if (tag == "CHECKPOINT") {
            std::string branch_name, turn_s, self_d, tree_d, parent_d, authored_s;
            std::getline(fields, branch_name, '\t');
            std::getline(fields, turn_s, '\t');
            std::getline(fields, self_d, '\t');
            std::getline(fields, tree_d, '\t');
            std::getline(fields, parent_d, '\t');
            std::getline(fields, authored_s, '\t');
            auto it = branches_.find(branch_name);
            if (it == branches_.end()) continue;   // a checkpoint for a branch line we never saw
                                                       // (truncated/corrupt tail) -- skip
            try {
                std::uint64_t const turn = std::stoull(turn_s);
                Checkpoint cp{self_d, tree_d, parent_d, std::stoull(authored_s), turn};
                it->second.checkpoints.insert_or_assign(turn, cp);
            } catch (...) { continue; }
        } else if (tag == "BLOB_ACL" || tag == "TREE_ACL") {
            std::string digest;
            std::getline(fields, digest, '\t');
            std::set<std::uint64_t> roots;
            std::string id_s;
            while (std::getline(fields, id_s, '\t')) {
                try { roots.insert(std::stoull(id_s)); } catch (...) {}
            }
            (tag == "BLOB_ACL" ? blob_acl_ : tree_acl_).insert_or_assign(digest, std::move(roots));
        }
    }
}

}  // namespace agentengine
