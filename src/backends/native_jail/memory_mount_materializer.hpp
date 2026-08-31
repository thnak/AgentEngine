#pragma once
// Implements decisions/ADR-153-agent-memory-codeact-bridging.md -- the read-materialization half of
// 026-Agent-Facing-Runtime-Surface.md §5's `agent.memory` ("ordinary read access to the memory
// worktree's files under `/memory`", `FsRead<mount>`) and the pre-run half of `agent.notes`
// (`FsWrite<mount>` on the SAME `/memory` mount -- 026 §5's table names both as capability grants over
// one conceptual mount point, not two separate directories). This file only ever produces a snapshot a
// host can fold into `MediatedPythonConfig::mount_roots`; the write-BACK half (`agent.notes` landing a
// script's writes as real `AgentAuthored` `MemoryItem`s) is a separate harvest step, ADR-156, using the
// SAME `mount_id` this file derives so a later harvest finds exactly what this file materialized.
//
// Reuses `materialize_mount` (worktree_mount_sync.hpp) -- the identical real, already-tested primitive
// `skill_mount_materializer.hpp`'s `materialize_skill_mounts` already established as the path from an
// abstract worktree `Mount` to real files on a real host directory. This file is that same shape,
// narrowed from "every mounted skill" to "exactly one caller-supplied `Mount`" (there is exactly one
// memory worktree per principal, `core/memory.hpp`'s `memory_mount(principal)`), so callers needing a
// `/memory` mount call THIS rather than reimplementing `materialize_skill_mounts`'s own loop for a
// single-element case.
//
// One-shot, matching skills' own "snapshotted per run" precedent (`skill_mount_materializer.hpp`'s own
// top comment) -- 029 never promised live sync either, and this file does not invent it. A script that
// writes into this mount and re-reads the SAME path within the same `execute_code` call sees its own
// write immediately (it is a real host directory, ordinary filesystem semantics apply within one
// process's own lifetime) -- what does NOT happen without ADR-156's harvest step is that write becoming
// a durable `MemoryItem` another run (or another `execute_code` call after a worker restart) can see.
// Named as a residual, not silently assumed away.
//
// Host-supplied capability only (I3): `granted` is never derived from model output or a script's own
// request -- the SAME "host explicitly decides what a Mount+cap::FsRead pair may read" discipline
// `materialize_skill_mounts`/`materialize_mount` already enforce, unchanged here.

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/worktree_mount_sync.hpp"

namespace agentengine::native_jail {

// (mount_id, host_directory) -- ready to fold directly into `MediatedPythonConfig::mount_roots`,
// matching `MaterializedSkillMount`'s own shape (`skill_mount_materializer.hpp`) exactly.
using MaterializedMemoryMount = std::pair<std::string, std::wstring>;

// Shared by `materialize_memory_mount()` below and ADR-156's `prepare_memory_notes_mount()`
// (`memory_notes_materializer.hpp`) -- the identical "fail closed on a reserved-name collision, else
// create a real host subdirectory" sequence both need, factored once rather than duplicated across
// both call sites (code-review finding on this ADR's own pass). `skill_mount_materializer.hpp`'s own
// `materialize_skill_mounts()` keeps its independent copy unchanged -- it loops over an enumerable
// SET of mounts with a different signature shape, not a natural fit to unify with the two
// single-mount callers here without a larger, out-of-scope refactor of already-landed, already-tested
// code.
[[nodiscard]] inline result<std::filesystem::path> reserve_and_create_mount_dir(
    std::string const& mount_name, std::filesystem::path const& host_root,
    std::vector<std::string> const& reserved_mount_ids) {
    if (std::ranges::find(reserved_mount_ids, mount_name) != reserved_mount_ids.end()) {
        return std::unexpected(
            error{failure_class::contract,
                  "mount '" + mount_name + "' collides with a reserved sandbox mount id -- refusing "
                  "to materialize it",
                  "memory.mount_id_reserved"});
    }

    std::filesystem::path const mount_dir = host_root / mount_name;
    std::error_code ec;
    std::filesystem::create_directories(mount_dir, ec);
    if (ec) {
        return std::unexpected(error{failure_class::resource,
                                      "failed to create mount directory: " + ec.message(),
                                      "memory.mount_directory_create_failed"});
    }
    return mount_dir;
}

// Materializes `principal`'s memory worktree (`memory_mount(principal)`, `core/memory.hpp`) into a
// real subdirectory of `host_root`, named by `mount_name` -- a HOST-CHOSEN, guest-facing presentation
// token (typically "memory", matching `memory_mount`'s own header comment: "the guest-visible mount
// PATH... is a separate, host-chosen presentation detail [`memory_mount`] does not touch").
// Deliberately NOT `memory_mount_id(principal)` ("memory:<tenant>:<id>"): that string is safe ONLY as
// an opaque capability-matching key compared inside `CapabilitySet`/`mount_read` -- it is never safe
// as a filesystem path segment (colons are illegal mid-path on Windows) or as something worth exposing
// to a model (026 §5's own "guessable from its name" bar wants "memory", not this principal's own
// tenant/id baked into a path it has to open by literal name). Found by this exact mismatch failing
// closed with a real `ERROR_INVALID_NAME`-class `std::error_code` during this ADR's own red-team pass
// (§4) -- not a hypothetical.
//
// Fails closed, nothing created on disk, if `mount_name` collides with `reserved_mount_ids` -- the
// SAME footgun class `materialize_skill_mounts` already guards against (a skill or another mount
// literally named "memory" would otherwise silently shadow this one, or vice versa).
//
// `granted` must be a HOST-authored `cap::FsRead` scoped to this exact mount (never model-derived,
// I3) -- ordinarily `cap::FsRead{memory_mount_id(principal), "", std::nullopt}`, the identical shape
// `test_memory_provider.cpp` already uses to read this same mount today; `materialize_mount()` itself
// still checks this against `memory_mount(principal)`'s own REAL `mount_id`, so the authority
// derivation is unaffected by `mount_name` being a separate, cosmetic identifier.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<MaterializedMemoryMount> materialize_memory_mount(
    OS& object_store, RS& ref_store, Principal const& principal, cap::FsRead const& granted,
    std::string const& mount_name, std::filesystem::path const& host_root,
    std::vector<std::string> const& reserved_mount_ids) {
    Mount const mount = memory_mount(principal);

    auto mount_dir = reserve_and_create_mount_dir(mount_name, host_root, reserved_mount_ids);
    if (!mount_dir) return std::unexpected(mount_dir.error());
    std::wstring const mount_dir_w = mount_dir->wstring();

    auto adapter = mediated_shell::MediatedFileSystemAdapter::create(mount_dir_w);
    if (!adapter) return std::unexpected(adapter.error());

    auto materialized = materialize_mount(object_store, ref_store, mount, granted, *adapter);
    if (!materialized) return std::unexpected(materialized.error());

    return MaterializedMemoryMount{mount_name, mount_dir_w};
}

}  // namespace agentengine::native_jail
