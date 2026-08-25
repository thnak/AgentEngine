#pragma once
// Mounts (025 §5) -- Phase C1. "A worktree subtree becomes visible to a sandbox only through a
// capability": `Mount` is the host-side declaration of WHICH worktree location a guest-visible
// mount_id names (never guest-supplied, I2 -- a host policy value, the same posture every other
// `cap::*` payload in trust/capability.hpp already has), and `mount_read`/`mount_write` are the only
// way a guest-relative path is ever turned into an actual `Blob`/`Tree` lookup, gated by an already-
// bound `cap::FsRead`/`cap::FsWrite` (trust/capability.hpp, existing since ADR-009 -- this phase
// consumes that machinery, it does not invent a second capability shape).
//
// **This is NOT yet 025 §5's OS-level path-escape corpus** (Phase C2, ADR-track per the milestone-3
// breakdown's decision 6, `decisions/ADR-0NN-worktree-mount-path-canonicalization.md`). There is no
// real filesystem here: a `Tree` is a plain `name -> digest` map with no parent pointers, no
// symlinks, no junctions, no ADS, nothing an OS resolves -- so `..` has no "walk up" to perform, and
// `split_mount_path` below rejects it outright as malformed input rather than defending against it
// as an attack via canonicalize-then-check (the fragile pattern C2's whole corpus exists BECAUSE
// canonicalize-then-check is where real path-escape bugs hide). C2 hardens the DIFFERENT, later
// mechanism that materializes a mount onto a real OS filesystem for a sandboxed process to see --
// that mechanism doesn't exist yet, and this header does not get ahead of it.

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree_merge.hpp"
#include "agentengine/core/worktree_ref_store.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// Host-side binding from a guest-visible `mount_id` (matched against `cap::FsRead::mount_id` /
// `cap::FsWrite::mount_id` exactly) to a concrete worktree location: `ref_name` names the Ref this
// mount reads/writes through, `subtree_path` (slash-joined, "" = the ref's own root) is where within
// that ref's tree this specific mount is rooted -- e.g. a "/work" mount and an "/input" mount might
// point at the SAME ref with different `subtree_path`s, or at entirely different refs. Never derived
// from a guest-supplied path (I2); constructed only by host policy, the same way a `Capability` is
// only ever minted by `CapabilitySet::grant_root`.
// ae-naming-lint: allow Mount — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct Mount {
    std::string mount_id;
    std::string ref_name;
    std::string subtree_path;
};

// Splits a slash-joined relative path into segments, rejecting `""`-as-a-segment (a leading `/`, a
// trailing `/`, or `//`), and `.`/`..` (meaningless in a content-addressed tree -- see the section
// comment above). `path == ""` is a legal INPUT meaning "the mount's own root" and returns an empty
// segment list; callers that cannot accept "the root itself" (both `mount_read` and `mount_write`
// below, since neither reads/writes raw tree-of-trees content) reject an empty combined segment list
// themselves, one level up, where the more specific `worktree.mount_path_is_root` code applies.
[[nodiscard]] inline result<std::vector<std::string>> split_mount_path(std::string const& path) {
    if (path.empty()) return std::vector<std::string>{};
    if (path.front() == '/') {
        return std::unexpected(error{failure_class::contract, "mount path must be relative, not start with '/'",
                                      "worktree.mount_path_absolute"});
    }
    std::vector<std::string> segments;
    std::string              current;
    for (char c : path) {
        if (c != '/') {
            current.push_back(c);
            continue;
        }
        if (current.empty()) {
            return std::unexpected(error{failure_class::contract,
                                          "mount path contains an empty segment (e.g. a double slash)",
                                          "worktree.mount_path_malformed"});
        }
        segments.push_back(std::exchange(current, std::string{}));
    }
    if (current.empty()) {
        return std::unexpected(error{failure_class::contract, "mount path must not end with '/'",
                                      "worktree.mount_path_malformed"});
    }
    segments.push_back(current);
    for (auto const& seg : segments) {
        if (seg == "." || seg == "..") {
            return std::unexpected(error{failure_class::contract,
                                          "'.'/'..' are not meaningful in a content-addressed tree path",
                                          "worktree.mount_path_malformed"});
        }
    }
    return segments;
}

