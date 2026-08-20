# Pluggable compaction, chaining, and provenance — MAF deep-dive plus external prior art

**Date:** 2026-08-20 · **Sources:** local checkout of Microsoft Agent Framework at
`C:\Users\thanh\Git\agent-framework` (read-only reference, state as checked out on this date; .NET
paths under `dotnet/src/Microsoft.Agents.AI*`), plus fetched external primary sources cited inline
per claim. Commissioned to ground a future ADR reopening OQ-18 (`ContextProvider` fan-out vs.
chaining) and closing OQ-22 (per-contributor provenance) — see `OpenQuestions.md` and
`include/agentengine/core/context_assembly.hpp`'s own top-comment for the current, still-binding
decision this research is scoping a *change proposal* against, not silently overriding.

This supersedes nothing already on record — `docs/research/2026-08-11-maf-middleware-codeact-skills-
deep-dive.md` §2 (the sequential-pipeline mechanics) and `docs/research/2026-maf-provider-concepts.md`
(2026-07-31, the original "concatenating" read) both stay accurate; this file goes one level deeper
into *compaction specifically*, plus brings in non-MAF prior art neither of those covers.

---

## 1. MAF's `CompactionProvider` — the exact mechanism

**The crux: it overrides `InvokingCoreAsync` directly** (`CompactionProvider.cs:113`), bypassing the
base class's default filter/merge/stamp path entirely (`AIContextProvider.cs:146-200`, previously
read). It reads `context.AIContext.Messages` — the list already merged by every earlier provider in
the pipeline, via the outer `foreach` in the agent's invocation loop that reassigns `aiContext` each
iteration (`ChatClientAgent.cs:89-95`, confirmed in the 2026-08-11 deep-dive). It never calls
`ProvideAIContextAsync` at all — the override *is* the whole mechanism, not an extension point on top
of it.

**Strategies** (`dotnet/src/Microsoft.Agents.AI/Compaction/`): `CompactionStrategy` abstract base with
`Trigger`/`Target` predicates and a template-method `CompactAsync` (`CompactionStrategy.cs:104-156`).
Concrete strategies: `SlidingWindowCompactionStrategy` (turn-based, `MinimumPreservedTurns` floor,
`SlidingWindowCompactionStrategy.cs:73-139`), `TruncationCompactionStrategy`,
`ToolResultCompactionStrategy`, `ContextWindowCompactionStrategy`, `SummarizationCompactionStrategy`
(calls a caller-supplied `IChatClient`, `MinimumPreservedGroups=8` floor, and states outright that its
own output is "trusted the same as any other assistant message" — naming the indirect-prompt-injection
risk explicitly, `SummarizationCompactionStrategy.cs:35-45`), `ChatReducerCompactionStrategy` (same
trust warning for an externally supplied `IChatReducer`), and `PipelineCompactionStrategy` (chains
multiple strategies over one shared index). State persists as `List<CompactionMessageGroup>` in
`ProviderSessionState<State>` (backed by `AgentSession.StateBag`); `CompactionMessageIndex.Update()`
finds the last-processed message and appends only the delta, full-rebuilding only if not found or the
history was front-trimmed (`CompactionMessageIndex.cs:120-175`).

