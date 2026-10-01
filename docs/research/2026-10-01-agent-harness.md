# 2026-10-01 — Microsoft Agent Framework's Agent Harness (for issue #60 / ADR-234)

What MAF's "Harness" is, what it turns on by default, and how each piece is switched off — read from the
framework's own source and its Learn page, to decide what AgentEngine's harness should and should not copy.
Every claim below is from one of the sources listed at the end; nothing is from memory.

## 1. What it is

Learn (`concepts/harness`, `ms.date` 2026-09-19, `updated_at` 2026-09-28): an agent harness "is the runtime
scaffolding that turns a language model into an agent that can perform work", and the Agent Framework Harness
"composes existing Agent Framework building blocks rather than defining a separate agent runtime"; "the
resulting object remains a normal Agent Framework agent" [L].

- .NET: `chatClient.AsHarnessAgent(options)` (`ChatClientHarnessExtensions.cs`) is `new HarnessAgent(chatClient,
  options, ...)`; `HarnessAgent` is a `DelegatingAIAgent` whose inner agent is built once, in the constructor,
  from `HarnessAgentOptions` [D1, D2].
- Python: `create_harness_agent(client, ...)` is a factory function returning an ordinary `Agent` with keyword
  arguments for every option (`_harness/_agent.py`) [P1].

Both are construction-time conveniences over the framework's ordinary agent/context-provider/middleware
types; neither is a new agent type with its own declaration syntax. (Relevant to ADR-234's §3 shape decision.)

## 2. Pieces and defaults (as of agent-framework commit `98a982a`, 2026-09-21)

| Piece | .NET default | Python default | Off switch / on switch |
|---|---|---|---|
| Function invocation (tool loop) | on | on | iteration limit configurable [D1, P1] |
| Per-service-call history persistence | on | on | none [D1, P1] |
| Compaction | on **only** when `MaxContextWindowTokens` and `MaxOutputTokens` (or a custom strategy) are given | same | `DisableCompaction` / `disable_compaction` [D1 `BuildInnerAgent`, P1 `_assemble_compaction`] |
| `TodoProvider` | on | on | `DisableTodoProvider` / `disable_todo` [D1, P1] |
| `AgentModeProvider` (plan/execute) | on | on | `DisableAgentModeProvider` / `disable_mode` [D1, P1] |
| `FileMemoryProvider` | on, store defaults to `{cwd}/agent-file-memory/{timestamp}_{guid}` | on, `Path.cwd() / "agent-file-memory"` | `DisableFileMemory` / `disable_file_memory` [D1 `BuildContextProviders`, D3, P1] |
| `FileAccessProvider` | off | off | set `FileAccessStore` / `file_access_store` [D1, P1] |
| Agent Skills | **on**, `AgentSkillsProvider(Directory.GetCurrentDirectory())` | **off** — "Skills are opt-in: only added when skills_provider or skills_paths is provided" | `DisableAgentSkillsProvider` / pass `skills_provider` [D1, P1, L] |
| Background agents | off | off | set `BackgroundAgents` / `background_agents` [D1, P1] |
| Tool approval ("don't ask again" standing rules, auto-approval rules) | on | on | `DisableToolAutoApproval` / `disable_tool_auto_approval` [D1 `BuildAgent`, P1] |
| OpenTelemetry | on | on (provider name set after construction) | `DisableOpenTelemetry` [D1, P1] |
| Web search tool | on where the client supports it | on where the client supports it (warns otherwise) | `DisableWebSearch` / `disable_web_search` [D1 `BuildChatOptions`, P1] |
| Looping (`LoopAgent` / `AgentLoopMiddleware`) | off; outermost decorator only when `LoopEvaluators` has ≥1 evaluator | off; only when `loop_should_continue` is given | [D1 `BuildAgent`, P1] |
| Shell | — | opt-in via `shell_executor` | [P1, L] |
| Harness instructions | `DefaultInstructions`, placed **before** the agent's own instructions | same order | `HarnessInstructions` / `harness_instructions` override [D1, P1, L] |

Python marks background agents, file access and looping experimental; `create_harness_agent` itself is
released [L, P1 `_warn_experimental_harness_params`].

## 3. Observations that matter for AgentEngine

1. **Default-on file memory and (.NET) skills read the process's current directory.** `FileSystemAgentFileStore
   (Path.Combine(Directory.GetCurrentDirectory(), "agent-file-memory"))` and `new AgentSkillsProvider(
   Directory.GetCurrentDirectory())` are reached with no caller-supplied location [D1 lines 337-358]. In
   AgentEngine terms that is ambient authority (I2): file-system reach derived from where the process happens
   to run, not from a capability the host passed. Python already narrowed skills to opt-in [P1 line 204].
2. **Compaction is off unless the caller gives limits** in both languages — MAF does not drop content by
   default either.
3. **Looping needs a caller-supplied evaluator/predicate**; there is no default evaluator.
4. **Approval in MAF is a decorator that remembers human "always approve" choices and applies host-supplied
   auto-approval rules** [D1 `UseToolApproval`, P1 `ToolApprovalMiddleware(auto_approval_rules=...)`]. The
   decisions still come from a human or from host code, never from the model.
5. **Telemetry goes to an ambient OpenTelemetry pipeline** (`UseOpenTelemetry(sourceName: ...)`) [D1]. AgentEngine
   has no OTel exporter (016 is not built), so the equivalent default has nowhere to send data.
6. **The loop stops on a pending approval**: Python's loop "returns any pending approval request to the caller"
   [P1 comment above `assembled_middleware`].

## Sources

- [L] Microsoft Learn, "Agent Harness", <https://learn.microsoft.com/en-us/agent-framework/concepts/harness>
  (fetched 2026-10-01; page metadata `ms.date: 2026-09-19`, `updated_at: 2026-09-28`, git commit
  `ce7cc1c9addd44be9f07182532cb05501db10950`).
- [D1] `D:\GitSrc\agent-framework\dotnet\src\Microsoft.Agents.AI.Harness\HarnessAgent.cs` (agent-framework commit
  `98a982a147212424d766ed993e6ecda13abe5faf`, 2026-09-21) — `BuildAgent`, `BuildInnerAgent`, `BuildChatOptions`,
  `BuildContextProviders`.
- [D2] `...\Microsoft.Agents.AI.Harness\ChatClientHarnessExtensions.cs` (same commit) — `AsHarnessAgent`.
- [D3] `...\Microsoft.Agents.AI.Harness\HarnessAgentOptions.cs` (same commit) — the `Disable*` properties and the
  `FileMemoryStore` default remark.
- [P1] `D:\GitSrc\agent-framework\python\packages\core\agent_framework\_harness\_agent.py` (same commit) —
  `create_harness_agent`, `_assemble_compaction`, `_assemble_context_providers`.
