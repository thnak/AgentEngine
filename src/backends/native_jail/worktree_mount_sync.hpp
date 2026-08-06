#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §5/§7 -- Milestone 3 Phase F1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). The bridge
// core/worktree_mount_fs.hpp's own header names as still missing: "syncing a Tree onto mount_root...
// that materialization/sync mechanism is a Phase [F1] dependency of PythonRunner/ShellRunner, not
// built here."
//
// Two directions, both reusing the ALREADY-real, capability-gated primitives one layer down rather
// than re-deriving authority checks: `materialize_mount` primes a real host directory (what
// MediatedPythonConfig::mount_roots / MediatedShellRunner's own root point at) with a worktree
// mount's CURRENT tree content, read through `granted` via the same `mount_read` guest code itself
// is mediated through (025 §5's own capability, checked once per file exactly as a guest `open()`
// would be); `harvest_mount` walks that real directory back into the mount's tree through `granted`
// via `mount_write` (same quota/capability enforcement a guest `open(..., "w")` gets), and reports
// each harvested file as a `ContentItem` (003) -- 025 §7's "the agent saves a file, the user
// receives an artifact" claim, made literal.
//
// `FileSystemAdapter&` is the host-I/O side, deliberately kept as the interface rather than a
// concrete adapter type: the real, production caller constructs one via
// `mediated_shell::MediatedFileSystemAdapter::create(mount_root)` (ADR-014, TOCTOU-safe,
// handle-anchored, its own `split_mount_path`-derived path grammar -- no leading '/', "" names the
// adapter's own root) -- decision 4's "no reuse of the ADR-001 spike's real_filesystem_adapter.
// {hpp,cpp}" applies here too, so this header never names that type. Tests may substitute any
// conforming adapter.
//
// PRECONDITION `materialize_mount` relies on rather than works around: `granted.path_prefix` must
// cover everything actually present in the mount's current subtree, or the walk fails closed on the
// first entry it doesn't cover (`mount_read`'s own policy error) rather than silently priming a
// partial tree -- fail-closed over "clever" partial materialization, matching this project's
// existing discipline (a host wanting a genuinely narrower prime should scope the MOUNT's
// `subtree_path`, not lean on a narrower capability against a wider tree).
//
// Neither direction commits a turn boundary (025 §6 -- that stays the not-yet-built session/turn
// loop's job, same as `commit_turn` itself); `harvest_mount`'s own `mount_write` calls commit the
// mount's Ref once per file written, exactly as a guest `open(..., "w").write()` already would.

#include <cctype>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail {

namespace detail {

// A crude, deliberately narrow extension->media-type guess -- 003 §3 names `media_type` as
// free-form, and this project has no MIME-sniffing infrastructure yet (a named gap, not silently
// worked around): covers the handful of shapes Phase H's own reference-agent tasks are expected to
// produce (010 §9 G1's chart artifact, plain text/JSON/CSV output), falling back to the generic
// octet-stream type for everything else rather than guessing wrong.
[[nodiscard]] inline std::string guess_media_type(std::string const& file_name) {
    auto dot = file_name.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = file_name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "txt") return "text/plain";
    if (ext == "json") return "application/json";
    if (ext == "csv") return "text/csv";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "html") return "text/html";
    return "application/octet-stream";
}

// Recursively walks `tree_digest` (already resolved to `mount`'s subtree root), writing every blob
// leaf into `fs` at its tree-relative path through `mount_read` -- so the SAME `granted`-capability
// check a guest `open(path, "r")` would get applies here, per file. Directories are created on the
// host side before anything beneath them is written: `FileSystemAdapter::write_file` does not
// create parents itself (both existing implementations fail closed on a missing parent rather than
// silently `mkdir -p`ing).
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<void> materialize_subtree(OS& object_store, RS& ref_store, Mount const& mount,
                                                cap::FsRead const& granted, Digest const& tree_digest,
                                                std::string const& guest_prefix, FileSystemAdapter& fs) {
    auto tree = object_store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    for (auto const& entry : tree->entries) {
        std::string guest_path = guest_prefix.empty() ? entry.name : guest_prefix + "/" + entry.name;
        if (entry.is_tree) {
            auto mk = fs.make_directory(guest_path, /*parents=*/true);
            if (!mk) return std::unexpected(mk.error());
            auto r = materialize_subtree(object_store, ref_store, mount, granted, entry.digest, guest_path, fs);
            if (!r) return r;
            continue;
        }
        auto bytes = mount_read(object_store, ref_store, mount, granted, guest_path);
        if (!bytes) return std::unexpected(bytes.error());
        auto written = fs.write_file(guest_path, *bytes, /*append=*/false);
        if (!written) return std::unexpected(written.error());
    }
    return {};
}

