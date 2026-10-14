cmake_minimum_required(VERSION 3.28)
project(myapp LANGUAGES CXX)

# Exactly the recipe documented on api/first-app.html, including the path variable -- verifying
# that the page's CMake works as printed, not just in principle.
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# The engine is vendored, not installed. Keep the path in a variable: a submodule move should not
# mean editing the add_subdirectory() line too.
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    CACHE PATH "Path to the vendored AgentEngine repository")
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE agentengine::core)