namespace detail {

// A Tree with zero entries, guaranteed to actually EXIST in `store` (unlike bare
// `empty_tree_digest()`, which only computes what the digest would be) -- needed here because
// `set_entry_at_path` below may need to recurse INTO a freshly-created intermediate directory that
// nothing has ever `put_tree`'d before. Idempotent: `put_tree` already dedups, so calling this
// repeatedly across many writes never grows the store past one empty-tree entry.
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> ensure_empty_tree(S& store) {
    return store.put_tree(Tree{});
}

// Walks `segments` down from `tree_digest`, returning the `TreeEntry` found at the end. Every
// intermediate segment must resolve to a Tree (a file "in the middle" of a path is a contract
// violation, not a valid deeper lookup).
template <WorktreeObjectStore S>
[[nodiscard]] result<TreeEntry> resolve_entry_at_path(S& store, Digest const& tree_digest,
                                                       std::span<std::string const> segments) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());

    std::string const& head = segments.front();
    auto it = std::ranges::find_if(tree->entries, [&](TreeEntry const& e) { return e.name == head; });
    if (it == tree->entries.end()) {
        return std::unexpected(error{failure_class::contract, "no entry named '" + head + "' at this path",
                                      "worktree.mount_path_not_found"});
    }
    if (segments.size() == 1) return *it;
    if (!it->is_tree) {
        return std::unexpected(error{failure_class::contract, "'" + head + "' is a file, not a directory",
                                      "worktree.mount_path_not_a_directory"});
    }
    return resolve_entry_at_path(store, it->digest, segments.subspan(1));
}

// Sets (inserts or replaces) the entry named by the LAST segment of `segments` to
// `{leaf_digest, leaf_is_tree}`, creating any missing intermediate directories along the way, and
// returns the digest of the (necessarily new) tree at `tree_digest`'s own level -- the caller
// commits that returned digest as the new Ref, propagating the change all the way to the root in
// one recursive unwind rather than needing a separate "patch the ancestors" pass.
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> set_entry_at_path(S& store, Digest const& tree_digest,
                                                std::span<std::string const> segments,
                                                Digest const& leaf_digest, bool leaf_is_tree) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    std::vector<TreeEntry> entries = tree->entries;

    std::string const& head = segments.front();
    auto it = std::ranges::find_if(entries, [&](TreeEntry const& e) { return e.name == head; });

    TreeEntry new_entry;
    if (segments.size() == 1) {
        new_entry = TreeEntry{head, leaf_digest, leaf_is_tree};
    } else {
        Digest child_digest;
        if (it != entries.end()) {
            if (!it->is_tree) {
                return std::unexpected(error{failure_class::contract,
                                              "cannot write through '" + head +
                                                  "': it already names a file, not a directory",
                                              "worktree.mount_write_type_conflict"});
            }
            child_digest = it->digest;
        } else {
            auto empty = ensure_empty_tree(store);
            if (!empty) return std::unexpected(empty.error());
            child_digest = *empty;
        }
        auto new_child = set_entry_at_path(store, child_digest, segments.subspan(1), leaf_digest, leaf_is_tree);
        if (!new_child) return std::unexpected(new_child.error());
        new_entry = TreeEntry{head, *new_child, true};
    }

    if (it != entries.end()) *it = new_entry; else entries.push_back(new_entry);
    return store.put_tree(Tree{std::move(entries)});
}

// Resolves `subtree_path` (a Mount's fixed, host-configured root within its ref) down from
// `root_digest` to the Digest of the Tree living there -- `""` means the mount is rooted at the
// ref's own root, returned as-is. Used by write-quota enforcement (025 §5, Milestone 3 Phase C3) to
// find exactly the subtree a quota is scoped to, never the whole ref (a ref may host several mounts
// with independent quotas via different `subtree_path`s -- 025 §5's own example, `/work` and
// `/input` on the same ref).
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> resolve_subtree_digest(S& store, Digest const& root_digest,
                                                      std::string const& subtree_path) {
    if (subtree_path.empty()) return root_digest;
    auto segments = split_mount_path(subtree_path);
    if (!segments) return std::unexpected(segments.error());
    if (segments->empty()) return root_digest;
    auto entry = resolve_entry_at_path(store, root_digest, *segments);
    if (!entry) return std::unexpected(entry.error());
    if (!entry->is_tree) {
        return std::unexpected(error{failure_class::fatal, "mount subtree path does not name a directory",
                                      "worktree.mount_subtree_not_a_directory"});
    }
    return entry->digest;
}

