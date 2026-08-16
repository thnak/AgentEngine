# Shared warning flags for AgentEngine targets (CONVENTIONS.md: "Compile clean under MSVC 19.4x,
# g++ 14+, clang 20+, -std=c++23 /W4 -Wall -Wextra").
# Applied via target_link_libraries(<t> PRIVATE agentengine_warnings).

add_library(agentengine_warnings INTERFACE)

# No -Wno-* / /wd* entry belongs in this file, and none is present. Project rule (owner, explicit):
# a warning is either fixed at the site that raises it or carried through the red-team process into
# an ADR -- never silenced here, where the silence would apply to code nobody has looked at yet.
if(MSVC)
  target_compile_options(agentengine_warnings INTERFACE /W4)
else()
  target_compile_options(agentengine_warnings INTERFACE -Wall -Wextra)
endif()
