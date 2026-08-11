# MAF middleware, ContextProvider pipeline, CodeAct, and Skills — deep-dive, source-grounded

**Date:** 2026-08-11 · **Source:** local checkouts of Microsoft Agent Framework at
`D:\GitSrc\agent-framework` and its `Microsoft.Extensions.AI`/`Microsoft.Extensions.AI.Abstractions`
NuGet dependency source at `D:\GitSrc\extensions` (both read-only references; state as checked out on
this date). File paths and excerpts below are verbatim from those trees, not paraphrase. This
supersedes/extends `docs/research/2026-maf-provider-concepts.md` (2026-07-31) — that file's §1
summary of `AIContextProvider`'s merge as "concatenating ... across all registered providers" is
accurate but incomplete; §2 below gives the exact pipeline mechanics that summary was missing.

---

## 1. Middleware decorator chain

**`IChatClient`** — `D:\GitSrc\extensions\src\Libraries\Microsoft.Extensions.AI.Abstractions\ChatCompletion\IChatClient.cs`.
Three members: `GetResponseAsync`, `GetStreamingResponseAsync`, `GetService(Type, object?)` (a
service-locator escape hatch to reach wrapped instances/metadata through a decorator chain).

**`DelegatingChatClient`** — same directory — stores `InnerClient` and virtually forwards every
call; a middleware subclass overrides only what it needs.

**`ChatClientBuilder`** — `Microsoft.Extensions.AI\ChatCompletion\ChatClientBuilder.cs` — holds
`List<Func<IChatClient, IServiceProvider, IChatClient>>`. `.Use(...)` appends a factory; `Build()`
applies them **in reverse order**, so the first `.Use()` call ends up outermost:

```csharp
for (var i = _clientFactories.Count - 1; i >= 0; i--)
    chatClient = _clientFactories[i](chatClient, services);
```

Shipped middleware: `FunctionInvokingChatClient` (`UseFunctionInvocation`), `LoggingChatClient`
(`UseLogging`), `OpenTelemetryChatClient` (`UseOpenTelemetry`), `CachingChatClient`/
`DistributedCachingChatClient`, `ReducingChatClient`, `ImageGeneratingChatClient`,
`ConfigureOptionsChatClient`. No built-in rate-limiting client ships (grep-confirmed absent).

**Agent-level middleware is a *separate* pipeline**, not a reuse of the chat-client one:
`AIAgentBuilder` (`dotnet/src/Microsoft.Agents.AI/AIAgentBuilder.cs`) is a structural clone of
`ChatClientBuilder`, wrapping `AIAgent`/`AgentRunOptions`/`AgentSession` instead of
`IChatClient`/`ChatOptions`, same reverse-apply `.Use()` semantics. Built-ins:
`LoggingAgentBuilderExtensions.UseLogging`, `OpenTelemetryAgentBuilderExtensions.UseOpenTelemetry`,
`FunctionInvocationDelegatingAgentBuilderExtensions.Use(...)`, and
`AIAgentBuilder.UseAIContextProviders(...)`.

**Python** — `python/packages/core/agent_framework/_middleware.py` defines `ChatMiddleware`,
`FunctionMiddleware`, `AgentMiddleware` (each an ABC with `async def process(self, context,
call_next)`), plus bare-function decorators (`chat_middleware`, `function_middleware`,
`agent_middleware`). Composition is **class-mixin + MRO**, not a builder object — e.g.
`agent_framework_anthropic/_chat_client.py:1599`:

```python
class AnthropicClient(
    FunctionInvocationLayer[AnthropicOptionsT],
    ChatMiddlewareLayer[AnthropicOptionsT],
    ChatTelemetryLayer[AnthropicOptionsT],
    RawAnthropicClient[AnthropicOptionsT],
):
```

MRO order *is* pipeline order (asserted by test, `packages/anthropic/tests/test_anthropic_client.py:134-140`):
function-invocation loop → user chat middleware → telemetry → raw provider call. `Agent`
(`_agents.py:1751`) is layered the same way: `AgentMiddlewareLayer, AgentTelemetryLayer, RawAgent`.
Agent-level `AgentMiddleware` intercepts the whole `agent.run()` call; `ChatMiddleware`/
`FunctionMiddleware` intercept individual model/tool calls — two distinct interception depths, same
as .NET's chat-client-vs-agent split.

**Relevance to AgentEngine:** `include/agentengine/core/agent.hpp`'s `Middleware<Ms...>` is currently
an empty CRTP tag type — no interface, no chain-composition logic, zero consumers in `src/` (grep-confirmed).
This section is the prior art to draw from whenever that policy moves from declared-but-unimplemented
to real: specifically, the **two-independent-pipelines** shape (a wrapping layer around the raw
model call, and a separate wrapping layer around the whole agent run/turn) and the **reverse-apply
ordering convention** so the first-registered middleware ends up outermost/first-to-see-the-call.

