# tests/worktree/: worktrees: object/ref stores, mounts, merges (025).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "worktree" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Worktree content-addressed object store (025 §2, Milestone 3 Phase A1) --------------------
# agentengine::worktree_store now provides compute_digest on both WIN32 (BCrypt) and NOT WIN32 (a
# self-contained SHA-256, src/core/worktree_digest_posix.cpp) -- no platform guard needed here
# anymore; the two branches share this one known-answer-tested proof.
agentengine_add_test(test_worktree_object_store
  LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

# ---- Worktree Ref persistence over Quark's Store seam (025 §2, Milestone 3 Phase A2) -----------
# Header-only (core/worktree.hpp's commit_ref/read_ref) -- links only agentengine::core, no
# platform-specific crypto dependency (unlike A1's digest computation).
agentengine_add_test(test_worktree_ref_store LIBS agentengine::core agentengine_warnings)

# ---- Worktree sub-worktree sharing modes (025 §3, Milestone 3 Phase B1) ------------------------
# `scratch` mode calls empty_tree_digest() -> compute_digest(), so this needs the same real,
# Windows-only digest implementation A1's own test does (unlike A2's, which never computes a digest).
if(WIN32)
  agentengine_add_test(test_worktree_sub_worktree
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Worktree three-way merge on branch join (025 §4, Milestone 3 Phase B2) ---------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, which need compute_digest() -- same
# Windows-only real-crypto dependency as A1/B1's own tests.
if(WIN32)
  agentengine_add_test(test_worktree_merge
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Conflict evidence materialization (025 §4, gap-14 closure, ADR-055) ------------------------
if(WIN32)
  agentengine_add_test(test_worktree_conflict_evidence
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- shared-mode staleness note (025 §3/§10 Q2, Milestone 3 Phase B3) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2.
if(WIN32)
  agentengine_add_test(test_worktree_staleness
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Branch-merge concurrency proof (025 §9 G3, Milestone 3 Phase B4) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2/B3.
# Hundreds of simulated-interleaving trials, each doing real SHA-256 work -- bounded (decision 7),
# but still real CPU cost, so this is registered with an explicit timeout rather than CTest's default
# unbounded wait (CLAUDE.md Machine Safety: a runaway proof must not be able to hang the box).
if(WIN32)
  agentengine_add_test(test_worktree_branch_concurrency
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings
    TIMEOUT 60)
endif()

# ---- Worktree mounts, capability-gated (025 §5, Milestone 3 Phase C1) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2/B3/B4.
if(WIN32)
  agentengine_add_test(test_worktree_mount
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Worktree mount<->real-directory sync (025 §5/§7, Milestone 3 Phase F1) ---------------------
# Proves materialize_mount/harvest_mount (src/backends/native_jail/worktree_mount_sync.hpp) against
# the REAL MediatedFileSystemAdapter (ADR-014), not a test double -- needs
# agentengine::mediated_shell_runner for that adapter, same as test_mediated_shell_runner_smoke.
if(WIN32)
  agentengine_add_test(test_worktree_mount_sync
    LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
      agentengine_warnings
    TIMEOUT 60)
endif()

# ---- Worktree mount write quotas (025 §5, Milestone 3 Phase C3) ---------------------------------
# Proves the byte/file-count quota enforcement mount_write (core/worktree.hpp) now performs, against
# the same in-memory content-addressed store as C1 -- no real filesystem needed for this layer.
if(WIN32)
  agentengine_add_test(test_worktree_mount_quota
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Worktree mount OS-level path-escape corpus (025 §5, 021 §6 G3, Milestone 3 Phase C2,
# decisions/ADR-014-worktree-mount-path-canonicalization.md) --------------------------------------
# Real Win32 filesystem I/O against a scratch directory under the system temp path: junctions,
# real reparse-point crossing, a real TOCTOU interleaving proven deterministically. Shells out to
# `cmd.exe /c mklink /J` for junction setup (test-only infrastructure), so this needs cmd.exe on
# PATH -- always true on a real Windows install.
if(WIN32)
  agentengine_add_test(test_worktree_mount_fs_escape_corpus
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings
    TIMEOUT 60)

  # ---- list_within_mount_root (Milestone 3 Phase G2, 026 §5's agent.files "listing" claim) -------
  agentengine_add_test(test_worktree_mount_fs_listdir
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings
    TIMEOUT 60)
endif()

# ---- Worktree mount OS-level path-escape corpus, Linux half (025 §5, 021 §6 G3, Milestone 3
# Phase C4, ADR-014's parity follow-on) ------------------------------------------------------------
# Real Linux filesystem I/O against a scratch directory under $TMPDIR: real symlinks (no special
# privilege needed, unlike Windows junctions), a real TOCTOU interleaving proven deterministically
# the same way the Windows corpus does. No CAP_SYS_ADMIN/cgroup delegation needed (unlike the Linux
# native-jail sandbox tests below) -- this is ordinary unprivileged file I/O, so it runs
# unconditionally under `if(NOT WIN32)`, not gated behind AGENTENGINE_LINUX_SANDBOX_TESTS.
if(NOT WIN32)
  agentengine_add_test(test_worktree_mount_fs_escape_corpus_linux
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings
    TIMEOUT 60)
endif()

# ---- Worktree turn-boundary commit and rewind (025 §6, §9 G5, Milestone 3 Phase D1/D2) ----------
# Builds real Blob/Tree fixtures via put_blob/put_tree and one mount_read integration check, same
# real-crypto dependency as A1/B1/B2/B3/B4/C1.
if(WIN32)
  agentengine_add_test(test_worktree_turn_commit
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# A quota-checked mount_write() sizes files through blob_size(), not by copying them out of the store.
# No daemon, no network; a real temp directory for the file-backed store.
agentengine_add_test(test_worktree_blob_size LIBS agentengine::worktree_store agentengine_warnings)

# materialize_mount()/harvest_mount() against verbatim copies of the code they replaced, on random
# trees and random capabilities. No daemon, no network, no real filesystem (a recording adapter).
agentengine_add_test(test_worktree_mount_sync_equivalence
  LIBS agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings
  TIMEOUT 300)
