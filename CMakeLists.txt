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

# ---- native_jail ShellRunner backend (src/backends/native_jail/) --------------
# Implements decisions/ADR-001-shellrunner-grammar-and-dispatch.md — Design A: the
# recursive-descent parser, the pmr-backed AST, the tree-walking evaluator, the fixed builtin
# table, CommandRegistry, and RealFileSystemAdapter. A STATIC library (not header-only) so Sh-S1's
# static "zero references to a process-creation primitive" check (ADR-001 §7 finding 1's fix) can
# be stated at link-target granularity: this library links ONLY against agentengine::core (header
# -only vocabulary + Quark) and the C++ standard library — nothing here can pull in an object that
# defines CreateProcess/_wspawnv/system/LoadLibrary/posix_spawn/exec*/fork by construction of what
# it's linked against, which tests/test_shell_runner_proof.cpp verifies against the actual built
# artifact, not against prose.
add_library(agentengine_shell_runner STATIC
  src/backends/native_jail/shell_parser.cpp
  src/backends/native_jail/shell_dispatch.cpp
  src/backends/native_jail/real_filesystem_adapter.cpp)
target_include_directories(agentengine_shell_runner PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agentengine_shell_runner PUBLIC agentengine::core)
target_link_libraries(agentengine_shell_runner PRIVATE agentengine_warnings)
add_library(agentengine::shell_runner ALIAS agentengine_shell_runner)

# ---- Tests --------------------------------------------------------------------
option(AGENTENGINE_BUILD_TESTS "Build AgentEngine tests" ON)
if(AGENTENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