// Total bytes (sum of every reachable Blob's size) and total file count (every reachable Blob leaf,
// recursively) under `tree_digest` -- what a write-quota check (025 §5) means by "this mount's
// current usage". Recomputed from the tree on every write rather than tracked as separate running
// state: the content-addressed store has no side state to keep in sync by construction, and this
// keeps the quota check correct-by-construction against whatever the tree actually contains (e.g. a
// write that REPLACES a large file with a small one must show reduced usage, not accumulate a
// stale delta) at the cost of walking the subtree on every write -- a named, accepted cost for a
// milestone whose own gates (025 §9 G4) defer real p99 cost measurement past M3 project-wide.
template <WorktreeObjectStore S>
[[nodiscard]] result<std::pair<std::uint64_t, std::uint32_t>> subtree_usage(S& store, Digest const& tree_digest) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    std::uint64_t bytes = 0;
    std::uint32_t files = 0;
    for (auto const& entry : tree->entries) {
        if (entry.is_tree) {
            auto sub = subtree_usage(store, entry.digest);
            if (!sub) return std::unexpected(sub.error());
            bytes += sub->first;
            files += sub->second;
        } else {
            auto blob = store.get_blob(entry.digest);
            if (!blob) return std::unexpected(blob.error());
            bytes += blob->size();
            ++files;
        }
    }
    return std::make_pair(bytes, files);
}

// `mount.subtree_path` (host-configured, fixed) followed by `guest_path` (per-call), as one combined
// segment list resolved in a single walk from the Ref's own root -- rather than descending
// `subtree_path` and `guest_path` as two separate phases, which would need to reconcile two
// intermediate digests instead of one.
[[nodiscard]] inline result<std::vector<std::string>> combined_mount_segments(Mount const& mount,
                                                                               std::string const& guest_path) {
    std::vector<std::string> full_segments;
    if (!mount.subtree_path.empty()) {
        auto sub = split_mount_path(mount.subtree_path);
        if (!sub) return std::unexpected(sub.error());
        full_segments = std::move(*sub);
    }
    auto guest = split_mount_path(guest_path);
    if (!guest) return std::unexpected(guest.error());
    full_segments.insert(full_segments.end(), guest->begin(), guest->end());
    if (full_segments.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "path names the mount root itself, not a file",
                                      "worktree.mount_path_is_root"});
    }
    return full_segments;
}

} // namespace detail

// Reads the file at `guest_path` (relative to `mount`) through `granted`, the caller's already-bound
// `cap::FsRead`. Two independent checks come before any store access at all (007 §3 -- capability
// grants ARE the authority, checked before the effect, not after): `granted.mount_id` must name
// exactly this `mount` (a capability for one mount can never reach another, even by accident), and
// `granted.path_prefix` -- reusing `capability_detail::path_prefix_covers`, the SAME subsumption
// primitive `CapabilitySet::attenuate`/`contains` already use, not a second path-matching routine
// invented here -- must cover `guest_path` (a capability scoped to `/work/output` can't read
// `/work/private`). `granted.size_cap_bytes`, if set, is enforced after the read (the size is only
// known once the blob is fetched).
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<std::vector<std::byte>> mount_read(OS& object_store, RS& ref_store, Mount const& mount,
                                                          cap::FsRead const& granted,
                                                          std::string const& guest_path) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }
    if (!capability_detail::path_prefix_covers(granted.path_prefix, guest_path)) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability's path scope does not cover the requested path",
                                      "worktree.mount_path_outside_capability"});
    }

    auto full_segments = detail::combined_mount_segments(mount, guest_path);
    if (!full_segments) return std::unexpected(full_segments.error());

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }

    auto entry = detail::resolve_entry_at_path(object_store, (*ref)->tree_digest, *full_segments);
    if (!entry) return std::unexpected(entry.error());
    if (entry->is_tree) {
        return std::unexpected(error{failure_class::contract, "the requested path names a directory, not a file",
                                      "worktree.mount_read_is_directory"});
    }

    auto bytes = object_store.get_blob(entry->digest);
    if (!bytes) return std::unexpected(bytes.error());
    if (granted.size_cap_bytes.has_value() && bytes->size() > *granted.size_cap_bytes) {
        return std::unexpected(error{failure_class::policy,
                                      "the requested file exceeds this capability's size cap",
                                      "worktree.mount_read_exceeds_size_cap"});
    }
    return bytes;
}