**Atomic tool-call/result pairing**, real code not just doc-comment: `CompactionMessageIndex.
AppendFromMessages` (`CompactionMessageIndex.cs:177-270`) groups an assistant message carrying
`FunctionCallContent` with every immediately-following `Tool`-role (and interleaved reasoning-only)
message into one `CompactionGroupKind.ToolCall` unit. Every strategy sets `IsExcluded` on a whole
group only — none splits one. This is the same invariant 005 §4 already states for AgentEngine's own
`history[]`-scoped compaction ("a compaction that drops a pending tool-call/result pair is a defect,
checked") — MAF proves the same rule in real code, independently.

**Ordering is a documented convention, not an enforced invariant — a real, confirmed weakness.**
`AIContextProviders` is a bare `IEnumerable<AIContextProvider>`; the pipeline iterates declaration
order with zero type-checking. "Add `CompactionProvider` last" is stated only in a doc-comment
(`CompactionProvider.cs:30-32`) — registering it earlier silently starves later providers of full
context, with no error at any point. MAF's own Python-side design ADR admits this as a known wart of
the equivalent shape: "ordering sensitivity is subtle — must come after storage providers but before
model invocation" (`docs/decisions/0019-python-context-compaction-strategy.md`, Option 3 cons list).

**Provenance is written, not read, by compaction.** `CompactionProvider` groups/compacts purely by
`ChatRole` structure, ignoring `AgentRequestMessageSourceType` entirely on the way in. It only
*writes* provenance afterward: every message it touches (reloaded groups, generated summaries) gets
re-stamped `SourceType=ChatHistory` (`CompactionProvider.cs:150-151,189-193`), stated purpose being
dedup ("avoid adding them to chat history... since they may be summaries already in chat history",
same file, lines 184-185) — not a safety or eligibility gate. Python's own design ADR, in contrast,
*proposes* provenance-gated eligibility as a future strategy ("Leveraging Source Attribution" section,
`0019-python-context-compaction-strategy.md` ~lines 1052-1061): "protect user input: messages without
a `source_id`... should typically be preserved." **This was never shipped in either SDK** — a real,
still-open gap in MAF itself, not something AgentEngine would be behind on by not having it either.

**A structural divergence between MAF's two SDKs, worth flagging for 027's citation discipline.**
`0019-python-context-compaction-strategy.md` opens by establishing MAF's middleware pipeline
*structurally could not* do in-run compaction before this design: history is loaded once at
`before_run`, `ChatMiddleware` mutates only a per-call copy, and `FunctionMiddleware` wraps tool
execution, not the LLM call (its own §"Why Current Architecture Cannot Support In-Run Compaction").
Four options were compared; **Python chose Option 1** — a standalone `CompactionStrategy` object
composed into `HistoryProvider`/`BaseChatClient` — explicitly because "chaining is natural" scored
well, versus Option 3 (a dedicated `ContextProvider` subclass, rated "dual roles muddy the
`ContextProvider` contract") and Option 2 (a mixin, rated "chaining is difficult"). **.NET's real,
shipped `CompactionProvider` is closest to Python's *rejected* Option 3 shape**, not the shape Python
actually chose. 027 §2 already notes "names verified against `agent_framework` (Python core) unless
marked **ours**" — this is a concrete case where the two SDKs disagree on architecture, not just
naming, and a future citation should say which one it means.

---

## 2. External prior art (beyond MAF)

### Letta / MemGPT — paged virtual context

OS-inspired hierarchy: in-context "core memory" (labeled, agent-editable blocks via
`core_memory_append`/`core_memory_replace`, 2,000-char default limit per block) plus out-of-context
recall/archival storage
([Core memory — Letta Docs](https://docs.letta.com/guides/ade/core-memory/),
[Memory blocks — Letta Docs](https://docs.letta.com/guides/core-concepts/memory/memory-blocks)).
Eviction triggers on `context_window_size_current > memory_warning_threshold`, moving older messages
into Recall Memory, then `partial_evict_summarization()` compresses evicted content recursively (new
summary = old summary + evicted messages)
([Agent Memory — Letta](https://www.letta.com/blog/agent-memory/),
[Agent Memory System — DeepWiki](https://deepwiki.com/letta-ai/letta/2.3-agent-memory-system)).
**Pluggable only partially**: memory *blocks* are user-definable, but no documented interface lets a
third party swap the eviction/compaction *strategy* itself — same "authoring extensible, policy
built-in" shape as MAF pre-`CompactionStrategy`.

### LangChain / LangGraph — summarization/trimming

Two independent mechanisms, not a chain reacting to multiple context contributors: `trim_messages`
(stateless, produces a shorter copy right before an LLM call) vs. `RemoveMessage`/`SummarizationNode`
(stateful, edits `MessagesState` directly once a token threshold is hit)
([Memory — Docs by LangChain](https://docs.langchain.com/oss/python/langgraph/add-memory),
[Message Handling and Summarization — DeepWiki](https://deepwiki.com/langchain-ai/langchain-academy/5.2-message-handling-and-summarization)).
LangGraph has no analogue of "N independent context-providers merging, one reacting to the others" —
there's one accumulated `MessagesState`, so this is a different-shaped problem than MAF's
`AIContextProvider` chain or AgentEngine's `ContextProvider` fan-out.

### Anthropic context editing / memory tool

Server-side, request-scoped, via `context_management.edits[]`: `clear_tool_uses_20250919` (clears old
`tool_result` blocks, keeps `tool_use`) and `clear_thinking_20251015`, configurable via `trigger`
(`input_tokens`/`tool_uses`), `keep`, `clear_at_least` (a floor so a partial clear doesn't thrash the
prompt cache for nothing), `exclude_tools`, `clear_tool_inputs`
([Context editing — Claude Platform Docs](https://platform.claude.com/docs/en/build-with-claude/context-editing)).
**No provenance anywhere** — cleared content becomes a placeholder; the response reports only
aggregate stats (`cleared_tool_uses`, `cleared_input_tokens`), nothing per-item. Exactly two fixed
strategies, not a general extension mechanism. One idea worth keeping regardless of the provenance
gap: the **memory-tool integration warns Claude before clearing**, so the model can proactively
persist important content elsewhere first — a "warn-before-evict" pattern.

### OpenAI Agents SDK

`OpenAIResponsesCompactionSession` is a **decorator/wrapper around any `Session` backend** (SQLite,
Redis, SQLAlchemy) — genuinely pluggable at the session layer. Compaction runs **after a full run
completes**, not interleaved with in-turn tool/retrieval calls; it's server-side (`/responses/compact`,
loss-aware, returns an encrypted opaque item; user messages kept verbatim, assistant/tool/reasoning
content replaced), and override-controllable (`should_trigger_compaction=lambda _: False`, manual
`run_compaction()`) ([Sessions — OpenAI Agents SDK](https://openai.github.io/openai-agents-python/sessions/),
[Compaction — OpenAI API](https://developers.openai.com/api/docs/guides/compaction)). No provenance
metadata documented. Architecturally this is the odd one out: compaction as a **post-turn** pass over
durable history, not an **in-turn** `ContextProvider`-shaped seam — closer to what 005 §4 already
specifies for AgentEngine's `history[]` than to MAF's in-run `CompactionProvider`.

### Attribution-aware context assembly (academic)

Primary source fetched: "From Agent Traces to Trust: Evidence Tracing and Execution Provenance in LLM
Agents" ([arXiv:2606.04990](https://arxiv.org/html/2606.04990v1)) — proposes a **typed provenance
graph**, kept separate from the message stream (not inline metadata), with 7 relation types (Support,
Derive, Depend-on, Contradict, Invalidate, Trigger, Update) connecting evidence/tool-calls/memory-
items/actions. Used for faithfulness verification, trace-based failure localization, safety/
information-flow enforcement, audit reconstruction, and selective invalidation of stale memory.
Secondary, not fetched primary: ARGUS ([arXiv:2605.03378](https://arxiv.org/html/2605.03378v1)) builds
an "influence provenance graph" to *gate whether untrusted context is permitted to justify an action*
— an authorization framing, stronger than mere attribution, worth a primary read only if provenance
needs to become capability-gating later, not for the current ask.

**Cross-cutting observation**: none of the four shipped systems above (Letta, LangGraph, Anthropic,
OpenAI) implement per-source provenance on merged context at all — only the academic work does, and
there as a separate graph, not inline stamping. MAF's inline `ChatMessage.AdditionalProperties` stamp
(§1 above, and the 2026-08-11 deep-dive) is the only *shipped* provenance mechanism found anywhere in
this research, in either direction.

---

## 3. What this settles vs. what it doesn't

**Settled, safe to build on without further research:**
- Atomic tool-call/result grouping under compaction — proven in two independent systems (MAF, and
  005 §4's own existing checked invariant); not a new risk.
- Inline per-message provenance stamping (MAF's shape) is the only shipped precedent for the exact
  problem OQ-22 names; no shipped system does it as a separate graph — that's academic-only, heavier
  machinery than this problem currently needs.
- "Warn before evict" (Anthropic) and "compaction as a post-turn wrapper, not in-run" (OpenAI) are
  both real, independently-invented alternative shapes worth weighing against MAF's in-run
  `ContextProvider` override shape — not just assuming MAF's is the only option.

**Confirmed open, not settled by this research — still needs the ADR's own judgment:**
- Whether AgentEngine should enforce contributor ordering mechanically (a gap MAF admits and never
  closed in either SDK) or accept the same documentation-only convention.
- Whether provenance should gate compaction *eligibility* (proposed in MAF's own Python ADR, never
  shipped anywhere) or stay write-only bookkeeping like .NET's shipped behavior.
- How a chained/terminal provider's read access to an earlier contributor's already-declassified
  `TaintedText` (ADR-042) interacts with I2/I3 — no external system surveyed here has an analogous
  capability-typed trust boundary to draw from; this is AgentEngine-specific design work.
