# devsim/p2 -- how I (a consumer of AgentEngine, not a contributor) wire the engine in.
#
# THE FINDING THIS FILE RECORDS: the published docs never state how to consume AgentEngine from
# another CMake project. web/marketing/src/data/content.ts:329 shows `cmake -S . -B build` from
# inside the engine's own checkout -- i.e. building the engine's own tests/examples -- and there is
# no `install(EXPORT)` rule anywhere in AgentEngine's root CMakeLists.txt, so `find_package` is not
# available either. The only two wiring options were:
#
#   (a) add_subdirectory(<AgentEngine>) and link the INTERFACE target `agentengine::core`, or
#   (b) compile against include/ and re-derive every static library the engine's INTERFACE target
#       would have pulled in (agentengine_rt_file_log, agentengine_pal_cpu, agentengine_message_json,
#       agentengine_chat_recording, agentengine_tool_pipeline, agentengine_rt_session,
#       agentengine_reactor_asio, agentengine_rt_workflow, agentengine_docker_surface, ...).
#
# (a) is what this file does; it is the only one that can stay correct as the engine grows. The fact
# that it had to be reverse-engineered from the engine's own root CMakeLists.txt is logged in
# REPORT.md under "Fallback log".

cmake_minimum_required(VERSION 3.28)  # the engine's own floor (its root CMakeLists.txt:4)
project(refund_approval_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Where the AgentEngine checkout (the *vendored* one) lives. Default assumes this directory sits at
# <AgentEngine>/devsim/p2.
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "AgentEngine checkout root")

# ------------------------------------------------------------------------------------------------
# The engine's own options, forced BEFORE add_subdirectory(). Every one of these is the engine's
# internal build policy leaking into the consumer's configure step -- a consumer should not have to
# know them, and the docs do not mention any of them. Setting AGENTENGINE_BUILD_TESTS=OFF is not
# optional: its default is ON, and turning it on drags the engine's whole test suite (and a GTest
# FetchContent download) into a consumer build.
# ------------------------------------------------------------------------------------------------
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_FAST_TESTS     OFF CACHE BOOL "" FORCE)

# The engine's default is to FetchContent a pinned standalone Asio (AGENTENGINE_VENDOR_ASIO=ON,
# CMakeLists.txt:189). A consumer that must build offline points AGENTENGINE_VENDOR_ASIO=OFF at an
# already-fetched copy -- the engine's own comment at CMakeLists.txt:187 is the only place this is
# written down. If a copy is already lying in one of the engine's own build trees, reuse it.
set(AGENTENGINE_ASIO_ROOT "${AGENTENGINE_ASIO_ROOT}" CACHE PATH "Pre-fetched Asio (see AGENTENGINE_VENDOR_ASIO)")
if(NOT AGENTENGINE_ASIO_ROOT AND EXISTS "${AGENTENGINE_ROOT}/build-dev/_deps/ae_vendored_asio-src/include/asio.hpp")
  set(AGENTENGINE_ASIO_ROOT "${AGENTENGINE_ROOT}/build-dev/_deps/ae_vendored_asio-src"
      CACHE PATH "Pre-fetched Asio (reused from an existing engine build tree)" FORCE)
endif()
if(AGENTENGINE_ASIO_ROOT)
  set(AGENTENGINE_VENDOR_ASIO OFF CACHE BOOL "" FORCE)
endif()

if(NOT EXISTS "${AGENTENGINE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "AGENTENGINE_ROOT='${AGENTENGINE_ROOT}' is not an AgentEngine checkout "
                      "(-DAGENTENGINE_ROOT=<path> to point at one)")
endif()

# EXCLUDE_FROM_ALL: the engine's targets must not be built unless something links them -- otherwise
# a consumer build compiles every shipped backend, including ones it will never use.
add_subdirectory("${AGENTENGINE_ROOT}" "${CMAKE_BINARY_DIR}/agentengine" EXCLUDE_FROM_ALL)

add_executable(refund_approval_demo main.cpp)

# `agentengine::core` is the engine's one public INTERFACE target (AgentEngine/CMakeLists.txt:103).
# It is header-only itself but propagates every static library the engine needs, so this single line
# is the whole link. agentengine_warnings is the engine's own /W4 /WX flag set -- linked here because
# every one of the engine's own 35 examples does it (AgentEngine/examples/CMakeLists.txt:12).
target_link_libraries(refund_approval_demo PRIVATE agentengine::core agentengine_warnings)
