#pragma once
// Implements ADR-102 Phase 2 (identity-native sandbox/worktree design, ADR-099 §3/§7) --
// `Ledger<Store>`/`BranchHandle<Store>`/`Checkpoint`: a content-addressed, identity-scoped
// checkpoint/branch/merge system built directly on the real, already-shipped
// `agentengine::WorktreeObjectStore` concept and `agentengine::InMemoryWorktreeObjectStore`
// (core/worktree_types.hpp) -- not a second, parallel object-storage layer. Authorization is
// entirely IdentityHandle/IdentityAuthority-scoped (trust/identity_authority.hpp, ADR-102 Phase 1),
// never the `CapabilitySet`/`Capability` system (trust/capability.hpp) -- deliberately: this is the
// identity-native design's own authority model, kept distinct on purpose (ADR-099's own rationale,
// carried forward unchanged by this port).
//
// Ported from docs/planning/proofs/worktree_io/{worktree_ledger.hpp,merge_trees.hpp} (ADR-099's own
// standalone, red-teamed, live-tested prove-phase originals -- kept as-is, unmodified; these are new
// files, not edits to them). Real changes made during the port, not cosmetic:
//   - `probe::Principal` -> `agentengine::IdentityHandle` throughout (ADR-102 Phase 1's naming
//     decision) -- every `owner`/`requested_by`/`writer`/`caller`/`authored_by`/`created_by`
//     parameter.
//   - `probe::result<T>`/`probe::error{message, code}` -> the real `agentengine::result<T>`/
//     `agentengine::error{failure_class, message, code}` (core/error.hpp) -- every constructed error
//     below picks a real failure_class: `policy` for an authorization refusal, `resource` for the
//     ACL-cap bound, `contract` for a caller-side violation (unknown branch, no such checkpoint, a
//     genuine merge conflict, a case-folding collision), `fatal` for an underlying object-store
//     failure.
//   - `agentengine::rt::task<T>` used fully-qualified throughout (this file lives in bare
//     `agentengine`, not `agentengine::rt`).
//   - Every object-store-failure error code's prefix changed from the prove-phase original's
//     `"worktree_ledger.*"` to `"ledger.*"`, matching this file's own module name (and this whole
//     design's own short-prefix convention, e.g. `"async_quota.*"`) -- a real, disclosed string-level
//     change, named here explicitly after an independent red-team pass found it silently missing from
//     an earlier version of this list. No real consumer pattern-matches the old prefix today.
//   - `merge_trees()`/`LedgerMergeResult`/`LedgerMergeConflict` (merge_trees.hpp) promoted alongside `Ledger`
//     itself, in the same file -- `Ledger::merge()` is its only real caller, matching the prove-phase
//     original's own tight coupling between the two.
//
// REAL FINDING (2026-08-28, surfaced by ADR-102 Phase 5's own cli_chat.cpp wiring -- the first time
// this file was ever compiled into the SAME translation unit as `core/worktree_merge.hpp`): this
// file's own struct names were originally bare `MergeConflict`/`MergeResult`, a real, undetected
// redefinition collision against the ALREADY-SHIPPED, pre-existing `agentengine::MergeConflict`/
// `agentengine::MergeResult` (025 §4's own branch-merge mechanism, `worktree_merge.hpp` -- a
// completely different system: `SubWorktree`/`Ref`/`AppendLogStore`-based, nothing to do with this
// file's `IdentityHandle`/`Ledger` model). `tools/naming_lint.py`'s own vocabulary-registration check
// never caught this -- it verifies a name is DOCUMENTED, not that it doesn't already exist as an
// UNRELATED type elsewhere in the same namespace -- and no build before this one ever `#include`d
// both files in one translation unit, so the redefinition stayed silently latent through Phases 2, 3,
// and 4's own full rebuilds and independent red-team rounds. Renamed to `LedgerMergeConflict`/
// `LedgerMergeResult` -- distinct names for a genuinely distinct concept, matching this whole design's
// own established discipline (`IdentityHandle` vs. `Principal`, `SurfaceRunOutcome` vs. `ExecOutcome`,
// `SandboxRunOutcome` vs. `a2a::RunOutcome`), this time found the hard way, by a real compile error,
// rather than caught in review. The free function `merge_trees()` itself needed NO rename -- its
// 3-`Tree`-argument signature is a legal C++ overload against `worktree_merge.hpp`'s own 4-argument,
// `WorktreeObjectStore`-templated `merge_trees(S&, Digest const&, Digest const&, Digest const&)`,
// confirmed by the same rebuild raising no further error once the two struct names stopped colliding.
//
// SCOPE, matching ADR-102's own Phase 2 boundary: only `Store = agentengine::
// InMemoryWorktreeObjectStore` (the real, already-shipped default) is exercised by this phase's own
// test. A durable conformer (the prove-phase design's own `FileWorktreeObjectStore`) is NOT ported
// here -- Ledger<Store> stays genuinely generic over any real `WorktreeObjectStore` conformer, so a
// future phase can supply a durable one without touching this file, but this phase does not build or
// prove one.

