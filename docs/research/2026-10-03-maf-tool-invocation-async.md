# Research record — how MAF standardizes tool invocation, and where AgentEngine's synchronous seams sit

**Compiled:** 2026-10-03 · **Status:** dated snapshot · **Feeds:** a future ADR making tool/script
invocation asynchronous end to end (006 §1's own `ae::task<result<Reply>> invoke` declaration)

Triggered by the project owner reading `/api/tool.html`, which described `Tool::invoke` as
"synchronous by deliberate design": *every tool call and every script a skill invokes must be
asynchronous, to support network-calling features* — and the conversion must be clean and complete,
not delivered piecemeal.

**Sources.** Microsoft Agent Framework (MAF) read from a local checkout of
<https://github.com/microsoft/agent-framework>, commit `e9857bd75` (2026-10-02). The .NET side
builds on `Microsoft.Extensions.AI` **10.10.0** (`dotnet/Directory.Packages.props:87`); that
package's API was read from its shipped XML documentation (`microsoft.extensions.ai.abstractions`
and `microsoft.extensions.ai` 10.10.0, `lib/net9.0/*.xml`). AgentEngine facts are from `main` at
`beb47b3`. Line numbers are as of those commits.

---

## 1. MAF: one tool contract, and it is asynchronous

### 1a. .NET

- **The root contract is `AIFunction.InvokeAsync(AIFunctionArguments, CancellationToken)` →
  `ValueTask<object?>`**, with subclasses overriding `protected InvokeCoreAsync(AIFunctionArguments,
  CancellationToken)` of the same shape (MEAI 10.10.0 XML doc members
  `M:Microsoft.Extensions.AI.AIFunction.InvokeAsync(...)` and `...InvokeCoreAsync(...)`). There is
  no synchronous invocation entry point.
- **Every special-purpose tool goes through that one contract**, rather than having its own:
  - MCP tools — `Microsoft.Agents.AI.Mcp/TaskAwareMcpClientAIFunction.cs:77` (`InvokeCoreAsync`),
    `Microsoft.Agents.AI.Foundry.Hosting/ConsentAwareMcpClientAIFunction.cs:28`.
  - Code interpreters — `Microsoft.Agents.AI.LocalCodeAct/LocalExecuteCodeFunction.cs:110-118`
    (forwards to an `ExecuteAsync(..., cancellationToken)`),
    `Microsoft.Agents.AI.Hyperlight/HyperlightExecuteCodeFunction.cs:33`.
  - The sandbox-to-host tool bridge (code running in the sandbox calling a host tool) —
    `Microsoft.Agents.AI.Hyperlight/Internal/ToolBridge.cs:35-42`:
    `await tool.InvokeAsync(new AIFunctionArguments(arguments))`.
- **Skill scripts are async and cancellable.** `Skills/AgentSkillScript.cs:50`:
  `public abstract Task<object?> RunAsync(AgentSkill, JsonElement?, IServiceProvider?,
  CancellationToken)`; both the file-based (`File/AgentFileSkillScript.cs:52`) and inline
  (`Programmatic/AgentInlineSkillScript.cs:79`) scripts implement it. The pluggable runner for file
  scripts is `File/AgentFileSkillScriptRunner.cs:24`, a delegate returning `Task<object?>` and taking
  a `CancellationToken`.
- **Function middleware is async**: `FunctionInvocationDelegatingAgent.cs:18` holds a
  `Func<AIAgent, FunctionInvocationContext, Func<FunctionInvocationContext, CancellationToken,
  ValueTask<object?>>, CancellationToken, ValueTask<object?>>` — a `next`-style async chain ending in
  `target.InvokeAsync(ctx.Arguments, cancellationToken)` (`:331`).
- **The loop**: MEAI's `FunctionInvokingChatClient`. Its `AllowConcurrentInvocation` property
  **defaults to `false`** (sequential, still asynchronous). Its own doc remark warns that even with
  `false`, concurrent *requests* to the same client can still run the same tool concurrently.

