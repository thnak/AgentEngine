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
# Proves PythonRunner/ShellRunner (src/backends/native_jail/) satisfy Runner and fail closed — see
# the file's own top comment for exactly what is and is not covered.
add_executable(test_native_jail_runner_stubs test_native_jail_runner_stubs.cpp)
target_link_libraries(test_native_jail_runner_stubs PRIVATE agentengine::core agentengine_warnings)
target_include_directories(test_native_jail_runner_stubs PRIVATE "${CMAKE_SOURCE_DIR}/src")

add_test(NAME test_native_jail_runner_stubs COMMAND test_native_jail_runner_stubs)
