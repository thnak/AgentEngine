# Correctness gate (CONVENTIONS.md: "tests/ — correctness gate, one per load-bearing invariant").
# smoke_vocabulary proves the abstract-vocabulary headers under include/agentengine/ actually
# compile together and that the declared concepts (ChatClient, Runner, SandboxBackend) are
# satisfiable by a conforming type.

add_executable(smoke_vocabulary smoke_vocabulary.cpp)
target_link_libraries(smoke_vocabulary PRIVATE agentengine::core agentengine_warnings)

add_test(NAME smoke_vocabulary COMMAND smoke_vocabulary)

# ---- RecordedChatClient fixture player (test-only) ---------------------------------------------
# Proves the ChatClient concept and ChatRequest/ChatResponse vocabulary are usable end-to-end
# against hand-authored JSON fixtures, without a real network call or API key (CLAUDE.md task
# brief; concept: 004-Model-Provider-Plane.md §6). nlohmann/json is a test-only dependency per
# CONVENTIONS.md's dependency-tier discipline: FetchContent-ed here (not in the root CMakeLists.txt)
# and linked only to this test target — never to agentengine_core or anything under
# include/agentengine/core.
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(test_recorded_chat_client test_recorded_chat_client.cpp)
target_link_libraries(test_recorded_chat_client PRIVATE
  agentengine::core agentengine_warnings nlohmann_json::nlohmann_json)
