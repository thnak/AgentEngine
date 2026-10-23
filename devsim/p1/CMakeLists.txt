# devsim/p1/CMakeLists.txt -- how a CONSUMER wires the vendored AgentEngine into its own build.
#
# Everything in this file was assembled from the engine's own build files, because the documentation
# site never documents consumer-side CMake: web/marketing/src/data/content.ts's `buildSteps` covers
# only "clone the repo and build the engine itself". See REPORT.md -> Fallback log.
#
# The four things a consumer has to know, and where each came from:
#   1. agentengine::core is the one INTERFACE target that carries the include dir and transitively
#      every compiled library a session needs.            (CMakeLists.txt:92-103,230,246, ...)
#   2. The real Anthropic/OpenAI backends exist ONLY under AGENTENGINE_WITH_HTTPS, which is OFF by
#      default, and they need agentengine::provider_http_client + the three MbedTLS targets.
#                                                          (CMakeLists.txt:674-822, examples/CMakeLists.txt:246-256)
#   3. AGENTENGINE_BUILD_TESTS / AGENTENGINE_BUILD_EXAMPLES must be set BEFORE add_subdirectory(),
#      or you build the engine's own ~400-TU test suite as a side effect of your app.
#                                                          (CMakeLists.txt:1433-1444)
#   4. AGENTENGINE_WITH_HTTPS fetches pinned mbedTLS/Asio/CA-bundle sources at configure time, and
#      each has a documented offline escape hatch (AGENTENGINE_VENDOR_*=OFF + a *_ROOT/PATH).
#                                                          (CMakeLists.txt:185-210,674-764)

cmake_minimum_required(VERSION 3.28)
project(order_assistant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Where the vendored engine lives. Default: two levels up from devsim/p1.
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Root of the vendored AgentEngine checkout")
if(NOT EXISTS "${AGENTENGINE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "AGENTENGINE_ROOT ('${AGENTENGINE_ROOT}') is not an AgentEngine checkout")
endif()

# ---- Engine build options: these MUST precede add_subdirectory() --------------------------------
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# The real provider backends (protocol/anthropic/chat_client.hpp, protocol/openai/chat_client.hpp)
# are inside `#ifdef AGENTENGINE_WITH_HTTPS`. Without this, AnthropicChatClient does not exist.
set(AGENTENGINE_WITH_HTTPS     ON  CACHE BOOL "" FORCE)

add_subdirectory("${AGENTENGINE_ROOT}" "${CMAKE_BINARY_DIR}/agentengine" EXCLUDE_FROM_ALL)

# ---- The application ---------------------------------------------------------------------------
add_executable(order_assistant main.cpp)

# NOMINMAX / WIN32_LEAN_AND_MEAN the same way the engine sets them project-wide for itself
# (CMakeLists.txt:73-75); a consumer TU that reaches <windows.h> through an engine header needs them.
if(WIN32)
  target_compile_definitions(order_assistant PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# /bigobj: coroutine-heavy AgentEngine TUs blow the COFF section limit -- the engine sets this on its
# own coroutine-heavy libraries (CMakeLists.txt:241-244) and a consumer instantiation needs it too.
if(MSVC)
  target_compile_options(order_assistant PRIVATE /bigobj /EHsc)
endif()

target_link_libraries(order_assistant PRIVATE
  agentengine::core                 # include dir + every compiled library a session needs
  agentengine::provider_http_client # the Anthropic/OpenAI backend bodies (HTTPS-gated)
  MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)

# Offline check that the declared tool really runs -- the same declaration, driven by a scripted
# in-process ChatClient instead of a live backend. OFF by default: it is a dev/CI aid, not the product.
option(ORDER_ASSISTANT_BUILD_OFFLINE_CHECK "Build the offline tool-call verifier" OFF)
if(ORDER_ASSISTANT_BUILD_OFFLINE_CHECK)
  add_executable(order_assistant_verify_tool_call verify_tool_call.cpp)
  target_link_libraries(order_assistant_verify_tool_call PRIVATE agentengine::core)
  if(MSVC)
    target_compile_options(order_assistant_verify_tool_call PRIVATE /bigobj /EHsc)
  endif()
endif()

# Deliberately NOT linked: `agentengine_warnings`, the engine's own INTERFACE warning target
# (cmake/AgentEngineWarnings.cmake, which is `/W4 /WX` and `-Wall -Wextra -Werror`). That is the
# engine's internal warning posture for its own sources, not something an application should inherit.