// Writes `content` to `guest_path` (relative to `mount`) through `granted`, the caller's already-
// bound `cap::FsWrite`, returning the mount's Ref after the commit. Same two capability checks as
// `mount_read` before any store access. **`granted.quota_bytes`/`granted.file_count_cap` ARE
// enforced here** (025 §5, Milestone 3 Phase C3): the candidate new tree is built first, then this
// mount's subtree usage is recomputed against it (`detail::subtree_usage`, scoped to
// `mount.subtree_path` via `detail::resolve_subtree_digest` -- never the whole ref, since a ref may
// host several independently-quota'd mounts) -- and the Ref is committed ONLY if usage stays within
// both caps. A write that would exceed either cap is rejected before `commit_ref` ever runs, same
// fail-closed shape 025 §5's other checks already use: the guest never observes a state where the
// Ref moved and THEN the quota was found to be exceeded. `std::nullopt` on either cap field means
// uncapped (the same convention `trust/capability.hpp`'s own header comment already documents for
// every `cap::*` limit field) -- an omitted cap is never treated as "0" or silently skipped.
//
// Error framing matches 026 §3's mapping table exactly ("Quota exhausted -> OSError (No space left
// on device)"): a `failure_class::resource` error with that literal message, so a future guest-
// facing translator (Phase E's `PythonRunner`/`ShellRunner`) has an ordinary OS-shaped message ready
// to raise, not a policy identifier to reword.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<Ref> mount_write(OS& object_store, RS& ref_store, Mount const& mount,
                                       cap::FsWrite const& granted, std::string const& guest_path,
                                       std::span<std::byte const> content) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }
    if (!capability_detail::path_prefix_covers(granted.path_prefix, guest_path)) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability's path scope does not cover the requested path",
                                      "worktree.mount_path_outside_capability"});
    }

    auto full_segments = detail::combined_mount_segments(mount, guest_path);
    if (!full_segments) return std::unexpected(full_segments.error());

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }

    auto blob_digest = object_store.put_blob(content);
    if (!blob_digest) return std::unexpected(blob_digest.error());

    auto new_root = detail::set_entry_at_path(object_store, (*ref)->tree_digest, *full_segments,
                                               *blob_digest, /*leaf_is_tree=*/false);
    if (!new_root) return std::unexpected(new_root.error());

    if (granted.quota_bytes.has_value() || granted.file_count_cap.has_value()) {
        auto subtree_digest = detail::resolve_subtree_digest(object_store, *new_root, mount.subtree_path);
        if (!subtree_digest) return std::unexpected(subtree_digest.error());
        auto usage = detail::subtree_usage(object_store, *subtree_digest);
        if (!usage) return std::unexpected(usage.error());
        if (granted.quota_bytes.has_value() && usage->first > *granted.quota_bytes) {
            return std::unexpected(
                error{failure_class::resource, "No space left on device", "worktree.mount_write_quota_exceeded"});
        }
        if (granted.file_count_cap.has_value() && usage->second > *granted.file_count_cap) {
            return std::unexpected(error{failure_class::resource, "No space left on device",
                                          "worktree.mount_write_file_count_exceeded"});
        }
    }

    return commit_ref(ref_store, mount.ref_name, *new_root);
}

