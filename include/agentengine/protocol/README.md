# `agentengine::protocol`

L4 protocol surfaces that project the internal run event stream and content model onto the open
agent protocols of 2026: MCP (client and server), A2A (peer exposure and consumption), and the
AG-UI / streaming projections. Governing RFCs: **011-MCP-Conformance.md**,
**012-A2A-Conformance.md**, **013-UI-and-Streaming-Surfaces.md**.

Per CONVENTIONS.md's protocol code rules, this is strictly L4: it translates to L2 vocabulary at
the boundary, and **a protocol type must never leak into `agentengine::core`** — `agentengine::core`
contains no `mcp::`, `a2a::`, or `agui::` type. Each protocol lives in its own subdirectory and
namespace (`mcp/`, `a2a/`, `agui/`, `openai/`).
