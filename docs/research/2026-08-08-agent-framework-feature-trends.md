# Research record — agent framework feature trends, 2025-2026

**Compiled:** 2026-08-08 · **Status:** dated snapshot, not a living document

A survey of what shipped or changed materially in 2025-2026 across five agent-building
frameworks/products: LangChain/LangGraph, the OpenAI Agents SDK + Responses API, Microsoft Agent
Framework (MAF, delta-only against the prior record below), CrewAI, AG2 (formerly AutoGen), and
LlamaIndex. Pure landscape research — no comparison against or judgment of AgentEngine's own specs
is made here. Every load-bearing claim carries a source.

Related prior records in this directory: `2026-standards-landscape.md` (protocol/isolation
landscape), `2026-maf-orchestration-patterns.md` (Magentic vs. Group Chat, compiled 2026-08-04),
`2026-maf-provider-concepts.md`, `2026-08-03-maf-workflow-and-hitl-model.md`.

---

## 1. LangChain / LangGraph

**LangChain v1.0** shipped 2025-10-22 with a no-breaking-changes-until-2.0 commitment. The
pre-1.0 abstraction sprawl (many chain types, `create_react_agent`) collapsed into a single
`create_agent()` factory that builds an agent graph looping over tool calls until a stop
condition, running on the LangGraph runtime underneath. Legacy functionality moved to a separate
`langchain-classic` package; Python 3.9 support dropped.

- **Middleware system**: hooks into the agent loop at defined stages (node-style, wrap-style,
  e.g. `wrap_model_call`). Built-in middleware for human-in-the-loop, summarization, PII
  redaction, and retry (`ModelRetryMiddleware`).
- **Standard content blocks**: a provider-agnostic message-content shape normalizing reasoning
  traces, citations, and tool calls across model providers.
- **Structured output** is a first-class `create_agent` parameter (`ToolStrategy`,
  `ProviderStrategy`, or a raw Pydantic model).
- **Checkpointing** (LangGraph): a checkpoint is a full graph-state snapshot keyed by `thread_id`,
  written after every superstep via a pluggable `Checkpointer` (in-memory for dev; Postgres/SQLite/
  Redis/DynamoDB for production). Enables resume-after-crash and time-travel replay from any prior
  checkpoint id.
- **Human-in-the-loop**: `interrupt()` called inside a node raises `GraphInterrupt`, halting
  execution and surfacing a value to the client; state is already checkpointed at that point.
  Resume requires the same `thread_id` and `graph.invoke(Command(resume=user_input), config)` —
  the paused node **re-executes from its start**, not mid-function, on resume.
- **Long-term memory**: a separate `BaseStore` abstraction (`put`/`get`/`search`), cross-thread and
  cross-session, with optional semantic-similarity search when the backing store supports
  embeddings — distinct from the thread-scoped checkpointer.
- **Streaming**: `stream_mode` supports `values` (full snapshot), `updates` (diff), `messages`
  (token streaming), `custom` (user-emitted events), plus `checkpoints`/`tasks`/`debug`; modes can
  be interleaved.
- **Multi-agent patterns**: *Supervisor* (central orchestrator dispatches to sub-agents, routing
  visible in traces), *Swarm* (decentralized — agents hold explicit hand-off tools to peers; graph
  state tracks "last-active agent" so follow-ups route directly), *Hierarchical*
  (supervisors-of-supervisors, used once flat agent count exceeds roughly 8).
- **MCP**: first-class via `langchain-mcp-adapters`, converting MCP tools to LangChain `@tool`
  objects; supports multiple simultaneous MCP server connections (remote and local subprocess).
- **A2A**: not native in LangGraph core; LangSmith's Agent Server exposes a built-in
  `/a2a/{assistant_id}` endpoint (`message/send`, `message/stream`, `tasks/get`) with A2A
  `contextId` auto-mapped to LangGraph `thread_id`. Declarative `AgentCard` attachment on a graph
  is still an open GitHub feature request as of this research.
- **OpenTelemetry GenAI semconv**: LangSmith emits `gen_ai.agent.name`/`gen_ai.agent.type`/
  `gen_ai.tool.name` spans, consumed by Microsoft's OTel distro and others.