// ============================================================================================
// Conflict evidence materialization (025 §4: "the merge fails and is surfaced, with both versions
// retained at /conflicts/<path>.<agent>, and the run's supervising agent or a human resolves it") --
// gap-14 closure, ADR-055. `docs/planning/conflict-evidence-materialization-design-draft.md`'s own
// design, implemented unchanged except one naming correction found while implementing it: the draft's
// prose says "SubWorktree::child_name" for the branch's own identity -- the real field is
// `SubWorktree::name` (`create_sub_worktree`'s own `child_name` PARAMETER becomes that struct's `name`
// member; there is no field literally spelled `child_name`). Used as `branch.name` below.
//
// Placed after `detail::ensure_empty_tree`/`detail::set_entry_at_path` above (the identical
// mechanism `mount_write()` already uses to build a real nested `Tree` one path at a time) -- needed
// so this function's own output is reachable through `mount_read()`/`mount_write()`'s ordinary
// segment-walking, not a flat entry whose OWN name happens to contain '/' (found by testing:
// `MergeConflict::path` is documented as slash-joined for a nested conflict, and `SubWorktree::name`
// -- a workflow executor's own branch identity -- routinely contains '/' via
// `workflow_scoping.hpp`'s own `parent + "/agents/" + id` convention; a flat `TreeEntry` named e.g.
// "a/b/c.txt.session:s-20/agents/writer" is a valid map key but NOT a path `mount_read()`'s
// segment-by-segment walk can ever reach -- a real reachability gap the original flat-tree design
// had, surfaced only once something actually tried to read materialized evidence back through the
// ordinary guest-facing path, ADR-055's own `/conflicts`-mount amendment).
// ============================================================================================

// A pure, deterministic function modeled directly on `memory.hpp`'s own `memory_ref_name(Principal
// const&)` pattern -- deterministic and derived, never caller-supplied, so two different parent Refs
// can never collide on the same conflicts Ref by construction (the identical rationale
// `memory_ref_name`'s own comment gives for why an aliasing-prone caller-chosen id is a real
// cross-something leakage hazard, not a hypothetical one).
[[nodiscard]] inline std::string conflicts_ref_name(std::string const& parent_ref_name) {
    return parent_ref_name + ":conflicts";
}

