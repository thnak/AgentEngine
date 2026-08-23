# "Get started" examples (mirrors Microsoft Agent Framework's samples/01-get-started progression):
# small, single-file, offline-runnable programs, one concept each. Each is also registered as a
# ctest entry -- like every other executable in this repo (tests/CMakeLists.txt's own convention),
# a runnable program that checks its own output IS the test, not a separate thing built alongside
# one.

add_executable(agentengine_example_01_hello_agent 01_hello_agent.cpp)
target_link_libraries(agentengine_example_01_hello_agent PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_01_hello_agent COMMAND agentengine_example_01_hello_agent)

add_executable(agentengine_example_02_add_tools 02_add_tools.cpp)
target_link_libraries(agentengine_example_02_add_tools PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_02_add_tools COMMAND agentengine_example_02_add_tools)

add_executable(agentengine_example_03_multi_turn 03_multi_turn.cpp)
target_link_libraries(agentengine_example_03_multi_turn PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_03_multi_turn COMMAND agentengine_example_03_multi_turn)

add_executable(agentengine_example_04_first_workflow 04_first_workflow.cpp)
target_link_libraries(agentengine_example_04_first_workflow PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_04_first_workflow COMMAND agentengine_example_04_first_workflow)
set_tests_properties(example_04_first_workflow PROPERTIES TIMEOUT 60)

# 05/06 go beyond MAF's own progression -- they demonstrate two AgentEngine invariants (ADR-029
# human-in-the-loop suspension, and I2's no-ambient-authority capability gate) that have no direct
# MAF equivalent but are central to this engine's own model.
add_executable(agentengine_example_05_human_approval 05_human_approval.cpp)
target_link_libraries(agentengine_example_05_human_approval PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_05_human_approval COMMAND agentengine_example_05_human_approval)

add_executable(agentengine_example_06_capabilities_and_denial 06_capabilities_and_denial.cpp)
target_link_libraries(agentengine_example_06_capabilities_and_denial PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_06_capabilities_and_denial COMMAND agentengine_example_06_capabilities_and_denial)

# 07-12: a curated sweep across the rest of MAF's own sample categories (streaming, memory,
# concurrent/conditional-routing workflow patterns, skill mounting, checkpoint/restart) -- each
# grounded in real, already-shipped AgentEngine code, not a re-implementation of anything.

add_executable(agentengine_example_07_streaming 07_streaming.cpp)
target_link_libraries(agentengine_example_07_streaming PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_07_streaming COMMAND agentengine_example_07_streaming)

