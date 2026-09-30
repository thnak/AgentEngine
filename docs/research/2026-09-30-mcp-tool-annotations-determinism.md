# Research record — does MCP let a server declare deterministic tool output?

**Compiled:** 2026-09-30 · **Status:** dated snapshot · **Feeds:** ADR-212 (issue #81), RFC 011

**Answer: no.** MCP `2026-07-28` has no tool annotation for output equality, and none of the
annotation proposals found (listed below) adds one.

## 1. `ToolAnnotations` in `2026-07-28`

Source: `schema/2026-07-28/schema.ts` at `modelcontextprotocol/modelcontextprotocol@main`, fetched
2026-09-30, lines 1912-1954. The interface has exactly five fields:

| Field | Default | Meaning (quoted) |
|---|---|---|
| `title` | — | "A human-readable title for the tool." |
| `readOnlyHint` | `false` | "If true, the tool does not modify its environment." |
| `destructiveHint` | `true` | "If true, the tool may perform destructive updates to its environment." Meaningful only when `readOnlyHint == false`. |
| `idempotentHint` | `false` | "If true, calling the tool repeatedly with the same arguments will have no additional effect on its environment." Meaningful only when `readOnlyHint == false`. |
| `openWorldHint` | `true` | "If true, this tool may interact with an 'open world' of external entities." |

The words "deterministic" and "determinism" do not appear anywhere in `schema.ts`.

`idempotentHint` is about **effects on the environment**, not about the reply. It is the MCP analogue
of `effect_class::idempotent`, not of `Deterministic`.

## 2. Trust

The tools page (`/specification/2026-07-28/server/tools`, "Data Types → Tool", fetched 2026-09-30):

> For trust & safety and security, clients **MUST** consider tool annotations to be untrusted unless
> they come from trusted servers.

So even if an annotation existed, a client bridge could map it only for a server the host has
explicitly marked trusted.

## 3. Proposals checked

- **SEP-1984** "Comprehensive Tool Annotations for Enhanced Governance" (PR
  modelcontextprotocol/modelcontextprotocol#1984): proposes `aiProcessingHint`, `slowExecutionHint`,
  `resourceIntensiveHint`, `sensitiveDataHint`, `privilegedAccessHint` and `reversibleHint`. None of
  them is about output equality. The author closed it on 2026-09-23, to take it to the tool
  annotations interest group.
- **SEP-1561** `unsafeOutputHint` (issue #1561): about whether the output is safe to inject into a
  prompt, not whether it repeats.
- MCP blog, "Tool Annotations as Risk Vocabulary" (2026-03-16,
  `blog.modelcontextprotocol.io/posts/2026-03-16-tool-annotations/`): frames annotations as risk
  hints. Not read in full for this note.

## 4. Consequences for AgentEngine

- The MCP client bridge (`protocol/mcp/mcp_tool_bridge.hpp`) leaves `ToolDescriptor::deterministic`
  `false` and must not derive it from `idempotentHint` or `readOnlyHint`.
- The MCP server role (`protocol/mcp/server.hpp`, `McpToolListEntry`) has no standard field to emit
  it in. It emits no `annotations` today at all.
- Re-check this if the tool annotations interest group publishes a determinism or cacheability hint.