// Called by `merge_branch_into_parent()`'s own CALLER, never by that function itself -- keeps its
// existing, already-relied-upon contract ("returns conflicts, touches nothing on failure") completely
// unchanged; `expected_parent`'s own Ref is provably untouched regardless of what this function does,
// since it commits only to a genuinely separate Ref (`conflicts_ref_name`). A no-op (returns success,
// touches nothing) when `conflicts` is empty -- there is nothing to materialize.
//
// `ours_agent_id` is a REQUIRED, explicit parameter: the parent side's identity is NOT knowable from
// inside the merge machinery itself (the parent could be the top-level session, another already-merged
// sibling, anything) -- inferring or guessing it was considered and rejected (design draft §2c), the
// caller is the only party that actually knows. `branch`'s own identity (`branch.name`) IS already
// real and available, unlike `ours`'s.
//
// A SINGLE shared `:conflicts` Ref per parent accumulates every failed merge attempt against that
// parent (new commits, tree grows) -- both versions retained, nothing silently overwritten, matching
// 025 §4's own wording literally. This is a deliberate choice with a named consequence (design draft
// §3): the conflicts Ref has no built-in retention/pruning, the same class of residual
// `decisions/README.md`'s own ADR-038 entry already names for passivation/archival generally.
//
// Digests are reused directly from the ALREADY-STORED `MergeConflict::ours`/`::theirs` entries, never
// re-written as fresh blobs: `object_store` is the SAME content-addressed store the parent/branch
// trees already live in, so a `TreeEntry` pointing at an existing digest IS the "both versions
// retained" requirement -- no data duplication, and correct regardless of whether a conflicting entry
// is itself a blob or (a blob-vs-tree type fork) a tree, since `is_tree` is carried through unchanged.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<void> materialize_merge_conflicts(OS& object_store, RS& ref_store,
                                                         std::string const& parent_ref_name,
                                                         std::string const& ours_agent_id,
                                                         SubWorktree const& branch,
                                                         std::vector<MergeConflict> const& conflicts) {
    if (conflicts.empty()) return {};

    std::string const ref_name = conflicts_ref_name(parent_ref_name);

    // Seeded from whatever this parent's conflicts Ref already holds (possibly nothing, on the first
    // failed merge against this parent) -- `set_entry_at_path` below only ever touches the ONE path
    // being set at a time, so every other existing entry/subtree (both an add/add fork's ours AND
    // theirs entries, and every earlier failed attempt's own entries) survives untouched; a REPEAT
    // conflict at the identical <path>.<agent> leaf simply replaces its own prior evidence there.
    Digest root_digest;
    auto existing = read_ref(ref_store, ref_name);
    if (!existing) return std::unexpected(existing.error());
    if (existing->has_value()) {
        root_digest = (*existing)->tree_digest;
    } else {
        auto empty = detail::ensure_empty_tree(object_store);
        if (!empty) return std::unexpected(empty.error());
        root_digest = *empty;
    }

    auto apply_one = [&](std::string const& conflict_path, std::string const& agent_id,
                          TreeEntry const& side) -> result<void> {
        // The agent id is an IDENTIFIER, not itself a navigable path -- sanitized so a '/' inside it
        // (routine for a workflow executor's own branch name) becomes part of the LEAF's own name,
        // never mistaken for a directory separator by set_entry_at_path below.
        std::string sanitized_agent = agent_id;
        std::replace(sanitized_agent.begin(), sanitized_agent.end(), '/', '_');

        // `conflict_path` itself may be slash-joined (MergeConflict::path's own documented shape) --
        // split it so real directory levels become real nested Tree structure, mirroring the
        // conflicting file's own original location under /conflicts, exactly like mount_write() does
        // for an ordinary write.
        std::vector<std::string> segments;
        std::size_t start = 0;
        for (;;) {
            std::size_t const slash = conflict_path.find('/', start);
            if (slash == std::string::npos) {
                segments.push_back(conflict_path.substr(start) + "." + sanitized_agent);
                break;
            }
            segments.push_back(conflict_path.substr(start, slash - start));
            start = slash + 1;
        }

        auto new_root =
            detail::set_entry_at_path(object_store, root_digest, segments, side.digest, side.is_tree);
        if (!new_root) return std::unexpected(new_root.error());
        root_digest = *new_root;
        return {};
    };

    for (MergeConflict const& c : conflicts) {
        if (c.ours.has_value()) {
            auto r = apply_one(c.path, ours_agent_id, *c.ours);
            if (!r) return r;
        }
        if (c.theirs.has_value()) {
            auto r = apply_one(c.path, branch.name, *c.theirs);
            if (!r) return r;
        }
    }

    auto committed = commit_ref(ref_store, ref_name, root_digest);
    if (!committed) return std::unexpected(committed.error());
    return {};
}

// ============================================================================================
// `/conflicts` mount (025 §4: "the merge fails and is surfaced... the run's supervising agent or a
// human resolves it") -- ADR-055 §6's own residual, closed here. Mirrors `memory.hpp`'s own
// `memory_mount_id`/`memory_mount` split EXACTLY: this file only builds the guest-visible binding,
// never a capability -- the same reason `memory_mount()` doesn't either (`Mount`'s own comment,
// above: "constructed only by host policy," and a `Capability` is "only ever minted by
// `CapabilitySet::grant_root`" -- two separate authorities, never fused into one function). WHICH
// run/session's capability set actually gets a `cap::FsRead` for this `mount_id`, and WHEN (e.g.
// only after a real `workflow_status::merge_conflict` outcome, vs. always present alongside `/work`)
// stays a host policy decision this file does not make -- the identical scope ADR-055 §6 already
// named, now answered with a real, reusable primitive instead of left unbuilt.
// ============================================================================================

[[nodiscard]] inline std::string conflicts_mount_id(std::string const& parent_ref_name) {
    return "/conflicts/" + parent_ref_name;
}

[[nodiscard]] inline Mount conflicts_mount(std::string const& parent_ref_name) {
    return Mount{conflicts_mount_id(parent_ref_name), conflicts_ref_name(parent_ref_name), ""};
}

}  // namespace agentengine
