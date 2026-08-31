#pragma once
// Implements decisions/ADR-156-agent-notes-write-back.md -- 026-Agent-Facing-Runtime-Surface.md §5's
// `agent.notes` ("Durable notes across turns and sessions — ordinary writes into `/memory`, landing as
// `AgentAuthored` `MemoryItem`s", `FsWrite<mount>`), GitHub issue #40.
//
// The "ordinary writes" half needs NO new Python/worker code at all: `agent.files`' own generic
// `open(path, mode)` (`agent_files_data_codegen.hpp` -> `_ae_internal.open`/`file_write`) already works
// against ANY mount_id present in a session's `MediatedPythonConfig::mount_roots` -- `split_guest_path`
// parses "/<mount_id>/<rest>" generically, never a hardcoded set of mount names. So once a host folds
// THIS file's staging directory into `mount_roots` under some presentation name (e.g. "notes"), a
// script can already do `open('/notes/todo.txt', 'w').write(...)` today, unchanged.
//
// What's actually missing -- the ONLY new mechanism this file provides -- is making a raw file written
// there DURABLE as a real `AgentAuthored` `MemoryItem` (029 §4). Two pieces:
//
// 1. A DEDICATED REF, genuinely separate from `memory_mount(principal)`'s own ref -- NOT merely a
//    different subtree of the SAME ref, which was this file's own FIRST draft and was found, by a real
//    failing test (`test_memory_notes_write_back.cpp`'s own R2, ADR-156 §4), to be actively wrong:
//    `rank_memory_items()`/`list_memory_items()` (memory.hpp) walk the WHOLE mount tree from
//    `mount.subtree_path` (`""` for the structural mount = the ENTIRE ref) and try to JSON-decode every
//    leaf blob as a `MemoryItem` record -- a subtree-only separation still leaves raw note text
//    ("buy milk") sitting as a SIBLING of the structural `<kind>/<id>` JSON records under the SAME ref
//    root, so an ordinary structural read (`rank_memory_items`, `recall`) chokes trying to parse it as
//    JSON (`json.malformed_number`, observed directly, not theorized). A genuinely separate ref makes
//    this collision impossible BY CONSTRUCTION: `list_memory_items`'s own tree walk starts from
//    `read_ref(ref_store, mount.ref_name)` -- a different ref NAME never resolves into the inbox's tree
//    at all, regardless of subtree_path.
// 2. `harvest_memory_notes()` -- called by the host AFTER a run completes, reusing `harvest_mount()`
//    (`worktree_mount_sync.hpp`, the SAME already-tested primitive ADR-153's own residual named as the
//    write-back mechanism to reuse) to pull every file the script wrote in the staging directory back
//    into the dedicated inbox ref, then wraps each one's real content into a real `MemoryItem`
//    (`source = agent_authored`) durably committed via `write_memory_item()` against the UNSCOPED
//    STRUCTURAL mount -- landing in the exact same area `MemoryProvider`'s own extraction path
//    (029 §4) writes to, so a later `agent.tools.recall(...)`/`agent.memory` read sees it exactly like
//    any other memory item.
//
// Residual, named honestly (not a capability-system guarantee): `cap::FsWrite`/`cap::FsRead` match on
// bare `mount_id` only (`trust/capability.hpp`), so the SAME capability value technically authorizes
// either the inbox `Mount` or the structural `Mount` if a caller passed the wrong one -- what actually
// keeps a guest sandbox out of the structural area is that its OWN `mount_roots` is NEVER folded from
// anything but `prepare_memory_notes_mount()`'s own inbox-ref-backed directory; `memory_mount(principal)`
// (the structural mount) is only ever touched inside this file's own trusted, host-side
// `harvest_memory_notes()`, never exposed to a sandboxed process at all. A code-structure guarantee,
// not a capability-level one -- named here rather than silently assumed stronger than it is.
//
// Ephemeral by design: the staging directory is never read back FROM the object store the way
// ADR-153's read mount is (no "materialize" step) -- each run gets a fresh, empty staging area, and a
// write only becomes durable once `harvest_memory_notes()` runs. A run that crashes before harvesting
// loses whatever was staged but never harvested -- named as a residual (§ ADR-156 §7), not silently
// assumed away.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/memory_mount_materializer.hpp"  // MaterializedMemoryMount
#include "backends/native_jail/worktree_mount_sync.hpp"

namespace agentengine::native_jail {

// A genuinely SEPARATE ref from `memory_ref_name(principal)`/`memory_mount_id(principal)` -- see
// file-top comment for why a shared-ref subtree was tried first and found to be wrong. Same
// tenant/id-scoped derivation discipline `memory_mount_id()` itself uses (Milestone 5 Phase I1) --
// never caller-supplied, so this cannot be pointed at the wrong principal's inbox by a string typo.
[[nodiscard]] inline std::string memory_notes_inbox_ref_name(Principal const& principal) {
    return "memory-notes-inbox:" + principal.tenant_id + ":" + principal.id;
}
[[nodiscard]] inline std::string memory_notes_inbox_mount_id(Principal const& principal) {
    return memory_notes_inbox_ref_name(principal);
}

// The GUEST-WRITABLE mount -- its own dedicated ref, never `memory_mount(principal)`'s.
[[nodiscard]] inline Mount memory_notes_inbox_mount(Principal const& principal) {
    return Mount{memory_notes_inbox_mount_id(principal), memory_notes_inbox_ref_name(principal), ""};
}

// Bootstraps the inbox ref if it doesn't exist yet, reusing `ensure_worktree_ref()` (memory.hpp) --
// the SAME shared bootstrap logic `ensure_memory_worktree()` uses for the structural ref, applied to
// this genuinely separate one, rather than a second hand-copied implementation. Called internally by
// `harvest_memory_notes()` below so callers never need their own separate bootstrap step.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<Ref> ensure_memory_notes_inbox_worktree(OS& object_store, RS& ref_store,
                                                                Principal const& principal) {
    return ensure_worktree_ref(object_store, ref_store, memory_notes_inbox_ref_name(principal));
}