// ADR-207 (#120 S7): the member bodies of Ledger<Store> are in core/ledger_impl.hpp, and src/core/ledger.cpp compiles
// them once for each of the two stores (InMemoryWorktreeObjectStore, FileWorktreeObjectStore). merge_trees() and its
// helpers are in src/core/ledger.cpp too. Declarations and comments stay here.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

// ---------------------------------------------------------------------------------------------
// merge_trees() -- a real three-way merge (ported from worktree_io/merge_trees.hpp), the same
// shape git/most VCS merges use: for every path across base/ours/theirs, only a path BOTH sides
// changed to DIFFERENT values is a real conflict; a path only one side touched takes that side's
// value; a path both sides changed to the IDENTICAL value is not a conflict either. `Ledger::
// merge()` below is its only real caller.
// ---------------------------------------------------------------------------------------------

struct LedgerMergeConflict {
    std::string path;
    std::optional<agentengine::Digest> base_digest;
    agentengine::Digest ours_digest;
    agentengine::Digest theirs_digest;
};

struct LedgerMergeResult {
    agentengine::Tree merged;
    std::vector<LedgerMergeConflict> conflicts;   // non-empty means `merged` is NOT authoritative for the
                                              // conflicting paths -- a real caller must resolve them
                                              // before treating `merged` as the real outcome.
};

namespace ledger_detail {
[[nodiscard]] inline std::unordered_map<std::string, agentengine::TreeEntry> index_by_path(
    agentengine::Tree const& t) {
    std::unordered_map<std::string, agentengine::TreeEntry> out;
    for (auto const& e : t.entries) out.emplace(e.name, e);
    return out;
}

// The search behind `Ledger::check_case_folding_collision()`: the first pair of entries whose names
// are DIFFERENT but case-fold (ASCII `tolower` per byte) to the same string, as indices `{i, j}` with
// `i < j`, or nullopt. Exact duplicates are not a collision here, as they never were.
//
// This used to be a nested loop that re-allocated and re-lowercased the inner name on every one of
// its n(n-1)/2 comparisons. `Ledger::commit()` runs it on every commit, against the flat tree
// `RealIoFileSystem::scan_and_drain_into_tree()` builds for a WHOLE sandbox (one entry per file, full
// relative path as the name), so a 2000-file sandbox cost ~2M string allocations per command. Now
// each name is folded once and the folded names are sorted, so equal folds become adjacent runs:
// O(n log n).
//
// It reports the SAME pair the nested loop did, so the rejection message is unchanged. The nested
// loop returned the smallest `i` that had any later differently-named match, paired with the
// smallest such `j`. Within one run of equal folds (ordered by index), a run contains a collision
// exactly when some name in it differs from its FIRST member's -- and then that first member is the
// run's smallest such `i`, and the first later member with a different name is its `j`. The answer
// is the run whose first member has the smallest index. `test_ledger_case_folding.cpp` checks this
// equivalence against a verbatim copy of the old loop on generated trees.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> find_case_folding_collision(
    agentengine::Tree const& tree);
}  // namespace ledger_detail

[[nodiscard]] LedgerMergeResult merge_trees(agentengine::Tree const& base, agentengine::Tree const& ours,
                                            agentengine::Tree const& theirs);

// ---------------------------------------------------------------------------------------------
// Ledger<Store> itself, ported from worktree_io/worktree_ledger.hpp.
// ---------------------------------------------------------------------------------------------

// Kind tags for AsyncQuota<Kind> (rt/async_quota.hpp) -- BranchCost gates branch_from(), StorageBytes
// gates commit(), MergeCost gates merge() (ADR-111). Defined here, not in async_quota.hpp itself,
// since they are Ledger-specific vocabulary, not part of the generic quota primitive (matching the
// prove-phase original's own split: worktree_ledger.hpp defined its own local BranchCost;
// async_quota.hpp's own StorageBytes moves here too, since Ledger is its only real consumer in this
// phase). MergeCost is its own dedicated tag, not a reuse of BranchCost or StorageBytes: merge()'s
// own cost is a fixed, roughly-constant per-call expense (three tree loads, one `put_tree`, up to two
// ACL mutations, one snapshot persist), unlike StorageBytes' byte-proportional basis, and a distinct
// budget from "how many branches may this identity create" even though both BranchCost and MergeCost
// happen to consume a fixed amount per call -- matching this class's own one-tag-per-verb convention.
struct BranchCost {};
struct StorageBytes {};
struct MergeCost {};

