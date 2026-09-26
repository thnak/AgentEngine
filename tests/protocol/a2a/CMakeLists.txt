# tests/protocol/a2a/: A2A (include/agentengine/protocol/a2a/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "protocol/a2a" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ADR-023 §7 R1/R2/R3: server-minted task handles are principal-bound, not-authorized is
# indistinguishable from not-found, and the A2A path really applies 018 §2 admission. Every negative
# case carries its own positive control (decisions/README.md's rule for security claims).
agentengine_add_test(test_task_principal_binding LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase D1 -- 012 §0/§A.4's A2A v1.0 wire object model (protocol\a2a\types.hpp):
# Part/Message/Artifact/TaskStatus/Task round-trip, the removed `kind` discriminator, full
# SCREAMING_SNAKE enum serialization, and unknown-Part-kind forward compatibility -----------------
agentengine_add_test(test_a2a_types LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase D2 -- 012 §2.1's Agent Card generation (protocol\a2a\agent_card.hpp): skills
# derived from a REAL register_agent<A>()-compiled AgentMetadata's ToolTable, never hand-maintained,
# advertising only what this codebase can actually serve (no fabricated bindings/capabilities) -------
agentengine_add_test(test_a2a_agent_card LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase D3 -- 012 §1/§2.3's Task <- Run projection and Task management
# (protocol\a2a\mapping.hpp, protocol\a2a\server.hpp): ContentItem/Message <-> Part/Message mapping,
# and SendMessage/GetTask/CancelTask wired to a REAL agentengine::rt::AgentSession (test_a2a_server.cpp;
# ADR-037 later ported that wiring off quark::Engine/Actor/ActorRef onto rt::AgentSession) -----------
agentengine_add_test(test_a2a_mapping LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_a2a_server LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase D4 -- 012 §3/§4a's A2A client role (protocol\a2a\client.hpp):
# send/get/cancel passthrough against a REAL A2aServer, plus digest-pinned Agent Card caching and
# rug-pull detection, mirroring McpClient's own identical discipline for MCP tool listings ----------
agentengine_add_test(test_a2a_client LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase E4 -- 012 §2.3/013 §3's A2A streaming projection (protocol\a2a\streaming.hpp)
# and 013 §3's MCP progress projection (protocol\mcp\progress.hpp), plus 013 §6 G3's cross-surface
# equivalence proof (a real run's SAME internal event sequence projected to both AG-UI and A2A) -----
agentengine_add_test(test_a2a_streaming LIBS agentengine::core agentengine_warnings)