---

## 2. `AIContextProvider`'s exact merge algorithm — a sequential pipeline, not fan-out

**Signatures** (`dotnet/src/Microsoft.Agents.AI.Abstractions/AIContextProvider.cs`):

```csharp
public ValueTask<AIContext> InvokingAsync(InvokingContext context, CancellationToken ct = default)
public ValueTask InvokedAsync(InvokedContext context, CancellationToken ct = default)
```

`AIContext` — exactly three properties: `Instructions: string?`, `Messages: IEnumerable<ChatMessage>?`,
`Tools: IEnumerable<AITool>?`.

**Multi-provider composition, `ChatClientAgent.cs:772-785`:**

```csharp
var aiContext = new AIContext { Instructions = chatOptions?.Instructions, Messages = inputMessagesForChatClient, Tools = chatOptions?.Tools };
foreach (var aiContextProvider in aiContextProviders)
{
    var invokingContext = new AIContextProvider.InvokingContext(this, typedSession, aiContext);
    aiContext = await aiContextProvider.InvokingAsync(invokingContext, cancellationToken).ConfigureAwait(false); // reassigned
}
```

`aiContext` is **reassigned** every iteration — provider N's `InvokingContext.AIContext` is provider
N−1's already-merged output. The doc comment on `AIContextProvider.cs:406-417` states this outright:
*"If multiple AIContextProvider instances are used in the same invocation, each AIContextProvider
will receive the context returned by the previous AIContextProvider."* Inside each provider, the
default `InvokingCoreAsync` (`AIContextProvider.cs:146-200`) does the actual per-provider merge —
`Concat` for `Messages`/`Tools`, `"\n"`-join for `Instructions` — against whatever it was handed.
`InvokedAsync` runs **forward** (same direction as `InvokingAsync`), all providers sharing one
`InvokedContext` (`ChatClientAgent.cs:489-497`).

Final fold into the real request (`ChatClientAgent.cs:787-805`): `aiContext.Tools`/`.Instructions`
are written back onto `ChatOptions.Tools`/`.Instructions` (the accumulation already happened via
`Concat` inside the loop); messages become the list sent to the underlying `ChatClient`, provider
contributions landing after history+input in list order.

**Python** achieves the same effect by a different mechanic — mutation of one shared object instead
of return-and-rebind (`_agents.py`):

```python
for provider in self.context_providers:          # before_run: forward
    await provider.before_run(agent=self, session=provider_session, context=session_context, state=...)
...
for provider in reversed(self.context_providers): # after_run: LIFO — diverges from .NET's forward InvokedAsync
    await provider.after_run(agent=self, session=provider_session, context=context, state=...)
```

All providers mutate the same `session_context` (`extend_messages`/`extend_instructions`/
`extend_tools`), so provider N still "sees" 1..N−1's additions by reading current state —
functionally a pipeline, implemented without reassignment. **One real .NET/Python divergence**:
`InvokedAsync` is forward-order in .NET, but Python's `after_run` is explicitly reversed/LIFO
(`_agents.py:549,573`, confirmed by docstring and code).

**Relevance to AgentEngine:** `include/agentengine/core/context_assembly.hpp`'s `assemble_context()`
runs contributors as **independent fan-out** — each `on_context()` call receives only
`SessionContext`/`EffectContext`, never a prior provider's `ContextContribution` — then merges all
results in declared order afterward (`instructions` concatenated, `messages`/`tools` appended). This
is the single largest verified divergence from MAF's actual mechanics found in this session: MAF's
pipeline lets provider N *react to* what provider N−1 added (e.g. dedupe against an earlier
provider's tool list, or skip its own contribution if an earlier one already covered it); AgentEngine's
current fan-out structurally cannot. See `OpenQuestions.md` for the tracked open question this
motivates.

---

## 3. CodeAct's tool-registry design — additive by rule, not by accident

MAF's own `docs/decisions/0024-codeact-integration.md` (in the `agent-framework` repo — **distinct
from, and unrelated to, AgentEngine's own** `decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md`;
same number, different repos, different topics — disambiguated here to prevent future confusion)
records three considered options for where CodeAt should live in the architecture:

- **Chosen**: `AIContextProvider`/`ContextProvider` — *"providers operate before model invocation,
  which is where CodeAct must add instructions and reshape tools"*; *"lets us preserve existing
  function invocation behavior rather than rewriting it."*
- Rejected: a dedicated `IChatClient` decorator — *"duplicates responsibilities already handled by
  provider abstractions."*
