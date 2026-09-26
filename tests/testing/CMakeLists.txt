# tests/testing/: test-only support that ships in include/agentengine/testing/ and tools/test_driver/ (ADR-182).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "testing" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-182 P1 + P2: the shared strict testing::ScriptedChatClient, and approval_requested carrying
# tool name / arguments / needs_approval (BUG-3, BUG-1 info).
agentengine_add_test(test_scripted_chat_client_and_approval_payload
  LIBS agentengine::core agentengine_warnings)

# ---- ADR-182 driver phase 1: tools/test_driver/test_driver.hpp's Driver, driven in-process through
# handle_line() with the exact JSON-RPC lines the stdio binary receives.
add_executable(test_agentengine_test_driver test_agentengine_test_driver.cpp)
target_include_directories(test_agentengine_test_driver PRIVATE ${PROJECT_SOURCE_DIR}/tools)
target_link_libraries(test_agentengine_test_driver PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agentengine_test_driver COMMAND test_agentengine_test_driver)
