#pragma once
// PROVE-PHASE PROBE for GrantSet (§13.5/§15.3's own still-unimplemented type at the time §22-24 were
// written) -- the identity-native analog of the real CapabilitySet, closing round 1 Finding 6's
// "missing enumeration surface" for real: encapsulated, heterogeneous (many Grant<Payload> kinds in
// one session-scoped object), with a find()/find_all() lookup surface mirroring CapabilitySet's own
// proven shape (find_fs_read/find_fs_write/etc., trust/capability.hpp) -- never a bag of public
// fields a caller could harvest, matching §13.1's own encapsulation discipline.

#include <any>
#include <memory>
#include <mutex>
#include <optional>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "../identity_authority/identity_authority.hpp"

namespace probe {

// FIX (post-review pass): GrantSet previously had NO synchronization at all -- unlike every other
// shared-state primitive in this design (IdentityAuthority, AsyncQuota, Ledger, MediatedFileSystem
// all received an explicit concurrency pass). An independent code review flagged this as a real,
// previously-unnamed gap: the design's own §18 already discusses `tool_pipeline.hpp::
// background_task()` dispatching real work on a detached `std::thread`, meaning two concurrent tool
// invocations FROM THE SAME SESSION (one backgrounded, one not) calling `insert()`/`find()` on the
// SAME session-owned GrantSet is a real, plausible scenario this type was never actually tested
// against -- `std::unordered_map`/`std::any` give no thread-safety of their own. A plain
// `std::mutex` member would reintroduce the by-value-non-movable-sync-primitive bug class (§21.1)
// this document has already hit three times, since GrantSet is moved by value elsewhere (e.g.
// SandboxStandIn's constructor) -- so, consistent with every prior fix of that class, the mutex is
// held behind a `unique_ptr` for a stable address across moves.
class GrantSet {
public:
    GrantSet() : mutex_(std::make_unique<std::mutex>()) {}

    // Host-only in intent (matches this design's own §16/§19.1 threat model: populated by trusted,
    // native, host-authored code -- e.g. inside a ContextProvider's own constructor/on_context(),
    // never reachable from model-parsed Args).
    template <class Payload>
    void insert(Grant<Payload> grant) {
        std::lock_guard<std::mutex> guard(*mutex_);
        auto& slot = storage_[std::type_index(typeid(Payload))];
        if (!slot.has_value()) slot = std::vector<Grant<Payload>>{};
        std::any_cast<std::vector<Grant<Payload>>&>(slot).push_back(std::move(grant));
    }

    // Single-match lookup -- the first grant of this PAYLOAD KIND whose issued_to is `caller` or an
    // ancestor of `caller` (real multi-hop check via IdentityAuthority::bootstrap(), same as
    // authorized()). Mirrors CapabilitySet::find_fs_read()/find_fs_write()'s own "return the grant's
    // own live parameters, not a yes/no" shape.
    template <class Payload>
    [[nodiscard]] std::optional<Grant<Payload>> find(Principal const& caller) const {
        std::lock_guard<std::mutex> guard(*mutex_);
        auto it = storage_.find(std::type_index(typeid(Payload)));
        if (it == storage_.end()) return std::nullopt;
        auto const& vec = std::any_cast<std::vector<Grant<Payload>> const&>(it->second);
        for (auto const& g : vec) {
            if (IdentityAuthority::bootstrap().is_ancestor_of(g.issued_to_id(), caller.id())) return g;
        }
        return std::nullopt;
    }

    // Multi-match lookup -- mirrors CapabilitySet::native_exec_grants()/fs_read_grants()'s own
    // "return ALL matches, verbatim, no filtering" shape, for a caller that may legitimately hold
    // several independent grants of the same kind (e.g. two different RollbackAuthority ceilings
    // issued at different times).
    template <class Payload>
    [[nodiscard]] std::vector<Grant<Payload>> find_all(Principal const& caller) const {
        std::vector<Grant<Payload>> out;
        std::lock_guard<std::mutex> guard(*mutex_);
        auto it = storage_.find(std::type_index(typeid(Payload)));
        if (it == storage_.end()) return out;
        auto const& vec = std::any_cast<std::vector<Grant<Payload>> const&>(it->second);
        for (auto const& g : vec) {
            if (IdentityAuthority::bootstrap().is_ancestor_of(g.issued_to_id(), caller.id())) {
                out.push_back(g);
            }
        }
        return out;
    }

    template <class Payload>
    [[nodiscard]] std::size_t count() const noexcept {
        std::lock_guard<std::mutex> guard(*mutex_);
        auto it = storage_.find(std::type_index(typeid(Payload)));
        if (it == storage_.end()) return 0;
        return std::any_cast<std::vector<Grant<Payload>> const&>(it->second).size();
    }

private:
    // Type-erased, but never exposes a raw Grant<T> bag to a caller that doesn't already know the
    // exact Payload type it's asking for -- the same "no public field a caller could harvest"
    // discipline §13.1 established for Grant<T> itself, applied to the collection holding many of
    // them.
    std::unordered_map<std::type_index, std::any> storage_;
    mutable std::unique_ptr<std::mutex> mutex_;
};

}  // namespace probe