// Creates a fresh, empty real host directory for this run's own staging area -- no read from the
// object store (see file-top comment: ephemeral, not a materialized snapshot). `mount_name` is a
// host-chosen presentation token (e.g. "notes"), the SAME "never the colon-bearing internal mount_id"
// discipline ADR-153's `materialize_memory_mount()` already established (`memory_mount_id()` is safe
// only as an opaque capability-matching key, never a filesystem path segment or a guest-facing name).
[[nodiscard]] inline result<MaterializedMemoryMount> prepare_memory_notes_mount(
    std::string const& mount_name, std::filesystem::path const& host_root,
    std::vector<std::string> const& reserved_mount_ids) {
    auto mount_dir = reserve_and_create_mount_dir(mount_name, host_root, reserved_mount_ids);
    if (!mount_dir) return std::unexpected(mount_dir.error());
    return MaterializedMemoryMount{mount_name, mount_dir->wstring()};
}

// Harvests every file currently in `host_dir` (a `prepare_memory_notes_mount()` result, after a run
// completed) into the dedicated inbox ref via `harvest_mount()`, then wraps each harvested file's
// REAL content into a durable `AgentAuthored` `MemoryItem`, written through the UNSCOPED structural
// memory mount (landing in the same area every other memory write uses). `inbox_granted` gates the
// inbox-ref write; `structural_granted` gates the durable `MemoryItem` write (the structural mount) --
// deliberately two separate capabilities, matching this codebase's own "narrowest capability for what's
// actually being done" discipline rather than one grant covering both very different writes. `run_id`/
// `turn_id` are host-supplied (from the SAME `EffectContext` the run itself carries, e.g.
// `ctx.run_id`/`ctx.turn_index`) -- never invented here (I4: every effect is attributable to a real
// run/turn, not a placeholder).
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<std::vector<MemoryItem>> harvest_memory_notes(
    OS& object_store, RS& ref_store, Principal const& principal, cap::FsWrite const& inbox_granted,
    cap::FsWrite const& structural_granted, std::wstring const& host_dir, std::string const& run_id,
    std::string const& turn_id) {
    auto bootstrapped = ensure_memory_notes_inbox_worktree(object_store, ref_store, principal);
    if (!bootstrapped) return std::unexpected(bootstrapped.error());

    auto adapter = mediated_shell::MediatedFileSystemAdapter::create(host_dir);
    if (!adapter) return std::unexpected(adapter.error());

    Mount const inbox_mount = memory_notes_inbox_mount(principal);
    auto harvested = harvest_mount(object_store, ref_store, inbox_mount, inbox_granted, *adapter);
    if (!harvested) return std::unexpected(harvested.error());

    // Best-effort per item, deliberately: `write_memory_item()` durably commits ONE item at a time
    // (`mount_write`'s own per-write `commit_ref`, matching `harvest_subtree`'s own established "not
    // silently rolled back" philosophy, `worktree_mount_sync.hpp`) -- an EARLIER item in this loop is
    // already durably in the store the instant its own `write_memory_item()` call returns, regardless
    // of what a LATER item does. A first draft of this function returned `std::unexpected` (discarding
    // the whole `items` vector, including already-committed entries) the moment any single item's
    // write failed -- a genuine gap a code-review pass on this ADR caught: the caller would see a bare
    // failure with no way to know some notes were already durably present. Fixed by NOT letting one
    // item's write failure abort the batch: every item this loop can commit, it does, and the
    // returned vector always reflects exactly what is durable, matching the harvest step's own
    // per-item commit granularity instead of pretending this were an atomic, all-or-nothing batch.
    Mount const structural_mount = memory_mount(principal);
    std::vector<MemoryItem> items;
    items.reserve(harvested->size());
    for (ContentItem const& content : *harvested) {
        auto const* media = std::get_if<Media>(&content.value);
        if (media == nullptr) continue;  // harvest_subtree always produces Media; defensive only.
        auto const* blob_ref = std::get_if<BlobRef>(&media->payload);
        if (blob_ref == nullptr) continue;  // harvest_subtree always produces the BlobRef variant.

        auto bytes = object_store.get_blob(blob_ref->digest);
        if (!bytes) continue;  // this ONE file's bytes are unreadable -- skip it, keep the rest.

        MemoryItem item{};
        item.kind = memory_kind::semantic;  // a durable, agent-authored fact -- not an episodic event
                                              // or an extracted procedure, 029 §3's own kind taxonomy.
        item.content.assign(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        item.origin = MemoryOrigin{memory_source::agent_authored, run_id, turn_id, principal};
        auto written = write_memory_item(object_store, ref_store, structural_mount, structural_granted, item);
        if (!written) continue;  // this ONE item's durable write failed -- skip it, keep the rest.
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace agentengine::native_jail