// Recursively walks `fs` (a real host directory) starting at `guest_prefix` ("" = the adapter's own
// root, matching `split_mount_path`'s convention), committing every file found into `mount`'s tree
// via `mount_write` -- the SAME `granted`-capability + quota enforcement a guest `open(path, "w")`
// gets -- and appending one `ContentItem` per file harvested to `out`.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<void> harvest_subtree(OS& object_store, RS& ref_store, Mount const& mount,
                                            cap::FsWrite const& granted, std::string const& guest_prefix,
                                            FileSystemAdapter& fs, std::vector<ContentItem>& out) {
    auto entries = fs.list_directory(guest_prefix);
    if (!entries) return std::unexpected(entries.error());
    for (auto const& entry : *entries) {
        std::string guest_path = guest_prefix.empty() ? entry.name : guest_prefix + "/" + entry.name;
        if (entry.is_directory) {
            auto r = harvest_subtree(object_store, ref_store, mount, granted, guest_path, fs, out);
            if (!r) return r;
            continue;
        }
        auto bytes = fs.read_file(guest_path);
        if (!bytes) return std::unexpected(bytes.error());
        auto digest = compute_digest(*bytes);
        if (!digest) return std::unexpected(digest.error());
        auto written = mount_write(object_store, ref_store, mount, granted, guest_path, *bytes);
        if (!written) return std::unexpected(written.error());

        std::string media_type = guess_media_type(entry.name);
        out.push_back(ContentItem{
            Media{BlobRef{*digest, media_type, bytes->size(), "worktree"}, media_type},
            content_origin::tool,
            /*tainted=*/true});
    }
    return {};
}

} // namespace detail

// Primes `fs` (a real host directory, e.g. what `MediatedPythonConfig::mount_roots[mount.mount_id]`
// points at) with `mount`'s CURRENT tree content, through `granted` (an already-bound cap::FsRead).
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<void> materialize_mount(OS& object_store, RS& ref_store, Mount const& mount,
                                              cap::FsRead const& granted, FileSystemAdapter& fs) {
    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }
    auto subtree_digest =
        agentengine::detail::resolve_subtree_digest(object_store, (*ref)->tree_digest, mount.subtree_path);
    if (!subtree_digest) return std::unexpected(subtree_digest.error());
    return detail::materialize_subtree(object_store, ref_store, mount, granted, *subtree_digest, "", fs);
}

// Harvests every real file currently under `fs` (e.g. what a run just wrote to /out's real host
// directory) into `mount`'s tree, through `granted` (an already-bound cap::FsWrite), returning one
// `ContentItem` per file harvested (025 §7: "the agent saves a file, the user receives an
// artifact"). Each file commits `mount`'s Ref individually (via `mount_write`), so a failure partway
// through leaves every file committed before it durably saved -- not silently rolled back --
// matching this seam's existing per-write-commit granularity rather than inventing a batch-commit
// transaction this codebase has nowhere else.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<std::vector<ContentItem>> harvest_mount(OS& object_store, RS& ref_store,
                                                               Mount const& mount, cap::FsWrite const& granted,
                                                               FileSystemAdapter& fs) {
    std::vector<ContentItem> out;
    auto r = detail::harvest_subtree(object_store, ref_store, mount, granted, "", fs, out);
    if (!r) return std::unexpected(r.error());
    return out;
}

} // namespace agentengine::native_jail
