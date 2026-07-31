# Correctness gate (CONVENTIONS.md: "tests/ — correctness gate, one per load-bearing invariant").
# smoke_vocabulary proves the abstract-vocabulary headers under include/agentengine/ actually
# compile together and that the declared concepts (ChatClient, Runner, SandboxBackend) are
# satisfiable by a conforming type.

add_executable(smoke_vocabulary smoke_vocabulary.cpp)
target_link_libraries(smoke_vocabulary PRIVATE agentengine::core agentengine_warnings)

add_test(NAME smoke_vocabulary COMMAND smoke_vocabulary)
