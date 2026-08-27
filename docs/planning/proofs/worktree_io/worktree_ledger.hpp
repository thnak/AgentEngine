#pragma once
// PROVE-PHASE PROBE: Ledger backed by the REAL agentengine content-addressed store
// (agentengine::WorktreeObjectStore/InMemoryWorktreeObjectStore, agentengine::compute_digest --
// real SHA-256 via Windows CNG/BCrypt, src/core/worktree_digest.cpp, linked as a real second
// translation unit, not reimplemented) instead of §23's FNV-1a placeholder "Digest is a plain
// hex-string" stand-in. Deliberately does NOT reuse agentengine::Mount/mount_read/mount_write or
// cap::FsRead/FsWrite (src/backends/native_jail/worktree_mount_sync.hpp) -- those are wired to the
// OLD Capability/CapabilitySet system this design's own Grant<T>/IdentityAuthority replaces; pulling
// them in here would quietly reintroduce exactly what this design was built to do without, at the one
// moment (real I/O) where that distinction actually matters. Authorization stays on THIS design's own
// GrantSet throughout -- only the CONTENT-ADDRESSING primitive (digest+blob+tree storage, which has
// no capability-system entanglement at all) is reused.
//
// §34 UNIFICATION: `Ledger` is now `Ledger<Store>` (default `agentengine::InMemoryWorktreeObjectStore`,
// exactly today's behavior, zero source change for every existing `Ledger ledger;`/`Ledger
// shared_ledger;` call site -- CTAD deduces the default from the defaulted constructor argument).
// This is deliberately NOT a second, parallel "DurableLedger" type: the whole reason to templatize
// rather than fork is that §26/§32/§33 each independently found real bugs caused by two
// independently-plausible implementations quietly diverging -- one Ledger, parameterized on its
// object store, is the only way to make "the durable configuration" and "the in-memory configuration"
// be, structurally, THE SAME CODE, not two files someone has to remember to keep in sync.
//
// §34 also closes A2 (durable `branches_`/ACL, atop the already-durable `FileWorktreeObjectStore`
// from §28.2) and A8 (a bounded per-digest ACL, enforced at every insertion point, failing closed
// rather than growing forever) -- see `persist_snapshot_locked()`/`load_durable_state()` and
// `kMaxAclRootsPerDigest` below, and §34 of the design doc for the real, twice-verified proof.

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/worktree_types.hpp"  // real Digest/Tree/TreeEntry/compute_digest/
                                                   // WorktreeObjectStore/InMemoryWorktreeObjectStore
#include "agentengine/rt/task.hpp"

#include "../async_quota/async_quota.hpp"
#include "../common/result.hpp"
#include "../identity_authority/identity_authority.hpp"
#include "merge_trees.hpp"

namespace probe {

// Real Checkpoint -- self_digest now computed via the REAL agentengine::compute_digest (SHA-256),
// not FNV-1a, over the same {tree, parent, authored_by, turn_index} fields §15.4/§17.4 specified.
//
// §34: `authored_by` is now a raw `authored_by_id` (uint64_t), not a `Principal`. Confirmed by a
// full audit of every real consumer in this tree (grep, not assumption) that nothing anywhere ever
// read anything from a stored Checkpoint's `authored_by` beyond `.id()` -- `Principal`'s label and
// friend-gated construction added nothing here except making durable serialization impossible
// without inventing a new "reconstitute a Principal for an id I already know about" capability on
// IdentityAuthority. Storing the id directly is both simpler and a strictly narrower surface.
struct Checkpoint {
    agentengine::Digest self_digest;
    agentengine::Digest tree;     // a REAL tree digest -- agentengine::WorktreeObjectStore::put_tree()'s
                                    // own return value, not a hand-rolled string
    agentengine::Digest parent;
    std::uint64_t authored_by_id = 0;
    std::uint64_t turn_index = 0;
};

[[nodiscard]] inline result<agentengine::Digest> compute_self_digest(agentengine::Digest const& tree,
                                                                        agentengine::Digest const& parent,
                                                                        std::uint64_t authored_by_id,
                                                                        std::uint64_t turn_index) {
    std::ostringstream in;
    in << tree << '|' << parent << '|' << authored_by_id << '|' << turn_index;
    std::string const s = in.str();
    std::vector<std::byte> bytes(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) bytes[i] = static_cast<std::byte>(s[i]);
    auto digest = agentengine::compute_digest(bytes);
    if (!digest) return std::unexpected(error{digest.error().message, "worktree_ledger.digest_failed"});
    return *digest;
}

struct BranchCost {};

template <class Store>
class Ledger;

template <class Store = agentengine::InMemoryWorktreeObjectStore>
class BranchHandle {
public:
    BranchHandle(BranchHandle&& other) noexcept
        : owner_(other.owner_), name_(std::move(other.name_)), created_by_id_(other.created_by_id_),
          base_(std::move(other.base_)), resolved_(other.resolved_) {
        other.owner_ = nullptr;
        other.resolved_ = true;
    }
    // FIX (post-review unification pass): this was `= delete`d here while the ORIGINAL
    // ledger/ledger.hpp's own BranchHandle implements a real move-assignment (overwriting a live
    // handle first queues-abandon of whatever it previously held, matching this class's own
    // destructor discipline) -- a genuine, previously-undisclosed API-surface gap between the two
    // Ledger implementations an independent code review's rewire onto this file exposed (any code
    // moving a BranchHandle by assignment, e.g. `SandboxSession`'s defaulted move-assignment
    // operator, would have this operator silently become deleted with only a compiler warning,
    // not an error, until actually used). Implemented identically to the original's own version.
    BranchHandle& operator=(BranchHandle&& other) noexcept {
        if (this != &other) {
            maybe_queue_abandon();
            owner_ = other.owner_;
            name_ = std::move(other.name_);
            created_by_id_ = other.created_by_id_;
            base_ = std::move(other.base_);
            resolved_ = other.resolved_;
            other.owner_ = nullptr;
            other.resolved_ = true;
        }
        return *this;
    }
    BranchHandle(BranchHandle const&) = delete;
    BranchHandle& operator=(BranchHandle const&) = delete;
    ~BranchHandle() { maybe_queue_abandon(); }

    [[nodiscard]] std::string const& name() const noexcept { return name_; }
    // §34: renamed from created_by() (which returned a Principal) to created_by_id() (returns the
    // raw id) -- confirmed by grep that no file anywhere outside this one ever called the accessor,
    // so this is a safe, contained rename, not a break of any real external contract.
    [[nodiscard]] std::uint64_t created_by_id() const noexcept { return created_by_id_; }

private:
    friend class Ledger<Store>;
    BranchHandle(Ledger<Store>* owner, std::string name, std::uint64_t created_by_id,
                  agentengine::Digest base)
        : owner_(owner), name_(std::move(name)), created_by_id_(created_by_id), base_(std::move(base)) {}
    void maybe_queue_abandon();

