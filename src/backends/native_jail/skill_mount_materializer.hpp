#pragma once
// Implements 009-Plugin-and-Extension-System.md §8b's other missing half: `SkillsProvider::mounted()`
// (core/skill_provider.hpp) resolves and mounts skills into its own PRIVATE, in-memory object/ref
// store -- nothing outside that provider instance ever sees it. `materialize_mount`
// (worktree_mount_sync.hpp) is the real, already-tested primitive for turning a worktree `Mount` into
// files on a real host directory (exactly what `MediatedPythonConfig::mount_roots` needs), but before
// this file it had zero non-test callers -- a skill's content had no path into any real sandbox at
// all. This file is that path: one real host subdirectory per mounted skill, named by the skill's own
// (bare-token) `mount_id`, ready to fold directly into `mount_roots`.
//
// One-shot: skills are "snapshotted per run" (009 §8c) and `MediatedPythonConfig::mount_roots` has no
// public mutator after construction (must be set before `.initialize()`) -- so this is meant to run
// ONCE, before a session's sandbox singleton is ever constructed, not re-invoked mid-run.

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/worktree_mount_sync.hpp"

namespace agentengine::native_jail {

// One (mount_id, host_directory) pair per successfully materialized skill.
using MaterializedSkillMount = std::pair<std::string, std::wstring>;

// Ensures `provider`'s skills are resolved (`SkillsProvider::ensure_loaded()`), then materializes
// every mounted skill's current content into its own real subdirectory of `host_root` (one
// subdirectory per skill, named by `mount_id` == the skill's own bare name), via `materialize_mount`.
//
// Fails closed, with NOTHING created on disk for this call, if any mounted skill's `mount_id`
// collides with `reserved_mount_ids` (the sandbox's own pre-existing mounts, e.g. {"work", "input",
// "out"}) -- a skill literally named "work" must never silently shadow the sandbox's own
// working-directory mount. Also fails closed, partway-materialized skills left on disk as a harmless
// orphaned side effect (same posture `SkillsProvider::resolve_and_mount()` itself already takes for
// its own object-store writes), if any one skill's materialization fails.
template <WorktreeObjectStore ObjectStoreT>
[[nodiscard]] result<std::vector<MaterializedSkillMount>> materialize_skill_mounts(
    SkillsProvider<ObjectStoreT>& provider, std::filesystem::path const& host_root,
    std::vector<std::string> const& reserved_mount_ids) {
    if (auto loaded = provider.ensure_loaded(); !loaded) return std::unexpected(loaded.error());

    for (Mount const& mount : provider.mounted()) {
        if (std::ranges::find(reserved_mount_ids, mount.mount_id) != reserved_mount_ids.end()) {
            return std::unexpected(
                error{failure_class::contract,
                      "skill '" + mount.mount_id + "' collides with a reserved sandbox mount id -- "
                      "refusing to materialize any skill mount for this call",
                      "skill.mount_id_reserved"});
        }
    }

    std::vector<MaterializedSkillMount> out;
    out.reserve(provider.mounted().size());
    for (Mount const& mount : provider.mounted()) {
        std::filesystem::path const skill_dir = host_root / mount.mount_id;
        std::error_code ec;
        std::filesystem::create_directories(skill_dir, ec);
        if (ec) {
            return std::unexpected(error{failure_class::resource,
                                          "failed to create skill mount directory: " + ec.message(),
                                          "skill.mount_directory_create_failed"});
        }
        std::wstring const skill_dir_w = skill_dir.wstring();

        auto adapter = mediated_shell::MediatedFileSystemAdapter::create(skill_dir_w);
        if (!adapter) return std::unexpected(adapter.error());

        cap::FsRead const granted{mount.mount_id, "", std::nullopt};
        auto materialized =
            materialize_mount(provider.object_store(), provider.ref_store(), mount, granted, *adapter);
        if (!materialized) return std::unexpected(materialized.error());

        out.emplace_back(mount.mount_id, skill_dir_w);
    }
    return out;
}

}  // namespace agentengine::native_jail
