#pragma once
// PROVE-PHASE PROBE for AsyncQuota<T>, per the design's Revision 3 fix (§13.3: fully coroutine-native,
// both allocate_child_share/try_consume take the SAME AsyncMutex, closing the original sync/async race)
// plus the Revision 5 anti-replay fix (§15.4/19: release_child_share erases its ledger entry on
// success) and the IdentityAuthority-gated mint_root from §15.1/§17.2 (closing "mint your own
// unlimited quota" -- requires authority.is_known(owner)).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"
#include "../identity_authority/identity_authority.hpp"

namespace probe {

// REAL FINDING, caught only by attempting to compile this (no textual red-team round found it):
// agentengine::rt::AsyncMutex deletes its copy ctor/assignment and declares no move members --
// per C++'s own rule, declaring ANY special member function (a deleted one counts) suppresses the
// implicitly-generated move constructor too. So AsyncMutex is non-copyable AND non-movable, which is
// deliberate, not an oversight: LockAwaiter::await_suspend stores a raw `self` pointer to the mutex
// instance a parked coroutine will later be resumed through -- relocating a live AsyncMutex would
// dangle that pointer. Consequently a type that embeds `AsyncMutex` BY VALUE can never itself be
// returned by value, moved, or held in a container that might relocate elements -- exactly what
// AsyncQuota<T>::mint_root()/allocate_child_share() need to do (both return a fresh AsyncQuota BY
// VALUE). Fixed here by holding the mutex behind a stable-address `unique_ptr` instead: moving the
// unique_ptr relocates the POINTER, never the pointee, so the real AsyncMutex object's address never
// changes regardless of how many times the owning AsyncQuota is moved.
template <class Kind>
class AsyncQuota {
public:
    // Synchronous -- minting a quota is a rare, host-driven, low-frequency operation (mirrors
    // IdentityAuthority's own §15.1/§16 rationale for why mint_root/derive_child are plain functions,
    // not coroutines). Gated through IdentityAuthority (§15.1/§17.2's fix): fails closed if `owner`
    // was never actually minted, closing round 3's "mint your own principal, mint your own unlimited
    // quota, hand it to a required-parameter check" bypass.
    [[nodiscard]] static result<AsyncQuota> mint_root(IdentityAuthority const& authority, Principal owner,
                                                        std::uint64_t total) {
        if (!authority.is_known(owner.id())) {
            return std::unexpected(error{"principal was never minted", "quota.unknown_principal"});
        }
        return AsyncQuota(owner, total);
    }

    // NOW a coroutine -- takes the SAME mutex_ try_consume() does, closing the original race where
    // this was a plain synchronous function racing a lock-guarded try_consume() on the same field.
    [[nodiscard]] agentengine::rt::task<result<AsyncQuota>> allocate_child_share(Principal child,
                                                                                    std::uint64_t amount) {
        agentengine::rt::AsyncMutex::Guard guard = co_await mutex_->lock();
        if (amount > remaining_) {
            co_return std::unexpected(error{"insufficient remaining budget to allocate child share",
                                              "quota.insufficient_remaining"});
        }
        remaining_ -= amount;
        children_[child.id()] = amount;
        co_return AsyncQuota(child, amount);
    }

    [[nodiscard]] agentengine::rt::task<result<void>> try_consume(std::uint64_t amount, Principal spender) {
        agentengine::rt::AsyncMutex::Guard guard = co_await mutex_->lock();
        if (amount > remaining_) {
            co_return std::unexpected(error{"spawn cost budget exhausted", "quota.exhausted"});
        }
        remaining_ -= amount;
        (void)spender;  // real design attributes consumption to spender; omitted here for brevity --
                          // orthogonal to what this probe exists to prove (the race + anti-replay
                          // mechanisms)
        co_return result<void>{};
    }

    // Anti-replay fix (§15.4/§19): the ledger entry is explicitly ERASED on success, so a second call
    // with the same (child, amount) fails closed instead of re-crediting remaining_ a second time --
    // round 3's finding was that Revision-3's text never stated this step; this probe exists
    // specifically to confirm the erase actually closes it, not just that it's now written down.
    [[nodiscard]] agentengine::rt::task<result<void>> release_child_share(Principal child,
                                                                             std::uint64_t amount) {
        agentengine::rt::AsyncMutex::Guard guard = co_await mutex_->lock();
        auto it = children_.find(child.id());
        if (it == children_.end() || it->second != amount) {
            co_return std::unexpected(error{"release does not match a live allocation",
                                              "quota.release_not_owed"});
        }
        children_.erase(it);
        remaining_ += amount;
        co_return result<void>{};
    }

    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }

private:
    AsyncQuota(Principal owner, std::uint64_t total)
        : owner_(owner), remaining_(total), mutex_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    Principal owner_;
    std::uint64_t remaining_;
    std::unordered_map<std::uint64_t, std::uint64_t> children_;
    std::unique_ptr<agentengine::rt::AsyncMutex> mutex_;   // stable-address indirection -- see the
                                                              // class-level comment above for why a
                                                              // by-value AsyncMutex member would make
                                                              // this whole type unreturnable
};

struct StorageBytes {};  // a Kind tag, matching the design's own AsyncQuota<StorageBytes> instantiation

}  // namespace probe