    Ledger<Store>* owner_ = nullptr;
    std::string name_;
    std::uint64_t created_by_id_ = 0;
    agentengine::Digest base_;
    bool resolved_ = false;
};

struct BranchState {
    std::uint64_t created_by_id = 0;
    agentengine::Digest head_self_digest;
    agentengine::Digest head_tree_digest;
    std::uint64_t head_turn_index = 0;
    std::unordered_map<std::uint64_t, Checkpoint> checkpoints;
    // §34/A4: the tree digest this branch started FROM -- the common ancestor a real three-way
    // merge needs as `base`. Immutable once set (never touched by this branch's OWN later
    // commit()/reset_to() calls, only by create_root_branch()/branch_from() at creation time) --
    // `head_tree_digest` above already tracks the CURRENT state and is not reusable for this,
    // since it moves forward with every commit.
    agentengine::Digest base_tree_digest;
};

// Ledger now holds the REAL object store directly -- every commit()/reset_to() call goes through
// agentengine::WorktreeObjectStore's real put_tree()/get_tree() (dedup, canonical serialization,
// real SHA-256), not a caller-supplied opaque string.
//
// IDENTITY-SCOPED ACCESS CONTROL (added after a real internal-attack simulation confirmed the
// content-addressed store had NONE): every blob/tree digest carries a set of "root" principal ids
// authorized to read it -- populated at the moment a principal legitimately WRITES that digest
// (put_blob_safe/commit), checked via the real, already-proven multi-hop IdentityAuthority ancestry
// table on every READ (get_blob_safe/get_tree_safe) and on every COMMIT that references a digest the
// committing principal didn't itself just write. This closes two real, confirmed leaks: (1) any
// caller holding a Ledger reference and a known/leaked digest could read ANY content ever stored by
// ANY session, with zero identity check; (2) a commit could reference (and thereby durably "adopt"
// into its own attributed history) a digest it never had any legitimate relationship to. Sharing ONE
// Ledger/object store across sessions (the real, tested multi-tenant shape from §26/§28) is only
// actually safe with this in place -- without it, "shared store" and "no isolation between sessions"
// were the same thing.
//
// §34: `Store` defaults to `agentengine::InMemoryWorktreeObjectStore` (today's exact behavior).
// Passing `probe::FileWorktreeObjectStore` (§28.2) instead, plus a non-nullopt `durable_dir`, makes
// BOTH the object content (already durable via that Store on its own) AND this Ledger's own
// `branches_`/ACL bookkeeping durable across a real process restart -- see `persist_snapshot_locked()`
// below. `durable_dir` is independent of whatever directory `store` itself was constructed against;
// callers typically use sibling subdirectories of one durable root (e.g. `root/objects` for `store`,
// `root/ledger` for this).
template <class Store = agentengine::InMemoryWorktreeObjectStore>
class Ledger {
public:
    // Bound for A8 (§29.6/§34): the maximum number of DISTINCT root principal ids one digest's ACL
    // entry may ever hold. Every insertion path below enforces this and fails CLOSED (rejects the
    // operation with `ledger.acl_root_cap_exceeded`) rather than growing the set forever or silently
    // evicting an existing, still-legitimate entry -- eviction was rejected as an option specifically
    // because it would silently revoke a real grant, which is a worse failure mode than a clear,
    // attributable rejection of a NEW one. 64 is a deliberately generous, arbitrary-but-documented
    // default for a prove-phase probe, not a tuned production value -- a real deployment's actual
    // fan-out (how many independent root sessions legitimately end up sharing one piece of content)
    // is exactly the kind of thing §11's own "cross-session/store-wide quota interaction" open
    // question would need real usage data to size correctly.
    static constexpr std::size_t kMaxAclRootsPerDigest = 64;

    // A8 fix (2026-08-27, design doc §40): `kMaxAclRootsPerDigest` above is kept, unchanged, as the
    // class-level DEFAULT (every existing probe/call site referencing `Ledger<>::kMaxAclRootsPerDigest`
    // directly is unaffected) -- real enforcement now reads this per-INSTANCE value instead, closing
    // the "not a tuned production value" residual by making the bound a real deployment knob rather
    // than a recompile-only constant. Reserved, never issued to a real `Principal` (`IdentityAuthority`
    // mints starting at 1, `identity_authority.hpp`'s own `next_id_ = 1`, and `Principal` has no
    // public default constructor at all) -- safe to use as a sentinel that can never collide with a
    // real principal id.
    static constexpr std::uint64_t kPubliclySharedSentinelRootId = 0;

    explicit Ledger(Store store = Store{}, std::optional<std::filesystem::path> durable_dir = std::nullopt,
                     std::size_t max_acl_roots_per_digest = kMaxAclRootsPerDigest)
        : store_(std::move(store)), durable_dir_(std::move(durable_dir)),
          max_acl_roots_per_digest_(max_acl_roots_per_digest) {
        if (durable_dir_) {
            std::filesystem::create_directories(*durable_dir_);
            load_durable_state();
        }
    }

