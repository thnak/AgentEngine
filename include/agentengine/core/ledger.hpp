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
}  // namespace ledger_detail

[[nodiscard]] inline LedgerMergeResult merge_trees(agentengine::Tree const& base, agentengine::Tree const& ours,
                                                agentengine::Tree const& theirs) {
    auto base_idx = ledger_detail::index_by_path(base);
    auto ours_idx = ledger_detail::index_by_path(ours);
    auto theirs_idx = ledger_detail::index_by_path(theirs);

    std::vector<std::string> all_paths;
    for (auto const& [k, v] : base_idx) all_paths.push_back(k);
    for (auto const& [k, v] : ours_idx) all_paths.push_back(k);
    for (auto const& [k, v] : theirs_idx) all_paths.push_back(k);
    std::sort(all_paths.begin(), all_paths.end());
    all_paths.erase(std::unique(all_paths.begin(), all_paths.end()), all_paths.end());

    LedgerMergeResult result;
    for (auto const& path : all_paths) {
        auto b = base_idx.find(path);
        auto o = ours_idx.find(path);
        auto t = theirs_idx.find(path);
        bool has_b = b != base_idx.end(), has_o = o != ours_idx.end(), has_t = t != theirs_idx.end();

        // Both sides agree (including "both deleted it", or "both added it identically" when base
        // never had this path at all) -- take that (possibly absent) value.
        bool const o_eq_t = (has_o == has_t) && (!has_o || o->second.digest == t->second.digest);
        if (o_eq_t) {
            if (has_o) result.merged.entries.push_back(o->second);
            continue;
        }

        // "Unchanged relative to base" must account for PRESENCE, not just digest equality when
        // present -- a path absent from base that one side newly ADDS is "changed" even though there
        // is no base entry to compare a digest against.
        bool const ours_matches_base =
            (has_o == has_b) && (!has_o || (has_b && o->second.digest == b->second.digest));
        bool const theirs_matches_base =
            (has_t == has_b) && (!has_t || (has_b && t->second.digest == b->second.digest));

        if (ours_matches_base && !theirs_matches_base) {
            // Ours left this path exactly as base had it (or never had it) -- theirs is the side
            // that actually changed something -- take theirs.
            if (has_t) result.merged.entries.push_back(t->second);
            continue;
        }
        if (theirs_matches_base && !ours_matches_base) {
            // Symmetric: theirs matches base, ours is the side that changed something -- take ours.
            if (has_o) result.merged.entries.push_back(o->second);
            continue;
        }

        // Both sides changed this path, to DIFFERENT values, and neither matches base -- a REAL
        // conflict. Recorded, not silently resolved by picking a side.
        result.conflicts.push_back(LedgerMergeConflict{
            path, has_b ? std::optional<agentengine::Digest>(b->second.digest) : std::nullopt,
            has_o ? o->second.digest : agentengine::Digest{"<deleted>"},
            has_t ? t->second.digest : agentengine::Digest{"<deleted>"}});
        // A real caller decides resolution; this function still includes OURS in `merged` as a
        // working default so `merged` remains a structurally valid Tree even with conflicts present
        // -- callers MUST check `conflicts.empty()` before trusting `merged` as final.
        if (has_o) result.merged.entries.push_back(o->second);
    }

    std::ranges::sort(result.merged.entries, {}, &agentengine::TreeEntry::name);
    return result;
}

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

