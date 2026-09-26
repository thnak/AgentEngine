# tests/core/ledger/: the content ledger, file object store durability and the shared filesystem walk.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/ledger" ctest label for every test here (cmake/AgentEngineTests.cmake).

agentengine_add_test(test_fs_walk LIBS agentengine::core agentengine_warnings)

# No daemon, no network: it drives RealIoFileSystem::materialize() directly against a real temp
# directory, so it does NOT belong on any of ci.yml's daemon-requiring exclusion lists.
agentengine_add_test(test_real_io_materialize_wipe_failure
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# ---- ADR-102 Phase 2 -- Ledger<Store>/BranchHandle/Checkpoint (core/ledger.hpp) ------------------
# Built on the real agentengine::WorktreeObjectStore/InMemoryWorktreeObjectStore/compute_digest --
# needs agentengine::worktree_store (real SHA-256 via Windows CNG/BCrypt or the Linux FIPS 180-4
# fallback), not just agentengine::core.
agentengine_add_test(test_ledger LIBS agentengine::worktree_store agentengine_warnings)

agentengine_add_test(test_ledger_case_folding
  LIBS agentengine::worktree_store agentengine_warnings
  TIMEOUT 300)

# Ledger::BlobWriteBatch: a drain writes the durable snapshot once, not once per file. No daemon, no
# network -- real temp directories only.
agentengine_add_test(test_ledger_blob_batch
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# Ledger::would_accept_blob_write() predicts put_blob_safe() under any configured ACL-root cap and for
# shared digests. No daemon, no network.
agentengine_add_test(test_ledger_scan_precheck
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# ---- Content-durability conformer: agentengine::FileWorktreeObjectStore (ported from
# docs/planning/proofs/worktree_io/file_object_store.hpp), closing the blob/tree content half of the
# gap ADR-102/ADR-126/ADR-128 each named and declined to close. A genuine TWO-PROCESS proof -- this one
# executable self-relaunches via std::system() to play both roles as real, separate OS processes.
agentengine_add_test(test_content_durability_cross_process
  LIBS agentengine::worktree_store agentengine_warnings)

# ---- Real, executed adversarial probe of the content-durability design draft's own §3/gate-item-3
# disclosure: content writes are safe under genuine concurrent cross-instance access; the pre-existing
# metadata-bookkeeping race under genuinely concurrent Ledger construction is real, reproduced with
# real threads, and confirmed unchanged (not worsened, not fixed) by adding durable content.
agentengine_add_test(test_content_durability_concurrency
  LIBS agentengine::worktree_store agentengine_warnings)

# ---- Real, adversarial, two-process proof of the content-durability design draft's own gate item 6:
# making Ledger content durable turns IdentityAuthority's own previously-latent id-recycling risk
# (identity-native-sandbox-worktree-design.md §33) into a live cross-principal leak when a host does not
# ALSO configure IdentityAuthority::bootstrap() durably and consistently -- proven both vulnerable and
# fixed-when-configured-correctly, on current production code.
agentengine_add_test(test_identity_durability_precondition
  LIBS agentengine::worktree_store agentengine_warnings)

# ---- GitHub issue #69 -- FileWorktreeObjectStore::put_blob()'s dedup check used the THROWING
# std::filesystem::is_regular_file() overload on the one path the surrounding code documents as being
# concurrently renamed onto. The escaping filesystem_error took the whole process with it (0xC0000409
# on MSVC, the intermittent CI crash), rather than failing closed into result<T> like every other
# error there. Carries a real positive control: the test first proves the race window opens HERE.
agentengine_add_test(test_file_worktree_object_store_race
  LIBS agentengine::worktree_store agentengine_warnings)
# Real threads hammering real filesystem renames -- bounded, but give it room on a loaded CI runner.
set_tests_properties(test_file_worktree_object_store_race PROPERTIES TIMEOUT 300)