    // A8 fix (2026-08-27): the real, previously-uncovered gap this closes -- §29.6/§34's own
    // "generous, documented-not-tuned default" framing understated the cap's actual failure mode.
    // It is not merely "untuned," it is a PERMANENT, non-evictable ceiling (eviction was already,
    // deliberately, rejected as an option -- see this class's own header comment above) with NO
    // escape hatch: once 64 distinct, non-descendant principals have ever touched one digest (the
    // realistic case is many independent sessions forking from an identical, differently-owned
    // SHARED base -- e.g. a common onboarding template -- `branch_from()`'s own ACL insertion at
    // line ~451 and `merge()`'s at line ~592 are the two real growth drivers found by tracing every
    // `insert_acl_root_bounded()` call site against A10's own real calling pattern), the 65th
    // legitimate session is permanently denied, forever, with no tuning knob available at runtime
    // (closed by the constructor parameter above) and no way for a content owner to say "this is
    // meant to be read by anyone" (closed here).
    //
    // Deliberately an EXPLICIT, principal-gated ratchet, never automatic and never inferable from
    // model output (I2/I3): `requested_by` must ALREADY be authorized for `digest` (the identical
    // `authorized_for()` check every read uses) before they can mark it shared -- this narrows/
    // decides among authority `requested_by` already possesses (their own already-legitimate read
    // access), it never mints new authority from nothing. Once marked, `authorized_for()` grants
    // EVERY principal (not just existing/future descendants of some root) read access to `digest`,
    // and -- the actual fix for the growth vector, not just the denial -- every future
    // `insert_acl_root_bounded()` call for that digest becomes a real no-op (see that function's own
    // comment): a publicly-shared digest's ACL set never grows again, so it is fully exempt from the
    // cap it would otherwise keep consuming headroom against, not merely allowed to exceed it once.
    // PERMANENT: matching this whole ACL mechanism's "no eviction, no silent revocation" posture,
    // there is no `unmark_digest_shared()` -- sharing content is a one-way ratchet, like every other
    // mutation this Ledger already only allows to move in one direction. `is_tree` disambiguates
    // which of `tree_acl_`/`blob_acl_` `digest` belongs to -- the same split every other ACL-touching
    // method here already requires, since a tree digest and a blob digest are never authorized
    // through the same set.
    //
    // REAL FINDING, disclosed not fixed (security red-team, 2026-08-27): unlike `HostSandboxSelection`
    // elsewhere in this codebase (`sandbox_backend_registry.hpp`), `digest` here is a bare
    // `agentengine::Digest` with no structural, non-implicitly-constructible defense-in-depth against
    // a future caller accidentally passing a model-influenced value. Dormant today (zero production
    // callers, and `requested_by` still needs genuine, already-existing authorization regardless of
    // what `digest` names), but a future integration layer wiring a real caller to this should not
    // assume the signature itself provides any I3 protection -- it does not.
    [[nodiscard]] result<void> mark_digest_shared(agentengine::Digest digest, bool is_tree,
                                                     Principal requested_by) {
        std::lock_guard<std::mutex> g(mutex_);
        auto& acl = is_tree ? tree_acl_ : blob_acl_;
        if (!authorized_for(acl, digest, requested_by)) {
            return std::unexpected(error{
                "requester is not authorized for this digest -- only a principal already "
                "authorized for it may mark it publicly shared",
                "ledger.mark_shared_unauthorized"});
        }
        // Direct insertion, deliberately bypassing insert_acl_root_bounded()'s own cap check -- this
        // IS the cap's escape hatch, so it must never itself be subject to the cap it exists to let
        // an owner opt out of.
        acl[digest].insert(kPubliclySharedSentinelRootId);
        persist_snapshot_locked();
        return {};
    }

    [[nodiscard]] result<agentengine::Digest> put_blob_safe(std::span<std::byte const> bytes,
                                                               Principal writer) {
        std::lock_guard<std::mutex> g(mutex_);
        auto d = store_.put_blob(bytes);
        if (!d) return std::unexpected(error{d.error().message, "worktree_ledger.put_blob_failed"});
        auto acl_ok = insert_acl_root_bounded(blob_acl_, *d, writer.id(), max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());
        persist_snapshot_locked();
        return *d;
    }
    [[nodiscard]] result<std::vector<std::byte>> get_blob_safe(agentengine::Digest const& digest,
                                                                  Principal caller) {
        std::lock_guard<std::mutex> g(mutex_);
        if (!authorized_for(blob_acl_, digest, caller)) {
            return std::unexpected(error{"caller is not authorized to read this blob digest",
                                          "ledger.blob_access_denied"});
        }
        auto b = store_.get_blob(digest);
        if (!b) return std::unexpected(error{b.error().message, "worktree_ledger.get_blob_failed"});
        return *b;
    }
    [[nodiscard]] result<agentengine::Tree> get_tree_safe(agentengine::Digest const& digest,
                                                             Principal caller) {
        std::lock_guard<std::mutex> g(mutex_);
        if (!authorized_for(tree_acl_, digest, caller)) {
            return std::unexpected(error{"caller is not authorized to read this tree digest",
                                          "ledger.tree_access_denied"});
        }
        auto t = store_.get_tree(digest);
        if (!t) return std::unexpected(error{t.error().message, "worktree_ledger.get_tree_failed"});
        return *t;
    }
    [[nodiscard]] std::size_t blob_count_safe() {
        std::lock_guard<std::mutex> g(mutex_);
        return store_.blob_count();
    }

    // Read-only dry-run for a caller (e.g. `combine_into_tree()`) that needs to write SEVERAL blobs
    // as one logical batch: true iff a blob with this exact byte content, written by `writer`, would
    // be accepted -- either because `writer` already holds an ACL entry for this digest, or because
    // inserting a new one would stay within `kMaxAclRootsPerDigest`. Performs NO mutation. Lets a
    // caller validate the WHOLE batch before writing any of it, instead of discovering an ACL-cap
    // rejection partway through with earlier blobs already durably persisted and unreferenced by any
    // Tree/Checkpoint (a real "silent partial persist" gap a code review pass found).
    [[nodiscard]] bool would_accept_blob_write(std::span<std::byte const> bytes, Principal writer) {
        auto digest = agentengine::compute_digest(bytes);
        if (!digest) return false;
        std::lock_guard<std::mutex> g(mutex_);
        auto it = blob_acl_.find(*digest);
        if (it == blob_acl_.end()) return true;         // brand-new digest, nothing to exceed yet
        if (it->second.contains(writer.id())) return true;  // no-op re-touch, always allowed
        return it->second.size() < kMaxAclRootsPerDigest;
    }

