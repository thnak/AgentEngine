# 027 — Vocabulary and Naming

**Status:** Draft · **Depends on:** all · **Gate:** §8 · **Normative for:** every public identifier

## Goal

One canonical name per concept, chosen to be **recognizable to someone who already knows Microsoft
Agent Framework**, and one register of the words that mean different things in different
specifications — so that neither a human nor a model has to guess what `Resource` or `Task` means in
a given file.

## 1. The naming rule

> **Adopt MAF's name wherever the concept is the same. Keep a different name only where the concept
> genuinely differs — and say why, here, in this file.**

Rationale, in order:

1. **Transfer.** A developer arriving from MAF (or a model trained on it) should not have to relearn
   vocabulary to express the same idea. The engine is deliberately MAF-shaped; the names should
   admit it.
2. **Fewer wrong guesses.** The same argument as 026 §1 applies to API names, not just to the
   in-sandbox surface: names a model has seen are names it gets right.
3. **Honest divergence.** Where we *do* differ, the difference is usually load-bearing (a capability
   model MAF does not have), and giving it a distinct name makes that visible instead of hiding a
   different meaning behind a familiar word.

The rule has one hard limit: **never reuse a MAF name for a different concept.** A familiar name
with unfamiliar semantics is worse than an unfamiliar name.

## 2. Canonical names — core

Names verified against `agent_framework` (Python core) unless marked **ours**.

| Name | Meaning here | Source |
|---|---|---|
| `Agent` | A named unit that turns input into output using a model, tools, and instructions (002) | MAF |
| `AgentSession` | The durable conversation state an agent operates on; one Quark actor instance (005) | MAF |
| `SessionContext` | The per-run view of a session handed to providers and middleware | MAF |
| `SessionStore` | The persistence seam for sessions | MAF |
| `ChatClient` | The inference seam — one provider/endpoint (004) | MAF (`BaseChatClient`) |
| `Message` | One turn: role + ordered contents (003) | MAF |
| `Content` | One element of a message: text, image, reasoning, tool call, tool result… (003) | MAF |
| `ChatResponse` / `ChatResponseUpdate` | A model call's result / streamed increment | MAF |
| `AgentResponse` / `AgentResponseUpdate` | A run's result / streamed increment | MAF |
| `ResponseStream` | The streamed-response handle | MAF |
| `UsageDetails` | Token and cost accounting (003 §6) | MAF |
| **`Run`** | One invocation of an agent against a session — the unit of tracing, checkpointing, replay, and the thing an A2A `Task` maps to (001) | **ours** |
| **`Turn`** | One model call plus the tool invocations it triggers, inside a run | **ours** |

### Why `Run` and `Turn` are ours

MAF has no first-class run object; the invocation is a method call and its identity lives in
telemetry. We need a *named, addressable, resumable* thing because 019 checkpoints it, 001 gives it
a lifecycle, 013 streams its events, and 012 projects it as an A2A `Task`. Inventing `Run` is
cheaper than overloading `AgentResponse` with a lifecycle it does not have.

## 3. Canonical names — tools, skills, plugins, providers

| Name | Meaning here | Source |
|---|---|---|
| `Tool` | A declared, schema-typed capability an agent may invoke (006) | MAF / MCP |
| `FunctionTool` | A tool backed by a native function | MAF |
| `ApprovalMode` | `never_require` / `always_require` / `policy_driven` (006 §4) | MAF |
| `Skill` | A bundle of instructions, resources and optional scripts — `SKILL.md` format (009 §8) | MAF / agentskills.io |
| `SkillFrontmatter` | The parsed `SKILL.md` YAML header | MAF |
| `SkillResource` / `SkillScript` | A bundled reference file / executable script within a skill | MAF |
| `SkillsSource` | Where skills are discovered from (filesystem, plugin, remote) | MAF |
| `ContextProvider` | The seam that shapes a run's context before model invocation: adds instructions, tools, and content | MAF |
| `HistoryProvider` | A `ContextProvider` supplying conversation history | MAF |
| `SkillsProvider` | A `ContextProvider` supplying skills | MAF |
| `Middleware` | An interceptor around a run, turn, model call, or tool call (002 §5) | MAF |
| `AgentMiddleware` / `ChatMiddleware` / `FunctionMiddleware` | The three interception scopes | MAF |
| **`Plugin`** | A signed package containing WASM components implementing a WIT world (009) | **ours** |
| **`MemoryProvider`** → **use `ContextProvider`** | *Superseded.* Memory is a kind of context provider, not a parallel concept | — |

