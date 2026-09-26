# tests/python/: the embedded CPython interpreter and the mediated Python runner.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "python" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- agent.tools codegen: schema -> Python-source generation (Milestone 3 Phase G1, 026 §4/§5).
# Header-only (src/backends/native_jail/agent_tools_codegen.hpp), core-tier -- links only
# agentengine::core for ToolDescriptor/json::Value, no Python.h dependency at all. The generated
# source actually RUNNING is proven separately, Python-gated, below.
add_executable(test_agent_tools_codegen test_agent_tools_codegen.cpp)
target_include_directories(test_agent_tools_codegen PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
target_link_libraries(test_agent_tools_codegen PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_tools_codegen COMMAND test_agent_tools_codegen)

# ---- agent.files/agent.data codegen: fixed static Python source (Milestone 3 Phase G2, 026 §5).
# Header-only (src/backends/native_jail/agent_files_data_codegen.hpp), core-tier -- no Python.h
# dependency. The generated source actually RUNNING is proven separately, Python-gated, below.
add_executable(test_agent_files_data_codegen test_agent_files_data_codegen.cpp)
target_include_directories(test_agent_files_data_codegen PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
target_link_libraries(test_agent_files_data_codegen PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_files_data_codegen COMMAND test_agent_files_data_codegen)

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
  agentengine_add_test(test_python_embed_smoke
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_python_embed_smoke PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase E2's real PythonRunner (mediated_python_runner.{hpp,cpp}) ---------------
  agentengine_add_test(test_mediated_python_runner_smoke
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_smoke PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- decisions/ADR-154-agent-output-codeact-module.md -- GitHub issue #39, 026 §5's `agent.output`.
  # A real script calling agent.output.set(value) against the real agentengine_python_worker.exe,
  # round-tripping into ExecOutcome::structured_output_json; proves the opt-in gate, last-call-wins,
  # and no cross-execution leak on the same worker.
  agentengine_add_test(test_agent_output_codegen
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_agent_output_codegen PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- decisions/ADR-155-agent-progress-codeact-module.md -- GitHub issue #31's own three-piece gap.
  # A real script calling agent.progress.report(text) against the real agentengine_python_worker.exe,
  # proving ctx.report_progress() fires for real, the worker survives (never hits exec_session()'s
  # fail-closed terminate_worker() fallback), and multiple calls stream as multiple events.
  agentengine_add_test(test_agent_progress_codegen
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_agent_progress_codegen PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G1: agent.tools generated-code execution proof (026 §4/§5) --------------
  # Generation-only correctness (no CPython dependency) is test_agent_tools_codegen.cpp, above.
  agentengine_add_test(test_mediated_python_runner_agent_tools
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_agent_tools PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G2: agent.files/agent.data generated-code execution proof (026 §5) -------
  # Generation-only correctness (no CPython dependency) is test_agent_files_data_codegen.cpp, above.
  agentengine_add_test(test_mediated_python_runner_agent_files_data
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_agent_files_data PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Skills Phase 2: a real mounted skill, materialized to a real host directory, read by real
  # Python code through a real mediated open() -- closes the gap where every prior skills test only
  # proved mount_read from test-harness C++, never from inside a running sandbox. Needs
  # agentengine::mediated_shell_runner too, for skill_mount_materializer.hpp's MediatedFileSystemAdapter
  # dependency (transitively brings in agentengine::worktree_store, relinked explicitly for clarity).
  agentengine_add_test(test_mediated_python_runner_skill_mounts
    LIBS agentengine::core agentengine::mediated_python_runner agentengine::mediated_shell_runner
      agentengine::worktree_store agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_skill_mounts PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G4: error mapping (026 §3, §9 Q2) execution proof -----------------------
  agentengine_add_test(test_mediated_python_runner_error_mapping
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_error_mapping PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase E4: hostile-corpus/mediation proof (010 §9 G2/G7) -----------------------
  agentengine_add_test(test_mediated_python_runner_hostile_corpus
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_mediated_python_runner_hostile_corpus PROPERTIES
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

  # ADR-002 §6 item 5 / backlog item #39: does Py_NewInterpreterFromConfig (PEP 684) actually work
  # as a per-session isolation unit on this concrete build, and does numpy load inside one? Talks
  # to CPython directly, same reason and same pattern as test_python_layer0_sweep above.
  add_executable(test_python_subinterpreter_spike test_python_subinterpreter_spike.cpp)
  target_include_directories(test_python_subinterpreter_spike PRIVATE "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(test_python_subinterpreter_spike PRIVATE
    agentengine::core "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(test_python_subinterpreter_spike PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  # TWO separate ctest entries, not one -- a real finding this investigation made (see the file's
  # own header comment): a failed numpy import inside a subinterpreter crashes the process on
  # teardown, so "strict" and "legacy" must never share a process, matching CLAUDE.md's "a test
  # proving a fork bomb is contained must not be able to take the machine with it".
  add_test(NAME test_python_subinterpreter_spike_strict COMMAND test_python_subinterpreter_spike strict)
  set_tests_properties(test_python_subinterpreter_spike_strict PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
  add_test(NAME test_python_subinterpreter_spike_legacy COMMAND test_python_subinterpreter_spike legacy)
  set_tests_properties(test_python_subinterpreter_spike_legacy PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 3: the real sys.meta_path finder -- claims A1, A3, plus the §3.4 item 3 / §5.5.2
  # per-call reassertion mechanism (A4).
  agentengine_add_test(test_python_meta_path_finder
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_python_meta_path_finder PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 4: claim A2 -- numpy/pandas actually importing and computing under the
  # locked-down interpreter, with the measured real dependency closure explicitly allowlisted.
  # Longer timeout: numpy/pandas's own import-time work (compiled ufunc table setup, etc.) is
  # noticeably slower than the other tests' trivial scripts.
  agentengine_add_test(test_python_numpy_pandas_import
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
      AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  set_tests_properties(test_python_numpy_pandas_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Stretch item (§4 claim C1, partial/observational -- see the test file's own header for exactly
  # what is and is not proven): the native PySys_AddAuditHook-installed hook.
  agentengine_add_test(test_python_audit_hook
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_python_audit_hook PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- decisions/ADR-003-caller-aware-import-gating.md prove-phase gates (Design B, revised) ----

  # Claims B1-B8, B10, B11, B13 (primary half): the caller-gated import tier end to end, against
  # the same real numpy/pandas target ADR-002 already measured, with ctypes/_ctypes/winreg/_wmi/
  # _winapi/subprocess moved into caller_gated_modules.
  agentengine_add_test(test_python_caller_gated_import
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
      AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}"
      AE_PYTHON_CALLER_GATED_TRUSTED_PROBE_FIXTURES="${AE_TESTS_SOURCE_DIR}/fixtures/python_caller_gated_trusted_probe")
  set_tests_properties(test_python_caller_gated_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Claim B9 (performance): stack walk vs. O(1) allowed-name lookup, N fresh imports each,
  # p50/p99. Uses tiny synthetic fixture modules (tests/fixtures/python_caller_gated_bench/), not
  # numpy/pandas, so the two conditions differ ONLY in which tier decides the name is allowed.
  agentengine_add_test(test_python_caller_gated_benchmark
    LIBS agentengine::core agentengine::python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
      AE_PYTHON_CALLER_GATED_BENCH_FIXTURES="${AE_TESTS_SOURCE_DIR}/fixtures/python_caller_gated_bench")
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

  agentengine_add_test(test_python_trusted_loader_proxy_heap_type_negative_control
    LIBS agentengine::core agentengine_python_runner_heap_proxy_probe agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_python_trusted_loader_proxy_heap_type_negative_control PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()
