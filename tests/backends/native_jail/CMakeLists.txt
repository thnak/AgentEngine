# tests/backends/native_jail/: the native jail backend and the mediated shell (src/backends/native_jail/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "backends/native_jail" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- call_tool bridge: sandbox-scoped invoke_tool(), bundled approval, tainted results (Milestone 3
# Phase F2, 010 §6 / 006 §3). Header-only (src/backends/native_jail/tool_bridge.hpp), core-tier --
# links only agentengine::core, no native_jail runner target needed for this proof.
add_executable(test_tool_bridge test_tool_bridge.cpp)
target_include_directories(test_tool_bridge PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
target_link_libraries(test_tool_bridge PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_bridge COMMAND test_tool_bridge)

# ---- Output discipline: cap_output()'s truncation/marker/UTF-8-boundary math (Milestone 3 Phase F3,
# 010 §3 items 4/5). Header-only (src/backends/native_jail/output_discipline.hpp), no Python/sandbox
# dependency -- Runner-level result_repr capture and integration is covered in the Python-gated
# MediatedPythonRunner smoke test instead.
add_executable(test_output_discipline test_output_discipline.cpp)
target_include_directories(test_output_discipline PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
target_link_libraries(test_output_discipline PRIVATE agentengine_warnings)
add_test(NAME test_output_discipline COMMAND test_output_discipline)

# ---- native_jail Runner stub contract ---------------------------------------------------------
# Proves PythonRunner (still a stub) satisfies Runner and fails closed, and that ShellRunner (now
# real, see below) satisfies Runner — see the file's own top comment for exactly what is and is
# not covered.
add_executable(test_native_jail_runner_stubs test_native_jail_runner_stubs.cpp)
target_link_libraries(test_native_jail_runner_stubs PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)

add_test(NAME test_native_jail_runner_stubs COMMAND test_native_jail_runner_stubs)

# ---- ShellRunner prove-phase gates (decisions/ADR-001-shellrunner-grammar-and-dispatch.md) -----

# RealFileSystemAdapter's path-escape list (§2.5.5).
agentengine_add_test(test_real_filesystem_adapter
  LIBS agentengine::core agentengine::shell_runner agentengine_warnings)

# End-to-end ShellRunner claims: Sh-C1, Sh-C2, Sh-C3, Sh-G4, kind-only capability checks, A-C2,
# Sh-S1's behavioral half.
agentengine_add_test(test_shell_runner_proof
  LIBS agentengine::core agentengine::shell_runner agentengine_warnings)

# A-S1: parser safety against a hand-written adversarial corpus. Fake-free — links only against
# the parser, not the dispatch/fs layer, matching the ADR's own "parse() needs nothing but a byte
# string" claim.
agentengine_add_test(test_shell_parser_adversarial
  LIBS agentengine::core agentengine::shell_runner agentengine_warnings)
# The literal ADR-named "100k-stage pipeline" figure is only compiled into a Release build (see
# test_shell_parser_adversarial.cpp's comment on this): the default configuration here links the
# MSVC debug CRT, under which that width triggers a checked-iterator/debug-STL slowdown severe
# enough to hang a routine `ctest` run (CLAUDE.md Machine Safety) — a real build-configuration
# finding from this pass, not a parser defect (confirmed via a side-by-side Release measurement).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_definitions(test_shell_parser_adversarial PRIVATE AE_RUN_100K_PIPELINE_PROBE)
endif()
# This test allocates ~12 MiB arenas repeatedly and runs bounded (kMaxTokens/kMaxNestingDepth-
# sized) adversarial loops — CLAUDE.md machine-safety timeout as a backstop, not because it is
# expected to run anywhere near this long.
set_tests_properties(test_shell_parser_adversarial PROPERTIES TIMEOUT 60)

# Sh-S1's static half: no process-creation symbol is referenced anywhere in the
# agentengine_shell_runner library's compiled object code (ADR-001 §7 finding 1's link-target-
# granularity fix). Checked via llvm-nm (or, when that specific LLVM tool isn't installed, plain
# GNU binutils `nm` -- both implement the same `--undefined-only` output shape this check's own
# parser relies on; ADR-103 §7's own residual note named this gap: a dev/CI Linux box very often has
# `nm` without ever having `llvm-nm`, and this check should not be silently vacuous there) against
# the actual built artifact; skips (CTest SKIP_RETURN_CODE) if neither is found rather than silently
# passing.
#
# PLATFORM-ORDERED search, not just a wider NAMES list: on a genuine Windows host only `llvm-nm`/
# `llvm-nm.exe` is ever a real option (GNU `nm` doesn't exist there), unchanged from before this
# pass. On Linux, `nm` is searched FIRST -- a real, ordinary Linux dev/CI box almost always has GNU
# binutils `nm` and often has no LLVM toolchain at all; `llvm-nm` stays a fallback for the (rarer)
# clang-only environment. Discovered the hard way in THIS session's own WSL2 test environment: WSL's
# default Windows-PATH interop makes a cross-mounted `llvm-nm.exe` reachable from inside Linux too,
# which would otherwise get picked over the native tool by pure NAMES-list ordering -- an artifact of
# this one dev box, not representative of a real, Windows-less Linux CI runner this check must also
# work correctly on.
if(WIN32)
  find_program(AE_LLVM_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe)
else()
  find_program(AE_LLVM_NM_EXECUTABLE NAMES nm llvm-nm)
endif()
agentengine_add_test(test_shell_runner_no_process_creation
  LIBS agentengine::core agentengine_warnings
  DEFINITIONS
    AE_SHELL_RUNNER_LIB_PATH="$<TARGET_FILE:agentengine_shell_runner>"
    AE_LLVM_NM_PATH="${AE_LLVM_NM_EXECUTABLE}")
set_tests_properties(test_shell_runner_no_process_creation PROPERTIES SKIP_RETURN_CODE 77)

# ---- Milestone 3 Phase E3's real ShellRunner (mediated_shell_*.{hpp,cpp}) -----------------------
# Portable across Windows and Linux (2026-08-28, ADR-103, the Linux-parity pass) -- moved OUT of the
# surrounding `if(WIN32)` block below, since `agentengine::mediated_shell_runner` (CMakeLists.txt's
# own WIN32/`NOT WIN32` split) and every test in this section now build and run on both platforms.
# Registered unconditionally, matching how `agentengine::worktree_store`'s own already-portable
# consumers are registered. (Was "Windows only, matches agentengine_mediated_shell_runner's own
# scope" before this pass -- that scope itself is what changed.)
agentengine_add_test(test_mediated_shell_runner_smoke
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings
  TIMEOUT 60)

# ---- ADR-100 §4 F3 fix: the mediated-shell evaluation loop's wall-clock budget -----------------
agentengine_add_test(test_mediated_shell_runner_wall_clock_timeout
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings
  TIMEOUT 60)

# ---- Session-scoped shell wiring (every-session-gets-a-sandbox roadmap, Phase 1):
# backends/native_jail/session_shell_wiring.hpp's SessionShellSandbox, proven end to end through
# the real invoke_tool pipeline against a real host directory.
agentengine_add_test(test_session_shell_wiring
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings)

# ---- Milestone 3 Phase E4: hostile-corpus/mediation proof (010 §9 G2/G7), platform-adapted
# (2026-08-28, ADR-103): SH7-SH10's backslash-leaf payloads have genuinely different, correctly
# adapted expected outcomes per platform (Windows: rejected escape; Linux: harmless in-mount
# create, backslash having no separator meaning there) -- see that test file's own top-of-block
# comment for the full reasoning, not silently ported or silently skipped.
agentengine_add_test(test_mediated_shell_runner_hostile_corpus
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings
  TIMEOUT 60)

# ---- ADR-147: 008 §9 G9, the task-scope containment corpus -- ordinary-sounding task narratives
# with one embedded, unlabeled out-of-scope action, run through the same already-Judged mount-
# escape containment (ADR-014) as the hostile-name corpus above. Corpus-coverage extension of G3,
# scripted (not a model-propensity test) -- see tests/helpers/task_scope_corpus.hpp's top comment.
agentengine_add_test(test_task_scope_containment_corpus
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings
  TIMEOUT 60)

# G2's ShellRunner-specific "must not exist as a reachable code path" bar, structural half, against
# the REAL agentengine_mediated_shell_runner artifact (test_shell_runner_no_process_creation.cpp
# already covers the untouched spike). Reuses the same llvm-nm/nm discovered above.
#
# Portable across Windows and Linux (2026-08-29, decisions/ADR-104-real-io-filesystem-linux-parity.md's
# own named follow-on) -- moved OUT of the surrounding `if(WIN32)` block below, same reasoning as the
# other mediated_shell_runner tests just above. The hardcoded Windows-only hostile-symbol list
# (CreateProcessA/W etc.) is now `#ifdef _WIN32`/`#else`-branched in the test source itself, with a
# separately-reasoned POSIX process-creation symbol list (fork/exec*/posix_spawn*/clone) -- NOT a
# mechanical rename, since the Windows list and the POSIX list name genuinely different platform
# APIs for the identical underlying property this check exists to prove ("no reachable code path to
# process creation").
agentengine_add_test(test_mediated_shell_runner_no_process_creation
  LIBS agentengine::core agentengine_warnings
  DEFINITIONS
    AE_MEDIATED_SHELL_RUNNER_LIB_PATH="$<TARGET_FILE:agentengine_mediated_shell_runner>"
    AE_LLVM_NM_PATH="${AE_LLVM_NM_EXECUTABLE}")
set_tests_properties(test_mediated_shell_runner_no_process_creation PROPERTIES SKIP_RETURN_CODE 77)

# ---- Everything below stays WIN32-only, a real, disclosed platform-parity gap (not a stale gate):
# the embedded-CPython Python-runner composition tests (AGENTENGINE_BUILD_PYTHON_RUNNER) need the
# embedded native CPython interpreter, itself Windows-only for reasons unrelated to SandboxToolProvider
# (CLAUDE.md's locked "embedded native CPython interpreter" decision) -- a materially larger, unrelated
# platform-parity question, not attempted here.
if(WIN32)
  # RunnerCall<python> composition against the REAL MediatedPythonRunner (E2) -- only built when
  # AGENTENGINE_BUILD_PYTHON_RUNNER is also ON (needs a real CPython install).
  if(AGENTENGINE_BUILD_PYTHON_RUNNER)
    agentengine_add_test(test_mediated_shell_runner_python_composition
      LIBS agentengine::core agentengine::mediated_shell_runner agentengine::mediated_python_runner
        agentengine::worktree_store agentengine_warnings
      DEFINITIONS
        AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
    # AE_PYTHON_TEST_ENV isn't defined yet at this point in the script (set later, alongside the
    # ADR-002/003 prove-phase test block) -- built directly here from AGENTENGINE_PYTHON_DLL_DIR
    # (set by the root CMakeLists.txt before add_subdirectory(tests) runs) for the identical
    # Windows-has-no-rpath reason those tests need it.
    set_tests_properties(test_mediated_shell_runner_python_composition PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}" TIMEOUT 60)

    # PROVE-PHASE test (docs/planning/identity-native-sandbox-worktree-design.md §30's own disclosed
    # gap: Shell and Python are never pointed at the same real host directory in the shipped
    # tools/cli_chat.cpp wiring) -- proves that explicitly setting Python's mount_roots["work"] to
    # the SAME directory Shell's own MediatedFileSystemAdapter uses gives them genuine cross-visible
    # file sharing, with no accidental authority widening. Additive only -- tools/cli_chat.cpp's own
    # real wiring is untouched by this test.
    agentengine_add_test(test_shell_python_shared_mount_prove
      LIBS agentengine::core agentengine::mediated_shell_runner agentengine::mediated_python_runner
        agentengine::worktree_store agentengine_warnings
      DEFINITIONS
        AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
    set_tests_properties(test_shell_python_shared_mount_prove PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}" TIMEOUT 60)
  endif()
endif()

# ---- Job Object resource-limit layer (decisions/ADR-004-appcontainer-native-jail-windows-backend.md) --
# Windows only -- see CMakeLists.txt's agentengine_job_object_limits target comment.
if(WIN32)
  add_executable(test_job_object_limits test_job_object_limits.cpp)
  target_link_libraries(test_job_object_limits PRIVATE
    agentengine::core agentengine::job_object_limits agentengine_warnings)
  target_compile_definitions(test_job_object_limits PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_job_object_limits job_object_hostile_child)
  add_test(NAME test_job_object_limits COMMAND test_job_object_limits)
  # Generous but finite: this test spawns real hostile child processes (unbounded memory growth,
  # CPU spin, unbounded process spawning) whose containment is exactly what is under test --
  # CLAUDE.md Machine Safety requires a backstop even though every individual wait inside the test
  # already has its own bounded deadline.
  set_tests_properties(test_job_object_limits PROPERTIES TIMEOUT 90)

  # ---- ADR-004 SS11 item 4 decision spike: LPAC vs regular AppContainer -------------------------
  # Not a regression test (no ongoing "must pass" claim about LPAC itself) -- a one-time decision
  # experiment, kept in the tree (unlike the ADR's own original ac_setup.cpp/ac_run.cpp spike,
  # never committed) so the evidence is reproducible, not merely quoted in the ADR's prose.
  add_executable(adr004_lpac_decision_spike adr004_lpac_decision_spike.cpp)
  target_link_libraries(adr004_lpac_decision_spike PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(adr004_lpac_decision_spike PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(adr004_lpac_decision_spike job_object_hostile_child)
  add_test(NAME adr004_lpac_decision_spike COMMAND adr004_lpac_decision_spike)
  set_tests_properties(adr004_lpac_decision_spike PROPERTIES TIMEOUT 60)

  # ---- NativeJailBackend end-to-end (M2 Phase C task C2, 008 §1b/§2/§3) -------------------------
  add_executable(test_native_jail_backend_windows test_native_jail_backend_windows.cpp)
  target_link_libraries(test_native_jail_backend_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_backend_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_backend_windows job_object_hostile_child)
  add_test(NAME test_native_jail_backend_windows COMMAND test_native_jail_backend_windows)
  # Same Machine Safety backstop as test_job_object_limits -- real child processes, real wall_ms
  # kills under test.
  set_tests_properties(test_native_jail_backend_windows PROPERTIES TIMEOUT 90)

  # ---- instances_ concurrency fix regression (Revision 4 round-3 finding-11 side-finding, office-
  # document-extraction-design-draft.md) -----------------------------------------------------------
  add_executable(test_native_jail_backend_concurrency_windows test_native_jail_backend_concurrency_windows.cpp)
  target_link_libraries(test_native_jail_backend_concurrency_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_backend_concurrency_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_backend_concurrency_windows job_object_hostile_child)
  add_test(NAME test_native_jail_backend_concurrency_windows COMMAND test_native_jail_backend_concurrency_windows)
  # 8 threads x 40 iterations of real child-process create/exec/destroy cycles -- generous but finite.
  set_tests_properties(test_native_jail_backend_concurrency_windows PROPERTIES TIMEOUT 120)

  # ---- 008 §7 abuse-case corpus + §9 G2 positive controls (M2 Phase C task C3) -------------------
  add_executable(test_native_jail_abuse_corpus_windows test_native_jail_abuse_corpus_windows.cpp)
  target_link_libraries(test_native_jail_abuse_corpus_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_abuse_corpus_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_abuse_corpus_windows job_object_hostile_child)
  add_test(NAME test_native_jail_abuse_corpus_windows COMMAND test_native_jail_abuse_corpus_windows)
  # Five abuse cases, each with a contained run and at least one positive control -- more real
  # child processes than C2's own test, same Machine Safety discipline.
  set_tests_properties(test_native_jail_abuse_corpus_windows PROPERTIES TIMEOUT 120)

  # ---- 008 §9 G1 cross-platform outcome-classification parity (M2 Phase C task C4) ---------------
  add_executable(test_native_jail_parity_windows test_native_jail_parity_windows.cpp)
  target_link_libraries(test_native_jail_parity_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_parity_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_parity_windows job_object_hostile_child)
  add_test(NAME test_native_jail_parity_windows COMMAND test_native_jail_parity_windows)
  set_tests_properties(test_native_jail_parity_windows PROPERTIES TIMEOUT 60)

  # ---- 008 §9 G3 no-ambient-authority probe (M2 Phase C task C5) ---------------------------------
  add_executable(test_native_jail_ambient_authority_windows test_native_jail_ambient_authority_windows.cpp)
  target_link_libraries(test_native_jail_ambient_authority_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings ws2_32)
  target_compile_definitions(test_native_jail_ambient_authority_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_ambient_authority_windows job_object_hostile_child)
  add_test(NAME test_native_jail_ambient_authority_windows COMMAND test_native_jail_ambient_authority_windows)
  set_tests_properties(test_native_jail_ambient_authority_windows PROPERTIES TIMEOUT 60)

  # ---- 008 §9 G4 teardown-cycle census (M2 Phase C task C6) --------------------------------------
  add_executable(test_native_jail_teardown_cycles_windows test_native_jail_teardown_cycles_windows.cpp)
  target_link_libraries(test_native_jail_teardown_cycles_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings psapi)
  target_compile_definitions(test_native_jail_teardown_cycles_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_teardown_cycles_windows job_object_hostile_child)
  add_test(NAME test_native_jail_teardown_cycles_windows COMMAND test_native_jail_teardown_cycles_windows)
  # 300 real AppContainer + Job Object launches, plus 5 warm-up -- generous but finite headroom over
  # the measured per-cycle cost, same Machine Safety backstop as the other native_jail tests.
  # RUN_SERIAL: minimizes overlap with sibling tests that share job_object_hostile_child.exe (a
  # real -j4 run surfaced this -- see the test file's own comment on hostile_child_name_lower).
  set_tests_properties(test_native_jail_teardown_cycles_windows PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)

  # ---- NativeJailBackend::grant_ro_path_once() (docs/planning/pdf-text-extraction-design-draft.md
  # Revision 6/7 -- the public entry point round-5/round-6 red-team found missing) -----------------
  agentengine_add_test(test_native_jail_grant_ro_path_once_windows
    LIBS agentengine::core agentengine::native_jail_backend agentengine_warnings
    TIMEOUT 30)

  # ---- AppContainerProfile::grant_path()/revoke_path() ACE lifecycle (docs/planning/office-document-
  # extraction-design-draft.md "Prove pass 2" -- the two central safety claims Revision 8's pool-
  # elimination collapse and Revision 9/10's revoke_path() safety net rest on, argued from code reading
  # across ten revisions/nine red-team rounds, proven for real here) -----------------------------------
  agentengine_add_test(test_native_jail_grant_path_ace_lifecycle_windows
    LIBS agentengine::core agentengine::native_jail_backend agentengine_warnings
    TIMEOUT 30)
endif()

# ---- LinuxNativeJailBackend end-to-end (M2 Phase C task C2's Linux follow-up, 008 §1b/§2/§3) ---
# Needs CAP_SYS_ADMIN (namespaces) and a delegated cgroup v2 root -- run via
# helpers/cgroup_v2_test_setup.sh inside a --privileged container, never assumed available on an
# ordinary CI runner without that setup, so this is opt-in (AGENTENGINE_LINUX_SANDBOX_TESTS), not
# built/run unconditionally the way test_job_object_limits is on Windows.
if(NOT WIN32)
  option(AGENTENGINE_LINUX_SANDBOX_TESTS
    "Build/run the LinuxNativeJailBackend end-to-end test (needs CAP_SYS_ADMIN + delegated cgroup v2)"
    OFF)
  if(AGENTENGINE_LINUX_SANDBOX_TESTS)
    add_executable(hostile_child_posix ${AE_TESTS_SOURCE_DIR}/helpers/hostile_child_posix.cpp)

    add_executable(test_native_jail_backend_linux test_native_jail_backend_linux.cpp)
    target_link_libraries(test_native_jail_backend_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_backend_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_backend_linux hostile_child_posix)
    add_test(NAME test_native_jail_backend_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_backend_linux>")
    # Same Machine Safety backstop as the Windows test -- real child processes under real
    # namespace/cgroup isolation, real wall_ms kills under test.
    # RUN_SERIAL on every test in this Linux native_jail group (not just this one): each
    # LinuxNativeJailBackend defaults to the SAME host paths (delegated_cgroup_root_,
    # jail_root_base_) and mints ids from a process-local, not host-unique, counter starting at 0 --
    # two of these test BINARIES running concurrently (`ctest -j`) can independently compute the
    # identical "linux_native_jail-0" id and collide creating the same cgroup/jail_root directory.
    # Observed directly in this harness's own `ctest -j 4` run before this property existed.
    set_tests_properties(test_native_jail_backend_linux PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    # ---- 008 §7 abuse-case corpus + §9 G2 positive controls (M2 Phase C task C3, Linux half) -----
    # fs-escape attempt is deliberately NOT included here -- see the test file's own header for the
    # tracked containment gap this surfaced.
    add_executable(test_native_jail_abuse_corpus_linux test_native_jail_abuse_corpus_linux.cpp)
    target_link_libraries(test_native_jail_abuse_corpus_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_abuse_corpus_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_abuse_corpus_linux hostile_child_posix)
    add_test(NAME test_native_jail_abuse_corpus_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_abuse_corpus_linux>")
    # The OOM case alone budgets wall_ms=25000 (C2-Linux's own measured worst case) -- 150s total
    # leaves headroom for the other four cases' shorter budgets plus real process/cgroup overhead.
    set_tests_properties(test_native_jail_abuse_corpus_linux PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)

    # ---- 008 §9 G1 cross-platform outcome-classification parity (M2 Phase C task C4, Linux half) --
    add_executable(test_native_jail_parity_linux test_native_jail_parity_linux.cpp)
    target_link_libraries(test_native_jail_parity_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_parity_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_parity_linux hostile_child_posix)
    add_test(NAME test_native_jail_parity_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_parity_linux>")
    # The oom case alone budgets wall_ms=25000 -- same rationale as the abuse-corpus test above;
    # 90s leaves headroom for the other four cases plus real process/cgroup overhead.
    set_tests_properties(test_native_jail_parity_linux PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    # ---- 008 §9 G3 no-ambient-authority probe (M2 Phase C task C5, Linux half) -------------------
    # fs axis is deliberately NOT included here -- see the test file's own header for why (same
    # tracked gap as C3's Linux fs-escape omission, GitHub issue #5).
    add_executable(test_native_jail_ambient_authority_linux test_native_jail_ambient_authority_linux.cpp)
    target_link_libraries(test_native_jail_ambient_authority_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_ambient_authority_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_ambient_authority_linux hostile_child_posix)
    add_test(NAME test_native_jail_ambient_authority_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_ambient_authority_linux>")
    set_tests_properties(test_native_jail_ambient_authority_linux PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    # ---- 008 §9 G2/G3 filesystem containment (pivot_root + bind-mount jail, Linux half) ------------
    # docs/planning/linux-native-jail-pivot-root-containment-design-draft.md's design + self-
    # red-team record, now built and proven: fs-escape (Windows abuse corpus's Case 4 analogue) plus
    # a read-only-bind-mount positive/negative pair (MUST-FIX 2).
    add_executable(test_native_jail_fs_containment_linux test_native_jail_fs_containment_linux.cpp)
    target_link_libraries(test_native_jail_fs_containment_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_fs_containment_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_fs_containment_linux hostile_child_posix)
    add_test(NAME test_native_jail_fs_containment_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_fs_containment_linux>")
    set_tests_properties(test_native_jail_fs_containment_linux PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    # ---- 008 §9 G4 teardown-cycle census (M2 Phase C task C6, Linux half) --------------------------
    add_executable(test_native_jail_teardown_cycles_linux test_native_jail_teardown_cycles_linux.cpp)
    target_link_libraries(test_native_jail_teardown_cycles_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_teardown_cycles_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_teardown_cycles_linux hostile_child_posix)
    add_test(NAME test_native_jail_teardown_cycles_linux
      COMMAND bash -c "source ${AE_TESTS_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_teardown_cycles_linux>")
    set_tests_properties(test_native_jail_teardown_cycles_linux PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endif()
endif()

if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  set(AE_PYTHON_TEST_ENV "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")

  # ---- Jailed-Python-worker design (final spec, 2026-08-23): the minimum-slice PROVE-phase positive
  # controls (G-Q1/G-Q2/G-lifecycle-2) -- a runaway CPU loop and a memory bomb, run against the REAL
  # agentengine_python_worker.exe, are actually caught/killed by the new session-scoped watchdog +
  # Job Object cap, closing component-role-audit-tracker.md's Finding Q for real.
  agentengine_add_test(test_native_jail_python_worker_slice1
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_native_jail_python_worker_slice1 PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- HandleRelay (ADR-081 Slice 2, decisions/ADR-085-jailed-python-worker-slice-2-handle-relay.md):
  # the two positive/negative controls the design draft's own §6 named but the pre-existing suite never
  # exercises -- a real successful socket relay round trip under a genuinely granted cap::NetOut, and a
  # kernel-level (not merely Python-wrapper-level) read-only enforcement check.
  agentengine_add_test(test_native_jail_python_worker_handle_relay
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_native_jail_python_worker_handle_relay PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- 008 SS9 G7 (session boundary) -- decisions/ADR-082-native-jail-promotion-gate-008-9.md SS4:
  # a real, previously-untracked gap. Two jailed worker processes, live at once, prove a canary
  # planted in one is unreachable from the other -- the kernel-enforced OS-process boundary Slice 1
  # gave this claim, exercised for real for the first time.
  agentengine_add_test(test_native_jail_session_boundary_windows
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_native_jail_session_boundary_windows PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()

# ---- ADR-134 independent red-team round -- tools/durable_sandboxed_shell_chat.cpp had ZERO automated
# coverage of its own (ADR-134 §3's own evidence was a human manually re-running the compiled binary
# twice and eyeballing the filesystem). This turns that same claim into a permanent regression guard:
# invokes the REAL, already-built agentengine_durable_sandboxed_shell_chat.exe TWICE via std::system(),
# as two genuinely separate OS process invocations, and checks THE CORE CLAIM directly on real disk (the
# on-disk tree-object count stays at exactly 1, not 2). Gated identically to the tool target itself
# (AGENTENGINE_WITH_HTTPS) -- the target this test invokes does not exist otherwise.
if(AGENTENGINE_WITH_HTTPS)
  add_executable(test_durable_sandboxed_shell_chat_cross_process
    test_durable_sandboxed_shell_chat_cross_process.cpp)
  target_link_libraries(test_durable_sandboxed_shell_chat_cross_process PRIVATE agentengine_warnings)
  target_compile_definitions(test_durable_sandboxed_shell_chat_cross_process PRIVATE
    AE_DURABLE_SHELL_CHAT_EXE="$<TARGET_FILE:agentengine_durable_sandboxed_shell_chat>")
  add_dependencies(test_durable_sandboxed_shell_chat_cross_process agentengine_durable_sandboxed_shell_chat)
  add_test(NAME test_durable_sandboxed_shell_chat_cross_process
    COMMAND test_durable_sandboxed_shell_chat_cross_process)
  # Two real subprocess launches plus real filesystem I/O -- generous but finite, matching this design
  # line's own established TIMEOUT posture for cross-process proofs of this shape.
  set_tests_properties(test_durable_sandboxed_shell_chat_cross_process PROPERTIES TIMEOUT 60)
endif()