- Rejected: integrating directly into `FunctionInvokingChatClient`/Python's `FunctionInvocationLayer`
  — *"it is the wrong layer for constructing the model-facing tool surface"* and would over-couple
  the agent framework to a lower layer.

Consequence: `LocalCodeActProvider : AIContextProvider` (`dotnet/src/Microsoft.Agents.AI.LocalCodeAct/LocalCodeActProvider.cs:31,182`)
overrides **only** `ProvideAIContextAsync`, never `InvokingCoreAsync` — so it goes through the
default `Concat`-based merge described in §2, contributing exactly one tool
(`execute_code`) that gets **appended** to whatever `ChatOptions.Tools` already held, never replacing
it. The design doc states this as a hard rule, not an implementation detail
(`docs/features/code_act/dotnet-implementation.md:47-51`): *"The provider must not infer its
CodeAct-managed tool set from the agent's direct tool configuration... Exclusive versus mixed
behavior is achieved by where tools are configured, not by rewriting the agent's direct tool list."*
`LocalCodeActProviderOptions.Tools` is a registry entirely separate from `ChatClientAgentOptions.Tools`
— a tool reachable both via direct function-calling and via `call_tool(...)` inside generated code
must be registered in both places explicitly; nothing syncs them.

**Relevance to AgentEngine:** `include/agentengine/core/codeact_tool_union.hpp::union_codeact_tools`
already implements the same additive-never-replace shape independently — a strict union across
`(agent_tools, skill_unlocked_tools, mcp_tools)`, appending every source's descriptors. **This is
corroboration of an already-correct design, not a gap**: AgentEngine's union goes one step further
than MAF's plain `Concat` by hard-erroring on any cross-source tool-name collision
(`codeact.tool_name_collision_across_sources`), where MAF's `Concat` has no such uniqueness
constraint at all.

---

## 4. `SkillsProvider` deep mechanics

**Constant 3-tool surface, never grows.** `AgentSkillsProvider.BuildTools`/Python's `_create_tools`
unconditionally return the same 3 generic tools (`load_skill`, `read_skill_resource`,
`run_skill_script`) on **every** turn regardless of catalog size or "loaded" state — confirmed by
grep across the whole `Skills/` directory (.NET) and `_skills.py` (Python) for any
`Promote`/`Expand`/`as_tool`/`tools.append` pattern that would surface an individual skill script as
its own tool schema entry: zero matches. "Progressive disclosure" happens entirely in the
**conversation-history/content layer**, not the tool-schema layer: `load_skill`'s result (the full
SKILL.md body) is an ordinary `FunctionResultContent` message; there is no provider-held "loaded
skills" state anywhere (`AgentSkillsSourceContext` carries only `Agent`/`Session`, no state bag;
Python's `before_run` signature documents its `state` param as literally *"unused by this
provider"*). Calling `load_skill` twice on the same skill is idempotent, not deduped.

**Two-tier fault tolerance in the dispatcher itself** (`AgentSkillsProvider.cs:414-452`):

```csharp
private async Task<object?> RunSkillScriptAsync(...)
{
    if (string.IsNullOrWhiteSpace(scriptName)) return "Error: Script name cannot be empty.";   // tier 1: soft text
    var skill = skills.FirstOrDefault(...);
    if (skill == null) return $"Error: Skill '{skillName}' not found.";                        // tier 1
    try { return await script.RunAsync(skill, arguments, serviceProvider, cancellationToken); }
    catch (Exception ex)                                                                        // tier 2
    {
        LogScriptExecutionError(this._logger, skillName, scriptName, ex);
        if (this._options?.IncludeDetailedErrors == true)
            return $"Error: Failed to execute script '{scriptName}'... {ex.Message}";
        throw; // deferred to the same policy ordinary tool calls get from FunctionInvokingChatClient
    }
}
```

Validation failures always come back as recoverable text the model can react to; execution
exceptions are logged either way, and whether the model *sees* the exception message is an opt-in
flag — otherwise it re-throws into the standard tool-call exception handling, not a
skills-specific mechanism. Python's `_run_skill_script` (`_skills.py:2596-2636`) mirrors this
exactly, its docstring stating the re-raise "delegat[es] error handling to the function-invocation
pipeline (which applies its own `include_detailed_errors` policy)."

**`InlineSkill`/`ClassSkill` embed a real `AIFunction`, but its schema becomes text, not a tool
declaration.** `AgentInlineSkillScript` wraps a delegate via `AIFunctionFactory.Create` — the same
machinery normal tools use — and re-exposes `ParametersSchema => this._function.JsonSchema`
(`AgentInlineSkillScript.cs:42-43,76`). That schema is rendered into the `<available_scripts>` block
of the skill's content by `AgentInlineSkillContentBuilder.BuildAvailableScriptsBlock`
(`:102-138`), i.e. the model reads the parameter schema as **text inside a `load_skill` response**,
never as its own tool's declared parameters. Python's `InlineSkillScript.parameters_schema`
(`_skills.py:392-404`) does the identical thing via `FunctionTool(...).parameters()`. Developers
never hand-write parameter docs — the schema is auto-derived from the callable's signature, just
repurposed from "tool declaration" to "documentation string."

