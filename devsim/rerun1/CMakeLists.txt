cmake_minimum_required(VERSION 3.28)
project(order_assistant LANGUAGES CXX)

# ------------------------------------------------------------------------------------------------
# Wiring a consumer onto a VENDORED AgentEngine.
# Sources: web/marketing/src/components/ApiFirstAppReference.tsx (SNIPPET_CMAKE, lines 80-108)
#          api/first-app.html#depend
# There is no find_package(AgentEngine); the engine is add_subdirectory()'d.
# ------------------------------------------------------------------------------------------------

# Turn the engine's own developer build off BEFORE adding it (ApiFirstAppReference.tsx:83-87).
# These must be CACHE variables with FORCE: a plain set() would be shadowed by the engine's own
# option() declaration.
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Reaching a real model needs the HTTPS gate (ApiFirstAppReference.tsx:103-108): both vendor
# backends sit behind AGENTENGINE_WITH_HTTPS, which defaults OFF. Same FORCE-before-
# add_subdirectory idiom as the two options above.
set(AGENTENGINE_WITH_HTTPS ON CACHE BOOL "" FORCE)

# The engine is vendored, not installed: point this at wherever the repository lives. The default
# here is this file's own grandparent (devsim/rerun1/../.. == the AgentEngine repository root).
if(NOT DEFINED AGENTENGINE_ROOT)
    get_filename_component(AGENTENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
message(STATUS "AgentEngine vendored at: ${AGENTENGINE_ROOT}")

add_subdirectory("${AGENTENGINE_ROOT}" "${CMAKE_BINARY_DIR}/agentengine")

add_executable(order_assistant main.cpp)

# agentengine::core brings the engine's own static libraries with it
# (ApiFirstAppReference.tsx:99-101). provider_http_client is the second target a real model needs
# (:103-108) -- without it the failure is a compile error, "'anthropic' is not a class or namespace
# name", because the HTTPS macro is propagated as a compile definition by a target core does not
# link.
target_link_libraries(order_assistant PRIVATE
    agentengine::core
    agentengine::provider_http_client)
