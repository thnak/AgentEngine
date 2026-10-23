# devsim/rerun7/CMakeLists.txt -- a consumer project that vendors the engine and links one target.
# Written from web/marketing/src/components/ApiFirstAppReference.tsx:80-123 (the "Your first
# AgentEngine program" page's own build file), with the vendored path pointed at this checkout.

cmake_minimum_required(VERSION 3.28)
project(docpipeline LANGUAGES CXX)

# Turn the engine's own developer build off BEFORE adding it, or you inherit its whole test suite
# (and a network fetch for its vendored dependencies). A cache variable with FORCE is the idiom that
# works from a parent project; a plain set() would be shadowed by the engine's own option().
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# The engine is vendored, not installed. The docs' own variable name, pointed at the checkout that
# contains this directory instead of a third_party/ submodule.
get_filename_component(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

add_executable(docpipeline main.cpp)

# One target, for this offline program. agentengine::core brings the engine's own static libraries
# with it. agentengine_warnings is deliberately NOT linked (ApiFirstAppReference.tsx:114-116).
target_link_libraries(docpipeline PRIVATE agentengine::core)