// self_digest computed via the REAL agentengine::compute_digest (SHA-256), over the same
// {tree, parent, authored_by, turn_index} fields.
//
// `authored_by` is a raw `authored_by_id` (uint64_t), not a full IdentityHandle -- nothing anywhere
// in this design ever reads anything from a stored Checkpoint's authored-by field beyond `.id()`,
// and IdentityHandle's own friend-gated construction would make durable serialization impossible
// without inventing a new "reconstitute a handle for an id I already know about" capability on
// IdentityAuthority. Storing the id directly is both simpler and a strictly narrower surface.
struct Checkpoint {
    agentengine::Digest self_digest;
    agentengine::Digest tree;     // a REAL tree digest -- WorktreeObjectStore::put_tree()'s own
                                    // return value, not a hand-rolled string
    agentengine::Digest parent;
    std::uint64_t authored_by_id = 0;
    std::uint64_t turn_index = 0;
};

[[nodiscard]] agentengine::result<agentengine::Digest> compute_self_digest(
        agentengine::Digest const& tree, agentengine::Digest const& parent,
        std::uint64_t authored_by_id, std::uint64_t turn_index);

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
    // Real move-assignment: overwriting a live handle first queues-abandon of whatever it previously
    // held, matching this class's own destructor discipline -- a plain `= delete` here would make any
    // code moving a BranchHandle by assignment silently ill-formed only once actually used.
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
    // The tree digest this branch started FROM -- the common ancestor a real three-way merge needs
    // as `base`. Immutable once set (never touched by this branch's OWN later commit()/reset_to()
    // calls, only by create_root_branch()/branch_from() at creation time).
    agentengine::Digest base_tree_digest;
};

// Ledger holds the REAL object store directly -- every commit()/reset_to() call goes through
// agentengine::WorktreeObjectStore's real put_tree()/get_tree() (dedup, canonical serialization,
// real SHA-256), never a caller-supplied opaque string.
//
// IDENTITY-SCOPED ACCESS CONTROL: every blob/tree digest carries a set of "root" IdentityHandle ids
// authorized to read it -- populated at the moment a handle legitimately WRITES that digest
// (put_blob_safe/commit), checked via the real, already-proven multi-hop IdentityAuthority ancestry
// table on every READ (get_blob_safe/get_tree_safe) and on every COMMIT that references a digest the
// committing handle didn't itself just write.
template <class Store = agentengine::InMemoryWorktreeObjectStore>
class Ledger {
public:
    // The maximum number of DISTINCT root ids one digest's ACL entry may ever hold. Every insertion
    // path below enforces this and fails CLOSED (`ledger.acl_root_cap_exceeded`) rather than growing
    // the set forever or silently evicting an existing, still-legitimate entry.
    static constexpr std::size_t kMaxAclRootsPerDigest = 64;

    // Reserved, never issued to a real IdentityHandle (IdentityAuthority mints starting at 1, and
    // IdentityHandle has no public default constructor at all) -- safe as a sentinel that can never
    // collide with a real id.
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

    // An EXPLICIT, principal-gated ratchet, never automatic and never inferable from model output
    // (I2/I3): `requested_by` must ALREADY be authorized for `digest` (the identical
    // `authorized_for()` check every read uses) before they can mark it shared -- this narrows/
    // decides among authority `requested_by` already possesses, it never mints new authority from
    // nothing. Once marked, `authorized_for()` grants EVERY handle read access to `digest`, and every
    // future `insert_acl_root_bounded()` call for that digest becomes a real no-op. PERMANENT: no
    // `unmark_digest_shared()` -- sharing content is a one-way ratchet.
    //
    // HONEST LIMIT, disclosed not fixed (carried forward from the prove-phase original): `digest`
    // here is a bare `agentengine::Digest` with no structural, non-implicitly-constructible
    // defense-in-depth against a future caller accidentally passing a model-influenced value. Dormant
    // today (zero production callers, and `requested_by` still needs genuine, already-existing
    // authorization regardless of what `digest` names).
    [[nodiscard]] agentengine::result<void> mark_digest_shared(agentengine::Digest digest, bool is_tree,
                                                                   agentengine::IdentityHandle requested_by);

    [[nodiscard]] agentengine::result<agentengine::Digest> put_blob_safe(std::span<std::byte const> bytes,
                                                                             agentengine::IdentityHandle writer) {
        std::lock_guard<std::mutex> g(mutex_);
        auto d = put_blob_locked(bytes, writer);
        if (d.has_value()) persist_snapshot_locked();
        return d;
    }