**Approval is coarse-grained by design, a documented MAF limitation.** There is no per-script
approval flag on `AgentInlineSkillScript`/`InlineSkillScript`; approval wraps only the shared
`run_skill_script` dispatcher tool (`AgentSkillsProvider.cs:314-341`,
`DisableRunSkillScriptApproval` defaulting to requiring approval). One approval decision covers
every script in every skill reachable through that one dispatcher call. MAF's own ADR names this as
a known trade-off, not an oversight: *"Approval breaks for `AIFunction`-based representations... the
second approval invocation will not work correctly"* — cited as the reason scripts were kept as a
custom type rather than raw `AIFunction`s that could otherwise plug into `FunctionInvokingChatClient`'s
existing per-tool approval.

**Relevance to AgentEngine:** this whole section is corroborating evidence for
`decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md`'s existing rationale, not new grounds to
revisit it. ADR-024 already found (§8 addendum) that MAF's `load_skill` is "advisory only, never
enforced in code" and built `MountedSkillsState` as real, persistent, per-run state instead. The
approval-coarseness finding here is the same shape of gap one level down (per-dispatcher, not
per-script) — AgentEngine's `allowed-tools`-scoped, mount-state-driven tool visibility is a stronger
answer to both problems at once, by construction, not by later patching MAF's dispatcher-approval
model.

---

## 5. Fixed-data-model escape hatches — two different strategies for the same problem

`ChatMessage`/`ChatOptions`/`ChatResponse` (.NET) and their Python equivalents are deliberately
lossy, provider-neutral. Two independent strategies compensate:

**.NET — pure escape-hatch fields, no schema forking.** `ChatOptions.RawRepresentationFactory:
Func<IChatClient, object?>?` (a callback letting a caller pre-seed/mutate the provider's native
request object before it's sent) plus `AdditionalProperties` bags, and `RawRepresentation: object?`
on `AIContent`/`ChatMessage`/`ChatResponse` (the provider's native SDK object, preserved for read
access). No provider-specific `ChatOptions` subclass ships anywhere in
`Microsoft.Agents.AI.OpenAI`/`Anthropic`/`AzureAI` (grep-confirmed absent) — all provider richness
flows through these two fields.

**Python — per-provider `TypedDict` subclassing, reusing the base schema.** e.g.
`AnthropicChatOptions(ChatOptions[ResponseModelT], total=False)`
(`agent_framework_anthropic/_chat_client.py:111`) adds Anthropic-only keys (`thinking`, `top_k`,
`service_tier`) and **explicitly types unsupported base fields as `None`** (`seed: None`, `store:
None`) — a compile-time-visible "not supported" signal rather than a silent drop. Every
message/content/response object still also carries `raw_representation: Any` +
`additional_properties: dict[str, Any]`, so the escape hatch exists in both languages; Python
additionally lets the type system communicate provider-specific shape.

**Relevance to AgentEngine:** input for `004-Model-Provider-Plane.md`'s existing porting note (which
already correctly identifies Anthropic's cumulative→incremental usage conversion, tool-schema
shaping, and structured-output shaping as portable logic) — the two schema-extension *strategies*
above are a further, previously-unrecorded data point for whichever escape-hatch mechanism
AgentEngine's own `ChatOptions`/`ChatClient` seam ends up needing for provider-specific
configuration.

---

## 6. Confirmed: no formal "AgentProvider" type exists in MAF

Multi-agent name resolution (used by `*WorkflowBuilder` classes —
`SequentialWorkflowBuilder`/`HandoffWorkflowBuilder`/`MagenticWorkflowBuilder`/etc. in
`Microsoft.Agents.AI.Workflows`, and Python's `DeclarativeWorkflowBuilder`) is plain `dict`/registry
arguments passed at build time — e.g. `DeclarativeWorkflowBuilder(agents: dict[str, Any] | None =
None)` (`python/packages/declarative/agent_framework_declarative/_workflows/_declarative_builder.py:132`)
— not a named abstraction with its own interface. Unlike `AIContextProvider`/`Middleware`
(genuine, interface-backed extension points), "agent provider" is convention/glue code at the
call site, not a framework concept. Recorded here because the term surfaced during this session's
research; no AgentEngine doc currently claims otherwise, and none should imply a formal MAF
precedent exists for it.
