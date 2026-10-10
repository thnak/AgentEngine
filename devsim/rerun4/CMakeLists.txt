# devsim/rerun4/CMakeLists.txt
#
# Built from the documented consumer CMakeLists.txt on api/first-app.html ("How to depend on the
# engine" + "Point it at a real model"), with exactly two changes: AGENTENGINE_ROOT points at this
# repository rather than a vendored third_party/ copy, and the two APPENDED lines the docs keep in a
# comment are uncommented, because this program really does reach a real model.
#
# Configure and build (the documented two commands, plus the documented HTTPS flag):
#   cmake -S . -B build -DAGENTENGINE_WITH_HTTPS=ON
#   cmake --build build --target ae_order_assistant -j 4
#   ./build/ae_order_assistant

cmake_minimum_required(VERSION 3.28)
project(ae_order_assistant LANGUAGES CXX)

# Turn the engine's own developer build off BEFORE adding it, or you inherit its whole test suite
# (and a network fetch for its vendored dependencies). A cache variable with FORCE is the idiom that
# works from a parent project; a plain set() would be shadowed by the engine's own option().
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# The engine is vendored, not installed. Here it IS this repository, two directories up.
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    CACHE PATH "Path to the vendored AgentEngine repository")
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

add_executable(ae_order_assistant main.cpp)

# One target brings the engine's own static libraries with it.
target_link_libraries(ae_order_assistant PRIVATE agentengine::core)

# Reaching a real model needs one more target AND one more configure flag. AGENTENGINE_WITH_HTTPS is
# propagated as a compile DEFINITION by agentengine_net_egress_proxy, which core does not link, so
# without provider_http_client the failure is "error C2653: 'anthropic' is not a class or namespace
# name" -- a compile error that looks nothing like a missing link line.
target_link_libraries(ae_order_assistant PRIVATE agentengine::provider_http_client)