    // Several blobs written as one drain. `put()` is `put_blob_safe()` minus its snapshot write: the
    // same store write and the same bounded ACL insertion, each under `mutex_`, which is released
    // between blobs exactly as it is between two `put_blob_safe()` calls. What the batch defers is only
    // `persist_snapshot_locked()`, to ONE call when the batch ends (`finish()`, or the destructor on
    // any early return) -- instead of one full rewrite of every branch, checkpoint and ACL per blob,
    // which made an n-file drain write Theta(n^2) bytes to disk.
    //
    // Nothing a reader of the in-memory Ledger sees changes: every ACL root is inserted before `put()`
    // returns. Only the durable snapshot lags, and only until the batch ends; a crash inside that
    // window loses the ACL roots of blobs no commit references yet, so the content they name is
    // unreadable by anyone (`authorized_for()` fails closed on a missing entry) until a later drain
    // writes it again. Any other mutation that persists meanwhile writes the batch's roots too, since
    // the snapshot is always of the whole in-memory state.
    class BlobWriteBatch {
    public:
        BlobWriteBatch(Ledger& ledger, agentengine::IdentityHandle writer)
            : ledger_(&ledger), writer_(std::move(writer)) {}
        BlobWriteBatch(BlobWriteBatch const&) = delete;
        BlobWriteBatch& operator=(BlobWriteBatch const&) = delete;
        ~BlobWriteBatch() { finish(); }

        [[nodiscard]] agentengine::result<agentengine::Digest> put(std::span<std::byte const> bytes) {
            std::lock_guard<std::mutex> g(ledger_->mutex_);
            auto d = ledger_->put_blob_locked(bytes, writer_);
            if (d.has_value()) unpersisted_ = true;
            return d;
        }

        // Writes the snapshot iff a `put()` succeeded since the last write. Idempotent.
        void finish() {
            if (!unpersisted_) return;
            std::lock_guard<std::mutex> g(ledger_->mutex_);
            ledger_->persist_snapshot_locked();
            unpersisted_ = false;
        }

    private:
        Ledger* ledger_;
        agentengine::IdentityHandle writer_;
        bool unpersisted_ = false;
    };

    // How many times the durable snapshot has been written by this Ledger (0 without `durable_dir`).
    // Observability for the drain batching above; not an authority input.
    [[nodiscard]] std::uint64_t snapshot_write_count() const {
        std::lock_guard<std::mutex> g(mutex_);
        return snapshot_writes_;
    }
    [[nodiscard]] agentengine::result<std::vector<std::byte>> get_blob_safe(
            agentengine::Digest const& digest, agentengine::IdentityHandle caller);
    [[nodiscard]] agentengine::result<agentengine::Tree> get_tree_safe(agentengine::Digest const& digest,
                                                                           agentengine::IdentityHandle caller);
    [[nodiscard]] std::size_t blob_count_safe() {
        std::lock_guard<std::mutex> g(mutex_);
        return store_.blob_count();
    }

    // Read-only dry-run for a caller that needs to write SEVERAL blobs as one logical batch: true
    // iff a blob with this exact byte content, written by `writer`, would be accepted. Performs NO
    // mutation. Lets a caller validate the WHOLE batch before writing any of it, instead of
    // discovering an ACL-cap rejection partway through with earlier blobs already durably persisted
    // and unreferenced by any Tree/Checkpoint.
    //
    // It answers with `acl_root_admissible()`, the same rule `insert_acl_root_bounded()` enforces, under
    // this ledger's own configured cap. It once restated that rule by hand and drifted from it twice:
    // it compared against the compile-time default `kMaxAclRootsPerDigest` instead of
    // `max_acl_roots_per_digest_`, and it ignored a digest marked publicly shared. So on a ledger with
    // a lower cap it approved a batch that `put_blob_safe()` then refused partway through -- the very
    // outcome it exists to prevent -- and it refused a scan of a digest `mark_digest_shared()` had
    // already exempted from the cap.
    [[nodiscard]] bool would_accept_blob_write(std::span<std::byte const> bytes,
                                                  agentengine::IdentityHandle writer);

    // `disambiguator` is OPTIONAL, empty by default. Omitting it reproduces the deterministic
    // "root-<owner_id>" name -- load-bearing for real cross-process crash-recovery reattachment (a
    // genuinely separate OS process, with no BranchHandle object to carry across the restart
    // boundary, has to RECOMPUTE the exact same name from only the owner identity it already holds).
    // A caller that genuinely needs several independent root branches for one owner opts in
    // explicitly by supplying one.
    [[nodiscard]] agentengine::rt::task<agentengine::result<BranchHandle<Store>>> create_root_branch(
        agentengine::IdentityHandle owner, std::string disambiguator = {});

    // Commits a REAL agentengine::Tree (already built by the caller's real I/O layer) through the
    // REAL object store. Validates, before accepting the tree at all, that `authored_by` (or an
    // ancestor) is authorized for EVERY entry's digest already in the store -- closing "commit a
    // tree referencing someone else's blob you never had access to."
    [[nodiscard]] agentengine::rt::task<agentengine::result<Checkpoint>> commit(
            BranchHandle<Store> const& branch, agentengine::Tree tree,
            agentengine::IdentityHandle authored_by, agentengine::rt::AsyncQuota<StorageBytes>& quota);

