cmake_minimum_required(VERSION 3.28)
project(refund_service LANGUAGES CXX)

# The engine is C++23 (CLAUDE.md / 021 §5) and is vendored, never installed -- the consumer's own
# language level has to match it.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------------------------
# 1. Turn the engine's own developer build OFF *before* add_subdirectory(). This is the one
#    consumption step the docs are explicit about (web/marketing/src/components/
#    ApiFirstAppReference.tsx:83-108): without it the consumer inherits the engine's whole test
#    suite and a network fetch for its vendored dependencies -- measured there as 2,336 build
#    statements instead of 159, and 49 s of configure instead of 4 s.
#    A cache variable with FORCE is the idiom that works from a parent project; a plain set()
#    would be shadowed by the engine's own option().
# ---------------------------------------------------------------------------------------------
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Everything else stays at its default: offline, no HTTPS, no WASM, no Python runner, no PDF.
# The program below needs none of them -- it talks to a deterministic stand-in for a model.
# (Reaching a real vendor needs -DAGENTENGINE_WITH_HTTPS=ON *and*
#  target_link_libraries(... agentengine::provider_http_client); the docs say so at
#  ApiFirstAppReference.tsx:103-108 and describe the misleading compile error you get without it.)

# ---------------------------------------------------------------------------------------------
# 2. Point this at wherever you put the repository. There is no find_package(AgentEngine) and no
#    versioned release to install (ApiFirstAppReference.tsx:169-172).
# ---------------------------------------------------------------------------------------------
set(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Vendored AgentEngine repository root")
add_subdirectory("${AGENTENGINE_ROOT}" agentengine)

# ---------------------------------------------------------------------------------------------
# 3. One executable, one engine target. agentengine::core brings the engine's own static libraries
#    with it (ApiFirstAppReference.tsx:99-101).
# ---------------------------------------------------------------------------------------------
add_executable(refund_agent main.cpp)
target_link_libraries(refund_agent PRIVATE agentengine::core)
