# devsim/p3/CMakeLists.txt -- how a CONSUMER wires AgentEngine into its own build.
#
# What the AgentEngine docs do NOT tell you (see REPORT.md's fallback log): the engine's root
# CMakeLists.txt has no install(), no install(EXPORT), and publishes no `AgentEngineConfig.cmake`,
# so there is no find_package(AgentEngine) to write. The only supported shapes today are
# add_subdirectory (a vendored checkout, what this file does) or FetchContent (the same thing with a
# download step in front of it). The link target, `agentengine::core`, is also undocumented on the
# marketing site -- it was found in the engine's own CMakeLists.txt:103 and its examples/CMakeLists.txt.

cmake_minimum_required(VERSION 3.28)

project(p3_doc_pipeline LANGUAGES CXX)

# ---- The vendored engine ------------------------------------------------------------------------
# 021 §5's floor is CMake >= 3.28 and C++23 throughout; state both here rather than inheriting them
# from whatever the host happens to default to.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
  "Path to a vendored AgentEngine checkout (the directory holding its root CMakeLists.txt)")

if(NOT EXISTS "${AGENTENGINE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "AGENTENGINE_ROOT ('${AGENTENGINE_ROOT}') has no CMakeLists.txt")
endif()

# ---- Turn off the engine's own deliverables -----------------------------------------------------
# These are AgentEngine's options, not ours. A consumer build must not compile the engine's ~400-file
# test suite or its 35 get-started examples. They are set with FORCE so a stale cache entry in a
# reused build directory cannot silently turn them back on.
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "AgentEngine: build tests"    FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "AgentEngine: build examples" FORCE)

# Optional tier-2 dependencies stay off; the workflow surface needs none of them. (These are the
# engine's defaults already -- stated explicitly so the consumer's intent is readable here.)
set(AGENTENGINE_WITH_HTTPS          OFF CACHE BOOL "AgentEngine: mbedTLS egress" FORCE)
set(AGENTENGINE_WITH_WASM           OFF CACHE BOOL "AgentEngine: wasmtime plugins" FORCE)
set(AGENTENGINE_BUILD_PYTHON_RUNNER OFF CACHE BOOL "AgentEngine: CPython runner" FORCE)
set(AGENTENGINE_WITH_PDF            OFF CACHE BOOL "AgentEngine: PDF tooling" FORCE)

# Vendored Asio: FetchContent by default, which needs network at configure time. Point
# AGENTENGINE_ASIO_ROOT at an existing Asio checkout and turn AGENTENGINE_VENDOR_ASIO off for an
# offline build, the override the engine's own comment at CMakeLists.txt:187-188 documents.
#   cmake -S . -B build -DAGENTENGINE_VENDOR_ASIO=OFF -DAGENTENGINE_ASIO_ROOT=<dir with include/asio.hpp>

add_subdirectory("${AGENTENGINE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_agentengine" EXCLUDE_FROM_ALL)

# ---- Our program --------------------------------------------------------------------------------
add_executable(p3_doc_pipeline main.cpp)

# agentengine::core is an INTERFACE target carrying the include/ path plus every compiled static
# library of the engine (CMakeLists.txt:92-103 and the INTERFACE links through line 262) -- including
# agentengine_rt_workflow, which holds WorkflowSupervisor's out-of-line bodies (CMakeLists.txt:237).
# There is no narrower documented target for "just the workflow surface".
target_link_libraries(p3_doc_pipeline PRIVATE agentengine::core)