    // DISCLOSED, NOT A GAP THIS PORT INTRODUCED OR SHOULD SILENTLY FIX: unlike every other mutating
    // method on this class, `reset_to()` performs NO `authorized_for()` check of its own against
    // `target_turn_index`'s own tree -- possession of the real `BranchHandle` (a caller cannot call
    // this at all without one) is the entire authorization boundary here, the same discipline
    // `abandon()`/`discard()`-shaped methods already rely on elsewhere in this design. Already named
    // explicitly at the design level (`decisions/ADR-099-identity-native-sandbox-worktree-capability-
    // model.md` §7/§8, the `SandboxRuntime::reset_to_turn()` composition's own comment) -- brought
    // inline here, matching this class's own `mark_digest_shared()` disclosure-in-code convention,
    // after an independent red-team pass on this port asked why the two residuals weren't disclosed
    // the same way. A real, live consequence worth naming plainly: a caller CAN reset a branch to a
    // checkpoint whose tree they are not currently authorized to READ (e.g. after `merge()`'s own
    // authorization boundary moved), locking themselves out of their own branch's new head via their
    // own call -- self-inflicted, not exploitable against a different principal, and not fixed here.
    [[nodiscard]] agentengine::rt::task<agentengine::result<Checkpoint>> reset_to(
            BranchHandle<Store> const& branch, std::uint64_t target_turn_index,
            agentengine::IdentityHandle requested_by);

    // COW branching: the child starts as a copy of the parent's current head (same tree/self digest,
    // same turn index) under a fresh branch name; no new content is copied since content is
    // addressed by digest, not by branch. The child's creator is granted ACL access to the parent's
    // current head tree digest so its first read/commit against inherited content succeeds even
    // before it has written anything of its own.
    [[nodiscard]] agentengine::rt::task<agentengine::result<BranchHandle<Store>>> branch_from(
        BranchHandle<Store> const& parent, agentengine::IdentityHandle created_by,
        agentengine::rt::AsyncQuota<BranchCost>& quota);

    // Real three-way merge, wired to the real `merge_trees()` above. `base` is the child's own
    // `base_tree_digest`; `ours` is the PARENT's CURRENT tree; `theirs` is the CHILD's CURRENT tree.
    // A real conflict FAILS the merge closed (`ledger.merge_conflict`) rather than silently picking a
    // side. On a clean merge, every entry in the merged tree is validated against the SAME
    // per-entry authorization commit() already requires.
    //
    // Requires possession of the PARENT's own BranchHandle -- a bare, guessable branch name is never
    // sufficient (branch names follow the deterministic "root-<owner_id>"/"<parent>/child-<id>-<seq>"
    // scheme), matching every other mutating Ledger method's own possession requirement.
    //
    // A rejected merge registers `child` into `orphaned_from_restart_` on every rejection path
    // (rather than leaving it a dead end with no live handle anywhere and not registered as an
    // orphan) -- the caller gets a real, working reclaim path via `reclaim_orphaned_branch()`.
    //
    // SINCE FIXED (ADR-111): this method is now `AsyncQuota<MergeCost>`-gated. `MergeCost` is a
    // dedicated Kind tag (matching this class's own one-tag-per-verb convention: `BranchCost` for
    // `branch_from()`, `StorageBytes` for `commit()`), not a reuse of either -- merge()'s own cost
    // shape (a fixed, roughly-constant per-call expense: three tree loads, one `put_tree`, up to two
    // ACL mutations, one snapshot persist) is neither proportional to caller-supplied bytes
    // (`StorageBytes`' own basis) nor the same budget as "how many branches may this identity
    // create" (`BranchCost`'s own basis). Closes an I8 gap of the same shape ADR-099 §42 already
    // found and fixed one level over for `SandboxRuntime::reset_to_turn()` (`AsyncQuota<ResetCost>`,
    // the same "call it in a tight loop for free" resource-exhaustion vector). Threaded through the
    // same restructure `commit()`/`branch_from()` already established: the quota is consumed BEFORE
    // `mutex_` is ever taken, the outcome is computed in a non-coroutine lambda so the lock is never
    // held across a `co_await`, and every one of this method's eight distinct failure paths refunds
    // exactly what was consumed.
    [[nodiscard]] agentengine::rt::task<agentengine::result<Checkpoint>> merge(
            BranchHandle<Store> child, BranchHandle<Store> const& parent,
            agentengine::IdentityHandle requested_by, agentengine::rt::AsyncQuota<MergeCost>& quota);

    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> abandon(BranchHandle<Store> child);

    [[nodiscard]] agentengine::rt::task<std::size_t> reap_pending_abandons();

    // Gated the same way get_tree_safe() gates the tree it names -- branch names are deterministically
    // guessable (root-<owner_id>, <parent>/child-<id>-<seq>), so knowing a branch's name must not be
    // enough to read its current head digest.
    [[nodiscard]] agentengine::result<agentengine::Digest> head_tree_digest(
            std::string const& branch_name, agentengine::IdentityHandle caller) const;

