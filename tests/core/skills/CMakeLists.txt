# tests/core/skills/: skills (009 §8): parsing, sources, mounting.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/skills" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Skills (009 §8), Phase 1: SKILL.md parser (§8a) -- pure text-in/value-out, no VFS dependency --
agentengine_add_test(test_skill_parser LIBS agentengine::core agentengine_warnings)

# ---- Skills (009 §8): InlineSkillSource + builtin_skills.hpp's one shipped example (§8f) ---------
agentengine_add_test(test_skill_source_inline LIBS agentengine::core agentengine_warnings)

# ---- Skills (009 §8): DiskSkillSource against a real host temp directory -- the "load from disk" --
# half of the MAF-parity ask (InlineSkillSource is the "inline skill" half, tested above).
agentengine_add_test(test_skill_source_disk LIBS agentengine::core agentengine_warnings)

# ---- Skills (009 §8b-8c): SkillsProvider mounts real skills through real worktree machinery ------
# Real Tree/Ref/Mount/mount_read, same real-crypto dependency as every other worktree test above.
if(WIN32)
  agentengine_add_test(test_skill_provider_mount
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- ADR-072: external, host-discovered skill sources (Claude Code / Codex / Copilot / generic
# agents installs already on the machine) -- real scratch-directory fixtures plus real evidence
# against this session's own already-installed tools, and a SkillsProvider integration proving the
# existing collision guard still protects discovered sources. WIN32-gated for the same reason
# test_skill_provider_mount is: it links agentengine::worktree_store for that integration proof.
if(WIN32)
  agentengine_add_test(test_external_skill_discovery
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Skills Phase 3 (decisions/ADR-024's addendum): agent-triggered, on-demand skill mounting.
# Core-tier only (no native_jail/Python dependency) -- proves MountedSkillsState's own contract plus
# the load-bearing chain: invoke_tool genuinely rejects a skill-named tool before that skill is
# mounted, and the SAME call succeeds after, through the real SkillsProvider/scope_tools_to_mounted_
# skills/invoke_tool pipeline.
if(WIN32)
  agentengine_add_test(test_on_demand_skill_mount
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Skills Phase 2: src/backends/native_jail/skill_mount_materializer.hpp -- the bridge that was
# missing entirely before this session: SkillsProvider::mounted()'s Mounts, materialized to real files
# on a real host directory via materialize_mount, plus the reserved-mount-id collision guard. Needs
# agentengine::mediated_shell_runner for the real MediatedFileSystemAdapter (ADR-014), same as
# test_worktree_mount_sync.
if(WIN32)
  agentengine_add_test(test_skill_mount_materializer
    LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
      agentengine_warnings
    TIMEOUT 60)
endif()