### 1b. Python

- **`FunctionTool.invoke(...)` is `async def`** (`python/packages/core/agent_framework/_tools.py:962-983`),
  returning `list[Content]` (or the raw value with `skip_parsing=True`).
- **Synchronous tool bodies are allowed, but never run on the event loop.**
  `_invoke_function` (`_tools.py:821-828`): a coroutine function is awaited directly; a plain
  function is run via `await asyncio.to_thread(self.__call__, **call_kwargs)`. The only exception is
  an explicit opt-in flag `_invoke_sync_on_event_loop`.
- **Concurrency is a configuration, not a tool property.** `FunctionInvocationConfiguration`
  (`_tools.py:1755-1860`) has `allow_concurrent_invocation`, **defaulting to `True`** in Python
  (the opposite default from MEAI's .NET client). When on, calls from one model response run under
  `asyncio.gather` (`_tools.py:2542-2545`).
- **Cancellation is cooperative, and MAF says so honestly** (`_tools.py:2546-2556`): if one call
  escapes loudly, in-flight siblings are cancelled and awaited — but "a synchronous tool body already
  running in a worker thread (`asyncio.to_thread`) cannot be interrupted and may complete its side
  effects"; its result is discarded and never reaches the transcript, the model, or history.
- **Skill scripts are async**: `_skills.py:366` `class SkillScript(ABC)`, `:402`
  `async def run(...)`. The file-script runner protocol `SkillScriptRunner` (`_skills.py:1942`)
  accepts "any callable (sync or async)". Code-defined scripts run in-process; file scripts go
  through the runner (e.g. a subprocess runner).
- **Function middleware is async**: `async def process(self, context: FunctionInvocationContext,
  call_next)` (`_middleware.py:129-137`).

### 1c. What MAF standardizes, in four lines

1. **One invocation contract per language, asynchronous**, shared by plain functions, MCP tools,
   code interpreters, sandbox→host bridges, skill scripts and middleware.
2. **Cancellation is part of the per-call contract** (`CancellationToken` / task cancellation),
   not only a deadline checked around the call.
3. **Synchronous bodies are an authoring convenience**, isolated from the async path (Python
   offloads them to a thread; nothing sync is a second contract).
4. **Concurrency is a loop setting** layered over the async contract, not something the contract's
   shape decides.

---

## 2. AgentEngine today: every seam that is synchronous where 006 and MAF are async

Note that `rt::task<T>` and `rt::block_on` exist and are real (ADR-037, ADR-175, ADR-219). The
problem is not a missing coroutine type; it is that the seams below were fixed at `result<T>`
under **milestone 2's "decision 2"** ("`ae::task<T>` deferred — no gate item this milestone needs
real coroutine concurrency to prove"), and that the one network primitive underneath them blocks.

### 2a. The spec already says async

`006-Tool-and-Function-Plane.md` §1 declares `static ae::task<result<Reply>> invoke(Args,
EffectContext&);`. The code does not. Under this repo's rule (spec wins), the code is in debt to the
spec; nothing has to be relitigated to make the switch, though the switch itself needs an ADR
(hot path, security-relevant, conformance-gated surfaces).

### 2b. Inventory

| Seam | Where | Current shape | Comment in code |
|---|---|---|---|
| Tool authoring contract | `core/tool.hpp:200-202` | `static result<Reply> invoke(Args, EffectContext&)` | "`ae::task<T>` stays deferred for M2 (decision 2)" |
| Type-erased tool | `ToolDescriptor::invoke` (`core/tool_pipeline.hpp`) | `std::function<result<json::Value>(json::Value const&, EffectContext&)>` | `tool_pipeline.hpp:12`, `:30` — deadline "checked at the call boundary, not preemptible mid-call" |
| MCP client tool bridge | `protocol/mcp/mcp_tool_bridge.hpp:38`, `protocol/mcp/client.hpp:344-552` | `list_tools`/`call_tool`/`get_task`/`cancel_task` all `result<T>` | — |
| Sandbox command runner (shell, skill scripts) | `sandbox/runner.hpp:64-68` | `result<ExecOutcome>` | "becomes `ae::task<result<ExecOutcome>>` once it is" |
| Sandbox backend | `sandbox/sandbox.hpp:366-370` | `result<T>` / `void` | "each becomes `ae::task<result<T>>` once it is" |
| Worktree object store | `core/worktree_types.hpp:93-96` | `result<T>` | "`ae::task<T>` is not yet wired in project-wide (M2's own decision 2, unchanged)" |
| Workflow executor body | `rt/workflow_supervisor.hpp:245-246` | `std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>` | `rt/workflow_as_executor.hpp:20-32` — a sync body has to `drive()` a coroutine and needs its own `std::mutex` to avoid a double-resume |
| Secret store | `trust/secret.hpp:281-284` | `result<SecretLease>` | justified: "no I/O to suspend on" — **wrong for any vault/KMS-backed store** |
| **The HTTPS primitive itself** | `sandbox/provider_http_client.hpp:94`, `src/sandbox/provider_http_client.cpp:7` | `result<NetEgressResponse> perform_provider_https_exchange(...)` — blocking | referenced in 10 files under `include/`+`src/` |
| Model call | `core/chat_client.hpp:224-238`, `protocol/{openai,anthropic}/chat_client.hpp` | `chat()` *is* `ae::task<...>`, but its body calls the blocking HTTPS exchange inline ("freely callable from inside chat()'s coroutine body"); `chat_stream()` uses a **detached background thread** | async in name only |
| Embedder / vector index | `protocol/openai/embedder.hpp:270`, `protocol/qdrant/vector_index.hpp:365/394/417` | `task<>` wrappers over blocking HTTPS | same |
| Sync→async patch | `rt/drive_leaf_task.hpp`, `Embedder::synchronous_leaf` (`core/embedder.hpp`), used by `vector_rag_context_provider.hpp`, `hybrid_rag_context_provider.hpp`, `corpus_source.hpp`, `remote_vector_index.hpp`, `session_builder.hpp`, `rt/agent_workflow_executor.hpp` | drives a `task` to completion from a sync `invoke`; **fails closed** (`rt.leaf_task_contract_violation`) the moment the task genuinely suspends | ADR-064 Design B — a workaround for exactly this gap |
| Parallel tool batches | ADR-160 scheduler | concurrency via OS threads (`rt::ThreadPool`), one thread held per in-flight call | — |

### 2c. What this costs in practice

- **Every network-bound call holds an OS thread for its whole duration** — tools, MCP calls, model
  calls, embeddings — because the bottom primitive (`perform_provider_https_exchange`) blocks. A
  coroutine signature on top does not change that.
- **No mid-call cancellation.** A deadline or session cancel can only be observed before/after a
  tool call; MAF's per-call `CancellationToken` has no equivalent in `EffectContext`.
- **Tools cannot compose async seams.** A tool cannot `co_await` `ChatClient::chat()`, an
  `Embedder`, or an MCP call; ADR-064 had to add `drive_leaf_task`, which is only sound while the
  driven task never truly suspends — i.e. only while the underlying I/O stays blocking. The patch
  and the blocking primitive keep each other alive.
- **Skill scripts and shell commands** (`Runner::exec`) block the calling thread for the life of
  the child process.
- **Workflow nodes** (`ExecutorBody`) need ad-hoc mutexes to call coroutine code safely.

---

## 3. Differences from MAF that are deliberate, and should stay

Recorded so the async conversion does not "fix" them by copying MAF:

- **Concurrency is a per-tool declaration** here (`Parallelizable`, `ExclusivityGroup<Name>`,
  ADR-158/160), not only a loop-wide boolean. Stricter than MAF; keep.
- **Effect class, approval and capabilities are compile-time policies** on the tool, read by the
  pipeline (I2). MAF carries approval as a runtime wrapper. Keep.
- **MAF's own residual — a sync body on a worker thread cannot be cancelled and may still commit its
  side effect** (`_tools.py:2550-2552`) — would apply to any AgentEngine "offload a blocking body to
  a pool" design too. Whatever the ADR picks, this has to be stated as a residual, attributed (I4),
  and not silently inherited.

---

## 4. Open questions for the ADR (not answered here)

1. **Bottom-up order is forced.** Converting `Tool::invoke` to `ae::task` without an I/O reactor
   that can park a coroutine on a socket/process handle and resume it (ADR-064 "Design D", with
   ADR-175/ADR-219's resume-home rules) gives coroutine signatures over blocking bodies — async in
   name only, the same state `ChatClient::chat()` is in today. Which reactor (platform IOCP/epoll
   directly, or a vetted library), and how it interacts with host-driven executors (ADR-219)?
2. **Scope of "complete".** The owner asked for a clean, complete conversion, not batches. §2b is the
   candidate list: tool contract, `ToolDescriptor`, pipeline, ADR-160 scheduler, `AgentSession`,
   MCP client and server, CodeAct bridge, `Runner`, `SandboxBackend`, `WorktreeObjectStore`,
   `ExecutorBody`, `SecretStore`, the HTTPS primitive, and removal of `drive_leaf_task` /
   `synchronous_leaf`. Is any of these legitimately synchronous (e.g. pure in-memory stores), and if
   so, is that stated per seam rather than by a project-wide "deferred"?
3. **Synchronous tool bodies.** MAF keeps them as an authoring convenience behind an offload. Does
   AgentEngine keep exactly one contract (`ae::task<result<Reply>>`, every tool rewritten to
   `co_return`) — the cleaner reading of the owner's direction — and offer an explicit
   `co_await rt::offload(...)` for genuinely blocking work, rather than a second sync contract?
4. **Cancellation token in `EffectContext`**: shape, propagation into `Runner` (kill the child),
   into the HTTPS primitive (abort the exchange), and into MCP (`notifications/cancelled`).
5. **Hot-path cost**: a bench of `ae::task` frame allocation for today's cheap synchronous tools
   (CONVENTIONS: a hot path without a bench is not done).
6. **Conformance**: the MCP server and CodeAct bridge are I7-gated; the ADR must re-run their
   conformance, not wrap them in `block_on` to avoid it.

---

## 5. Addendum (same day): how many ways there are to declare a tool

Asked after a docs page described `Described<T, "...">` as "a second channel" for field
descriptions. **That part was a docs defect, not a code one**: there is exactly one mechanism
(`AE_JSON_SCHEMA` lists names/types, `Described<T, "...">` carries a field's description); the page's
older note telling authors to put parameter details in the tool's top-level `description` predated
`Described` and was rewritten (`web/marketing/src/components/ApiToolReference.tsx`).

The audit of the tool *declaration* surface did find a real duplication. There are **three** ways
to produce a `ToolDescriptor` today:

| # | Shape | Sites | Gets `Tool<>`'s policies? |
|---|---|---|---|
| 1 | `Tool<Derived, Policies...>` + `static invoke` → `make_tool_descriptor<T>()` / `ToolTable::from_tools<Ts...>` | most tools | yes |
| 2 | `Tool<>` + a **sentinel** `static invoke` that always fails (`*.invoke_unreachable`), real body supplied as a closure to `make_tool_descriptor_with_invoke<T>(fn)` (ADR-028) | 16 tools: `core/tool_optimizer_provider.hpp` (3), `sandbox/live_shell_sandbox_provider.hpp` (1), `sandbox/mandatory_sandbox_provider.hpp` (5), `trust/secret_quarantine.hpp` (1), `src/backends/native_jail/session_shell_wiring.hpp` (1), `rt/agent_spawn.hpp`, `src/rt/agent_session_core.cpp` (`ScheduleWakeupTool`), `tools/cli_chat.cpp` (2) | yes |
| 3 | `ToolDescriptor d;` filled field by field | static tools: `core/todo_provider.hpp` (5), `core/plan_execute_mode.hpp`, `core/memory_provider.hpp`, `core/vector_rag_context_provider.hpp`, `core/hybrid_rag_context_provider.hpp`, `src/backends/native_process/native_providers.hpp`, `src/backends/native_process/native_shell_session_provider.hpp`; genuinely dynamic tools: `protocol/mcp/mcp_tool_bridge.hpp`, `src/backends/wasm/wasm_tool_bridge.hpp`, `src/backends/wasm/wasm_backend.cpp`, `eval/eval_stub_tool.hpp` | **no** — every policy field defaults, and must be set by hand |

Shape 2 exists only because `invoke` is `static`: a tool that needs per-session state (a provider's
`this`, a session handle) has no instance to hang it on. MAF has no such split — an `AIFunction` /
`FunctionTool` is an object, stateful or not. Shape 3 for **static** tools exists for the same
reason plus convenience, and it is where ADR-028's own warning ("a hand-built-from-scratch descriptor
… would silently default to an empty capability ceiling and `never_require` approval") actually
bites:

- `VectorRagContextProvider` / `HybridRagContextProvider` `recall` capture `this` but leave
  `captures_session_state = false` (every other closure-bearing descriptor sets it) and declare no
  `capability_ceiling` — the exact state `MemoryProvider`'s `recall` was in before ADR-153's
  red-team made it declare `{read_cap_}`. Harmless today only because both descriptor factories are
  `private` and `parallelizable`/`deterministic` also default to `false`; not verified whether any
  CodeAct/sandbox bridge can reach a provider-contributed tool.
- The two native-process providers leave the ceiling empty **on purpose**: the ceiling is an
  AND-of-all list and cannot express "one of these N `NativeExec` grants" (comment at
  `native_providers.hpp:225-230`). That is a gap in the policy vocabulary, papered over per site.
- A shape-2 tool registered through the ordinary path (`from_tools<RunCommandTool>()`) compiles and
  fails only at run time, on the sentinel.

---

## 6. Consolidated open questions

**A. Asynchronous invocation** (§4, restated as decisions to make)

- **A1** Reactor: which one (IOCP/epoll directly vs. a vetted library), and its contract with
  ADR-175/ADR-219 resume homes. Must land before or with A2, or the conversion is async in name only.
- **A2** Scope: confirm the §2b seam list as the "complete" set; for each seam kept synchronous, a
  per-seam reason (not a project-wide "deferred").
- **A3** One contract (`ae::task<result<Reply>>` everywhere, blocking work via an explicit
  `co_await rt::offload(...)`) vs. MAF-style sync bodies auto-offloaded. Owner direction leans to one
  contract.
- **A4** Cancellation token in `EffectContext`: shape, and propagation into `Runner` (kill), the
  HTTPS primitive (abort) and MCP (`notifications/cancelled`). How the "offloaded body cannot be
  interrupted" residual is stated and attributed.
- **A5** Bench of `ae::task` frame cost for today's cheap synchronous tools.
- **A6** Re-run MCP server and CodeAct conformance; no `block_on` at those edges.
- **A7** Remove `drive_leaf_task` / `Embedder::synchronous_leaf` and the `if constexpr` branches in
  the RAG providers once tools can `co_await`.

**B. One way to declare a tool** (§5)

- **B1** Make `invoke` an instance member (a tool is an object; a stateless tool is an empty one),
  so shape 2 and its 16 sentinels disappear and stateful tools keep `Tool<>`'s compile-time policies.
  This changes 006 §1's `static` declaration — spec amendment + ADR. Do it in the **same** contract
  change as A3, so the tool signature changes once, not twice.
- **B2** After B1, restrict shape 3 (hand-built `ToolDescriptor`) to tools whose schema is only
  known at run time (MCP, WASM, eval stub); migrate the ~11 static hand-built tools to `Tool<>`.
  Should the hand-built path then require its policy fields explicitly (no silent defaults)?
- **B3** A ceiling form for "any one of these grants" (the native-process providers' case), or a
  named, audited exception — instead of an empty ceiling per site.
- **B4** Fix or justify `recall` in the two RAG providers (`captures_session_state`, missing
  ceiling); check whether any CodeAct/sandbox bridge can reach provider-contributed tools.
- **B5** If shape 2 survives in any form, make registering it through the static path a compile
  error rather than a run-time sentinel.

**C. Documentation**

- **C1** `/api/tool.html` still says `invoke` is "synchronous by deliberate design" — reword now to
  "temporarily synchronous; debt against 006 §1", or only with the ADR?
- **C2** The marketing site's `dist/` is untracked and stale after source edits; who rebuilds it, and
  when?

---

## 7. Owner direction (same day): every extension point is asynchronous

> "Everything should be asynchronous — any component can be written to call the network, sandbox and
> shell included." — project owner, 2026-10-03

This widens §2b from "the tool contract" to a **rule**: *any seam a conformer implements — a concept
or a host-supplied callable — returns `ae::task<...>`*, because the engine cannot know whether a given
implementation is local or remote (a remote sandbox, a shell over SSH, a vault-backed secret store, a
model-backed chunker or grader, a policy service behind an approval decider). Internal helpers nobody
substitutes (JSON parse, schema generation, capability binding) stay synchronous; the rule is about
seams, not about every function.

### 7a. Inventory (from `main` at `beb47b3`; `^concept` and `using X = std::function<...>` under `include/`)

**Already `task<>` in signature** — but several implementations block inside (§2b): `ChatClient`
(`chat_stream` returns `stream<>`, produced by a detached thread over blocking HTTPS), `Embedder`,
`RemoteVectorIndex`, `ContextProvider` (`on_context`/`on_turn_end`), middleware
(`before_model`/`after_model`/`on_turn`), `ToolCallHook`, `TurnMiddlewareHook`, `OnContextFn`/
`OnTurnEndFn`/`OnRunEndFn`, `CheckpointHook`, `ReflectionEvaluator`.

**Synchronous concepts — must become async under the rule:**

| Seam | Header | Why it can reach the network |
|---|---|---|
| `Runner::run` | `sandbox/runner.hpp` | remote/SSH runner; child process wait |
| `SandboxBackend::create/exec/destroy` | `sandbox/sandbox.hpp` | the `remote` profile is by definition remote |
| `ExecutionSurface::reset/run/drain_to` | `sandbox/execution_surface.hpp` | container/remote surfaces |
| `PersistentShellSurface::open/exec/drain_to/close` | `sandbox/persistent_shell.hpp` | held shell over Docker/SSH (ADR-209) |
| `NetEgressBackend::fetch` | `sandbox/net_egress_proxy.hpp` | it *is* the network |
| `SecretStore::resolve` | `trust/secret.hpp` | vault/KMS (the "no I/O" justification in the header is wrong for these) |
| `WorktreeObjectStore` (`put/get_blob`, `put/get_tree`) | `core/worktree_types.hpp` | object storage (S3-style) |
| `AppendLogStore`, `SessionStore` | `rt/append_log_store.hpp`, `rt/session_store.hpp` | database/remote durability |
| `VectorIndex`, `PersistentVectorIndex`, `SparseIndex` | `core/vector_index.hpp`, `core/sparse_index.hpp` | collapses into `RemoteVectorIndex`'s async shape; the local/remote split exists only because one side is sync |
| `SkillSource::load_skills` | `core/skill_source.hpp` | registry / git / HTTP skill sources |
| `ChunkingPolicy::chunk` | `core/corpus_source.hpp` | model-backed semantic chunking |

**Synchronous host callables — must become async under the rule:**

| Callable | Header | Note |
|---|---|---|
| `ToolDescriptor::invoke` / `InvokeFn` | `core/tool_descriptor.hpp` | §2b, B1 |
| `ApprovalDecider`, `PolicyDecider` | `core/tool_pipeline.hpp` | an external policy service or a human-approval backend; returns `bool`/`policy_decision` today |
| `ExecutorBody`, `MergeOnJoinHook` | `rt/workflow_supervisor.hpp` | workflow nodes do arbitrary work |
| `RequestSender(WithHeaders)`, `InputRequestHandler` | `protocol/mcp/client.hpp` | the MCP transport itself |
| `CardFetcher` | `protocol/a2a/client.hpp` | fetches an A2A agent card over HTTP |
| `RunStarter` | `protocol/a2a/server.hpp` | starts a run |
| `ChildRunner`, `SessionFactory` | `rt/agent_spawn.hpp`, `rt/multi_agent.hpp` | a child agent's whole run |
| `ToolSourceFetch` | `core/tool_optimizer_provider.hpp` | lists tools from MCP/plugins |
| `GraderFn`, `OutputValidator` | `eval/eval_grader.hpp`, `core/agent_session_bridge.hpp` | LLM-as-judge / remote validators |
| `SurfaceFactory` | `sandbox/live_shell_sandbox_provider.hpp` | constructs a possibly-remote surface |
| `Resolver` | `protocol/provider_chat_wire.hpp`, `protocol/qdrant/vector_index.hpp` | DNS |

**Candidates to keep synchronous, each needing a stated reason in the ADR** (the owner's rule says
"everything"; these are listed so the exception is a decision, not an omission):

- Fire-and-forget event sinks returning `void` — `RecordingSink`, `BackgroundJobSink`, `EmitFn`,
  the `*TraceHook`/`*AuditHook` family. Option: keep `void` but require a non-blocking enqueue
  (a sink that writes to the network drains its own queue), so a slow sink never stalls a run.
- Pure decisions over in-memory data — `ContentReplayTrigger`, `DescriptorFilter`, `JitterSource`,
  `LiveShellClock`, `ShellLifetime::close` (returns `bool`), `SleepFn` (test seam).
- Trivial accessors in concepts — `capabilities()`, `traits`, `origin_id()`, `is_live()`,
  `contains()` on local indexes, `last_seq()`, `exists()` (but note `exists()`/`last_seq()` on a remote
  store *are* I/O).

### 7b. What this changes in §6

- **A2** is now settled in direction: the scope is "every seam in §7a", and the open question is only
  the explicit list of exceptions above.
- **A1** gets harder and more central: the reactor must cover sockets **and** child processes /
  pipes (Runner, shells, CPython worker) **and** timers, on Windows and Linux.
- **New A8 — decided (owner, 2026-10-03): migrate all of them; the codebase is pre-production, so
  existing shapes may be redone rather than preserved.** The "async in name only" seams already shipped: `ChatClient`, `Embedder`,
  `RemoteVectorIndex` (OpenAI/Anthropic/Qdrant) return `task<>`/`stream<>` but block on
  `perform_provider_https_exchange`; `chat_stream()` spawns a detached `std::thread` per call. These
  convert with the HTTPS primitive, not separately.
- **New A9 — order of work.** The rule is applied in one design (owner: clean and complete, not
  batched), but implementation still has a dependency order: reactor + async HTTPS/process primitives
  → seam signatures (§7a) → callers (pipeline, `AgentSession`, workflow, MCP/A2A) → removal of
  `drive_leaf_task`/`synchronous_leaf`/`block_on`-at-the-edge workarounds. The question is whether
  this lands as one ADR with one merge, or one ADR with a sequence of merges that never leave a seam
  half-converted.