    // Read-only checkpoint-history introspection -- deliberately NOT a "resolve a branch by name into
    // a mutation-capable handle" capability, which would reopen the object-possession security
    // property this Ledger's real public API deliberately preserves. Gated on the SPECIFIC
    // checkpoint's own tree digest (not the branch's current head), so access to one historical
    // checkpoint doesn't require -- or imply -- access to whatever the branch's head has since become.
    [[nodiscard]] agentengine::result<Checkpoint> checkpoint_at(std::string const& branch_name,
                                                                    std::uint64_t turn_index,
                                                                    agentengine::IdentityHandle caller) const;

    // Every branch name restored by load_durable_state() at construction time -- i.e. every branch
    // with genuinely NO live handle anywhere in this process, because the process that held its one
    // legitimate handle is the one that just exited (cleanly or via a crash; this Ledger cannot tell
    // the difference, and does not need to). A host inspects this list and explicitly decides:
    // reclaim or abandon -- never automatic.
    [[nodiscard]] std::vector<std::string> orphaned_branches() const {
        std::lock_guard<std::mutex> g(mutex_);
        return std::vector<std::string>(orphaned_from_restart_.begin(), orphaned_from_restart_.end());
    }

    // Mints a genuinely fresh, legitimate BranchHandle for a branch this Ledger's own restart logic
    // identified as orphaned -- NOT a general "resolve any branch by name" bypass: fails closed if
    // the name was never actually in orphaned_from_restart_, and fails closed if `requested_by` is
    // not this branch's own true owner (its recorded `created_by_id`, or an ancestor of it).
    //
    // MUST-FIX (independent red-team, 2026-08-30, found while reviewing ADR-128's reliance on this
    // method): the ORIGINAL check here was `authorized_for(tree_acl_, head_tree_digest, requested_by)`
    // -- i.e. "is `requested_by` in the ACL set for this branch's CURRENT TREE CONTENT's digest",
    // not "is `requested_by` this branch's actual owner". Those are NOT the same question:
    // `create_root_branch()`'s freshly-minted, never-committed root branch has its head tree at the
    // digest of an EMPTY `Tree{}` -- CONTENT-ADDRESSED, and therefore IDENTICAL across every owner,
    // since an empty tree carries no owner-specific content at all. `insert_acl_root_bounded()` adds
    // EVERY owner who has EVER created ANY fresh root/child branch into `tree_acl_[empty_tree_
    // digest]` -- so ANY owner B who has created their own, entirely unrelated branch was ALREADY
    // "authorized_for" that same shared digest, and could reclaim owner A's still-orphaned, never-
    // committed root branch using only B's own, legitimately-held identity. Empirically confirmed
    // with a real probe (owner B, having created only their own root branch, successfully reclaimed
    // owner A's orphaned "root-<A>" via a direct `reclaim_orphaned_branch(root_a_name, B)` call) --
    // this is a real I2 violation (B obtains a live, controlling handle to A's branch with no
    // authority A ever granted B), not a hypothetical. Root cause: digest-ACL membership answers "can
    // this principal read/write THIS CONTENT", which is the wrong question for "is this principal
    // this BRANCH's rightful owner" whenever two branches can independently arrive at identical
    // content (trivially true for any two never-yet-committed branches, and possible more generally
    // for any content collision). Fixed by checking `BranchState::created_by_id` (the field
    // `reap_pending_abandons()` already treats as this branch's authoritative owner for its own,
    // internal, unauthenticated cleanup path) directly, with the same ancestor-of-owner allowance
    // `authorized_for()` already extends elsewhere in this class -- this is authorization by real
    // branch identity, never by incidental content overlap. `created_by_id` round-trips through
    // `persist_snapshot_locked()`/`load_durable_state()` unchanged, so this fix costs nothing across
    // a genuine crash-recovery reconstruction either.
    [[nodiscard]] agentengine::result<BranchHandle<Store>> reclaim_orphaned_branch(
            std::string const& branch_name, agentengine::IdentityHandle requested_by);

    // The explicit "discard, don't reclaim" decision -- same orphan-only and true-ownership gating as
    // reclaim_orphaned_branch() (see that method's own MUST-FIX comment: this used to share its now-
    // fixed digest-ACL-based check, and the same fix applies here for the identical reason).
    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> abandon_orphaned_branch(
            std::string const& branch_name, agentengine::IdentityHandle requested_by);

private:
    friend class BranchHandle<Store>;
    void queue_pending_abandon(std::string const& name) {
        std::lock_guard<std::mutex> g(mutex_);
        pending_abandons_.push_back(name);
    }

    // The store write and bounded ACL insertion shared by `put_blob_safe()` and `BlobWriteBatch::put()`.
    // Must be called with mutex_ already held. Does not persist.
    [[nodiscard]] agentengine::result<agentengine::Digest> put_blob_locked(std::span<std::byte const> bytes,
                                                                               agentengine::IdentityHandle const& writer);

