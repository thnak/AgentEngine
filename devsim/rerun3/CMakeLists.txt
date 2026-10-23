cmake_minimum_required(VERSION 3.28)
project(docpipe LANGUAGES CXX)

# Written the way the docs site says a CONSUMER should wire the engine in
# (ApiFirstAppReference.tsx, "CMakeLists.txt" panel + "Vendored, not installed").
#
# 1. Turn the engine's own developer build off BEFORE adding it, or you inherit its
#    whole test suite (and a network fetch for its vendored dependencies). A cache
#    variable with FORCE is the idiom that works from a parent project; a plain
#    set() would be shadowed by the engine's own option().
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 2. The engine is vendored, not installed: point this at wherever you put the
#    repository. The docs panel writes `add_subdirectory(third_party/AgentEngine agentengine)`;
#    here the vendored copy is the repository this program lives inside.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. agentengine)

add_executable(docpipe main.cpp)

# 3. One target. agentengine::core brings the engine's own static libraries with it.
target_link_libraries(docpipe PRIVATE agentengine::core)