    [[nodiscard]] agentengine::rt::task<result<BranchHandle<Store>>> create_root_branch(Principal owner) {
        std::string name = "root-" + std::to_string(owner.id());
        agentengine::Digest empty_tree_digest;
        {
            // REAL BUG this probe's own first concurrent run caught (a segfault, not a hang or a
            // wrong answer): agentengine::InMemoryWorktreeObjectStore has NO internal
            // synchronization of its own (its real, shipped std::unordered_map members are plain,
            // unguarded) -- every store_ access MUST be serialized by THIS Ledger's own mutex_, the
            // same one guarding branches_. The original version called store_.put_tree() BEFORE
            // acquiring mutex_, so two concurrent commit()/create_root_branch() calls raced two
            // unsynchronized std::unordered_map::insert_or_assign() calls on the SAME store_
            // instance -- a real data race that corrupted the map's internal bucket structure and
            // segfaulted under real multi-threaded load. Fixed by moving every store_ call inside
            // the SAME critical section as the branches_ access.
            std::lock_guard<std::mutex> g(mutex_);
            auto put = store_.put_tree(agentengine::Tree{});
            if (!put) co_return std::unexpected(error{put.error().message, "worktree_ledger.put_tree_failed"});
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

    // Commits a REAL agentengine::Tree (already built by the caller's real I/O layer) through the
    // REAL object store -- put_tree() sorts entries, computes a REAL SHA-256 digest, and dedups
    // against anything already stored. store_.put_tree() now runs INSIDE the same mutex_-guarded
    // section as the branches_ mutation -- see create_root_branch()'s own comment for why this is
    // required, not merely tidy. NOW ALSO validates, before accepting the tree at all, that
    // `authored_by` (or an ancestor) is authorized for EVERY entry's digest already in the store --
    // closing the real "commit a tree referencing someone else's blob you never had access to"
    // attack a real internal-attack simulation confirmed.
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> commit(BranchHandle<Store> const& branch,
                                                                      agentengine::Tree tree,
                                                                      Principal authored_by,
                                                                      AsyncQuota<StorageBytes>& quota) {
        std::size_t const approx_bytes = agentengine::canonical_tree_bytes(tree).size();
        auto consumed = co_await quota.try_consume(approx_bytes, authored_by);
        if (!consumed.has_value()) co_return std::unexpected(consumed.error());

        // A REAL FINDING a code-review pass caught: every failure path below used to `co_return`
        // straight out of this coroutine with the quota already debited above and no refund --
        // a caller whose commit was rejected for an unrelated reason (stale branch, unauthorized
        // reference) permanently lost budget for zero stored content, a self-inflicted or
        // adversarial quota-exhaustion DoS. Fixed by computing the outcome in a plain (non-
        // coroutine) helper first -- so `mutex_` is fully released before any `co_await` -- then
        // refunding exactly what was consumed on any failure. This also avoids holding a
        // `std::lock_guard<std::mutex>` across a coroutine suspension point (a real correctness
        // hazard on its own: AsyncMutex's coroutine may resume on a different thread than it
        // suspended on, and unlocking a `std::mutex` from a different thread than locked it is UB).
        // REAL FINDING an external-validation pass caught, cross-referencing git's own real
        // CVE-2014-9390 (a tree entry named e.g. ".Git" case-folds to the SAME real path as ".git"
        // on a case-insensitive filesystem -- Windows NTFS/FAT, default macOS HFS+ -- letting one
        // silently overwrite the other on checkout): this design's own `commit()`/`put_tree()`
        // never rejected two entries whose NAMES case-fold to the same real path (e.g.
        // "readme.txt" and "README.txt" -- two perfectly legal, genuinely distinct digests/content
        // as far as the content-addressed store is concerned). `materialize()` writing such a tree
        // to a REAL Windows filesystem was empirically confirmed to silently drop one entry's
        // content with NO error, NO warning, and a plain successful `result<void>{}` -- a real,
        // previously-undiscovered content-integrity gap, not a hypothetical one. Fixed by rejecting
        // the commit outright if any two entries case-fold to the same name, matching git's own
        // eventual fix DIRECTION for CVE-2014-9390 (reject rather than silently materialize).
        // HONEST RESIDUAL, not claimed solved: this checks ASCII case-folding only (`tolower` per
        // byte) -- git's own real CVE-2014-9390 fix additionally had to handle HFS+'s Unicode
        // "ignorable" codepoints (characters that fold to nothing at all under certain Unicode
        // normalizations), a materially harder problem this check does not attempt.
        for (std::size_t i = 0; i < tree.entries.size(); ++i) {
            std::string folded_i = tree.entries[i].name;
            for (char& c : folded_i) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (std::size_t j = i + 1; j < tree.entries.size(); ++j) {
                std::string folded_j = tree.entries[j].name;
                for (char& c : folded_j) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (folded_i == folded_j && tree.entries[i].name != tree.entries[j].name) {
                    (void)co_await quota.refund(approx_bytes);
                    co_return std::unexpected(error{
                        "tree contains two entries that case-fold to the same real path ('" +
                            tree.entries[i].name + "' and '" + tree.entries[j].name +
                            "') -- rejected before materialize() could silently drop one of them "
                            "on a case-insensitive filesystem, matching git's own real CVE-2014-9390 "
                            "fix direction",
                        "ledger.case_folding_collision"});
                }
            }
        }

        result<Checkpoint> outcome = [&]() -> result<Checkpoint> {
            std::lock_guard<std::mutex> g(mutex_);
            for (auto const& entry : tree.entries) {
                auto const& acl = entry.is_tree ? tree_acl_ : blob_acl_;
                if (!authorized_for(acl, entry.digest, authored_by)) {
                    return std::unexpected(error{
                        "commit references digest '" + entry.digest.substr(0, 12) +
                            "...' (path '" + entry.name +
                            "') that the committing principal is not authorized for -- every entry "
                            "a commit references must already be legitimately accessible to the "
                            "committing principal (via a prior put_blob_safe()/commit() of its own, "
                            "or inheritance from an ancestor principal)",
                        "ledger.commit_unauthorized_reference"});
                }
            }
            auto tree_digest = store_.put_tree(std::move(tree));
            if (!tree_digest) return std::unexpected(error{tree_digest.error().message, "worktree_ledger.put_tree_failed"});
            auto acl_ok = insert_acl_root_bounded(tree_acl_, *tree_digest, authored_by.id(),
                                                     max_acl_roots_per_digest_);
            if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());

            auto it = branches_.find(branch.name());
            if (it == branches_.end())
                return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
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
            // refund() only fails if the quota's own AsyncMutex is somehow unusable -- nothing this
            // caller can act on differently than the commit failure it's already returning; the
            // [[nodiscard]] result is deliberately discarded via (void), not swallowed silently.
            (void)co_await quota.refund(approx_bytes);
            co_return std::unexpected(outcome.error());
        }
        co_return *outcome;
    }

    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> reset_to(BranchHandle<Store> const& branch,
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

    // REAL branch_from()/merge() -- added by the post-review unification pass. Before this, this
    // Ledger (real content-addressing + the §28.4 mutex fix + the §29 ACL fix) had NO branching
    // capability at all, while the ORIGINAL ledger/ledger.hpp (no real storage, no ACL) was the
    // only one that could branch/merge -- a real, previously-undisclosed feature-surface
    // regression an independent code review caught: no single Ledger anywhere in the tree had
    // real storage + the concurrency fix + the ACL fix + branch/merge all at once, meaning §26's
    // "full stack" demo and §29's "attacks confirmed and fixed" demo could never have been the
    // same artifact. This closes that gap for real, not just in documentation.
    //
    // COW branching: the child starts as a copy of the parent's current head (same tree/self
    // digest, same turn index) under a fresh branch name; no new content is copied since content
    // is addressed by digest, not by branch -- the child and parent simply point at the same
    // existing tree entry in store_ until the child's own commit() calls diverge it. The child's
    // creator is granted ACL access to the parent's current head tree digest so its first
    // read/commit against inherited content succeeds even before it has written anything of its
    // own (mirrors put_blob_safe/commit's "writer/committer is authorized for what it touches"
    // discipline -- the child didn't WRITE the parent's tree, but it legitimately inherits read
    // access to it via being spawned from it, the same relationship IdentityAuthority's own
    // multi-hop ancestry table already recognizes for principals).
    [[nodiscard]] agentengine::rt::task<result<BranchHandle<Store>>> branch_from(
        BranchHandle<Store> const& parent, Principal created_by, AsyncQuota<BranchCost>& quota) {
        auto consumed = co_await quota.try_consume(1, created_by);
        if (!consumed.has_value()) co_return std::unexpected(consumed.error());

        // Same fix as commit() above: compute the outcome with `mutex_` released before any
        // co_await, and refund the branch-cost unit on any failure rather than burning it silently.
        result<BranchHandle<Store>> outcome = [&]() -> result<BranchHandle<Store>> {
            std::lock_guard<std::mutex> g(mutex_);
            auto it = branches_.find(parent.name());
            if (it == branches_.end())
                return std::unexpected(error{"unknown parent branch", "ledger.unknown_branch"});
            BranchState const& parent_state = it->second;
            std::string child_name =
                parent.name() + "/child-" + std::to_string(created_by.id()) + "-" +
                std::to_string(branch_seq_++);
            auto acl_ok = insert_acl_root_bounded(tree_acl_, parent_state.head_tree_digest,
                                                     created_by.id(), max_acl_roots_per_digest_);
            if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());
            BranchState child_state = parent_state;
            child_state.created_by_id = created_by.id();
            // The child's merge `base` is the PARENT's tree AT THIS EXACT MOMENT -- not copied
            // from parent_state.base_tree_digest (that would be the PARENT's own ancestor, wrong
            // common ancestor entirely). child_state.head_tree_digest (already correctly copied
            // from parent_state above) IS the right value here, since a fresh child's head starts
            // equal to its base by definition; the two fields diverge only once the child (or
            // parent) commits further.
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

    // §34/A4: REAL three-way merge, wired to the actual, previously-standalone-only `merge_trees()`
    // (§28.3) instead of the original's "parent wholesale-adopts the child's head" fast-forward
    // stub -- confirmed by grep, before this change, that NOTHING anywhere ever called
    // `merge_trees()` except its own probe, a fourth instance of this document's own recurring
    // "two independently-proven pieces, never actually wired together" pattern (§26/§32/§33).
    // Never called by any existing probe before this change (confirmed by grep), so this replaces
    // the stub in place rather than adding a second, parallel method -- no existing caller to
    // preserve compatibility for, and one merge() avoids yet another future divergence pair.
    //
    // `base` is the child's own `base_tree_digest` (the parent's tree at the moment the child was
    // branched off, §34's new BranchState field) -- `ours` is the PARENT's CURRENT tree, `theirs`
    // is the CHILD's CURRENT tree. A real conflict (§28.3's own algorithm) FAILS the merge closed
    // (`ledger.merge_conflict`) rather than silently picking a side -- resolution UX remains
    // explicitly out of scope (§11's own "MergeStrategy is named, not designed" boundary; detecting
    // a conflict correctly is what this closes, not resolving one automatically). On a clean merge,
    // every entry in the merged tree is validated against the SAME per-entry authorization
    // commit() already requires (an entry inherited from `theirs` that the merge requester has no
    // legitimate relationship to must not be smuggled into the parent's history via a merge any
    // more than via a direct commit) before the merged tree is ever stored.
    //
    // REAL, CONFIRMED VULNERABILITY FOUND AND FIXED BY B1's OWN ADVERSARIAL PASS (§34): this
    // method originally took `std::string const& parent_name` -- a bare, GUESSABLE string (branch
    // names follow the deterministic "root-<owner_id>" / "<parent>/child-<id>-<seq>" scheme), with
    // NO possession check on the parent side at all, unlike every other mutating Ledger method
    // (commit/reset_to/abandon all require the caller to already HOLD the actual handle for
    // whatever they act on). A real attack probe (`attack_sim/probe_attack_variant_empty_victim.cpp`)
    // confirmed this was exploitable: an attacker possessing ONLY their own, completely unrelated
    // branch could merge it into a victim's freshly-created (still-empty) branch merely by knowing
    // its name -- the incidental per-entry authorization check below happened to block the
    // non-empty-victim case (§29's own attack_sim probe) purely because the merged result then
    // included the victim's own pre-existing entries the attacker isn't authorized for, but an
    // EMPTY victim branch has no such entries to trip that check, and the attack succeeded outright.
    // Fixed by requiring possession of the PARENT's own `BranchHandle` too, exactly like every
    // other mutating method already requires -- `parent.name()` replaces the caller-supplied
    // string entirely, so resolving which branch to merge into now depends on ALREADY HOLDING it,
    // never on merely knowing or guessing its name.
    // REAL FINDING a code-review pass caught: every rejection path below used to set
    // `child.resolved_ = true` directly, which suppresses `BranchHandle`'s own destructor-time
    // `maybe_queue_abandon()` -- since `child` is taken BY VALUE (the caller's own handle is
    // consumed regardless of outcome), a rejected merge permanently stranded the branch: still
    // present in `branches_` (so `head_tree_digest()`/`checkpoint_at()` could still read it, which
    // is what `probe_ledger_merge.cpp`'s own Scenario 2 checked) but with NO live handle anywhere
    // and NOT registered as an orphan, so `reclaim_orphaned_branch()`/`abandon_orphaned_branch()`
    // both reject it as "not a recognized orphan" -- a real dead end, not the "still there for a
    // real caller to retry or explicitly abandon" that same probe's own comment claimed. Fixed by
    // registering the branch into `orphaned_from_restart_` on every rejection path instead: it is,
    // by A7's own definition, a branch with no live handle in this process -- the exact condition
    // that set already exists to track -- so the caller now gets a real, working reclaim path via
    // the already-proven A7 API, not silent, permanent loss.
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> merge(BranchHandle<Store> child,
                                                                      BranchHandle<Store> const& parent,
                                                                      Principal requested_by) {
        std::lock_guard<std::mutex> g(mutex_);
        auto child_it = branches_.find(child.name());
        auto parent_it = branches_.find(parent.name());
        if (child_it == branches_.end() || parent_it == branches_.end()) {
            // The child branch itself may still be unknown too (a stale/already-consumed handle) --
            // only register it as a reclaimable orphan if it genuinely still exists.
            if (child_it != branches_.end()) orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(error{"unknown branch in merge()", "ledger.unknown_branch"});
        }
        BranchState const& child_state = child_it->second;
        BranchState& parent_state = parent_it->second;
        agentengine::Digest const theirs_digest = child_state.head_tree_digest;
        agentengine::Digest const ours_digest = parent_state.head_tree_digest;
        agentengine::Digest const base_digest = child_state.base_tree_digest;

        if (!authorized_for(tree_acl_, theirs_digest, requested_by)) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(error{
                "merge requester is not authorized for the child branch's head tree digest",
                "ledger.merge_unauthorized_reference"});
        }

        auto base_tree = store_.get_tree(base_digest);
        auto ours_tree = store_.get_tree(ours_digest);
        auto theirs_tree = store_.get_tree(theirs_digest);
        if (!base_tree.has_value() || !ours_tree.has_value() || !theirs_tree.has_value()) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(error{"merge could not load base/ours/theirs from the object "
                                              "store", "ledger.merge_tree_load_failed"});
        }

        MergeResult merged = merge_trees(*base_tree, *ours_tree, *theirs_tree);
        if (!merged.conflicts.empty()) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(error{
                "merge produced " + std::to_string(merged.conflicts.size()) +
                    " real conflicting path(s) (first: '" + merged.conflicts.front().path +
                    "') -- automatic conflict resolution is explicitly out of this design's scope "
                    "(§11); the merge is rejected rather than silently picking a side",
                "ledger.merge_conflict"});
        }

        for (auto const& entry : merged.merged.entries) {
            auto const& acl = entry.is_tree ? tree_acl_ : blob_acl_;
            if (!authorized_for(acl, entry.digest, requested_by)) {
                orphaned_from_restart_.insert(child.name());
                child.resolved_ = true;
                co_return std::unexpected(error{
                    "merge result references digest '" + entry.digest.substr(0, 12) +
                        "...' (path '" + entry.name +
                        "') that the merge requester is not authorized for",
                    "ledger.merge_unauthorized_reference"});
            }
        }

        auto merged_tree_digest = store_.put_tree(std::move(merged.merged));
        if (!merged_tree_digest.has_value()) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(error{merged_tree_digest.error().message,
                                              "worktree_ledger.put_tree_failed"});
        }
        auto acl_ok = insert_acl_root_bounded(tree_acl_, *merged_tree_digest, requested_by.id(),
                                                 max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(acl_ok.error());
        }