    // True iff `requested_by` is `state`'s own recorded creator, or an ancestor of that creator (the
    // same ancestor-inclusion `authorized_for()` already extends for content-digest ACLs) -- i.e.
    // real branch ownership, never incidental content-digest overlap. See `reclaim_orphaned_branch()`'s
    // own MUST-FIX comment for why this is a distinct question from `authorized_for()` and must not be
    // answered by it. Must be called with mutex_ already held.
    [[nodiscard]] static bool is_branch_owner(BranchState const& state,
                                                 agentengine::IdentityHandle const& requested_by) {
        return requested_by.id() == state.created_by_id ||
               agentengine::IdentityAuthority::bootstrap().is_ancestor_of(state.created_by_id,
                                                                             requested_by.id());
    }

    // True iff `caller` (or an ancestor of `caller`, via the real IdentityAuthority ancestry table)
    // is in `acl[digest]`'s recorded set of ids -- OR the digest has been explicitly marked publicly
    // shared. Must be called with mutex_ already held.
    [[nodiscard]] static bool authorized_for(
        std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> const& acl,
        agentengine::Digest const& digest, agentengine::IdentityHandle const& caller);

    // The admission rule itself, shared with `would_accept_blob_write()`'s dry run so the two cannot
    // disagree: an already-recorded root re-touches for free, a publicly shared digest is exempt, and
    // otherwise a new root needs room under `cap`.
    [[nodiscard]] static bool acl_root_admissible(std::set<std::uint64_t> const& roots, std::uint64_t root_id,
                                                  std::size_t cap) {
        return roots.contains(root_id) || roots.contains(kPubliclySharedSentinelRootId) || roots.size() < cap;
    }

    // Bounded ACL insertion. Must be called with mutex_ already held. A root id already present is a
    // no-op success; only a genuinely NEW distinct root id past the cap fails, and it fails CLOSED
    // (the whole calling operation is rejected) rather than silently dropping the id.
    [[nodiscard]] static agentengine::result<void> insert_acl_root_bounded(
        std::unordered_map<agentengine::Digest, std::set<std::uint64_t>>& acl,
        agentengine::Digest const& digest, std::uint64_t root_id, std::size_t cap);

    // A case-folding collision check (git's own real CVE-2014-9390 fix direction): two tree
    // entries whose NAMES case-fold to the same real path (e.g. "readme.txt" and "README.txt")
    // are two perfectly legal, genuinely distinct digests as far as the content-addressed store
    // is concerned, but silently collide on a case-insensitive filesystem (Windows NTFS/FAT,
    // default macOS HFS+) on materialize. Rejected outright rather than silently dropping one.
    // HONEST RESIDUAL: this checks ASCII case-folding only (`tolower` per byte) -- not Unicode
    // "ignorable" codepoints, a materially harder problem this check does not attempt.
    // The search itself is `ledger_detail::find_case_folding_collision()`, above.
    [[nodiscard]] static agentengine::result<void> check_case_folding_collision(agentengine::Tree const& tree);

    // Must be called with mutex_ already held. Marks `child` as a reclaimable orphan iff it still
    // exists in branches_ (a stale/already-consumed handle may not), and marks the handle resolved --
    // the single shared shape every merge() rejection path below needs: register the child so
    // `reclaim_orphaned_branch()` remains a real recovery path, and prevent
    // `BranchHandle::~BranchHandle()`'s `maybe_queue_abandon()` from later queuing a real, destructive
    // abandon for a handle that already resolved here.
    void orphan_child_locked(BranchHandle<Store>& child) {
        if (branches_.find(child.name()) != branches_.end()) {
            orphaned_from_restart_.insert(child.name());
        }
        child.resolved_ = true;
    }

    // merge() step 1/4. Must be called with mutex_ already held. Validates both branches exist and
    // `requested_by` is authorized for the child's head tree digest; returns the three digests
    // `perform_three_way_merge_locked()` needs plus a stable pointer to the parent's own BranchState
    // (unordered_map references/pointers to existing elements survive later insertions into OTHER
    // buckets/maps within the same locked critical section, per the container's own guarantee).
    struct MergeParticipants {
        BranchState* parent_state = nullptr;
        agentengine::Digest theirs_digest;
        agentengine::Digest ours_digest;
        agentengine::Digest base_digest;
    };
    [[nodiscard]] agentengine::result<MergeParticipants> load_merge_participants_locked(
            BranchHandle<Store>& child, BranchHandle<Store> const& parent,
            agentengine::IdentityHandle requested_by);

    // merge() step 2/4. Must be called with mutex_ already held. Loads base/ours/theirs from the
    // object store and runs the real three-way `merge_trees()` above; a real conflict fails closed
    // (`ledger.merge_conflict`) rather than silently picking a side.
    [[nodiscard]] agentengine::result<LedgerMergeResult> perform_three_way_merge_locked(
            agentengine::Digest const& base_digest, agentengine::Digest const& ours_digest,
            agentengine::Digest const& theirs_digest, BranchHandle<Store>& child);