target_compile_definitions(test_recorded_chat_client PRIVATE
  AE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chat_client")

add_test(NAME test_recorded_chat_client COMMAND test_recorded_chat_client)

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
add_executable(test_real_filesystem_adapter test_real_filesystem_adapter.cpp)
target_link_libraries(test_real_filesystem_adapter PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_real_filesystem_adapter COMMAND test_real_filesystem_adapter)

# End-to-end ShellRunner claims: Sh-C1, Sh-C2, Sh-C3, Sh-G4, kind-only capability checks, A-C2,
# Sh-S1's behavioral half.
add_executable(test_shell_runner_proof test_shell_runner_proof.cpp)
target_link_libraries(test_shell_runner_proof PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_shell_runner_proof COMMAND test_shell_runner_proof)

# A-S1: parser safety against a hand-written adversarial corpus. Fake-free — links only against
# the parser, not the dispatch/fs layer, matching the ADR's own "parse() needs nothing but a byte
# string" claim.
add_executable(test_shell_parser_adversarial test_shell_parser_adversarial.cpp)
target_link_libraries(test_shell_parser_adversarial PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_shell_parser_adversarial COMMAND test_shell_parser_adversarial)
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
# granularity fix). Checked via llvm-nm against the actual built artifact; skips (CTest
# SKIP_RETURN_CODE) if llvm-nm isn't found rather than silently passing.
find_program(AE_LLVM_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe)
add_executable(test_shell_runner_no_process_creation test_shell_runner_no_process_creation.cpp)
target_link_libraries(test_shell_runner_no_process_creation PRIVATE
  agentengine::core agentengine_warnings)
target_compile_definitions(test_shell_runner_no_process_creation PRIVATE
  AE_SHELL_RUNNER_LIB_PATH="$<TARGET_FILE:agentengine_shell_runner>"
  AE_LLVM_NM_PATH="${AE_LLVM_NM_EXECUTABLE}")
add_test(NAME test_shell_runner_no_process_creation COMMAND test_shell_runner_no_process_creation)
set_tests_properties(test_shell_runner_no_process_creation PROPERTIES SKIP_RETURN_CODE 77)

# ---- Job Object resource-limit layer (decisions/ADR-004-appcontainer-native-jail-windows-backend.md) --
# Windows only -- see CMakeLists.txt's agentengine_job_object_limits target comment.
if(WIN32)
  # Deliberately hostile test-only helper process (tests/helpers/hostile_child.cpp) the Job Object
  # tests launch and try to contain -- never a product target, never linked into anything else.
  add_executable(job_object_hostile_child helpers/hostile_child.cpp)

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
endif()

# ---- Capability bearer-token prove phase (decisions/ADR-005-capability-bearer-tokens-cross-process.md)
# Resolves OQ-3. Design A (capability_token.hpp) correctness + red-team, Design B
# (capability_registry.hpp) red-team, and the verify()-vs-check() cost comparison §5 rests on.
if(WIN32)
  add_executable(test_capability_token_proof test_capability_token_proof.cpp)
  target_link_libraries(test_capability_token_proof PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_proof COMMAND test_capability_token_proof)

  add_executable(test_capability_token_redteam test_capability_token_redteam.cpp)
  target_link_libraries(test_capability_token_redteam PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_redteam COMMAND test_capability_token_redteam)

  add_executable(test_capability_token_benchmark test_capability_token_benchmark.cpp)
  target_link_libraries(test_capability_token_benchmark PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_benchmark COMMAND test_capability_token_benchmark)
endif()

# ---- PythonRunner prove-phase gates (decisions/ADR-002-pythonrunner-embedding-and-mediation.md)
# Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON (CMakeLists.txt) -- these link a real
# CPython import lib and need python3<minor>.dll reachable via PATH at run time (Windows has no
# rpath), wired below via each test's ENVIRONMENT property.
if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  set(AE_PYTHON_TEST_ENV "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")
  # Every one of these tests spawns a real CPython interpreter and, in some cases, imports numpy/
  # pandas -- bounded per CLAUDE.md's Machine Safety section (a hung or runaway interpreter must
  # not be able to take the dev box down) rather than left to CTest's default (no timeout).

  # Priority item 1: a minimal embedded CPython interpreter actually runs, linked against the
  # configured CPython install, through PythonRunner's real (non-stub) `run()`.
  add_executable(test_python_embed_smoke test_python_embed_smoke.cpp)
  target_link_libraries(test_python_embed_smoke PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_embed_smoke PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_embed_smoke COMMAND test_python_embed_smoke)
  set_tests_properties(test_python_embed_smoke PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 2: Layer 0's sys.modules sweep, its measured scope on this target, and the
  # _imp-removal/reimport findings (ADR-002 §6 items 4/7). Talks to CPython directly (no
  # PythonLockdownInterpreter, no meta-path finder) -- see the file's own header comment for why.
  add_executable(test_python_layer0_sweep test_python_layer0_sweep.cpp)
  target_include_directories(test_python_layer0_sweep PRIVATE "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(test_python_layer0_sweep PRIVATE
    agentengine::core "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(test_python_layer0_sweep PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  add_test(NAME test_python_layer0_sweep COMMAND test_python_layer0_sweep)
  set_tests_properties(test_python_layer0_sweep PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 3: the real sys.meta_path finder -- claims A1, A3, plus the §3.4 item 3 / §5.5.2
  # per-call reassertion mechanism (A4).
  add_executable(test_python_meta_path_finder test_python_meta_path_finder.cpp)
  target_link_libraries(test_python_meta_path_finder PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_meta_path_finder PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_meta_path_finder COMMAND test_python_meta_path_finder)
  set_tests_properties(test_python_meta_path_finder PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 4: claim A2 -- numpy/pandas actually importing and computing under the
  # locked-down interpreter, with the measured real dependency closure explicitly allowlisted.
  # Longer timeout: numpy/pandas's own import-time work (compiled ufunc table setup, etc.) is
  # noticeably slower than the other tests' trivial scripts.
  add_executable(test_python_numpy_pandas_import test_python_numpy_pandas_import.cpp)
  target_link_libraries(test_python_numpy_pandas_import PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_numpy_pandas_import PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  add_test(NAME test_python_numpy_pandas_import COMMAND test_python_numpy_pandas_import)
  set_tests_properties(test_python_numpy_pandas_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Stretch item (§4 claim C1, partial/observational -- see the test file's own header for exactly
  # what is and is not proven): the native PySys_AddAuditHook-installed hook.
  add_executable(test_python_audit_hook test_python_audit_hook.cpp)
  target_link_libraries(test_python_audit_hook PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_audit_hook PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_audit_hook COMMAND test_python_audit_hook)
  set_tests_properties(test_python_audit_hook PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- decisions/ADR-003-caller-aware-import-gating.md prove-phase gates (Design B, revised) ----

  # Claims B1-B8, B10, B11, B13 (primary half): the caller-gated import tier end to end, against
  # the same real numpy/pandas target ADR-002 already measured, with ctypes/_ctypes/winreg/_wmi/
  # _winapi/subprocess moved into caller_gated_modules.
  add_executable(test_python_caller_gated_import test_python_caller_gated_import.cpp)
  target_link_libraries(test_python_caller_gated_import PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_caller_gated_import PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}"
    # B14 (added during this prove pass's own verification, not in the ADR's original §4 table):
    # a trusted package using importlib.import_module() -- not the `import` statement -- for a
    # not-yet-cached gated name. See python_lockdown.cpp's g_importlib_dict comment.
    AE_PYTHON_CALLER_GATED_TRUSTED_PROBE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/python_caller_gated_trusted_probe")
  add_test(NAME test_python_caller_gated_import COMMAND test_python_caller_gated_import)
  set_tests_properties(test_python_caller_gated_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Claim B9 (performance): stack walk vs. O(1) allowed-name lookup, N fresh imports each,
  # p50/p99. Uses tiny synthetic fixture modules (tests/fixtures/python_caller_gated_bench/), not
  # numpy/pandas, so the two conditions differ ONLY in which tier decides the name is allowed.
  add_executable(test_python_caller_gated_benchmark test_python_caller_gated_benchmark.cpp)
  target_link_libraries(test_python_caller_gated_benchmark PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_caller_gated_benchmark PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_CALLER_GATED_BENCH_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/python_caller_gated_bench")
  add_test(NAME test_python_caller_gated_benchmark COMMAND test_python_caller_gated_benchmark)
  set_tests_properties(test_python_caller_gated_benchmark PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Claim B13, second half: lockdown_identity_intact() must detect a TrustedLoaderProxy built as a
  # mutable heap type (no Py_TPFLAGS_IMMUTABLETYPE). Needs its OWN compilation of
  # python_lockdown.cpp with AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY defined -- a separate static
  # library, never linked into agentengine_python_runner (the real backend), so the real target's
  # own §3.3.4 construction is never at risk of picking up this test-only branch by accident.
  add_library(agentengine_python_runner_heap_proxy_probe STATIC
    "${CMAKE_SOURCE_DIR}/src/backends/native_jail/python_lockdown.cpp")
  target_include_directories(agentengine_python_runner_heap_proxy_probe PUBLIC
    "${CMAKE_SOURCE_DIR}/src")
  target_include_directories(agentengine_python_runner_heap_proxy_probe PRIVATE
    "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(agentengine_python_runner_heap_proxy_probe PUBLIC agentengine::core)
  target_link_libraries(agentengine_python_runner_heap_proxy_probe PRIVATE
    "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(agentengine_python_runner_heap_proxy_probe PRIVATE
    AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY)

  add_executable(test_python_trusted_loader_proxy_heap_type_negative_control
    test_python_trusted_loader_proxy_heap_type_negative_control.cpp)
  target_link_libraries(test_python_trusted_loader_proxy_heap_type_negative_control PRIVATE
    agentengine::core agentengine_python_runner_heap_proxy_probe agentengine_warnings)
  target_compile_definitions(test_python_trusted_loader_proxy_heap_type_negative_control PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_trusted_loader_proxy_heap_type_negative_control
    COMMAND test_python_trusted_loader_proxy_heap_type_negative_control)
  set_tests_properties(test_python_trusted_loader_proxy_heap_type_negative_control PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()
