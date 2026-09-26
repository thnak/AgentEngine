# tests/protocol/agui/: AG-UI (include/agentengine/protocol/agui/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "protocol/agui" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Milestone 7 Phase E1 -- 013 §2.1/§2.2's AG-UI event vocabulary (protocol\agui\types.hpp):
# every event kind's exact wire identifier and field shape, cited from docs/research/2026-a2a-and-
# agui-detail.md Part B -------------------------------------------------------------------------
agentengine_add_test(test_agui_types LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- 013 §2.1/§2.2's projection from the internal RunEvent stream onto AgUiEvent
# (protocol\agui\projection.hpp), ported onto rt::AgentSession directly for its two AgentSession-
# driven blocks (the hand-fed-RunEvent blocks needed no changes) -- supersedes test_agui_projection.
agentengine_add_test(test_rt_agui_projection LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase E3 -- 013 §4's SSE framing (protocol\agui\sse.hpp): one "data: <json>\n\n"
# frame per AG-UI event, proven against real RunEventProjector output ------------------------------
agentengine_add_test(test_agui_sse LIBS agentengine::core agentengine_warnings)
