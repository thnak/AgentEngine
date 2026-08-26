#pragma once
// PROVE-PHASE PROBE for Ledger (§4/§13.2/§15.4/§17), scoped deliberately to what the design actually
// specifies -- MergeStrategy/real tree-diff merge semantics are explicitly named-not-designed (§11)
// and are NOT implemented here; a trivial stub stands in for merge() only to prove the
// consumes-by-value/resolution mechanics, never claiming to prove real three-way merge correctness.
//
// Digest is a plain hex-string content hash here (FNV-1a, not a real crypto primitive) -- a stand-in
// sufficient to test WELL-DEFINEDNESS (same inputs -> same digest; different inputs -> different
// digest) without pulling in a real hash library; the design itself never commits to a specific
// algorithm (§11 names this an open question).

#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"
#include "../identity_authority/identity_authority.hpp"
#include "../async_quota/async_quota.hpp"

namespace probe {

using Digest = std::string;

[[nodiscard]] inline Digest fnv1a(std::string const& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << h;
    return out.str();
}

struct Checkpoint {
    Digest self_digest;
    Digest tree;
    Digest parent;   // {} (empty string) for a branch's root checkpoint
    Principal authored_by;
    std::uint64_t turn_index = 0;
};

[[nodiscard]] inline Digest compute_self_digest(Digest const& tree, Digest const& parent,
                                                  Principal const& authored_by,
                                                  std::uint64_t turn_index) {
    // hash(tree || parent || authored_by || turn_index) -- self_digest itself excluded from its own
    // input by construction (it doesn't exist yet at the point this is called), matching §15.4's
    // formula exactly.
    std::ostringstream in;
    in << tree << '|' << parent << '|' << authored_by.id() << '|' << turn_index;
    return fnv1a(in.str());
}

struct BranchCost {};   // AsyncQuota<BranchCost>'s payload tag
// StorageBytes is already defined in async_quota.hpp (included above) -- reused here, not redefined.

class Ledger;

class BranchHandle {
public:
    BranchHandle(BranchHandle&& other) noexcept
        : owner_(other.owner_), name_(std::move(other.name_)), created_by_(other.created_by_),
          base_(std::move(other.base_)), resolved_(other.resolved_) {
        other.owner_ = nullptr;
        other.resolved_ = true;   // moved-from: never queues an abandon of its own
    }
    BranchHandle& operator=(BranchHandle&& other) noexcept {
        if (this != &other) {
            maybe_queue_abandon();
            owner_ = other.owner_;
            name_ = std::move(other.name_);
            created_by_ = other.created_by_;
            base_ = std::move(other.base_);
            resolved_ = other.resolved_;
            other.owner_ = nullptr;
            other.resolved_ = true;
        }
        return *this;
    }
    BranchHandle(BranchHandle const&) = delete;
    BranchHandle& operator=(BranchHandle const&) = delete;

    // §13.2's fix: PURELY SYNCHRONOUS. Never tries to run abandon()'s real (coroutine) body -- only
    // records that this branch needs resolving, via a plain, non-suspending queue push.
    ~BranchHandle() { maybe_queue_abandon(); }

    [[nodiscard]] std::string const& name() const noexcept { return name_; }
    [[nodiscard]] Principal created_by() const noexcept { return created_by_; }
    [[nodiscard]] Digest const& base_digest() const noexcept { return base_; }

private:
    friend class Ledger;
    BranchHandle(Ledger* owner, std::string name, Principal created_by, Digest base)
        : owner_(owner), name_(std::move(name)), created_by_(created_by), base_(std::move(base)) {}

    void maybe_queue_abandon();   // defined after Ledger, needs its full definition

    Ledger* owner_ = nullptr;
    std::string name_;
    Principal created_by_;   // Principal has no default constructor by design (identity_authority.hpp)
                               // -- both real constructors above always initialize this explicitly, so
                               // no default-member-initializer is needed or attempted here.
    Digest base_;
    bool resolved_ = false;
};

struct BranchState {
    Principal created_by;
    Digest base_digest;
    Digest head_self_digest;   // {} until the first commit
    std::uint64_t head_turn_index = 0;
    std::unordered_map<std::uint64_t, Checkpoint> checkpoints;
};

class Ledger {
public:
    [[nodiscard]] agentengine::rt::task<result<BranchHandle>> create_root_branch(Principal owner) {
        std::string name = "root-" + std::to_string(owner.id());
        {
            std::lock_guard<std::mutex> g(mutex_);
            branches_.insert_or_assign(name, BranchState{owner, Digest{}, Digest{}, 0, {}});
        }
        co_return BranchHandle(this, name, owner, Digest{});
    }

    [[nodiscard]] agentengine::rt::task<result<BranchHandle>> branch_from(BranchHandle const& parent,
                                                                             Principal created_by,
                                                                             AsyncQuota<BranchCost>& quota) {
        auto consumed = co_await quota.try_consume(1, created_by);
        if (!consumed.has_value()) co_return std::unexpected(consumed.error());

        std::lock_guard<std::mutex> g(mutex_);
        auto& parent_state = branches_.at(parent.name());
        std::string child_name = parent.name() + "/child-" + std::to_string(created_by.id());
        BranchState child_state = parent_state;   // COW snapshot at branch time
        child_state.created_by = created_by;
        child_state.base_digest = parent_state.head_self_digest;
        branches_.insert_or_assign(child_name, child_state);
        co_return BranchHandle(this, child_name, created_by, parent_state.head_self_digest);
    }

    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> commit(BranchHandle const& branch,
                                                                      Digest new_tree,
                                                                      Principal authored_by,
                                                                      AsyncQuota<StorageBytes>& quota) {
        auto consumed = co_await quota.try_consume(new_tree.size(), authored_by);
        if (!consumed.has_value()) co_return std::unexpected(consumed.error());

        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(branch.name());
        if (it == branches_.end())
            co_return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
        auto& state = it->second;
        std::uint64_t const new_turn = state.head_turn_index + 1;
        Digest const self = compute_self_digest(new_tree, state.head_self_digest, authored_by, new_turn);
        Checkpoint cp{self, new_tree, state.head_self_digest, authored_by, new_turn};
        state.head_self_digest = self;
        state.head_turn_index = new_turn;
        state.checkpoints.insert_or_assign(new_turn, cp);
        co_return cp;
    }

