# tests/core/agent/: agent registry, agent YAML and the reference agent.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/agent" ctest label for every test here (cmake/AgentEngineTests.cmake).

add_executable(smoke_vocabulary smoke_vocabulary.cpp)
target_link_libraries(smoke_vocabulary PRIVATE agentengine::core agentengine_warnings)

add_test(NAME smoke_vocabulary COMMAND smoke_vocabulary)

# ---- Reference-agent prompt assembly (M3 Phase H1, 026-Agent-Facing-Runtime-Surface.md §7/§8) --
# Header-only (core/reference_agent_prompt.hpp, core/token_estimate.hpp), core-tier, model-independent.
agentengine_add_test(test_reference_agent_prompt LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase B6 -- register_agent<A>()'s additive ChatClientRegistry parameter --------
agentengine_add_test(test_agent_registry_chat_client_registry LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase F3 -- 015 §2's Agent-document compiler (core\agent_yaml_compiler.hpp):
# compiles a parsed YAML Agent document to a real AgentMetadata, cross-checked against register_
# agent<A>()'s own C++-form output for the same agent (I6 in action) --------------------------
agentengine_add_test(test_agent_yaml_limits LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_agent_yaml_compiler LIBS agentengine::core agentengine_warnings)

# ---- Agent metadata compiler: register_agent<A>() (M2 Phase E task E2, 002 §6) -----------------
# Header-only (core/agent_registry.hpp), core-tier -- links only agentengine::core.
agentengine_add_test(test_agent_registry LIBS agentengine::core agentengine_warnings)

# ---- Agent tool invocation end-to-end (M2 Phase E task E3, the milestone's headline sentence) ---
# Header-only (core\agent_registry.hpp's invoke_agent_tool(), core\tool_pipeline.hpp), core-tier.
agentengine_add_test(test_agent_tool_invocation LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_agent_registry_sandbox_backend_registry LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  set(AE_PYTHON_TEST_ENV "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")

  # ---- Milestone 3 Phase H1: reference-agent task corpus (026 §8 G1) ------------------------------
  # Hand-authored RecordedChatClient fixtures (tests/fixtures/chat_client/reference_agent/) played
  # back and actually executed through the real MediatedPythonRunner -- needs nlohmann_json (the
  # fixture loader) on top of the usual mediated_python_runner link. Longer timeout: shares the
  # numpy/pandas closure, so pays the same import-time cost as test_python_numpy_pandas_import.
  agentengine_add_test(test_reference_agent_task_corpus
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
      nlohmann_json::nlohmann_json
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
      AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}"
      AE_TEST_FIXTURE_DIR="${AE_TESTS_SOURCE_DIR}/fixtures/chat_client/reference_agent")
  set_tests_properties(test_reference_agent_task_corpus PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # ---- Milestone 3 Phase H2: transparency-is-not-security containment invariance (026 §1a/§8 G4) --
  agentengine_add_test(test_reference_agent_containment_invariance
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_reference_agent_containment_invariance PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()
