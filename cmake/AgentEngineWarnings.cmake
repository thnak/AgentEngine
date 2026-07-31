# Shared warning flags for AgentEngine targets (CONVENTIONS.md: "Compile clean under MSVC 19.4x,
# g++ 14+, clang 20+, -std=c++23 /W4 -Wall -Wextra").
# Applied via target_link_libraries(<t> PRIVATE agentengine_warnings).

add_library(agentengine_warnings INTERFACE)

if(MSVC)
  target_compile_options(agentengine_warnings INTERFACE /W4)
else()
  target_compile_options(agentengine_warnings INTERFACE -Wall -Wextra)
endif()