**Notable in 2026 specifically**:
- **Deep Agents** (`deepagents` package, ~2026-03-15): a separate harness on top of LangGraph for
  long-running tasks — a planning-tool primitive for task decomposition/progress tracking,
  subagent spawning with isolated per-subtask context, a virtual filesystem for
  prompts/skills/long-term memory. **Dynamic subagents** let the top agent write short driver
  scripts (loops/branches/concurrency) to orchestrate subagents programmatically — a genuinely new
  orchestration primitive beyond supervisor/swarm/hierarchical.
- **LangSmith Insights Agent + Multi-turn Evals**: auto-categorizes production-trace behavior
  patterns; scores whether an agent achieved the user's goal across a full multi-turn interaction
  rather than per-trace, plus 30+ reusable evaluator templates (safety, quality, trajectory, user
  behavior, multimodal).
- **LangSmith Engine**: analyzes traces and auto-suggests fixes for failing/expensive runs.
- **LangSmith Fleet** (rebrand of "Agent Builder", 2026-03): one-click agent deployment/ops.

Sources: [LangGraph 1.0 GA](https://changelog.langchain.com/announcements/langgraph-1-0-is-now-generally-available),
[LangChain 1.0 GA](https://changelog.langchain.com/announcements/langchain-1-0-now-generally-available),
[LangChain/LangGraph 1.0 blog](https://www.langchain.com/blog/langchain-langgraph-1dot0),
[Microsoft: LangChain v1 GA](https://techcommunity.microsoft.com/blog/azuredevcommunityblog/langchain-v1-is-now-generally-available/4462159),
[LangGraph persistence](https://docs.langchain.com/oss/python/langgraph/persistence),
[Checkpoints reference](https://reference.langchain.com/python/langgraph/checkpoints),
[interrupt() reference](https://reference.langchain.com/python/langgraph/types/interrupt),
[HITL interrupt blog](https://www.langchain.com/blog/making-it-easier-to-build-human-in-the-loop-agents-with-interrupt),
[Streaming docs](https://docs.langchain.com/oss/python/langgraph/streaming),
[Semantic memory search blog](https://www.langchain.com/blog/semantic-search-for-langgraph-memory),
[langgraph-supervisor reference](https://reference.langchain.com/python/langgraph-supervisor),
[Supervisor vs Swarm](https://dev.to/focused_dot_io/multi-agent-orchestration-in-langgraph-supervisor-vs-swarm-tradeoffs-and-architecture-1b7e),
[LangChain MCP integration](https://leanware.co/insights/langchain-mcp-integrating-langchain-with-model-context-protocol),
[LangSmith A2A server](https://docs.langchain.com/langsmith/server-a2a),
[LangGraph A2A feature request](https://github.com/langchain-ai/langgraph/issues/7398),
[OpenTelemetry + LangSmith](https://blog.langchain.com/opentelemetry-langsmith/),
[create_agent reference](https://reference.langchain.com/python/langchain/agents/factory/create_agent),
[Middleware blog](https://www.langchain.com/blog/how-middleware-lets-you-customize-your-agent-harness),
[Deep Agents](https://www.langchain.com/deep-agents),
[Dynamic subagents](https://www.langchain.com/blog/introducing-dynamic-subagents-in-deep-agents),
[Insights Agent + Multi-turn Evals](https://forum.langchain.com/t/introducing-insights-agent-multi-turn-evals/1914).

---

## 2. OpenAI Agents SDK / Responses API

- **Agent / Runner**: an `Agent` is an LLM configured with instructions + tools; a `Runner`
  executes the loop (call model → execute tool calls → check for final output → repeat).
- **Handoffs — mechanism**: implemented as a tool call. `handoff()` injects a tool (default name
  `transfer_to_<agent_name>`) into agent A's tool list; when the model invokes it, the SDK
  switches execution to agent B **within the same run**, transferring conversation history
  (an `input_filter` can strip/transform what B sees). This is ownership transfer, not a
  function call that returns to A. Supports `input_type` (structured args schema validated before
  handoff), `on_handoff` callback, `is_enabled` (static or dynamic gating). A newer
  `nest_handoff_history` compacts history across nested handoffs.
- **Guardrails — mechanism**: plain functions returning `GuardrailFunctionOutput` with a
  `tripwire_triggered: bool`, run **in parallel** with the agent (not sequentially).
  `InputGuardrail`s fire before the agent starts (blocking token spend entirely if tripped);
  `OutputGuardrail`s check the final output of the last agent. A tripped guardrail raises and
  halts the run.
- **Sessions/memory**: a pluggable persistent-history layer (SQLite/SQLAlchemy/Redis/MongoDB
  backends) keeping conversation state across separate `Runner` calls, distinct from
  server-side Responses-API state.
- **Tracing**: on by default, recording every agent invocation, tool call, handoff, guardrail
  check, and model turn.
- **Responses API vs. Chat Completions**: two statefulness modes — server-managed via
  `previous_response_id` chaining or the durable Conversations API object (30-day retention under
  `store: true`), or fully client-managed. Output is a typed array interleaving messages,
  reasoning items, and tool calls (vs. Chat Completions' flat `choices`).
- **Background mode**: `background: true` runs a Responses request asynchronously; poll via GET
  while `queued`/`in_progress`, cancel via `POST /v1/responses/{id}/cancel`. Combinable with
  `stream: true` — stream immediately while work continues, resumable via `starting_after`/
  `sequence_number`. Under Zero-Data-Retention (`store: false`), data is kept ~10 minutes, just
  enough for polling.
- **Encrypted reasoning items**: for ZDR orgs, reasoning models return `encrypted_content` by
  default so workflows stay stateless while still passing prior reasoning back for continuity.
- **Built-in tools**: web search, file search, code interpreter, computer use, image generation —
  declared as native tool entries the model invokes server-side. 2026 addition: web search's
  `return_token_budget` parameter for deep-research-style multi-step search.
- **MCP**: a first-class `mcp` tool type, **Responses API only** (not Chat Completions), connecting
  to remote MCP servers over Streamable HTTP or HTTP/SSE, listing and exposing tools — MCP
  resources/prompts are not currently surfaced, only tools. A "Secure MCP Tunnel" reaches
  private/on-prem servers without public exposure.
- **Structured outputs**: `text.format`/`response_format` with `type: "json_schema"` and
  `strict: true` constrains the token decoder so schema-violating output cannot be emitted;
  refusals are a distinct first-class field.
- **Batch API**: unchanged core shape — JSONL upload, `batches.create()`,
  `validating → in_progress → finalizing → completed`, poll or webhook, up to 50,000
  requests/file, 24-hour window, ~50% cost discount.
- **Prompt caching**: automatic by default (server-side prefix match, ≥1024 tokens, 128-token
  increments, implicit breakpoint at latest user/tool message); retention via
  `prompt_cache_retention` (`in_memory` 5-10 min, or `24h`). **2026 change (GPT-5.6+)**:
  `prompt_cache_options.mode: "explicit"` allows manually placed breakpoints with a `ttl` (only
  `30m`) and a required `prompt_cache_key`; cache writes now cost 1.25× the uncached input rate —
  a real pricing shift from the prior free-implicit-write model. Usage accounting:
  `usage.input_tokens_details.cached_tokens` (Responses) /
  `usage.prompt_tokens_details.cached_tokens` (Chat Completions); GPT-5.6+ also reports
  `cache_write_tokens`.
- **Multi-agent patterns**: *Manager* (central orchestrator calls specialists as tools,
  synthesizing outputs itself — edges are tool calls) vs. *Decentralized/handoff* (no central hub;
  an out-of-scope agent hands off and the specialist takes over ownership directly — edges are
  handoffs, control does not return).

**Notable in 2026 specifically**: the **Assistants API** is deprecated (2025-08-26) with hard
shutdown **2026-08-26** — imminent as of this research; the Responses API is the only forward
path. The GPT-5.6 caching overhaul (explicit breakpoints, write premium) is a genuine behavior
change, not just a new flag.

Sources: [Agents SDK guide](https://developers.openai.com/api/docs/guides/agents),
[openai-agents-python](https://openai.github.io/openai-agents-python/),
[Handoffs](https://openai.github.io/openai-agents-python/handoffs/),
[Guardrails](https://openai.github.io/openai-agents-python/guardrails/),
[Multi-agent orchestration](https://openai.github.io/openai-agents-python/multi_agent/),
[Conversation state](https://developers.openai.com/api/docs/guides/conversation-state),
[Background mode](https://developers.openai.com/api/docs/guides/background),
[Prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching),
[Batch API](https://developers.openai.com/api/docs/guides/batch),
[MCP and Connectors](https://developers.openai.com/api/docs/guides/tools-connectors-mcp),
[Structured Outputs](https://openai.com/index/introducing-structured-outputs-in-the-api/),
[New tools in Responses API](https://openai.com/index/new-tools-and-features-in-the-responses-api/),
[Deprecations](https://developers.openai.com/api/docs/deprecations),
[Assistants API sunset thread](https://community.openai.com/t/assistants-api-beta-deprecation-august-26-2026-sunset/1354666).

---

## 3. Microsoft Agent Framework — delta since 2026-07

This project's own `2026-maf-orchestration-patterns.md` (compiled 2026-08-04) is current as of
MAF doc revision `ms.date: 2026-05-27`, last updated 2026-07-10. This section reports **only what
changed since then**, distinguishing NEW from CONFIRMED-STILL-CURRENT.

- **CONFIRMED-STILL-CURRENT**: the five orchestration patterns (Sequential, Concurrent, Group
  Chat, Handoff, Magentic) have not grown to a sixth.
- **NEW (2026-07-08)**: all five patterns crossed 1.0/stable simultaneously (Python
  `agent-framework-orchestrations` 1.0.0 and .NET) — previously some were experimental. All now
  uniformly support streaming, checkpointing, HITL approvals, and pause/resume.
- **NEW, ecosystem-adjacent not core (2026-07-30)**: "Squad" — an external multi-agent framework
  built on GitHub Copilot CLI/SDK, bridged into MAF via a `Squad.Agents.AI` package wrapping a
  Squad team as a standard `AIAgent`. Adds institutional memory (persistent decisions, skill
  extraction, learned preferences) that Magentic/Group Chat don't natively provide — not a
  built-in MAF orchestration pattern.
- **NEW, breaking change (Python 1.13.0, ~2026-07-30)**: checkpoints are now fully replayable from
  initial input plus HITL responses — pending requests are saved into checkpoint state, and
  restoring re-emits them as `RequestInfoEvent`s. `workflow.run(checkpoint_id=..., responses=...)`
  can resume and supply responses in the same call — a materially different replay model than what
  would have been current in the May/July doc revisions this project's prior research cites.
- **CONFIRMED-STILL-CURRENT**: core MCP tool-invocation and A2A cross-runtime agent messaging have
  been native since the 2026-04-03 1.0 GA, unchanged in kind since.
- **NEW (.NET, 2026-07-28)**: Agent Skills discovery **from MCP servers** — skill-md and
  archive/zip distribution, size/count-bounded extraction, no script execution.
- **NEW (Python 1.12.0, 2026-07-21)**: app-owned MCP hosting helpers — expose your own
  agents/workflows *as* native MCP tools (previously consume-only-oriented in the helper layer).
- **NEW (2026-08-04)**: GitHub Copilot Harness integration adds MCP server support plus
  permission-gated shell/file/web tools layered with MAF middleware/approval/observability.
- **NEW (2026-07-23)**: Declarative Workflows reached 1.0 in both SDKs — newly
  documented/stabilized: `If` conditional branching, looping/jump constructs, Power Fx
  expressions for in-workflow state computation, HTTP-request invocation as a step type, MCP tool
  invocation from YAML, multi-agent handoff routing purely in YAML, pause-for-human-input/
  checkpoint-resume — loadable via `WorkflowFactory`/`DeclarativeWorkflowBuilder` alongside
  code-first workflows.
- **NEW (~2026-07-24/25)**: `CosmosMemoryContextProvider` (Python) — durable cross-session memory
  backed by Azure Cosmos DB as a first-class context-provider plug-in.
- **NEW (Python 1.13.0, 2026-07-30)**: reusable session stores for persisting complete Foundry
  Responses sessions.
- **CONFIRMED-STILL-CURRENT**: the base context-provider abstraction (`before_run`/`after_run`
  hooks, pluggable Mem0/Redis/Neo4j/custom backends) predates July, unchanged in shape.
- **NEW, GA (2026-07-22)**: **Agent Framework Harness** — a production-ready runtime wrapper
  bundling tool-call loop, history persistence, context compaction, plan/execute modes with
  persistent todos, durable memory, skills discovery, tool-approval workflows, built-in
  OpenTelemetry (some sub-features, e.g. background agents, remain preview).
- **NEW**: Agent Skills subsystem reached stable (.NET 2026-07-07, Python 2026-07-15); harness
  agents, message-injection middleware, and tool-approval middleware graduated from experimental
  to stable in Python 1.12.0.

**Explicit non-findings**: no sixth built-in orchestration pattern; no new interop protocol beyond
MCP/A2A; no deprecation of the harness/orchestration/declarative-workflow lines since July 2026.

Sources: [Orchestration patterns reach 1.0](https://devblogs.microsoft.com/agent-framework/agent-frameworks-orchestration-patterns-reach-1-0/),
[Squad + Agent Framework](https://devblogs.microsoft.com/agent-framework/building-agent-teams-with-agent-framework-github-copilot-cli-and-squad/),
[Agent Skills from MCP servers](https://devblogs.microsoft.com/agent-framework/discover-agent-skills-from-mcp-servers-in-net/),
[Declarative Workflows 1.0](https://devblogs.microsoft.com/agent-framework/move-agent-orchestration-workflows-out-of-code-with-agent-framework-declarative-workflows-1-0/),
[Agent Framework Harness GA](https://devblogs.microsoft.com/agent-framework/the-microsoft-agent-framework-harness-is-now-released/),
[GitHub Copilot Harness](https://devblogs.microsoft.com/agent-framework/build-production-ready-agents-with-the-github-copilot-harness-and-agent-framework/),
[Cosmos DB native memory](https://devblogs.microsoft.com/cosmosdb/native-agent-memory-for-microsoft-agent-framework-powered-by-azure-cosmos-db/),
[GitHub Releases](https://github.com/microsoft/agent-framework/releases).

---

## 4. CrewAI

- **Crews vs. Flows**: a Crew is a role-based agent team (agents + tasks) that runs autonomously
  to completion, stateless by default, no orchestration-level branching/recovery. A Flow is a
  plain Python class using `@start`/`@listen`/`@router` decorators to build an event-driven
  execution graph; state is a Pydantic model shared across steps (in-memory by default — crash
  recovery requires manually serializing `self.state.model_dump()`). Composition: Flows are the
  deterministic outer control layer that invokes Crews (and direct LLM calls) as steps requiring
  autonomous reasoning.
- **Structured output**: `output_json` (validates into a dict) and `output_pydantic` (returns a
  validated Pydantic model, at the cost of ~50-100 extra prompt tokens for the embedded schema),
  both task-level parameters.
- **MCP**: full support via an `mcps` field on agents (string refs or structured
  `MCPServerAdapter` config); stdio, SSE, and Streamable HTTP transports; tool schemas
  auto-convert to Pydantic.
- **A2A**: first-class delegation primitive (`pip install 'crewai[a2a]'`) — agents can act as A2A
  clients (autonomously choosing local execution vs. remote delegation) or A2A-compliant servers,
  via `A2AClientConfig`/`A2AServerConfig` with Bearer/OAuth2/API-key/HTTP auth.
- **2026-notable**: **AOP** (Agent Operations Platform) and **AMP** (Agent Management Platform) —
  enterprise infra with a no-code visual builder, RBAC, audit logs, governance; **AMP Factory**
  adds private-infra deployment (on-prem, AWS/Azure/GCP VPC) with SSO. Also: dynamic LLM loading
  in the crew wizard, inline skill definitions, new flow authoring/templating/streaming.

Sources: [CrewAI Flows guide](https://www.jahanzaib.ai/blog/crewai-flows-production-multi-agent-guide),
[Flow state management](https://docs.crewai.com/en/guides/flows/mastering-flow-state),
[output_pydantic](https://markaicode.com/crewai-output-pydantic-structured-agent-results/),
[MCP servers as tools](https://docs.crewai.com/v1.15.4/en/mcp/overview),
[A2A delegation](https://docs.crewai.com/en/learn/a2a-agent-delegation),
[CrewAI AOP announcement](https://www.businesswire.com/news/home/20251119857048/en/CrewAI-Strengthens-AI-Agent-Operations-Platform-With-New-Product-Global-Expansion-and-AI-Course-with-Andrew-Ng),
[CrewAI changelog](https://docs.crewai.com/en/changelog).

---

## 5. AG2 (formerly AutoGen)

The original AutoGen lineage split three ways as of March 2026:

- **Microsoft Agent Framework (MAF)** — 1.0 GA 2026-04-03, merging AutoGen's multi-agent
  orchestration with Semantic Kernel's enterprise plumbing (session state, type safety, filters,
  telemetry, connectors), Microsoft's official production successor.
- **microsoft/autogen v0.7.x** — maintenance mode only (bug/security fixes, no new
  features/patterns).
- **AG2** (`ag2ai/ag2`, Apache 2.0) — the community-led continuation under org AG2AI (founded
  2024-11), led by original AutoGen paper authors Chi Wang and Qingyun Wu, backward-compatible
  with the legacy v0.2 GroupChat style, actively developed independently with a release as recent
  as 2026-07-29.

**Orchestration**: AG2 v0.9 (2025-04) merged Group Chat and Swarm into a single unified Group Chat
system — ConversableAgents, Patterns (turn-taking logic), Handoffs (structured control transfer),
Guardrails, Context Variables (shared state). Built-in patterns include `AutoPattern`
(LLM-selected next speaker) and `RoundRobinPattern`; the standalone `Swarm` API still works but is
deprecated.

**Protocol support**: AG-UI lists AG2 among frameworks with out-of-the-box integration. A2A and
MCP are both demonstrated via published sample implementations (AG2 agents as both MCP tool
consumers and A2A interop participants) — this reads as sample/ecosystem-level support rather than
a first-class built-in field comparable to CrewAI's.

Sources: [Two Lineages, One Framework](https://alexbevi.com/blog/2026/06/18/two-lineages-one-framework-how-autogen-and-semantic-kernel-became-the-microsoft-agent-framework/),
[Microsoft retires AutoGen](https://agentmarketcap.ai/blog/2026/04/13/microsoft-autogen-maintenance-mode-agent-framework-sunset-2026),
[ag2ai/ag2](https://github.com/ag2ai/ag2),
[AG2 v0.9 release](https://docs.ag2.ai/latest/docs/blog/2025/04/28/0.9-Release-Announcement/),
[AG2 orchestration docs](https://docs.ag2.ai/latest/docs/user-guide/advanced-concepts/orchestration/orchestrations/),
[AG-UI agentic protocols](https://docs.ag-ui.com/agentic-protocols),
[A2A MCP AG2 sample](https://a2aprotocol.ai/docs/guide/a2a-mcp-ag2-sample).

---

## 6. LlamaIndex

- **Workflows**: `llama-index-workflows` is an event-driven, async-first, step-based execution
  model, independent of the RAG stack. Steps consume/emit typed Events; the runtime routes events
  to whichever step subscribes to that event type. Supports branching, looping, parallelization,
  stateful pause/resume, and failure recovery in plain Python (no DSL). Agents (ReAct,
  function-calling patterns) are built on top of Workflows, making agent internals themselves
  inspectable event graphs. "Workflows 1.0" was announced as a standalone lightweight agentic
  framework.
- **Memory**: the long-standing `ChatMemoryBuffer` (token-capped, in-process) is being deprecated
  in favor of a new `Memory` class supporting pluggable `MemoryBlock` modules (e.g.
  `FactExtractionMemoryBlock`) backed by SQLite for cross-restart persistence. Other variants:
  `ChatSummaryMemoryBuffer` (summarizes on overflow), `SimpleComposableMemory` (stacks multiple
  memory sources). All conform to a `BaseMemory` interface (`put`/`get`/`reset`).
- **MCP**: first-class and bidirectional — LlamaIndex agents can consume existing MCP servers'
  tools, and LlamaIndex Workflows can themselves be served as MCP servers.
- **A2A**: LlamaIndex Workflows-based conversational agents can be exposed via A2A (demonstrated
  in a published "file chat" sample with multi-turn dialogue, streaming, citations) — again
  ecosystem/sample-level rather than a dedicated built-in flag.
- **2026-notable**: repositioned around three pillars — Workflows (composition primitive),
  llama-deploy (production serving), eval tooling. Shipped **LlamaAgents** (one-click
  document-agent deployment, templates for invoice processing/contract review/claims handling) and
  **LlamaAgents Builder** (natural-language description → deployable agent with API+UI, file
  upload). January 2026 added Agent Client Protocol integration. March 2026 open-sourced
  **LiteParse**, a lightweight local document parser (layout preservation, local OCR, multimodal-LLM
  support).

Sources: [llama-index-workflows](https://pypi.org/project/llama-index-workflows/),
[Workflows 1.0](https://www.llamaindex.ai/blog/announcing-workflows-1-0-a-lightweight-framework-for-agentic-systems),
[Memory module guide](https://developers.llamaindex.ai/python/framework/module_guides/deploying/agents/memory/),
[MCP module guide](https://developers.llamaindex.ai/python/framework/module_guides/mcp/),
[A2A file-chat sample](https://a2aprotocol.ai/blog/a2a-samples-llama-index-file-chat-openrouter),
[LlamaAgents](https://www.llamaindex.ai/blog/llamaagents-build-serve-and-deploy-document-agents),
[Newsletter 2026-01-06](https://www.llamaindex.ai/blog/llamaindex-newsletter-2026-01-06),
[Newsletter 2026-03-24](https://www.llamaindex.ai/blog/llamaindex-newsletter-2026-03-24).

---

## 7. Cross-framework patterns worth naming

Recurring shapes across independently-built frameworks, noted here as landscape signal only (no
claim about any particular project's design is made or implied):

- **MCP has converged as the default tool-calling interop protocol.** Every framework surveyed has
  first-class client support in 2026; several (LlamaIndex, CrewAI, MAF as of July) now also expose
  the framework's own agents *as* MCP servers, not just consume them.
  **A2A support is consistently the newer, thinner layer** — native in MAF and CrewAI, but
  sample/ecosystem-level (not a dedicated built-in surface) in LangGraph core and AG2.
- **Durable checkpointing + interrupt-based human-in-the-loop is now a standard pairing**, not a
  differentiator: LangGraph's `interrupt()`/`Command(resume=...)`, MAF's `RequestInfoEvent`
  replay-from-checkpoint (new July 2026), and the OpenAI Agents SDK's pause-for-approval flows all
  independently converge on "checkpoint state before the pause point, resume by replaying from a
  saved point with the human's answer injected."
- **Declarative/YAML workflow authoring is trending as a second surface alongside code**, not a
  replacement for it: MAF's Declarative Workflows reached 1.0 in July 2026; LangGraph exposes an
  equivalent via `langgraph.json` server config; CrewAI's Flow decorators are the code-first
  analogue Crews' YAML-configured agents sit alongside.
- **Handoff-as-tool-call is the dominant multi-agent control-transfer mechanism** across frameworks
  that support decentralized agent-to-agent transfer (OpenAI Agents SDK, LangGraph Swarm, MAF
  Handoff, AG2's Handoffs) — the common shape is: expose the transfer as an ordinary tool in the
  model's tool list, and when invoked, the runtime — not the model — performs the actual context
  handoff and ownership transfer.
- **Strict-schema structured output is now table stakes**: every framework surveyed defaults or
  strongly steers toward schema-constrained decoding (OpenAI's `strict: true` JSON schema mode,
  LangChain's `ToolStrategy`/`ProviderStrategy`, CrewAI's `output_pydantic`) rather than
  prompt-only JSON-mode.
- **Long-term/cross-session memory is being split from short-term/thread-scoped state** as a
  distinct abstraction in every framework that has both: LangGraph's `BaseStore` vs.
  `Checkpointer`, MAF's `CosmosMemoryContextProvider` vs. session stores, LlamaIndex's `Memory`
  class vs. per-run state, OpenAI's Sessions vs. per-response state.
- **Agent-ops/observability platforms are the newest layer being built on top of all of this**:
  LangSmith (Insights Agent, Multi-turn Evals, Engine, Fleet) and CrewAI's AOP/AMP both shipped
  major enterprise-ops surfaces in the last ~6 months, distinct from the underlying orchestration
  runtime.