### The `ContextProvider` correction

RFC 005 §5 currently specifies `MemoryProvider` as its own seam. **That was a mistake and this RFC
supersedes it.** In MAF, `HistoryProvider(ContextProvider)` and `SkillsProvider(ContextProvider)`
are both context providers — the seam is "contribute to the context before the model is called", and
memory, history, skills, and retrieval are *kinds* of that one thing. Having a separate
`MemoryProvider` would give us two seams doing one job, and would make CodeAct's integration point
(which in MAF is a `ContextProvider`) a special case for no reason.

**Action:** 005 §5 is renamed to context providers, with memory as a kind (§7).

### Why `Plugin` is ours

Semantic Kernel used "plugin" for a group of functions — which is our **tool set**, not our plugin.
MAF has no equivalent of a signed, capability-declaring, sandboxed WASM component package, so the
word is free, and we take it. §5 records the collision so nobody re-imports the SK meaning.

## 4. Canonical names — workflow, trust, runtime

| Name | Meaning here | Source |
|---|---|---|
| `Workflow` / `WorkflowBuilder` / `WorkflowContext` / `WorkflowEvent` | The typed executor graph and its parts (014) | MAF |
| `Executor` | A node in a workflow graph: an agent, function, sub-workflow, or request port | MAF |
| `Edge` | A typed connection between executors | MAF |
| **`Capability`** | An unforgeable handle authorizing one class of effect (007) | **ours** |
| **`Principal`** | The authenticated identity a run executes on behalf of (007) | **ours** |
| **`EffectContext`** | The mandatory attribution parameter carried into every effect (007, I4) | **ours** |
| **`Sandbox`** / **`Profile`** | An isolation boundary instance / a named backend + limits configuration (008) | **ours** |
| **`Worktree`** | The session's content-addressed virtual disk (025) | **ours** |
| `Actor` · `Activation` · `Worker` · `Shard` · `Mailbox` · `ActorRef<A>` · `Policy` | Quark's runtime vocabulary, used **verbatim and unchanged** | Quark |
| `ae::task<T>` · `ae::result<T>` | Coroutine return type · `std::expected<T, error>` | Quark |

The trust and isolation names are ours because **MAF has no capability model, no sandbox seam, and
no worktree**. That is the substantive difference between the two systems, and it deserves its own
vocabulary rather than being smuggled into MAF words.

## 5. The collision register

Words that mean different things depending on which specification you are reading. Every one of
these has caused a real bug or a real misunderstanding somewhere in the ecosystem.

| Word | In AgentEngine core | Elsewhere | Rule |
|---|---|---|---|
| **`Resource`** | **Quark's**: a dependency with a lifetime scope, resolved at activation (Quark 004) | **MCP**: server-exposed content addressed by URI. **MAF**: `SkillResource`, a bundled file | A bare `Resource` is always Quark's. MCP's is `mcp::Resource`, never imported unqualified. Skill files are `SkillResource`. |
| **`Task`** | **Nothing.** There is no `Task` type in core | `ae::task<T>` is the coroutine type. **A2A**: `Task`, a unit of delegated work = our `Run`. **MCP**: the `tasks` extension | Lowercase `task<T>` is the coroutine and only that. `a2a::Task` and `mcp::Task` stay namespaced. **Never declare a bare `Task` type.** |
| **`Skill`** | A `SKILL.md` bundle (§3) | **A2A**: `AgentSkill`, a discovery record in an Agent Card — no instructions body, no files, no progressive disclosure. Functionally closer to a tool listing | `a2a::AgentSkill` is never abbreviated to `Skill`, in code or prose. |
| **`Plugin`** | A signed WASM component package (009) | **Semantic Kernel**: a group of functions ≈ our tool set | Do not re-import the SK meaning. |
| **`Session`** | `AgentSession` — durable conversation state | **MCP ≤ 2025-11-25**: a transport-level session, **removed** in `2026-07-28` | Never use "session" for a connection or transport concept. That word is now free precisely because MCP gave it up. |
| **`Executor`** | A workflow graph node (014) | **Quark**: informally, the worker currently holding an activation | In AgentEngine prose "executor" always means the graph node; Quark's sense is written as "the worker holding the activation". |
| **`Context`** | Four distinct types | `SessionContext` (per-run session view), `WorkflowContext` (per-executor), `MessageContext` (Quark: ambient per-message stop token, deadline, trace id), `EffectContext` (attribution for an effect) | Never write bare `Context`. Each is spelled in full at every use. |
| **`Provider`** | `ContextProvider`, `HistoryProvider`, `SkillsProvider` | Colloquially, "provider" often means the *model vendor* | The inference seam is `ChatClient`, never `Provider`. **This supersedes RFC 004's use of "provider".** |
| **`Content`** | One element of a message (§2) | **A2A**: `Part`. **MCP**: content block | `Content` in core; `Part` appears only in `a2a::` mapping code. |
| **`Tool`** | Same concept everywhere | MCP `Tool`, MAF `FunctionTool`/`AITool` | No collision. Use freely. |

