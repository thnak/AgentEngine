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
//
// ADR-141 (2026-08-30, this session's own final-review pass): closed a real, shipped double-spend/
// double-credit bug in the allocate_child_share()/try_consume()/release_child_share() trio -- a child
// granted ANY share (however small) could previously also call try_consume() directly against the
// PARENT object using its own identity, checked only against the parent's general `remaining_` pool,
// never against how much was actually allocated to that child. release_child_share() then blindly
// re-credited the FULL original allocation with no knowledge of what had already flowed out through
// that channel or through the child's own separate returned object -- together, a caller could mint
// unlimited quota. Fixed by removing the direct-parent-spend channel entirely (try_consume() is now
// owner-only) and having release_child_share() take the child's own AsyncQuota object BY VALUE,
// crediting back exactly its real, mutation-tracked `remaining_` instead of a caller-supplied number.
// Not yet reachable through any real production caller (only tests/test_identity_authority_grant.cpp
// exercised this primitive at the time the bug was found) -- fixed now, before this API accretes a
// real caller depending on the broken shape.

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
    //
    // ADR-142 (this session's own follow-on adversarial pass over ADR-141): two real, reproduced gaps
    // closed here and in release_child_share() below.
    //
    // (1) A second allocate_child_share() call for a child identity that ALREADY has a live,
    // unreleased share now fails closed instead of silently overwriting `children_[child.id()]`.
    // Reproduced for real: allocate 40 to a child, then allocate 25 more to the SAME child before
    // releasing the first -- the pre-fix code accepted both (decrementing remaining_ by 65 total) but
    // the ledger entry only ever remembered the SECOND amount (25). Releasing the second share back
    // succeeded and credited 25; the FIRST share's still-live AsyncQuota object (40, genuinely unspent)
    // then had no ledger entry left to match against and could never be released -- its 40 units were
    // permanently stranded, spendable through neither the parent's own tracked remaining() nor any
    // release path. A real I8 budget-enforcement defect (a leak, not an escalation): the parent
    // silently loses usable capacity forever. Closed by refusing the second allocation outright; a
    // caller that genuinely wants to split further must release the first share back first.
    //
    // (2) release_child_share() now verifies the child object actually came from THIS parent, not
    // merely that `children_` happens to have an entry keyed by the same child id() -- see that
    // method's own comment for the reproduced cross-parent confusion this closes.
    [[nodiscard]] task<agentengine::result<AsyncQuota>> allocate_child_share(
            agentengine::IdentityHandle child, std::uint64_t amount) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        if (children_.find(child.id()) != children_.end()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "this child identity already has a live, unreleased share -- release it via "
                "release_child_share() before allocating another",
                "async_quota.child_share_already_live"});
        }
        if (amount > remaining_) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::resource,
                "insufficient remaining budget to allocate child share",
                "async_quota.insufficient_remaining"});
        }
        remaining_ -= amount;
        children_[child.id()] = amount;
        AsyncQuota out(child, amount);
        out.origin_mutex_ = mutex_.get();   // binds `out` to THIS parent instance -- see (2) above
        co_return out;
    }

    // ADR-141: owner-only, deliberately -- see this file's own top comment for the real, shipped
    // double-spend/double-credit bug this closes. A child that was split a share via
    // allocate_child_share() must spend through the SEPARATE AsyncQuota object that call returned,
    // never through this parent object too: before this fix, `is_child_share` authorized a registered
    // child to call try_consume() HERE, checked only against THIS object's own `remaining_` (the
    // general, still-unallocated pool) -- not against `children_[spender.id()]` (what was actually
    // allocated to that specific child) -- so a child holding any registered share, however small,
    // could draw from the whole parent pool, unbounded by its own allocation. Combined with
    // release_child_share() (below) blindly re-crediting the FULL original amount with no knowledge of
    // what flowed out through this channel, the two together let a caller mint unlimited quota:
    // allocate a share, spend some of it directly against the parent (never touching the returned
    // child object at all), then release the share and watch the full amount come back anyway.
    [[nodiscard]] task<agentengine::result<void>> try_consume(std::uint64_t amount,
                                                                  agentengine::IdentityHandle spender) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        if (spender.id() != owner_.id()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "spender is not this quota's owner -- a split-off child share must be spent through "
                "the separate AsyncQuota object allocate_child_share() returned, not this one",
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

    // ADR-141: takes the child's own, independent AsyncQuota object BY VALUE (moved, consuming it)
    // and credits back exactly what THAT object's own `remaining_` says is genuinely unspent -- never
    // a caller-supplied number, which is exactly how the pre-fix shape (`release_child_share(child,
    // amount)`, re-crediting `amount` unconditionally) could be tricked into crediting back more than
    // was ever actually left: a caller cannot fabricate a bogus "unspent" AsyncQuota without going
    // through that type's own mutex-guarded try_consume(), so whatever `child_quota.remaining_` holds
    // here is the real, mutation-tracked truth. Anti-replay is unchanged in spirit from the original
    // design: the `children_` ledger entry is still explicitly ERASED on success, so a second release
    // for the same child identity -- whether via a fresh, empty allocation or a stale/already-moved-
    // from handle to this same object -- fails closed instead of re-crediting a second time.
    // ADR-142: also verifies `child_quota` actually originated from THIS parent's own
    // allocate_child_share() call, via `origin_mutex_` -- a raw, non-owning pointer to the SAME stable
    // `AsyncMutex*` this parent's own `mutex_` unique_ptr already points at (stable across any later
    // move of the parent AsyncQuota itself, for the exact reason this file's own top comment gives for
    // using a unique_ptr indirection at all). Before this, `children_` is keyed ONLY by child id() --
    // reproduced for real: the SAME child IdentityHandle legitimately holding a share from TWO
    // independent root quotas (nothing prevents one agent identity receiving BranchCost grants from
    // two separate sessions) let `quota_x.release_child_share(std::move(share_from_quota_y))` succeed:
    // it found quota_x's OWN live entry for that child id (coincidentally present), erased THAT entry
    // even though quota_x's real, still-outstanding share to the child was untouched, and credited
    // quota_x's remaining_ with quota_y's leftover amount -- crediting the wrong parent while stranding
    // quota_y's own ledger entry (now un-releasable, since the only live AsyncQuota object for it was
    // just consumed against the wrong parent). `origin_mutex_` closes this: a child object can only
    // ever be released back through the exact parent instance that minted it.
    [[nodiscard]] task<agentengine::result<void>> release_child_share(AsyncQuota child_quota) {
        AsyncMutex::Guard guard = co_await mutex_->lock();
        if (child_quota.origin_mutex_ != mutex_.get()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "this AsyncQuota object was not allocated by THIS parent's own "
                "allocate_child_share() -- it belongs to a different quota tree",
                "async_quota.release_wrong_parent"});
        }
        auto it = children_.find(child_quota.owner_.id());
        if (it == children_.end()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                            "release does not match a live allocation",
                                                            "async_quota.release_not_owed"});
        }
        children_.erase(it);
        remaining_ += child_quota.remaining_;
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
    // ADR-142: non-owning, set ONLY by allocate_child_share() to the allocating parent's own
    // `mutex_.get()` -- nullptr for a root-minted AsyncQuota (mint_root()), which is never a valid
    // `child_quota` argument to any release_child_share() call regardless (no parent's `mutex_.get()`
    // is ever nullptr, since `mutex_` is always populated by the constructor). See
    // release_child_share()'s own comment for the real cross-parent confusion this closes.
    AsyncMutex const* origin_mutex_ = nullptr;
};

}  // namespace agentengine::rt
