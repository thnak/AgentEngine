# tests/backends/native_process/: native unsandboxed process-execution providers (src/backends/native_process/, ADR-071).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "backends/native_process" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- native_process backend (decisions/ADR-071-native-unsandboxed-process-execution-providers.md)
# Only built when AGENTENGINE_WITH_NATIVE_PROCESS is explicitly turned ON (root CMakeLists.txt) --
# matching AGENTENGINE_BUILD_PYTHON_RUNNER/AGENTENGINE_WITH_WASM's own gating pattern. Windows-only
# for now (native_process_spawn/native_worktree_bridge have no POSIX implementation yet, a named,
# honest gap -- see root CMakeLists.txt's own AGENTENGINE_WITH_NATIVE_PROCESS block).
if(AGENTENGINE_WITH_NATIVE_PROCESS AND WIN32)
  agentengine_add_test(test_native_process_spawn
    LIBS agentengine::native_process_backend agentengine_warnings)

  agentengine_add_test(test_native_path_scan LIBS agentengine::native_process_backend agentengine_warnings)

  agentengine_add_test(test_native_worktree_bridge
    LIBS agentengine::native_process_backend agentengine_warnings)

  agentengine_add_test(test_native_providers
    LIBS agentengine::core agentengine::native_process_backend agentengine_warnings)

  agentengine_add_test(test_native_capability_announcer
    LIBS agentengine::core agentengine::native_process_backend agentengine_warnings)
endif()