## 6. Namespaces

```
agentengine          (alias ae)   the public core
agentengine::detail               internals, not user-facing
agentengine::mcp  ::a2a  ::agui   protocol wire types and mappings only
agentengine::pal                  nothing — platform code lives in Quark's pal
quark                             the runtime, used verbatim
```

**The boundary rule from CONVENTIONS restated as a naming rule:** a `mcp::` or `a2a::` type never
appears in `agentengine::core`. Translation happens at the protocol boundary. This is what keeps the
collision register short — most collisions cannot reach the core because the type system stops them.

## 7. Terminology debt

Names settled here that existing RFCs have not yet adopted. Tracked, not silently ignored:

| RFC | Change |
|---|---|
| 003 | `Part` → `Content` (keep `Part` only in `a2a::` mapping prose) |
| 004 | "Provider" / "Model Provider Plane" → `ChatClient`; retitle |
| 005 | `MemoryProvider` → `ContextProvider`, memory as a kind (§3) |
| 005, 001, 019 | `Session` → `AgentSession` for the type; "session" stays fine in prose |
| 026 | `agent.memory` module name reviewed against `ContextProvider` naming |

A follow-up commit applies these; this table exists so the debt is visible rather than discovered
later by someone reading two RFCs that disagree.

## 8. Rules for introducing a new name

1. **Check MAF first.** If the concept exists there, use its name.
2. **Check the collision register.** If the word is already taken by a spec we speak, either qualify
   it or pick another word — and add the row.
3. **A new public name requires a row in §2–4** in the same change that introduces it.
4. **Do not abbreviate across a boundary.** `AgentSkill` does not become `Skill`; `ChatClient` does
   not become `Client`.
5. **No .NET or managed-runtime vocabulary** (CONVENTIONS) — MAF supplies the shape, not the
   spelling. `IAgent`, `AgentBase`, `Manager`, `Helper`, `Service` are all wrong here.

## 9. Promotion gate

- **G1** — a lint over public headers verifies every exported type appears in this RFC's tables; an
  unlisted public name fails CI.
- **G2** — a grep gate proves no `mcp::`, `a2a::`, or `agui::` type name appears in
  `agentengine::core` (the §6 boundary rule, mechanically checked).
- **G3** — the §7 debt table is empty, or every remaining row has an issue reference.
- **G4** — no bare `Task`, `Context`, or `Resource` type is declared in the public surface.

## 10. Open questions

- **Q1** — `ChatClient` is MAF's name and it is slightly wrong for a seam that also covers embeddings
  and non-chat completion; MAF handles that with a separate `BaseEmbeddingClient` and
  `Supports*` capability protocols. Adopting that split wholesale is probably right and is not yet
  decided.
- **Q2** — MAF's capability-detection pattern (`SupportsCodeInterpreterTool`,
  `SupportsWebSearchTool`, `SupportsMCPTool`…) is very close to RFC 004's capability bitset. Whether
  to mirror the protocol-per-capability shape in C++ concepts, or keep the bitset, is open.
- **Q3** — Whether `Run` should be renamed to `Task` to match A2A, accepting the collision with
  `ae::task<>`. Current answer is no, emphatically, but it will be asked again.
- **Q4** — Whether the declarative format (015) should use these names verbatim as its YAML keys
  (probably yes — one vocabulary, two syntaxes).
