cmake_minimum_required(VERSION 3.28)
project(docpipeline LANGUAGES CXX)

# ---------------------------------------------------------------------------------------------
# Built from the documented consumer recipe on web/marketing/src/components/ApiFirstAppReference.tsx
# (the SNIPPET_CMAKE block, lines 80-114 of that file). Deviations are marked "DEVIATION" and each
# one is logged in REPORT.md's fallback log.
# ---------------------------------------------------------------------------------------------

# Documented: turn the engine's own developer build off BEFORE add_subdirectory(), or you inherit
# its whole test suite and a network fetch for its vendored dependencies. A cache variable with
# FORCE is the idiom that works from a parent project.
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# DEVIATION (environment, not a doc gap): the engine is the repository root here, not a vendored
# third_party/AgentEngine. AGENTENGINE_ROOT stays a cache PATH exactly as documented.
if(NOT DEFINED AGENTENGINE_ROOT)
  set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to the vendored AgentEngine repository")
endif()
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

add_executable(docpipeline main.cpp)

# Documented: one target for an offline program; agentengine::core brings the engine's own static
# libraries with it (including agentengine_rt_workflow).
target_link_libraries(docpipeline PRIVATE agentengine::core)

# Documented: do NOT link agentengine_warnings (/W4 /WX) from a consumer.