[[nodiscard]] inline agentengine::result<agentengine::Digest> compute_self_digest(
        agentengine::Digest const& tree, agentengine::Digest const& parent,
        std::uint64_t authored_by_id, std::uint64_t turn_index) {
    std::ostringstream in;
    in << tree << '|' << parent << '|' << authored_by_id << '|' << turn_index;
    std::string const s = in.str();
    std::vector<std::byte> bytes(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) bytes[i] = static_cast<std::byte>(s[i]);
    auto digest = agentengine::compute_digest(bytes);
    if (!digest) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, digest.error().message,
                                                     "ledger.digest_failed"});
    }
    return *digest;
}

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

    [[nodiscard]] agentengine::result<agentengine::Digest> put_blob_safe(std::span<std::byte const> bytes,
                                                                             agentengine::IdentityHandle writer) {
        std::lock_guard<std::mutex> g(mutex_);
        auto d = store_.put_blob(bytes);
        if (!d) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal, d.error().message,
                                                          "ledger.put_blob_failed"});
        }
        auto acl_ok = insert_acl_root_bounded(blob_acl_, *d, writer.id(), max_acl_roots_per_digest_);
        if (!acl_ok.has_value()) return std::unexpected(acl_ok.error());
        persist_snapshot_locked();
        return *d;
    }
    [[nodiscard]] agentengine::result<std::vector<std::byte>> get_blob_safe(
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
    [[nodiscard]] agentengine::result<agentengine::Tree> get_tree_safe(agentengine::Digest const& digest,
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
    [[nodiscard]] std::size_t blob_count_safe() {
        std::lock_guard<std::mutex> g(mutex_);
        return store_.blob_count();
    }

    // Read-only dry-run for a caller that needs to write SEVERAL blobs as one logical batch: true
    // iff a blob with this exact byte content, written by `writer`, would be accepted. Performs NO
    // mutation. Lets a caller validate the WHOLE batch before writing any of it, instead of
    // discovering an ACL-cap rejection partway through with earlier blobs already durably persisted
    // and unreferenced by any Tree/Checkpoint.
    [[nodiscard]] bool would_accept_blob_write(std::span<std::byte const> bytes,
                                                  agentengine::IdentityHandle writer) {
        auto digest = agentengine::compute_digest(bytes);
        if (!digest) return false;
        std::lock_guard<std::mutex> g(mutex_);
        auto it = blob_acl_.find(*digest);
        if (it == blob_acl_.end()) return true;         // brand-new digest, nothing to exceed yet
        if (it->second.contains(writer.id())) return true;  // no-op re-touch, always allowed
        return it->second.size() < kMaxAclRootsPerDigest;
    }

    // `disambiguator` is OPTIONAL, empty by default. Omitting it reproduces the deterministic
    // "root-<owner_id>" name -- load-bearing for real cross-process crash-recovery reattachment (a
    // genuinely separate OS process, with no BranchHandle object to carry across the restart
    // boundary, has to RECOMPUTE the exact same name from only the owner identity it already holds).
    // A caller that genuinely needs several independent root branches for one owner opts in
    // explicitly by supplying one.
    [[nodiscard]] agentengine::rt::task<agentengine::result<BranchHandle<Store>>> create_root_branch(
        agentengine::IdentityHandle owner, std::string disambiguator = {}) {
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

    // Commits a REAL agentengine::Tree (already built by the caller's real I/O layer) through the
    // REAL object store. Validates, before accepting the tree at all, that `authored_by` (or an
    // ancestor) is authorized for EVERY entry's digest already in the store -- closing "commit a
    // tree referencing someone else's blob you never had access to."
    [[nodiscard]] agentengine::rt::task<agentengine::result<Checkpoint>> commit(
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

    // COW branching: the child starts as a copy of the parent's current head (same tree/self digest,
    // same turn index) under a fresh branch name; no new content is copied since content is
    // addressed by digest, not by branch. The child's creator is granted ACL access to the parent's
    // current head tree digest so its first read/commit against inherited content succeeds even
    // before it has written anything of its own.
    [[nodiscard]] agentengine::rt::task<agentengine::result<BranchHandle<Store>>> branch_from(
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

    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> abandon(BranchHandle<Store> child) {
        std::lock_guard<std::mutex> g(mutex_);
        branches_.erase(child.name());
        child.resolved_ = true;
        persist_snapshot_locked();
        co_return agentengine::result<void>{};
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

    // Gated the same way get_tree_safe() gates the tree it names -- branch names are deterministically
    // guessable (root-<owner_id>, <parent>/child-<id>-<seq>), so knowing a branch's name must not be
    // enough to read its current head digest.
    [[nodiscard]] agentengine::result<agentengine::Digest> head_tree_digest(
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

    // Read-only checkpoint-history introspection -- deliberately NOT a "resolve a branch by name into
    // a mutation-capable handle" capability, which would reopen the object-possession security
    // property this Ledger's real public API deliberately preserves. Gated on the SPECIFIC
    // checkpoint's own tree digest (not the branch's current head), so access to one historical
    // checkpoint doesn't require -- or imply -- access to whatever the branch's head has since become.
    [[nodiscard]] agentengine::result<Checkpoint> checkpoint_at(std::string const& branch_name,
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
    // not authorized for the branch's own current head tree.
    [[nodiscard]] agentengine::result<BranchHandle<Store>> reclaim_orphaned_branch(
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
        if (!authorized_for(tree_acl_, it->second.head_tree_digest, requested_by)) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "requester is not authorized for this orphaned branch's current head tree",
                "ledger.reclaim_unauthorized"});
        }
        orphaned_from_restart_.erase(branch_name);
        return BranchHandle<Store>(this, branch_name, requested_by.id(), it->second.base_tree_digest);
    }

    // The explicit "discard, don't reclaim" decision -- same orphan-only and ACL gating as
    // reclaim_orphaned_branch(), but erases the branch instead of handing back a live handle for it.
    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> abandon_orphaned_branch(
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
            if (!authorized_for(tree_acl_, it->second.head_tree_digest, requested_by)) {
                co_return std::unexpected(agentengine::error{
                    agentengine::failure_class::policy,
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
    // is in `acl[digest]`'s recorded set of ids -- OR the digest has been explicitly marked publicly
    // shared. Must be called with mutex_ already held.
    [[nodiscard]] static bool authorized_for(
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

    // Bounded ACL insertion. Must be called with mutex_ already held. A root id already present is a
    // no-op success; only a genuinely NEW distinct root id past the cap fails, and it fails CLOSED
    // (the whole calling operation is rejected) rather than silently dropping the id.
    [[nodiscard]] static agentengine::result<void> insert_acl_root_bounded(
        std::unordered_map<agentengine::Digest, std::set<std::uint64_t>>& acl,
        agentengine::Digest const& digest, std::uint64_t root_id, std::size_t cap) {
        auto& set = acl[digest];
        if (set.contains(root_id)) return agentengine::result<void>{};
        if (set.contains(kPubliclySharedSentinelRootId)) return agentengine::result<void>{};
        if (set.size() >= cap) {
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

    // A case-folding collision check (git's own real CVE-2014-9390 fix direction): two tree
    // entries whose NAMES case-fold to the same real path (e.g. "readme.txt" and "README.txt")
    // are two perfectly legal, genuinely distinct digests as far as the content-addressed store
    // is concerned, but silently collide on a case-insensitive filesystem (Windows NTFS/FAT,
    // default macOS HFS+) on materialize. Rejected outright rather than silently dropping one.
    // HONEST RESIDUAL: this checks ASCII case-folding only (`tolower` per byte) -- not Unicode
    // "ignorable" codepoints, a materially harder problem this check does not attempt.
    [[nodiscard]] static agentengine::result<void> check_case_folding_collision(agentengine::Tree const& tree) {
        for (std::size_t i = 0; i < tree.entries.size(); ++i) {
            std::string folded_i = tree.entries[i].name;
            for (char& c : folded_i) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (std::size_t j = i + 1; j < tree.entries.size(); ++j) {
                std::string folded_j = tree.entries[j].name;
                for (char& c : folded_j) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (folded_i == folded_j && tree.entries[i].name != tree.entries[j].name) {
                    return std::unexpected(agentengine::error{
                        agentengine::failure_class::contract,
                        "tree contains two entries that case-fold to the same real path ('" +
                            tree.entries[i].name + "' and '" + tree.entries[j].name +
                            "') -- rejected before materialize() could silently drop one of them on a "
                            "case-insensitive filesystem, matching git's own real CVE-2014-9390 fix "
                            "direction",
                        "ledger.case_folding_collision"});
                }
            }
        }
        return {};
    }

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

    // merge() step 2/4. Must be called with mutex_ already held. Loads base/ours/theirs from the
    // object store and runs the real three-way `merge_trees()` above; a real conflict fails closed
    // (`ledger.merge_conflict`) rather than silently picking a side.
    [[nodiscard]] agentengine::result<LedgerMergeResult> perform_three_way_merge_locked(
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

    // Durable branches_/ACL persistence, atop whatever durability `Store` itself already provides for
    // blob/tree CONTENT. A full-snapshot rewrite (temp file + atomic rename) on every mutation, not
    // an append-only event log. A no-op whenever `durable_dir_` is unset, so every in-memory-only
    // call site's behavior is completely unaffected.
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
        // A rename failure here is intentionally not escalated -- durability is a best-effort
        // addition on top of an already-successful in-memory mutation.
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

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BranchState> branches_;
    std::vector<std::string> pending_abandons_;
    std::set<std::string> orphaned_from_restart_;   // branch names restored by load_durable_state()
                                                        // with no live handle anywhere in THIS process
    std::uint64_t branch_seq_ = 0;
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
