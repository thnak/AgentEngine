#pragma once
// Implements ADR-102 Phase 1 (identity-native sandbox/worktree design, ADR-099 §3) --
// AsyncQuota<Kind>, one coroutine-native quota primitive, identity-scoped via IdentityAuthority/
// IdentityHandle (trust/identity_authority.hpp). Real, shipped precedent for this exact shape:
// rt/spawn_cost_budget.hpp's SpawnCostBudget (check-and-decrement as one atomic step under
// AsyncMutex) -- this type generalizes that pattern to be identity-scoped (a spender must be the
// quota's owner or a subject it explicitly split a share to) and reusable across every quota kind
// this design needs (BranchCost/StorageBytes/RunCost/ResetCost/a SpawnDepth-shaped kind), instead of
// one bespoke type per kind.
//
// Ported from docs/planning/proofs/async_quota/async_quota.hpp (ADR-099's own standalone,
// red-teamed, live-tested prove-phase original -- kept as-is, this is a new file). Real changes made
// during the port: `probe::Principal` -> `agentengine::IdentityHandle`; `probe::result<T>`/
// `probe::error{message, code}` -> the real `agentengine::result<T>`/`agentengine::error{
// failure_class, message, code}` (core/error.hpp), matching SpawnCostBudget's own
// `failure_class::resource` for quota exhaustion and capability.hpp's own `failure_class::policy`
// for an authorization refusal (`quota.unauthorized_spender`).
//
// AsyncMutex (rt/async_mutex.hpp) deletes its copy ctor/assignment and declares no move members --
// declaring ANY special member function (a deleted one counts) suppresses the implicitly-generated
// move constructor too, so AsyncMutex is non-copyable AND non-movable, deliberately: LockAwaiter::
// await_suspend stores a raw `self` pointer to the mutex instance a parked coroutine will later be
// resumed through -- relocating a live AsyncMutex would dangle that pointer. A type that embeds
// AsyncMutex BY VALUE can therefore never itself be returned by value or moved -- exactly what
// AsyncQuota<T>::mint_root()/allocate_child_share() need to do (both return a fresh AsyncQuota BY
// VALUE). Held behind a stable-address unique_ptr instead: moving the unique_ptr relocates the
// POINTER, never the pointee, so the real AsyncMutex object's address never changes regardless of
// how many times the owning AsyncQuota is moved.

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "agentengine/core/error.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine::rt {

template <class Kind>
class AsyncQuota {
public:
    // Synchronous -- minting a quota is a rare, host-driven, low-frequency operation, mirroring
    // IdentityAuthority's own mint_root()/derive_child() being plain functions, not coroutines.
    // Gated through IdentityAuthority: fails closed if `owner` was never actually minted, closing
    // "mint your own subject, mint your own unlimited quota, hand it to a required-parameter check."
    [[nodiscard]] static agentengine::result<AsyncQuota> mint_root(
            agentengine::IdentityAuthority const& authority, agentengine::IdentityHandle owner,
            std::uint64_t total) {
        if (!authority.is_known(owner.id())) {
            return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                         "identity-authority subject was never minted",
                                                         "async_quota.unknown_subject"});
        }
        return AsyncQuota(owner, total);
    }

    // A coroutine, taking the SAME mutex_ try_consume() does -- no sync/async race between the two.
    [[nodiscard]] task<agentengine::result<AsyncQuota>> allocate_child_share(
            agentengine::IdentityHandle child, std::uint64_t amount) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        if (amount > remaining_) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::resource,
                "insufficient remaining budget to allocate child share",
                "async_quota.insufficient_remaining"});
        }
        remaining_ -= amount;
        children_[child.id()] = amount;
        co_return AsyncQuota(child, amount);
    }

    [[nodiscard]] task<agentengine::result<void>> try_consume(std::uint64_t amount,
                                                                  agentengine::IdentityHandle spender) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        // Fails closed unless spender is this quota's owner, or a descendant it was split to via
        // allocate_child_share() on THIS instance -- identity-scoped consumption, never "any
        // IdentityHandle can drain a shared quota."
        bool const is_owner = spender.id() == owner_.id();
        bool const is_child_share = children_.find(spender.id()) != children_.end();
        if (!is_owner && !is_child_share) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "spender is not this quota's owner or a split-to descendant",
                "async_quota.unauthorized_spender"});
        }
        if (amount > remaining_) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                            "quota budget exhausted",
                                                            "async_quota.exhausted"});
        }
        remaining_ -= amount;
        co_return agentengine::result<void>{};
    }

    // Anti-replay: the ledger entry is explicitly ERASED on success, so a second call with the same
    // (child, amount) fails closed instead of re-crediting remaining_ a second time.
    [[nodiscard]] task<agentengine::result<void>> release_child_share(agentengine::IdentityHandle child,
                                                                          std::uint64_t amount) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        auto it = children_.find(child.id());
        if (it == children_.end() || it->second != amount) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                            "release does not match a live allocation",
                                                            "async_quota.release_not_owed"});
        }
        children_.erase(it);
        remaining_ += amount;
        co_return agentengine::result<void>{};
    }

    // A compensating action for a caller whose OWN try_consume() succeeded but whose surrounding
    // operation subsequently failed for an unrelated reason -- reverses exactly the amount that same
    // caller already deducted. Not a general-purpose credit grant: callers must only ever refund an
    // amount they themselves just consumed, the same discipline release_child_share() applies to
    // undoing an allocate_child_share().
    [[nodiscard]] task<agentengine::result<void>> refund(std::uint64_t amount) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        remaining_ += amount;
        co_return agentengine::result<void>{};
    }

    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }

private:
    AsyncQuota(agentengine::IdentityHandle owner, std::uint64_t total)
        : owner_(owner), remaining_(total), mutex_(std::make_unique<AsyncMutex>()) {}

    agentengine::IdentityHandle owner_;
    std::uint64_t remaining_;
    std::unordered_map<std::uint64_t, std::uint64_t> children_;
    std::unique_ptr<AsyncMutex> mutex_;   // stable-address indirection -- see the file-top comment
                                             // for why a by-value AsyncMutex member would make this
                                             // whole type unreturnable
};

}  // namespace agentengine::rt