    // §15.4's fix: turn_index is ALWAYS current_head + 1, never copied from the restored target --
    // strictly monotonic even across any number of rollbacks.
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> reset_to(BranchHandle const& branch,
                                                                        std::uint64_t target_turn_index,
                                                                        Principal requested_by) {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(branch.name());
        if (it == branches_.end())
            co_return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
        auto& state = it->second;
        auto cp_it = state.checkpoints.find(target_turn_index);
        if (cp_it == state.checkpoints.end())
            co_return std::unexpected(error{"no such checkpoint", "ledger.no_such_checkpoint"});

        Digest const target_tree = cp_it->second.tree;
        std::uint64_t const new_turn = state.head_turn_index + 1;
        Digest const self = compute_self_digest(target_tree, state.head_self_digest, requested_by, new_turn);
        Checkpoint cp{self, target_tree, state.head_self_digest, requested_by, new_turn};
        state.head_self_digest = self;
        state.head_turn_index = new_turn;
        state.checkpoints.insert_or_assign(new_turn, cp);
        co_return cp;
    }

    // Consumes the handle BY VALUE -- a moved-from handle cannot be resolved twice (§4's own claim,
    // tested for real below).
    [[nodiscard]] agentengine::rt::task<result<void>> abandon(BranchHandle child) {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(child.name());
        if (it == branches_.end()) {
            child.resolved_ = true;  // already gone (e.g. reap_pending_abandons() racing a direct
                                       // abandon() of the same name) -- idempotent no-op, not an error
            co_return result<void>{};
        }
        branches_.erase(it);
        child.resolved_ = true;   // stop the destructor from re-queuing
        co_return result<void>{};
    }

    // Trivial stub -- real three-way merge is explicitly NOT designed (§11); this only proves the
    // consumes-by-value/resolution mechanics identically to abandon(), not merge correctness.
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> merge(BranchHandle child,
                                                                     BranchHandle const& parent_branch) {
        std::lock_guard<std::mutex> g(mutex_);
        auto child_it = branches_.find(child.name());
        auto parent_it = branches_.find(parent_branch.name());
        if (child_it == branches_.end() || parent_it == branches_.end()) {
            child.resolved_ = true;   // still consumed -- merge() always resolves the handle it's
                                        // given, error or not, same discipline abandon() follows
            co_return std::unexpected(error{"unknown branch in merge()", "ledger.unknown_branch"});
        }
        // Stub "merge": parent adopts the child's current head tree wholesale (NOT a real
        // three-way merge -- see this file's own banner). Principal has no default constructor, so
        // this Checkpoint is only ever constructed with real, valid field values -- never
        // default/aggregate-initialized empty.
        auto& parent_state = parent_it->second;
        std::uint64_t const new_turn = parent_state.head_turn_index + 1;
        Digest const self = compute_self_digest(child_it->second.head_self_digest,
                                                   parent_state.head_self_digest,
                                                   child.created_by(), new_turn);
        Checkpoint const result_cp{self, child_it->second.head_self_digest,
                                     parent_state.head_self_digest, child.created_by(), new_turn};
        parent_state.head_self_digest = self;
        parent_state.head_turn_index = new_turn;
        parent_state.checkpoints.insert_or_assign(new_turn, result_cp);
        branches_.erase(child_it);
        child.resolved_ = true;
        co_return result_cp;
    }

    // §13.2's real driver: genuinely co_awaits abandon()'s real body for every pending name, unlike
    // Revision 1's discarded-coroutine claim.
    [[nodiscard]] agentengine::rt::task<std::size_t> reap_pending_abandons() {
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> g(mutex_);
            pending = std::move(pending_abandons_);
            pending_abandons_.clear();
        }
        std::size_t processed = 0;
        for (auto& name : pending) {
            std::optional<BranchState> state_copy;
            {
                std::lock_guard<std::mutex> g(mutex_);
                auto it = branches_.find(name);
                if (it != branches_.end()) state_copy = it->second;
            }
            if (!state_copy) continue;   // already resolved by something else
            BranchHandle handle(this, name, state_copy->created_by, state_copy->base_digest);
            auto r = co_await abandon(std::move(handle));
            if (r.has_value()) ++processed;
        }
        co_return processed;
    }

    [[nodiscard]] std::size_t pending_abandon_queue_size() const {
        std::lock_guard<std::mutex> g(mutex_);
        return pending_abandons_.size();
    }
    [[nodiscard]] bool has_branch(std::string const& name) const {
        std::lock_guard<std::mutex> g(mutex_);
        return branches_.find(name) != branches_.end();
    }

private:
    friend class BranchHandle;
    void queue_pending_abandon(std::string const& name) {
        std::lock_guard<std::mutex> g(mutex_);
        pending_abandons_.push_back(name);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BranchState> branches_;
    std::vector<std::string> pending_abandons_;
};

inline void BranchHandle::maybe_queue_abandon() {
    if (owner_ && !resolved_) {
        owner_->queue_pending_abandon(name_);   // plain, synchronous, non-suspending push -- the
                                                   // ENTIRE point of §13.2's fix: this is all a
                                                   // destructor safely can do
        resolved_ = true;
    }
}

}  // namespace probe