    // merge() step 3/4. Must be called with mutex_ already held. Validates every merged entry is
    // authorized for `requested_by`, writes the merged tree, and grants `requested_by` the tree-level
    // ACL root -- the same per-entry authorization commit() itself already requires.
    struct CommittedMergeTree {
        agentengine::Digest merged_tree_digest;
        // ADR-112: a snapshot of the merged entries, taken ONLY when the parent owner grant in
        // `grant_parent_owner_access_locked()` below will actually run, since `merged.merged` is
        // moved-from immediately after this point -- needed for the per-entry content-level grants
        // that close this method's own long-disclosed "structural, not content-wise" scope limit.
        // Cheap relative to everything else this step already does per call (a handful of small
        // {name, digest, bool} structs, not blob/tree content itself). Empty when the parent already
        // owns the branch (`parent_state.created_by_id == requested_by.id()`).
        std::vector<TreeEntry> entries_for_owner_grant;
    };
    [[nodiscard]] agentengine::result<CommittedMergeTree> commit_merged_tree_locked(
            LedgerMergeResult merged, agentengine::IdentityHandle requested_by,
            BranchState const& parent_state, BranchHandle<Store>& child);

    // merge() step 4/4. Must be called with mutex_ already held.
    //
    // REAL FINDING an independent red-team pass caught (2026-08-28, same day as the port),
    // empirically confirmed with a live probe, not just reasoned about: `authorized_for()`'s
    // ancestry check flows DOWNWARD only (a descendant inherits what an ancestor wrote, never the
    // reverse) -- so without this second grant, `parent_state`'s own creator (the branch's OWN
    // OWNER, who created `parent` and still holds its BranchHandle) would be permanently denied
    // read access to their OWN branch's new head the moment ANY authorized descendant merges into
    // it, with no narrow recovery path (only `mark_digest_shared()`'s global "readable by
    // literally anyone" escape hatch). This directly broke the exact "an orchestrator spawns a
    // sub-agent, the sub-agent merges its work back, the orchestrator resumes" flow this whole
    // design exists to support -- confirmed BROKEN for real (a live probe reproduced
    // `ledger.tree_access_denied` on the parent's own creator immediately after a successful
    // merge) before this fix. A comment on an earlier version of this file's own test claimed
    // this "matches" the real task-branch tool track's (A10) own merge flow -- independently
    // re-checked against `docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp` and
    // found FALSE: A10 uses exactly ONE identity throughout (`spawn_child_branch(owner_,...)`,
    // `merge_into(*main_, owner_)`), never a real `derive_child()`-distinct sub-identity, so A10
    // never actually exercises (or is protected from) this gap at all -- it sidesteps the
    // question entirely rather than answering it. FIXED here, not merely disclosed: the merged
    // tree's ACL also grants `parent_state.created_by_id` (the parent branch's own owner) direct
    // root access, alongside `requested_by` (the actual merger) -- neither widens authority to a
    // NEW principal that didn't already have a legitimate relationship to this branch; it
    // preserves an already-legitimate owner's own continued access to their own resource, the
    // same category of grant `branch_from()` itself already makes to a new child's creator.
    [[nodiscard]] agentengine::result<void> grant_parent_owner_access_locked(
            agentengine::Digest const& merged_tree_digest,
            std::vector<TreeEntry> const& merged_entries_for_owner_grant, BranchState const& parent_state,
            agentengine::IdentityHandle requested_by, BranchHandle<Store>& child);

    // Durable branches_/ACL persistence, atop whatever durability `Store` itself already provides for
    // blob/tree CONTENT. A full-snapshot rewrite (temp file + atomic rename) on every mutation, not
    // an append-only event log. A no-op whenever `durable_dir_` is unset, so every in-memory-only
    // call site's behavior is completely unaffected.
    void persist_snapshot_locked() const;

    void load_durable_state();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BranchState> branches_;
    std::vector<std::string> pending_abandons_;
    std::set<std::string> orphaned_from_restart_;   // branch names restored by load_durable_state()
                                                        // with no live handle anywhere in THIS process
    std::uint64_t branch_seq_ = 0;
    mutable std::uint64_t snapshot_writes_ = 0;   // see snapshot_write_count()
    Store store_;   // the REAL content-addressed store -- InMemoryWorktreeObjectStore by default
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> blob_acl_;
    std::unordered_map<agentengine::Digest, std::set<std::uint64_t>> tree_acl_;
    std::optional<std::filesystem::path> durable_dir_;   // nullopt => pure in-memory branches_/ACL
                                                             // bookkeeping
    std::size_t max_acl_roots_per_digest_;
};

template <class Store>
inline void BranchHandle<Store>::maybe_queue_abandon() {
    if (owner_ && !resolved_) {
        owner_->queue_pending_abandon(name_);
        resolved_ = true;
    }
}

}  // namespace agentengine
