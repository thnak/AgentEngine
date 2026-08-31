# Shared warning flags for AgentEngine targets (CONVENTIONS.md: "Compile clean under MSVC 19.4x,
# g++ 14+, clang 20+, -std=c++23 /W4 -Wall -Wextra"). /WX and -Werror turn that "clean" requirement
# into a hard build failure rather than a log line someone has to notice -- consistent with this
# file's own no-suppression rule below: a warning gets fixed at its site, not silenced or left as
# advisory-only noise a push can ignore.
# Applied via target_link_libraries(<t> PRIVATE agentengine_warnings).

add_library(agentengine_warnings INTERFACE)

# No -Wno-* / /wd* entry belongs in this file, and none is present. Project rule (owner, explicit):
# a warning is either fixed at the site that raises it or carried through the red-team process into
# an ADR -- never silenced here, where the silence would apply to code nobody has looked at yet.
if(MSVC)
  target_compile_options(agentengine_warnings INTERFACE /W4 /WX)
else()
  target_compile_options(agentengine_warnings INTERFACE -Wall -Wextra -Werror)
endif()
