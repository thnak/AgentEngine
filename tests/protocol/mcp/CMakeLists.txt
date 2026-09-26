# tests/protocol/mcp/: MCP (include/agentengine/protocol/mcp/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "protocol/mcp" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-061 §10b claim 4 (decisions/ADR-061-host-provided-inbound-transport.md, Tier 1 prove -------
# phase): the MCP conformance client's resolve_host/resolve_and_validate two-way SSRF control. Written
# for real -- tools/mcp_conformance_client.cpp's own top comment referred to this file before it
# existed.
agentengine_add_test(test_mcp_conformance_transport LIBS agentengine::net_egress_proxy agentengine_warnings)

# ---- Milestone 7 Phase C1 -- 011 MCP's JSON-RPC 2.0 wire envelope (protocol/mcp/json_rpc.hpp) ------
agentengine_add_test(test_mcp_json_rpc LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase C2 -- 011 §4's MCP server role (protocol/mcp/server.hpp): server/discover,
# tools/list, tools/call over a real ToolTable and the real invoke_tool() pipeline -------------------
agentengine_add_test(test_mcp_server LIBS agentengine::core agentengine_warnings)

# ---- ADR-061 §39/§40 prove phase -- McpServer's per-request CapabilityGrant (decisions/ADR-061) -----
agentengine_add_test(test_mcp_capability_grant LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase C3 -- 011 §3's MCP client role (protocol/mcp/client.hpp): caching,
# pagination, digest-pinning/rug-pull detection, isError surfaced not thrown -------------------------
agentengine_add_test(test_mcp_client LIBS agentengine::core agentengine_warnings)

# ---- CodeAct tool-bridge union (core/codeact_tool_union.hpp, protocol/mcp/mcp_tool_bridge.hpp):
# agent's own tools + skill-unlocked tools + MCP-discovered tools merged into one bridge-ready
# ToolTable, cross-source name collisions rejected, MCP path proven against a real McpServer. No
# CPython dependency -- unconditional, unlike the mediated_python_runner-linked tests below.
agentengine_add_test(test_codeact_tool_union LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase C4 -- the io.modelcontextprotocol/tasks extension (011 §3.6/§12): a
# Backgroundable tool call started via a real McpServer, polled to completion via tasks/get, cancelled
# via tasks/cancel -- reusing background_task() (real since Phase B) end to end -------------------
agentengine_add_test(test_mcp_tasks_extension LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_mcp_progress LIBS agentengine::core agentengine_warnings)

if(WIN32)
  # ---- ADR-061 §42/§43 prove phase -- trust::steady_deadline_from() (shared clock-conversion
  # primitive) and mcp::capability_grant_from_bearer_claims(), the bearer-credential-to-CapabilityGrant
  # bridge for McpServer::dispatch()'s per-request grant (§39-§41, decisions/ADR-061) -----------------
  agentengine_add_test(test_capability_grant_bridge
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)
endif()
