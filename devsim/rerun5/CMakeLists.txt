# devsim/rerun5/CMakeLists.txt -- consumer build for the refund back-office service.
#
# Shape and option names come from the docs: web/marketing/src/components/ApiFirstAppReference.tsx
# ("How to depend on the engine" + its CMakeLists.txt snippet). Nothing here is invented.

cmake_minimum_required(VERSION 3.28)
project(rerun5_refunds LANGUAGES CXX)

# Docs: set these BEFORE add_subdirectory() or the consumer inherits the engine's whole developer
# build (tests + examples + a network fetch for its vendored dependencies).
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Docs: there is no find_package(AgentEngine) and no installed release; the engine is vendored and
# added as a subdirectory. Keep the path in a variable.
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    CACHE PATH "Path to the vendored AgentEngine repository")
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

add_executable(rerun5_refunds main.cpp)

# Docs: one target -- agentengine::core brings the engine's own static libraries with it.
target_link_libraries(rerun5_refunds PRIVATE agentengine::core)
