# MAF ContextProvider and ChatClient implementations — source-grounded notes

**Date:** 2026-07-31 · **Source:** local checkout of Microsoft Agent Framework at
`D:\GitSrc\agent-framework` (read-only reference; commit as checked out on that date). File paths
and excerpts below are verbatim from that tree.

## 1. `AIContextProvider` / `ContextProvider`

**.NET** — `dotnet/src/Microsoft.Agents.AI.Abstractions/AIContextProvider.cs`. Abstract base,
two-phase lifecycle:

```csharp
public ValueTask<AIContext> InvokingAsync(InvokingContext context, CancellationToken ct = default)
protected virtual ValueTask<AIContext> ProvideAIContextAsync(InvokingContext context, ...)
public ValueTask InvokedAsync(InvokedContext context, CancellationToken ct = default)
protected virtual ValueTask StoreAIContextAsync(InvokedContext context, ...)
```

Result type `AIContext` (`AIContext.cs`) — exactly three fields:

```csharp
public string? Instructions { get; set; }
public IEnumerable<ChatMessage>? Messages { get; set; }
public IEnumerable<AITool>? Tools { get; set; }
```

`InvokingCoreAsync` merges the provider's returned `AIContext` into the running one —
**concatenating** `Instructions`, `Messages`, and `Tools` across all registered providers — and
stamps contributed messages with `AgentRequestMessageSourceType.AIContextProvider`.

**Python** — `python/packages/core/agent_framework/_sessions.py:750`, base class `ContextProvider`:
`async def before_run(self, *, agent, session, context: SessionContext, state: dict)` and
`async def after_run(...)`. Mutates the passed `SessionContext` in place (`instructions: list[str]`,
`tools: list[Any]`, `context_messages: dict[str, list[Message]]`) rather than returning a value —
different mechanic, same three contributed channels.

**Built-in providers found:** `TextSearchProvider` (RAG — either injects a formatted message *or*
exposes an on-demand `Search` tool via `AIFunctionFactory.Create`, the concrete precedent for
per-turn dynamic tool injection), `Mem0Provider`, `FoundryMemoryProvider`, `AgentSkillsProvider`,
`CompactionProvider`, `TodoProvider`, `FileMemoryProvider`, `ShellEnvironmentProvider` (.NET);
`HistoryProvider`, `SkillsProvider`, `CompactionProvider`, plus package-level
`agent_framework_mem0`/`_redis`/`_azure_ai_search`/`_azure_cosmos_memory` (Python).

**No vector/semantic tool-search mechanism exists anywhere in the repo.** The documented answer to
"agent has too many tools/skills to fit in context" is `docs/decisions/0021-agent-skills-design.md`:
**progressive disclosure** via exactly **3 fixed model-facing tools regardless of catalog size** —
`load_skill(skillName)`, `read_skill_resource(skillName, resourceName)`,
`run_skill_script(skillName, scriptName, arguments?)` — with a docstring estimate of "~100 tokens per
skill" for the advertisement step (`_skills.py:1837-1844`). This is a **compare-and-contrast point**
against our own design: `009-Plugin-and-Extension-System.md` §8b goes further still — no loader
tools at all, skills mounted read-only on the worktree, agent uses ordinary file reads.

## 2. Anthropic / OpenAI chat client backends

**.NET** (`Microsoft.Agents.AI.OpenAI`, `Microsoft.Agents.AI.Anthropic`) and **Python**
(`agent_framework_openai`, `agent_framework_anthropic`) are both **thin adapters over each vendor's
own official SDK** — not custom HTTP/SSE implementations. Confirmed directly in
`agent_framework_anthropic/_chat_client.py`:

```python
await self.anthropic_client.beta.messages.create(**run_options, stream=True/False)
```

Core Python abstraction (`agent_framework/_clients.py`): `BaseChatClient(SerializationMixin, ABC,
Generic[OptionsCoT])`, one method `get_response(...)`, subclasses implement
`_inner_get_response(...)`. A third package, `agent_framework_claude`, wraps `claude-agent-sdk` (the
Claude Code CLI-based Agent SDK) — a distinct abstraction, not relevant to a `ChatClient` seam.

**Consequence for a C++ port:** no vendor ships a C++ SDK, so unlike MAF's own adapters, our
Anthropic/OpenAI `ChatClient` backends must implement the HTTP/SSE transport from scratch (004
§3 "seam backend" tier — one HTTP/TLS dependency, never core). What **is** portable design from MAF,
because it is translation logic independent of the transport, is:

- **Streaming event parsing.** Anthropic's `_process_stream_event` pattern-matches raw SDK event
  types (`message_start`, `message_delta`, `content_block_start/delta/stop`) into update objects.
- **Cumulative→incremental usage conversion.** Anthropic reports *cumulative* usage per stream event;
  MAF converts to incremental deltas with a per-stream accumulator (`_incremental_usage`). Any engine
  that wants uniform per-chunk `Usage` (003 §6) across providers needs the same conversion.
- **Tool schema shaping.** `_prepare_tools_for_anthropic` converts a generic function-tool
  declaration to `{"type": "custom", "name", "description", "input_schema": ...}`, with MCP tools
  routed to a separate `mcp_servers` param and a special case for `bash_20250124`.
- **Structured output shaping.** `_prepare_response_format` builds
  `output_config.format = {"type": "json_schema", "schema": ...}`, forcing `additionalProperties:
  false`, accepting either a Pydantic-style schema or an OpenAI-style `{"json_schema": {...}}` dict.

None of this is vendored code we can copy (different language, different object model) — it is
the *shape of the problem* to re-solve in C++, and worth treating as a checklist during
implementation rather than rediscovering it from provider docs cold.