        std::uint64_t const new_turn = parent_state.head_turn_index + 1;
        auto self = compute_self_digest(*merged_tree_digest, parent_state.head_self_digest,
                                          requested_by.id(), new_turn);
        if (!self.has_value()) {
            orphaned_from_restart_.insert(child.name());
            child.resolved_ = true;
            co_return std::unexpected(self.error());
        }
        Checkpoint cp{*self, *merged_tree_digest, parent_state.head_self_digest, requested_by.id(), new_turn};
        parent_state.head_self_digest = *self;
        parent_state.head_tree_digest = *merged_tree_digest;
        parent_state.head_turn_index = new_turn;
        parent_state.checkpoints.insert_or_assign(new_turn, cp);
        branches_.erase(child_it);
        child.resolved_ = true;
        persist_snapshot_locked();
        co_return cp;
    }

    [[nodiscard]] agentengine::rt::task<result<void>> abandon(BranchHandle<Store> child) {
        std::lock_guard<std::mutex> g(mutex_);
        branches_.erase(child.name());
        child.resolved_ = true;
        persist_snapshot_locked();
        co_return result<void>{};
    }

    [[nodiscard]] agentengine::rt::task<std::size_t> reap_pending_abandons() {
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> g(mutex_);
            pending = std::move(pending_abandons_);
            pending_abandons_.clear();
        }
        std::size_t processed = 0;
        for (auto& name : pending) {
            // REGRESSION this review pass caught and fixed: the original version of this loop
            // unconditionally minted a real, throwaway root Principal via
            // IdentityAuthority::bootstrap().mint_root("reap-placeholder") on EVERY iteration,
            // even when the branch turned out not to exist (never used in that case) -- exactly
            // the "speculative mint before knowing it's needed" bug class §23.1 already found
            // and fixed elsewhere in the ORIGINAL ledger/ledger.hpp. Using std::optional avoids
            // minting anything at all unless a real creator id is found.
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

    // REAL FINDING a code-review pass caught: this method used to take no `Principal` at all and
    // perform no `authorized_for()` check, unlike every other Ledger accessor -- since branch names
    // are deterministically guessable (`root-<owner_id>`, `<parent>/child-<id>-<seq>`), ANY caller
    // could enumerate an arbitrary branch's current head digest with zero identity check. The
    // digest itself is metadata, not content, but it is still gated the same way `get_tree_safe()`
    // gates the tree it names -- knowing a branch's name must not be enough on its own.
    [[nodiscard]] result<agentengine::Digest> head_tree_digest(std::string const& branch_name,
                                                                   Principal caller) const {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(branch_name);
        if (it == branches_.end())
            return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
        if (!authorized_for(tree_acl_, it->second.head_tree_digest, caller)) {
            return std::unexpected(error{"caller is not authorized to read this branch's head tree "
                                          "digest", "ledger.tree_access_denied"});
        }
        return it->second.head_tree_digest;
    }

    // Read-only checkpoint-history introspection -- deliberately NOT a "resolve a branch by name
    // into a mutation-capable handle" capability, which would reopen exactly the object-possession
    // security property §29's own Attack 4 analysis confirmed this Ledger's real public API
    // deliberately lacks. Added to let a caller that has lost its live `BranchHandle` (e.g. across
    // a process restart, before A7's own reservation mechanism exists) still verify checkpoint
    // HISTORY survived durably, without granting it any new mutating power.
    //
    // REAL FINDING a code-review pass caught: this method, like `head_tree_digest()` above, used to
    // take no `Principal` and perform no `authorized_for()` check -- since `checkpoint_at()` returns
    // `authored_by_id` and turn-index history, not merely a digest, "digest metadata isn't identity-
    // gated" never actually covered it: any caller could enumerate WHO authored each turn of an
    // arbitrary branch and how many turns occurred, by guessable name alone. Gated on the SPECIFIC
    // checkpoint's own tree digest (not the branch's current head), so access to one historical
    // checkpoint doesn't require -- or imply -- access to whatever the branch's head has since
    // become.
    [[nodiscard]] result<Checkpoint> checkpoint_at(std::string const& branch_name,
                                                      std::uint64_t turn_index,
                                                      Principal caller) const {
        std::lock_guard<std::mutex> g(mutex_);
        auto it = branches_.find(branch_name);
        if (it == branches_.end())
            return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
        auto cp_it = it->second.checkpoints.find(turn_index);
        if (cp_it == it->second.checkpoints.end())
            return std::unexpected(error{"no such checkpoint", "ledger.no_such_checkpoint"});
        if (!authorized_for(tree_acl_, cp_it->second.tree, caller)) {
            return std::unexpected(error{"caller is not authorized to read this checkpoint's tree "
                                          "digest", "ledger.tree_access_denied"});
        }
        return cp_it->second;
    }

    // A7 (§34): every branch name restored by load_durable_state() at construction time -- i.e.
    // every branch with genuinely NO live handle anywhere in this process, because the process
    // that held its one legitimate handle is the one that just exited (cleanly or via a crash;
    // this Ledger cannot tell the difference, and does not need to). A host inspects this list and
    // explicitly decides: reclaim (mint a fresh handle and keep using the branch) or abandon
    // (discard it) -- never automatic.
    [[nodiscard]] std::vector<std::string> orphaned_branches() const {
        std::lock_guard<std::mutex> g(mutex_);
        return std::vector<std::string>(orphaned_from_restart_.begin(), orphaned_from_restart_.end());
    }

    // Mints a genuinely fresh, legitimate BranchHandle for a branch this Ledger's own restart
    // logic identified as orphaned -- NOT a general "resolve any branch by name" bypass: fails
    // closed if the name was never actually in `orphaned_from_restart_` (a live branch, or one
    // already reclaimed once, cannot be re-reclaimed this way), and fails closed if `requested_by`
    // is not authorized for the branch's own current head tree -- the same `authorized_for()` gate
    // every read already uses, so an unrelated caller cannot reclaim a stranger's orphaned branch
    // merely by knowing its name.
    [[nodiscard]] result<BranchHandle<Store>> reclaim_orphaned_branch(std::string const& branch_name,
                                                                         Principal requested_by) {
        std::lock_guard<std::mutex> g(mutex_);
        if (!orphaned_from_restart_.contains(branch_name)) {
            return std::unexpected(error{"branch is not a recognized orphan (either it never "
                                          "existed, is still live, or was already reclaimed once)",
                                          "ledger.not_an_orphan"});
        }
        auto it = branches_.find(branch_name);
        if (it == branches_.end()) {
            return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
        }
        if (!authorized_for(tree_acl_, it->second.head_tree_digest, requested_by)) {
            return std::unexpected(error{
                "requester is not authorized for this orphaned branch's current head tree",
                "ledger.reclaim_unauthorized"});
        }
        orphaned_from_restart_.erase(branch_name);
        return BranchHandle<Store>(this, branch_name, requested_by.id(), it->second.base_tree_digest);
    }

    // The explicit "discard, don't reclaim" decision -- same orphan-only and ACL gating as
    // reclaim_orphaned_branch(), but erases the branch instead of handing back a live handle for
    // it. Uses the SAME internal, Ledger-privileged handle-construction path
    // reap_pending_abandons() already relies on (this class is `friend`-gated to itself, not
    // exposing a new "any caller can construct a handle" capability).
    [[nodiscard]] agentengine::rt::task<result<void>> abandon_orphaned_branch(
        std::string const& branch_name, Principal requested_by) {
        std::optional<std::uint64_t> base_owner_check;
        agentengine::Digest base;
        {
            std::lock_guard<std::mutex> g(mutex_);
            if (!orphaned_from_restart_.contains(branch_name)) {
                co_return std::unexpected(error{"branch is not a recognized orphan",
                                                  "ledger.not_an_orphan"});
            }
            auto it = branches_.find(branch_name);
            if (it == branches_.end())
                co_return std::unexpected(error{"unknown branch", "ledger.unknown_branch"});
            if (!authorized_for(tree_acl_, it->second.head_tree_digest, requested_by)) {
                co_return std::unexpected(error{
                    "requester is not authorized for this orphaned branch's current head tree",
                    "ledger.reclaim_unauthorized"});
            }
            base = it->second.base_tree_digest;
            base_owner_check = requested_by.id();
            orphaned_from_restart_.erase(branch_name);
        }
        BranchHandle<Store> handle(this, branch_name, *base_owner_check, base);
        co_return co_await abandon(std::move(handle));
    }

private:
    friend class BranchHandle<Store>;
    void queue_pending_abandon(std::string const& name) {
        std::lock_guard<std::mutex> g(mutex_);
        pending_abandons_.push_back(name);
    }

    // True iff `caller` (or an ancestor of `caller`, via the real IdentityAuthority ancestry table)
    // is in `acl[digest]`'s recorded set of principals authorized for that digest -- OR the digest
    // has been explicitly, deliberately marked publicly shared (A8 fix, `mark_digest_shared()`
    // below, checked via the reserved `kPubliclySharedSentinelRootId` sentinel actually present in
    // the SAME set, not a second parallel structure). Must be called with mutex_ already held (this
    // is a private helper, never a public entry point of its own).
    [[nodiscard]] static bool authorized_for(
        std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> const& acl,
        agentengine::Digest const& digest, Principal const& caller) {
        auto it = acl.find(digest);
        if (it == acl.end()) return false;
        if (it->second.contains(kPubliclySharedSentinelRootId)) return true;
        for (std::uint64_t allowed_root : it->second) {
            if (allowed_root == caller.id() ||
                IdentityAuthority::bootstrap().is_ancestor_of(allowed_root, caller.id())) {
                return true;
            }
        }
        return false;
    }

    // A8 (§29.6/§34, cap now per-instance-configurable and given a real escape hatch -- see
    // `mark_digest_shared()` above and design doc §40): bounded ACL insertion. Must be called with
    // mutex_ already held. A root id already present is a no-op success (re-touching content you
    // already have access to must never fail just because the set happens to be at its cap) -- only
    // a genuinely NEW distinct root id past the cap fails, and it fails CLOSED (the whole calling
    // operation is rejected) rather than silently dropping the id (which would let the write
    // "succeed" while producing a digest the writer then can't read back -- a confusing, unsafe
    // partial-success state). A8 fix: a digest already marked publicly shared is a real, permanent
    // no-op here too -- not just readable-by-anyone despite the cap, but genuinely EXEMPT from ever
    // growing again, since `authorized_for()` no longer needs a per-principal entry for it at all.
    [[nodiscard]] static result<void> insert_acl_root_bounded(
        std::unordered_map<agentengine::Digest, std::set<std::uint64_t>>& acl,
        agentengine::Digest const& digest, std::uint64_t root_id, std::size_t cap) {
        auto& set = acl[digest];
        if (set.contains(root_id)) return result<void>{};
        if (set.contains(kPubliclySharedSentinelRootId)) return result<void>{};
        if (set.size() >= cap) {
            return std::unexpected(error{
                "digest '" + digest.substr(0, 12) + "...' has reached its maximum of " +
                    std::to_string(cap) +
                    " distinct authorized root principals; a new, unrelated root cannot be added "
                    "(a deliberate, disclosed bound, not an accidental limit -- an already-authorized "
                    "principal may call mark_digest_shared() to exempt this digest from the bound "
                    "entirely, if it is genuinely meant to be read by anyone)",
                "ledger.acl_root_cap_exceeded"});
        }
        set.insert(root_id);
        return result<void>{};
    }

    // ---------------------------------------------------------------------------------------
    // §34/A2: durable branches_/ACL persistence, atop whatever durability `Store` itself already
    // provides for blob/tree CONTENT. Deliberately a full-snapshot rewrite (temp file + atomic
    // rename) on every mutation, not an append-only event log -- simpler to reason about and
    // review (no replay/compaction logic to get subtly wrong), at a real, disclosed, UNMEASURED
    // I/O cost proportional to total state size per mutation (the same honest "not yet measured"
    // posture §11's own MediatedFileSystem-performance open question already carries). A no-op
    // (immediate return) whenever `durable_dir_` is unset, so every existing in-memory-only call
    // site's behavior is completely unaffected -- this is pure addition, not a rewrite of any
    // existing code path's semantics.
    //
    // Crash-safety scope matches this document's own already-established bar (§28.2's own
    // `FileWorktreeObjectStore`, plain `std::ofstream`, no fsync): safe across a clean process
    // exit or crash, NOT across a genuine power loss mid-write. The temp-file-then-rename step
    // specifically guards against a crash mid-WRITE producing a half-written, unparseable
    // snapshot -- `std::filesystem::rename` is atomic at the filesystem level, so a reader after a
    // restart sees either the complete OLD snapshot or the complete NEW one, never a partial file.
    // ---------------------------------------------------------------------------------------

    // A7 (§34, real implementation -- simpler than this section's own original sketch): what
    // happens to a `BranchHandle` across a process crash (§11 item 6). The sketch written during
    // A2 proposed a lease/expiry-based "reservation" record needing a heartbeat renewal
    // mechanism -- implementing it for real found that unnecessary: a `BranchHandle` is a plain
    // in-process C++ object that CANNOT survive a process exit under any circumstances, clean or
    // crashed. Therefore EVERY branch present in a snapshot `load_durable_state()` restores is,
    // by construction, orphaned relative to the NEW process -- no live handle for it exists
    // anywhere, full stop, no timing window or lease-expiry judgment call needed. This is a real
    // simplification an actual implementation attempt found, disclosed here rather than force-
    // fitting the original lease-based sketch for its own sake.
    //
    // Reclaiming an orphaned branch is deliberately NOT automatic (no silent abandon-on-restart,
    // matching this design's own "never silently discard state" discipline) -- a host must
    // explicitly call `reclaim_orphaned_branch()` (mints a genuinely fresh, legitimate
    // `BranchHandle`) or `abandon_orphaned_branch()` (discards it) after inspecting
    // `orphaned_branches()`. `reclaim_orphaned_branch()` still requires the requesting principal
    // be authorized for the branch's own current head tree (the SAME `authorized_for()` check
    // every read already uses) -- an unrelated caller cannot reclaim a stranger's orphaned branch
    // merely by knowing its name, preserving this Ledger's "possession or legitimate ACL
    // relationship, never bare knowledge of a name" security bar for every other operation.
    void persist_snapshot_locked() const {
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
        std::error_code ec;
        std::filesystem::rename(temp_path, final_path, ec);
        // A rename failure here (e.g. a hostile/broken filesystem) is intentionally not escalated
        // to the caller of the mutating operation that triggered it -- durability is a best-effort
        // addition on top of an already-successful in-memory mutation, matching this document's
        // own established posture that a durability probe's job is to PROVE the happy path works,
        // not to invent new failure-injection machinery beyond what real testing already covers
        // (§28.2's own FileWorktreeObjectStore takes the identical stance).
    }

    void load_durable_state() {
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
                std::getline(fields, base_d, '\t');   // absent on a pre-A4 snapshot -- getline
                                                          // simply yields an empty string, handled
                                                          // the same as "no known base" below
                try {
                    BranchState state;
                    state.created_by_id = std::stoull(created_by_id_s);
                    state.head_self_digest = self_d;
                    state.head_tree_digest = tree_d;
                    state.head_turn_index = std::stoull(turn_s);
                    state.base_tree_digest = base_d;
                    branches_.insert_or_assign(name, std::move(state));
                    orphaned_from_restart_.insert(name);   // A7: every restored branch has no live
                                                              // handle in this new process, by
                                                              // construction -- see this file's own
                                                              // A7 comment above
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
                if (it == branches_.end()) continue;   // a checkpoint for a branch line we never
                                                           // saw (truncated/corrupt tail) -- skip
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

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BranchState> branches_;
    std::vector<std::string> pending_abandons_;
    std::set<std::string> orphaned_from_restart_;   // A7: branch names restored by
                                                        // load_durable_state() with no live handle
                                                        // anywhere in THIS process
    std::uint64_t branch_seq_ = 0;
    Store store_;   // the REAL content-addressed store -- InMemoryWorktreeObjectStore by default,
                      // or a durable conformer like FileWorktreeObjectStore
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> blob_acl_;
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> tree_acl_;
    std::optional<std::filesystem::path> durable_dir_;   // nullopt => pure in-memory branches_/ACL
                                                             // bookkeeping, today's existing behavior
    std::size_t max_acl_roots_per_digest_;   // A8 fix: per-instance, constructor-configurable --
                                                 // see kMaxAclRootsPerDigest's own comment above
};

template <class Store>
inline void BranchHandle<Store>::maybe_queue_abandon() {
    if (owner_ && !resolved_) {
        owner_->queue_pending_abandon(name_);
        resolved_ = true;
    }
}

}  // namespace probe