# Reuses tests/support/run_task_sync.hpp -- the same minimal, documented "drive a leaf task<T> that
# never genuinely parks, outside any Activation/Engine" driver test_memory_provider.cpp itself uses
# to call MemoryProvider::on_context()/on_turn_end() directly from a plain main(). MemoryProvider's
# InMemoryWorktreeObjectStore needs agentengine::worktree_store (compute_digest et al. are compiled,
# not header-only) -- Windows-only today, matching test_memory_provider.cpp's own guard.
if(WIN32)
  add_executable(agentengine_example_08_memory 08_memory.cpp)
  target_include_directories(agentengine_example_08_memory PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
  target_link_libraries(agentengine_example_08_memory PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME example_08_memory COMMAND agentengine_example_08_memory)
endif()

add_executable(agentengine_example_09_concurrent_workflow 09_concurrent_workflow.cpp)
target_link_libraries(agentengine_example_09_concurrent_workflow PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_09_concurrent_workflow COMMAND agentengine_example_09_concurrent_workflow)

add_executable(agentengine_example_10_conditional_routing 10_conditional_routing.cpp)
target_link_libraries(agentengine_example_10_conditional_routing PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_10_conditional_routing COMMAND agentengine_example_10_conditional_routing)

# SkillsProvider resolves a mounted skill into real worktree Tree/Ref storage -- needs
# agentengine::worktree_store (compiled, not header-only), Windows-only today, matching
# test_on_demand_skill_mount.cpp's own guard.
if(WIN32)
  add_executable(agentengine_example_11_skill_mount 11_skill_mount.cpp)
  target_link_libraries(agentengine_example_11_skill_mount PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME example_11_skill_mount COMMAND agentengine_example_11_skill_mount)
endif()

add_executable(agentengine_example_12_session_checkpoint 12_session_checkpoint.cpp)
target_link_libraries(agentengine_example_12_session_checkpoint PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_12_session_checkpoint COMMAND agentengine_example_12_session_checkpoint)

# 13/14: two more 014 §3 workflow patterns not yet covered by 04/09/10 -- a bounded cycle
# (Reflection/Critic) and the sixth edge kind, multi_selection (a caller-chosen subset, with no
# §3 row of its own).
add_executable(agentengine_example_13_reflection_loop 13_reflection_loop.cpp)
target_link_libraries(agentengine_example_13_reflection_loop PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_13_reflection_loop COMMAND agentengine_example_13_reflection_loop)

add_executable(agentengine_example_14_multi_selection 14_multi_selection.cpp)
target_link_libraries(agentengine_example_14_multi_selection PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_14_multi_selection COMMAND agentengine_example_14_multi_selection)

# 15: Group Chat and Planner (Magentic) -- 014 §3 gives both the SAME graph shape (a moderator
# cycling among participants) and distinguishes them only by who owns the stopping decision, so
# this one file builds that shape twice rather than pretending they're separate mechanisms.
add_executable(agentengine_example_15_group_chat_and_planner 15_group_chat_and_planner.cpp)
target_link_libraries(agentengine_example_15_group_chat_and_planner PRIVATE agentengine::core agentengine_warnings)
add_test(NAME example_15_group_chat_and_planner COMMAND agentengine_example_15_group_chat_and_planner)

# 16: the live counterpart to 15 -- two real OpenAIChatClient (OpenRouter) calls as workflow-node
# bodies instead of plain functions. Needs AGENTENGINE_WITH_HTTPS (the real HTTPS client) and, like
# every other live-network test in this repo, SKIPS (exit 0) rather than failing when
# AGENTENGINE_OPENROUTER_API_KEY isn't set -- labelled live-network so a default `ctest` run and
# CI both stay green with no credential and no network. Reuses tests/support/run_task_sync.hpp, the
# same driver test_openrouter_live_e2e.cpp itself uses to call a real chat() from a plain main().
if(AGENTENGINE_WITH_HTTPS)
  add_executable(agentengine_example_16_group_chat_live 16_group_chat_live.cpp)
  target_include_directories(agentengine_example_16_group_chat_live PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
  target_link_libraries(agentengine_example_16_group_chat_live PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME example_16_group_chat_live COMMAND agentengine_example_16_group_chat_live)
  set_tests_properties(example_16_group_chat_live PROPERTIES LABELS "live-network" TIMEOUT 300)

  # 17: the live counterpart to 15's Planner half -- the moderator's "ledger" is a real model
  # decision (parsed into ExecutorOutcome::routes), not a hardcoded turn counter, with a
  # deterministic fallback ledger as a safety net if a live reply is ever unparseable.
  add_executable(agentengine_example_17_planner_live 17_planner_live.cpp)
  target_include_directories(agentengine_example_17_planner_live PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
  target_link_libraries(agentengine_example_17_planner_live PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME example_17_planner_live COMMAND agentengine_example_17_planner_live)
  set_tests_properties(example_17_planner_live PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()

# 18: not part of MAF's own progression -- closes docs/planning/2026-08-22-component-role-audit-
# tracker.md Finding C (no example anywhere showed writing a from-scratch, non-built-in
# ContextProvider composed alongside ordinary history). Fully offline, no special build flags.
add_executable(agentengine_example_18_custom_context_provider 18_custom_context_provider.cpp)
target_link_libraries(agentengine_example_18_custom_context_provider PRIVATE
  agentengine::core agentengine_warnings)
add_test(NAME example_18_custom_context_provider COMMAND agentengine_example_18_custom_context_provider)
