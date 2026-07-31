# AgentEngine — root build. See CONVENTIONS.md for the binding coding contract (layout, dependency
# tiers, machine-safety build rules) and AgentEngineSpecification.md for the design this implements.

cmake_minimum_required(VERSION 3.24)
project(AgentEngine LANGUAGES CXX)

# ---- Standard ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AgentEngineWarnings)

# ---- Windows toolchain wiring -------------------------------------------------
# Mirrors third_party/quark/CMakeLists.txt's pattern: NOMINMAX/WIN32_LEAN_AND_MEAN project-wide so
# any TU that pulls in <windows.h> (directly or transitively, e.g. via a future sandbox backend)
# gets the lean, min/max-free definition before it does.
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---- Quark (submodule; never forked or patched in-tree, CLAUDE.md) -----------
# Force Quark's own test/bench suite off before pulling it in as a subdirectory — AgentEngine's
# build must not drag in Quark's CTest targets or benchmarks.
set(QUARK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_BENCH OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/quark)

# ---- Core vocabulary (header-only for now; no .cpp under include/agentengine) --
add_library(agentengine_core INTERFACE)
target_include_directories(agentengine_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(agentengine_core INTERFACE quark::quark)
target_compile_features(agentengine_core INTERFACE cxx_std_23)
add_library(agentengine::core ALIAS agentengine_core)

# ---- Tests --------------------------------------------------------------------
option(AGENTENGINE_BUILD_TESTS "Build AgentEngine tests" ON)
if(AGENTENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
