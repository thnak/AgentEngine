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
