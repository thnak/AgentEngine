# Test and conformance tools. Moved here from the root CMakeLists.txt (#120 S2) so they can be built
# without optimization under AGENTENGINE_FAST_TESTS: none of them ships or is linked by anything.
# Shipped executables (cli_chat, the python/pdf workers, ...) stay in the root file, optimized.
agentengine_fast_build_this_directory()

# ---- Policy-reachability CI tool -- 007 §9 gate G6, M2 Phase F task F2 ------------------
# Enumerates {capability kind, tool, taint} against the real mechanical enforcement
# decisions/ADR-009-capability-set-enforcement-mechanism.md built (CapabilitySet::contains(),
# trust/capability.hpp) via include/agentengine/trust/policy_reachability.hpp's enumerator -- see
# that header's own top comment for the RFC-vs-code scope note (no declarative rule set exists yet,
# decision 4; taint is enumerated as the boolean the mechanism actually has, not 007 §5's
# aspirational graded levels). Header-only logic (agentengine::core is enough); no new dependency.
add_executable(agentengine_policy_reachability policy_reachability.cpp)
target_link_libraries(agentengine_policy_reachability PRIVATE agentengine::core agentengine_warnings)

# ---- MCP conformance client -- ADR-023 §10 Tier 1, 011 §10 G2 --------------------------
# The binary `conformance client --command` spawns. See mcp_conformance_client.cpp's own top
# comment for why the CLIENT role needs no listener and no fixture, and
# docs/research/2026-08-15-mcp-conformance-harness.md for the empirically confirmed harness contract.
# Links the real egress proxy (ADR-011/ADR-016) rather than a test double -- the point of this target
# is that the conformance number it produces is attributable to engine code.
add_executable(agentengine_mcp_conformance_client mcp_conformance_client.cpp)
target_link_libraries(agentengine_mcp_conformance_client
  PRIVATE agentengine::core agentengine::net_egress_proxy agentengine_warnings)

# ---- ADR-182 test driver -- an MCP server over stdio that drives in-process sessions with
# a scripted model, for a Claude Code test agent. A host executable like cli_chat: the engine library
# binds nothing. Built by default (ADR-182 §12 R11) -- it needs only agentengine::core, and it is
# never installed or linked by anything else.
add_executable(agentengine_test_driver agentengine_test_driver.cpp)
target_include_directories(agentengine_test_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agentengine_test_driver PRIVATE agentengine::core agentengine_warnings)
# ADR-182 §16: replays checked-in scenarios with the scripted model (no network); tests/CMakeLists.txt
# registers one ctest per tests/scenarios/*.json.
add_executable(agentengine_scenario_runner agentengine_scenario_runner.cpp)
target_include_directories(agentengine_scenario_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agentengine_scenario_runner PRIVATE agentengine::core agentengine_warnings)
if(MSVC)
  # Both include the whole AgentSession surface and exceed MSVC's default section limit (C1128).
  target_compile_options(agentengine_test_driver PRIVATE /bigobj)
  target_compile_options(agentengine_scenario_runner PRIVATE /bigobj)
endif()
# Live mode (--allow-live: a real OpenAI-compatible model, DeepSeek by default) needs the TLS client.
if(AGENTENGINE_WITH_HTTPS)
  target_link_libraries(agentengine_test_driver PRIVATE
    agentengine::provider_http_client MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  target_compile_definitions(agentengine_test_driver PRIVATE AGENTENGINE_WITH_HTTPS)
endif()

# Keep the executables where they were built before the move (the build root): .mcp.json and the docs
# run build/agentengine_test_driver.
set_target_properties(
  agentengine_policy_reachability agentengine_mcp_conformance_client
  agentengine_test_driver agentengine_scenario_runner
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
