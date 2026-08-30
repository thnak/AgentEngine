# 027 — Vocabulary and Naming

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** all · **Gate:** §8 · **Normative for:** every public identifier

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
| `AgentSession` | The durable conversation state an agent operates on; a plain templated class instance, no actor lifecycle (005) (historical: "one Quark actor instance" before ADR-037 removed Quark as a dependency) | MAF |
| `SessionContext` | The per-run view of a session handed to providers and middleware | MAF |
| `SessionStore` | The persistence seam for sessions | MAF |
| `ChatClient` | The inference seam — one provider/endpoint (004) | MAF (`BaseChatClient`) |
| `Message` | One turn: role + ordered contents (003) | MAF |
| `Content` | One element of a message: text, image, reasoning, tool call, tool result… (003) | MAF |
| `ChatResponse` / `ChatResponseUpdate` | A model call's result / streamed increment | MAF |
| `AgentResponse` / `AgentResponseUpdate` | A run's result / streamed increment | MAF |
| `ResponseStream` | The streamed-response handle | MAF |
| **`Usage`** | Token and cost accounting (003 §6) | **ours** (deliberately shorter than MAF's `UsageDetails` — 003 §6 and 004 have both normatively spelled it `Usage` since those RFCs were first written; this row itself was the drifted one, corrected 2026-08-14, gap-audit finding 9, `decisions/ADR-050-*.md` — not a rename of already-shipped code) |
| **`Run`** | One invocation of an agent against a session — the unit of tracing, checkpointing, replay, and the thing an A2A `Task` maps to (001) | **ours** |
| **`Turn`** | One model call plus the tool invocations it triggers, inside a run | **ours** |
| **`ContentReplayGateway<Inner>`** | Wraps a `ModelCallGatewayLike` and discards a settled response that a pluggable trigger flags (a secret, a policy hit), re-invoking the call with a corrective instruction instead of letting the tainted content commit to durable history (`decisions/ADR-069-content-triggered-model-response-replay.md` §3) | **ours** |
| **`ContentReplayDecision`** | The verdict a `ContentReplayTrigger` returns over a settled `ChatResponse` — discard-and-retry, with a corrective instruction, or stand (ADR-069 §3) | **ours** |
| **`ContentReplayTrigger`** | The host-supplied callable producing a `ContentReplayDecision` (ADR-069 §3) | **ours** |
| **`ContentReplayAttemptEvent`** | Fires once per attempt inside a `ContentReplayGateway::call()` retry loop, discarded or not, carrying that attempt's real `Usage` (ADR-069) | **ours** |
| **`ContentReplayTraceHook`** | The optional hook receiving each `ContentReplayAttemptEvent`, so a host can account every attempt's true cost (ADR-069) | **ours** |
| **`ToolCallArgumentChunk`** | A raw, possibly-incomplete fragment of one tool call's arguments, streamed ahead of (not instead of) a backend's own internal accumulation buffer (unified-streaming-design-draft.md §1 Piece B) | **ours** |
| **`Bundle<ChatClientT, Store, HistoryProviderT>`** | `agentengine::quickstart`'s move-only owner of every long-lived object a constructed `AgentSession` references — the object `QuickstartSessionBuilder::build()` returns | **ours** |
| **`QuickstartSessionBuilder<P, Store>`** | `agentengine::quickstart`'s fluent, single-backend session builder — `P` (a `quickstart::Provider`) picks the compile-time backend, matching `AgentSession`'s own backend-templated shape | **ours** |
| **`OpenAiSessionBuilder`** | `QuickstartSessionBuilder<Provider::openai>` — the ready-to-use OpenAI alias most quickstart call sites reach for directly | **ours** |
| **`AnthropicSessionBuilder`** | `QuickstartSessionBuilder<Provider::anthropic>` — the ready-to-use Anthropic alias most quickstart call sites reach for directly | **ours** |
| **`ComposedQuickstartSessionBuilder<P, Store, Ms...>`** | `QuickstartSessionBuilder`'s multi-context-provider sibling — `Ms...` composes via `ComposedContextProvider<Ms...>` instead of the single-slot `HistoryProvider` default | **ours** |

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
| **`HasContextProviderName`** | The compile-time concept a `ContextProvider` type must satisfy — declares `static constexpr std::string_view name`, so provenance stamping (005 §3) has a real name to stamp with, the same requirement `Middleware`'s own `HasMiddlewareName` already carries (`decisions/ADR-066-context-provider-attribution-provenance.md` §3) | **ours** |
| **`ToolSurfaceView`** | The turn-middleware point's structural seam for mutating a round's tool surface — `redact()`/`reorder()`/`annotate_description()` only, with no path to a mutable `ToolDescriptor&` that could touch `invoke`/`capability_ceiling`/`approval_mode` (`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md` §3) | **ours** |
| **`TurnContext`** | The `turn`/`pre_model` interception point's context — the assembled context plus the one shared `ToolSurfaceView` every turn middleware in the chain sees and mutates (ADR-067) | **ours** |
| **`TurnMiddlewareHook`** | The `std::function`-erased seam `rt::AgentSession` calls once per round, before building that round's `ChatRequest`, to run a host-supplied turn-middleware chain (ADR-067) | **ours** |
| **`Compactor<N>`** | A `turn`-level middleware that keeps the last `N` messages of the assembled (not durable `history[]`) view for the model call about to happen, closing 005 §8 Q3 (ADR-067 §4) | **ours** |
| **`Plugin`** | A signed package containing WASM components implementing a WIT world (009) | **ours** |
| **`MemoryProvider`** → **use `ContextProvider`** | *Superseded.* Memory is a kind of context provider, not a parallel concept | — |
| **`ToolOptimizerProvider`** | A `ContextProvider` that gates MCP/WASM-plugin/agent tool exposure behind mount/unmount, the same shape `mount_skill` already uses for skill tools (009 §8b, ADR-065) | **ours** |
| **`SearchToolsTool`** | `ToolOptimizerProvider`'s always-on, zero-capability `search_tools` management tool — read-only keyword search over the full tool universe (ADR-065) | **ours** |
| **`MountToolTool`** | `ToolOptimizerProvider`'s always-on, zero-capability `mount_tool` management tool — moves the visibility window to include an already-authorized tool, granting no new capability (ADR-065) | **ours** |
| **`UnmountToolTool`** | `ToolOptimizerProvider`'s always-on, zero-capability `unmount_tool` management tool — the inverse of `MountToolTool` (ADR-065) | **ours** |
| **`ToolSourceFetch`** | The closure seam `ToolOptimizerProvider` uses to fetch a tool source's current `ToolDescriptor`s fresh on every `on_context()` call (ADR-065) | **ours** |

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
| `edge_kind` / `executor_kind` | 014 §1's two closed enumerations — `direct`/`fan_out`/`fan_in`/`switch_case`/`multi_selection`/`chain`, and `agent`/`function`/`sub_workflow`/`request_port`. Added 2026-08-07 with Milestone 6 Phase A, the milestone that owns 014 | **ours** |
| `TerminationBound` | 014 §2's required bound — `MaxRounds`, deadline, or budget. Named as a type because §2 makes it mandatory ("an unbounded workflow does not run"), so it is a field a `Workflow` cannot omit, not a policy some workflows carry. Added 2026-08-07 with Milestone 6 Phase A | **ours** |
| `edge_failure_policy` / `EdgeFailurePolicy` | 014 §6's four alternatives for an executor failure — `fail`/`propagate`/`retry`/`fallback` — and the struct declaring one on an `Edge` (with the retry budget and the fallback target). §6 says "the edge's declared policy", so it is spelled on the edge; because the thing it decides is a property of the *source*, the validator requires every edge out of one executor to agree. Added 2026-08-07 with Milestone 6 Phase D | **ours** |
| `RunWorkflow` / `ResumeWorkflow` / `WorkflowResult` | The workflow-supervising actor's protocol. `ResumeWorkflow` exists because 014 §4's suspended run **returns** rather than parking inside a handler — a run parked mid-handler could not passivate, and §8 G5 measures that it does. Added 2026-08-07 with Milestone 6 Phases B and E | **ours** |
| `ExecutorOutput` | One executor's completed output in a `WorkflowResult` — 014 §6's "partial results are preserved… not discarded". Added 2026-08-07 with Milestone 6 Phase D | **ours** |
| `MessageTypeId` | The surface-independent identity of an executor port's message type — the string the C++ form derives from a declared trait and the declarative form (015) reads verbatim, so **one** validator serves both (I6, 014 §1). Added 2026-08-07 with Milestone 6 Phase A | **ours** |
| **`Capability`** | An unforgeable handle authorizing one class of effect (007) | **ours** |
| **`Principal`** | The authenticated identity a run executes on behalf of (007) | **ours** |
| **`EffectContext`** | The mandatory attribution parameter carried into every effect (007, I4) | **ours** |
| **`EffectClass<C>`** / `effect_class` | A tool's declared repeat-safety for exactly-once effects — `pure` / `idempotent` / `at_most_once` (006 §1, 019 §3, §6) | **ours** |
| **`Handoff`** | `Handoff<Writer>` exposes another agent as a tool that transfers control of the run to it (002 §4) | MAF (`HandoffBuilder`, verified: `agent_framework_orchestrations/_handoff.py`) |
| **`Sandbox`** / **`Profile`** | An isolation boundary instance / a named backend + limits configuration (008) | **ours** |
| **`Worktree`** | The session's content-addressed virtual disk (025) | **ours** |
| **`Runner`** / **`PythonRunner`** / **`ShellRunner`** | A code/shell execution unit inside a session's sandbox (010 §1a) | **ours** |
| **`EmbeddedHost`** | The in-process bring-up object a C++ application constructs to link the engine as a library and mint `Run`/`ReplyStream` handles (020 §3a) | **ours** |
| **`Project`** | A durable index above a session — a root session plus every session it transitively owns, with directed pause/restore distinct from idle passivation (030) | **ours** |
| **`ExecState`** | The `{cwd, env}` shared by reference across every `Runner` call in a session (010 §3a) | **ours** |
| **`SecretDetector`** | A host-injected seam scanning content for secrets (a pasted API key, a leaked credential) — AgentEngine ships no regex/NER of its own (`decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md` §2) | **ours** |
| **`DetectedSpan`** | One match a `SecretDetector` found in some content — purely descriptive, never itself a decision (ADR-068 §2) | **ours** |
| **`QuarantineSecretStore`** | A mint-at-runtime, content-addressed `SecretStore` for a secret that shows up incidentally inside ordinary content, as opposed to `trust/secret.hpp`'s declare-then-resolve path for operator-declared config secrets (ADR-068 §2b) | **ours** |
| **`quarantine_trigger`** | Bookkeeping-only provenance of how a quarantine happened — `verified_user_content` (engine-verified, host-grant-eligible) vs. `agent_initiated` (model-supplied, never grant-eligible, I3) (ADR-068) | **ours** |
| **`QuarantineAuditEvent`** | Fires once per `quarantine()` call with no path to carry the secret's own bytes (ADR-068) | **ours** |
| **`QuarantineAuditHook`** | The optional host-wired hook receiving each `QuarantineAuditEvent` (ADR-068) | **ours** |
| **`QuarantineSecretArgs`** | The agent-initiated quarantine path's tool args shape — the exact text the model wants hidden (ADR-068 §2e) | **ours** |
| **`QuarantineSecretReply`** | The agent-initiated quarantine path's tool reply shape — the text with the secret replaced by an opaque reference (ADR-068 §2e) | **ours** |
| **`QuarantineSecretTool`** | A zero-capability tool letting the model itself hide text it's about to say, always `agent_initiated` and so never grant-eligible (ADR-068 §2e) | **ours** |
| **`QuarantineToolProvider`** | Wraps a `QuarantineSecretStore` as a real `ContextProvider` conformer, contributing the `quarantine_secret` tool (ADR-066/068) | **ours** |
| **`AgentSpawnArgs`** | The `agent.spawn` tool's model-facing args — an `agent_id` from a host-curated registry, plus plain-text `input`. Deliberately no depth/budget/ceiling field: every numeric bound this mechanism enforces comes from the caller's already-held `CapabilitySet`/`SpawnBudget`, never the call's own args (I3) | **ours** |
| **`AgentSpawnReply`** | The `agent.spawn` tool's model-facing reply — `output` plus input/output token counts | **ours** |
| **`AgentSpawnTool`** | The `agent.spawn` tool's poison-sentinel `Tool<Derived>` conformer — its static `invoke()` never actually runs; real dispatch is `AgentSpawnToolProvider`'s host-bound closure | **ours** |
| **`ChildRunner`** | The type-erased "construct a fresh child `AgentSession`, wire it, drive it to completion, tear it down" thunk a `SpawnTargetDescriptor` carries — host-authored only, at registry-build time, never derived from model output (I3) | **ours** |
| **`SpawnTargetDescriptor`** | One `agent.spawn` registry entry: the spawnable agent's `AgentMetadata`, its host-configured spawn cost and child token budget, its worktree-sharing mode, and its `ChildRunner` | **ours** |
| **`SpawnQuota`** | A host-configured, per-caller-`Principal` soft ceiling on spawn attempts, checked before the shared, non-refundable `SpawnCostBudget` pool is ever touched | **ours** |
| **`SpawnQuotaTracker`** | The thread-safe counter map enforcing a `SpawnQuota` per caller `Principal` | **ours** |
| **`SpawnTargetRegistry`** | The host-curated, closed table of spawnable `agent_id` → `SpawnTargetDescriptor` entries a model's `AgentSpawnArgs::agent_id` only ever indexes into, never widens (I3) | **ours** |
| **`SpawnPump<StoreT>`** | The single-threaded serialization point for every spawn-time mutation of shared state (`SpawnCostBudget::consume()`, child-id minting, the worktree mint's read-then-write) — one dedicated worker thread per host process is the only thread that ever touches that state | **ours** |
| **`AgentSpawnToolProvider<StoreT>`** | The `ContextProvider` conformer wiring `agent.spawn` into a session via the existing `ComposedContextProvider` composition seam | **ours** |
| **`ChildSpawnRequest`** | A child run's inputs: the input `Message`, the capabilities minted for it (trusted exactly as given, never re-derived or widened), the re-derived caller `Principal`, and its token/turn budgets | **ours** |
| **`SessionFactory<ChatClientT, StateT, HistoryProviderT>`** | `multi_agent.hpp`'s per-call factory producing a genuinely fresh, uniquely-owned child `AgentSession` — never one that memoizes or reuses a session across calls (that would reopen a concurrent double-resume race) | **ours** |
| **`Budget`** | `multi_agent.hpp`'s not-copyable/movable spawn-fan-out budget — two independent ceilings (`max_spawns` a true reservation, `max_tokens` an honest backstop) plus `max_in_flight`, checked together under one mutex | **ours** |
| **`SpawnWorktreeGrant`** | The data shape a real worktree mint hands a spawned child: a `SubWorktree`, optional `Mount`, and optional `FsRead`/`FsWrite` capabilities — never constructed from anything a model's tool-call output could influence (I3) | **ours** |
| **`IdentityAuthority`** | The durable, ancestor-tracked minting authority for `IdentityHandle`/`Grant<T>` — the identity-native sandbox/worktree design's own authority model (ADR-099, ADR-102 §7), distinct from `CapabilitySet`'s effect-authorization role. Added 2026-08-28 with ADR-102 Phase 1 | **ours** |
| **`IdentityHandle`** | A durable, `IdentityAuthority`-minted authority-subject identity — deliberately NOT named `Principal`: it answers "does this durable subject remain the same subject across a process restart, and what is its real ancestry chain," a question the real, per-request `Principal` (007) was never built to answer (ADR-102 §2's Design A explicitly rejects retrofitting `Principal` to answer it). Bridged to the real `Principal` via `IdentityAuthority::adopt(Principal const&)`, the one seam between the two. Added 2026-08-28 with ADR-102 Phase 1 | **ours** |
| **`Grant<Payload>`** | An `IdentityAuthority`-minted, identity-scoped authority object carrying a typed payload — construction friend-gated to `IdentityAuthority`, no public constructor. Added 2026-08-28 with ADR-102 Phase 1 | **ours** |
| **`AsyncQuota<Kind>`** | A coroutine-native, `IdentityHandle`-scoped quota primitive (mint/split/consume/release/refund), reused across every quota kind the identity-native design needs — generalizes `SpawnCostBudget`'s own real "check-and-decrement as one atomic step under `AsyncMutex`" pattern to be identity-scoped and kind-generic. Added 2026-08-28 with ADR-102 Phase 1 | **ours** |
| **`Ledger<Store>`** | A content-addressed, identity-scoped checkpoint/branch/merge system built on the real `WorktreeObjectStore` concept — the identity-native design's own durable history mechanism, authorized entirely via `IdentityHandle`/`IdentityAuthority`, never `CapabilitySet`. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`Checkpoint`** | One committed turn on a `Ledger` branch — a real tree digest, its parent, the authoring identity's raw id, and a turn index, self-addressed by a real SHA-256 digest over those fields. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`BranchHandle<Store>`** | Move-only proof of legitimate access to one `Ledger` branch — every mutating `Ledger` method requires possession of one, never a bare branch name, closing the guessable-name class of attack a real red-team pass found (`merge()`'s own history). Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`BranchState`** | A `Ledger`'s own internal per-branch record (creator, head digests, checkpoint history, base tree) — never exposed directly, only through `BranchHandle`/`Checkpoint`-shaped accessors. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`LedgerMergeResult`** | The real three-way-merge outcome `Ledger::merge()` computes — a merged `Tree` plus any real, unresolved conflicting paths; a non-empty `conflicts` means `merged` is not authoritative. RENAMED 2026-08-28 (ADR-102 Phase 5) from bare `MergeResult` — a real, undetected redefinition collision against the already-shipped `agentengine::MergeResult` (025 §4, `core/worktree_merge.hpp`, a completely different branch-merge mechanism) that no build had ever surfaced until Phase 5's `cli_chat.cpp` wiring first compiled both files into one translation unit. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`LedgerMergeConflict`** | One real, unresolved conflicting path inside a `LedgerMergeResult` — the path, its optional base digest, and both sides' differing digests. RENAMED alongside `LedgerMergeResult`, same real collision, same fix. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`BranchCost`** | An `AsyncQuota<Kind>` tag type gating `Ledger::branch_from()` — an empty marker struct, no data of its own. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`StorageBytes`** | An `AsyncQuota<Kind>` tag type gating `Ledger::commit()`, sized by the real committed tree's byte footprint — an empty marker struct, no data of its own. Added 2026-08-28 with ADR-102 Phase 2 | **ours** |
| **`MergeCost`** | An `AsyncQuota<Kind>` tag type gating `Ledger::merge()` — an empty marker struct, no data of its own. A dedicated tag, not a reuse of `BranchCost`/`StorageBytes`: `merge()`'s own cost is a fixed, roughly-constant per-call expense (three tree loads, one `put_tree`, up to two ACL mutations, one snapshot persist), neither proportional to caller-supplied bytes nor the same budget as "how many branches may this identity create." Closes an I8 gap named by ADR-102 Phases 2/3/4/5 and never fixed until now. Added 2026-08-29 with ADR-111 | **ours** |
| **`ExecutionSurface`** | The concept `SandboxRuntime::run()` is templated over — `reset(host_dir)`/`run(command)`/`drain_to(host_dir)`, the generic "isolated place, seed it, run one command, drain output" contract. Deliberately NOT a conforming `SandboxBackend` (008 §2a) and not wired to `NativeJailBackend`/`WasmBackend`/`KataBackend` — an explicit, disclosed scope boundary carried from ADR-099 §7's own project-owner direction, unchanged by this port. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`SurfaceRunOutcome`** | The real outcome of one command run inside an `ExecutionSurface` — a raw process exit code plus captured stdout/stderr. Deliberately NOT reused as bare `ExecOutcome`: the real, shipped `agentengine::ExecOutcome` (sandbox/sandbox.hpp, `SandboxBackend`'s own outcome vocabulary) answers a genuinely different question and has no raw exit-code field `ExecutionSurface::run()`'s real callers depend on. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`DockerCliBackend`** | A real `docker` CLI shell-out wrapper (create/exec/destroy/copy_to_container/copy_from_container) — deliberately NOT named `DockerBackend` or any name implying `SandboxBackend` conformance; it is `DockerExecutionSurface`'s own private implementation detail, not a registry-facing backend. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`DockerExecutionSurface`** | The one real `ExecutionSurface` conformer this phase builds and proves, backed by `DockerCliBackend`. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`ContainerdCliBackend`** | A real `ctr` CLI (containerd's own client) shell-out wrapper — create()/exec()/destroy() over `ctr run`/`ctr tasks exec`/`ctr task kill`+`ctr task rm`+`ctr container rm`. Deliberately NOT a `SandboxBackend` conformer, mirroring `DockerCliBackend`'s own naming discipline — it is `ContainerdExecutionSurface`'s own private implementation detail, not a registry-facing backend. Added 2026-08-29 with ADR-106 | **ours** |
| **`ContainerdExecutionSurface`** | The SECOND real `ExecutionSurface` conformer, closing the design's own "whether the three-verb shape generalizes past Docker" residual — built on `ctr run`'s convenience-flag path with a bind mount replacing `DockerExecutionSurface`'s `docker cp`-based copy-in/copy-out, backed by `ContainerdCliBackend`. Added 2026-08-29 with ADR-106 | **ours** |
| **`RealIoFileSystem`** | Real host-filesystem I/O staging bytes for `Ledger` and materializing a `Ledger` tree back onto real disk for rollback — `write()`/`read_real_file()`/`drain_into_tree()`/`scan_and_drain_into_tree()`/`materialize()`, built on the real `agentengine::open_within_mount_root` (ADR-014 Design B). Windows-only, disclosed not silently assumed — a Linux port needs `core/worktree_mount_fs_posix.hpp`'s own equivalent primitive, not attempted in this phase. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`SandboxRuntime`** | Composes `Ledger` + `RealIoFileSystem` + one `ExecutionSurface` conformer into a single, quota-gated `run()` verb producing a real `Ledger` checkpoint, plus `reset_to_turn()`/`spawn_child_branch()`/`merge_into()`/`reclaim_orphaned_child()`/`discard()`. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`SandboxRunOutcome`** | `SandboxRuntime::run()`'s own return shape — a `SurfaceRunOutcome` plus the `Checkpoint` it produced. Deliberately NOT named bare `RunOutcome`: a real name collision was found during vocabulary registration against the existing, shipped `agentengine::a2a::RunOutcome` (an A2A task's outcome, a different concept), matching this design's own "keep a distinct name where the concept genuinely differs" discipline. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`RunCost`** | An `AsyncQuota<Kind>` tag type gating `SandboxRuntime::run()` — an empty marker struct. Closes a real "run a command for free before storage quota is ever checked" bypass the prove-phase original's own red-team pass found. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`ResetCost`** | An `AsyncQuota<Kind>` tag type gating `SandboxRuntime::reset_to_turn()` — an empty marker struct. Closes a real unbudgeted resource-exhaustion vector (unbounded checkpoint growth / full re-serialization on every call) the prove-phase original's own red-team pass found on `reset_to_turn()`. Added 2026-08-28 with ADR-102 Phase 3 | **ours** |
| **`MandatorySandboxProvider<Surface>`** | The real `ContextProvider` conformer wiring `SandboxRuntime`/`ExecutionSurface` (Phase 3) into a session's bare `HistoryProviderT` slot — mirrors `tools/cli_chat.cpp`'s own real, shipped `ToolDeclaringHistoryProvider` pattern, deliberately NOT `ComposedContextProvider<Ms...>` (zero real production consumers anywhere in this codebase). Default-constructible into a real "no sandbox bound yet" state (required for `AgentSession::clear_in_process_state()`'s fixed `HistoryProviderT{}` statement to compile); `bind_sandbox()` is the real, host-only, config-time binding call. Copy-assignment performs a real `SandboxRuntime::spawn_child_branch()` call, the mechanism `AgentSession::fork_from()`'s own plain copy-assign statement relies on for real session forking. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| **`RunCommandTool`** | The one real `Tool<>` conformer `MandatorySandboxProvider` contributes — `run_command`, dispatched entirely through a `make_tool_descriptor_with_invoke()` closure (its own static `invoke()` is an unreachable sentinel). Deliberately declares NO static `Capabilities<...>` ceiling, unlike the real, shipped `RunShellTool` (ADR-096) — it authorizes against this design's own `Grant<T>`/`IdentityAuthority`/`AsyncQuota<T>` model, which has no `CapabilitySet`-shaped capability notion to declare a ceiling against. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| **`RunCommandArgs`** | `RunCommandTool`'s own JSON-schema-typed argument shape — a shell command string. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| **`RunCommandReply`** | `RunCommandTool`'s own JSON-schema-typed reply shape — exit code, stdout, committed tree digest, turn index. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| **`StartTaskBranchTool`** | One of four real `Tool<>` conformers `MandatorySandboxProvider::bind_task_branch_tools()` opts a session into — `start_task_branch`, giving `SandboxRuntime::merge_into()`'s sibling verbs (`spawn_child_branch`) their first real production caller. Ported from `docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp` (ADR-099's own prove-phase original). Deliberately declares NO static `Capabilities<...>` ceiling, mirroring `RunCommandTool`'s own precedent. Added 2026-08-30 with ADR-114 | **ours** |
| **`RunInTaskBranchTool`** | `run_in_task_branch` — runs a command inside a task branch previously started by `StartTaskBranchTool`, via the same `SandboxRuntime::run()` every other execution path already uses. Added 2026-08-30 with ADR-114 | **ours** |
| **`CommitTaskBranchTool`** | `commit_task_branch` — the first real production caller of `SandboxRuntime::merge_into()` anywhere in this codebase. On a real conflict, the rejected branch is reclaimed and re-surfaced under the SAME `handle_id` rather than lost. Added 2026-08-30 with ADR-114 | **ours** |
| **`DiscardTaskBranchTool`** | `discard_task_branch` — abandons a task branch outright and refunds the `BranchCost` unit `start_task_branch` spent, matching `RunCost`'s own refund-on-"nothing kept" precedent. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchStartArgs`** | `StartTaskBranchTool`'s own JSON-schema-typed argument shape — `label` is agent-supplied free text for logging ONLY (I3: never used to select which branch/authority is operated on). Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchStartReply`** | `StartTaskBranchTool`'s own JSON-schema-typed reply shape — the new task branch's opaque `handle_id`. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchRunArgs`** | `RunInTaskBranchTool`'s own JSON-schema-typed argument shape — a `handle_id` plus a shell command string. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchRunReply`** | `RunInTaskBranchTool`'s own JSON-schema-typed reply shape — exit code and stdout. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchCommitArgs`** | `CommitTaskBranchTool`'s own JSON-schema-typed argument shape — the `handle_id` to commit. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchCommitReply`** | `CommitTaskBranchTool`'s own JSON-schema-typed reply shape — success flag and the committed checkpoint's turn index. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchDiscardArgs`** | `DiscardTaskBranchTool`'s own JSON-schema-typed argument shape — the `handle_id` to discard. Added 2026-08-30 with ADR-114 | **ours** |
| **`TaskBranchDiscardReply`** | `DiscardTaskBranchTool`'s own JSON-schema-typed reply shape — a bare success flag. Added 2026-08-30 with ADR-114 | **ours** |
| **`block_on<T>()`** (`agentengine::rt::block_on`) | A synchronous `task<T>` driver correct under genuine cross-thread `AsyncMutex` contention (unlike this codebase's ubiquitous naive `while(!done) resume()` loop) — built specifically because `MandatorySandboxProvider` shares one `AsyncQuota` across sibling `SandboxRuntime` instances, a real contention path an independent red-team pass proved the naive loop corrupts. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| **`BlockOnState<T>`** | `block_on<T>()`'s own internal completion-signaling type — stores the delivered value and the cross-thread-safe "done" flag its driver coroutine's `final_suspend()` sets as the last touch on its own frame. Added 2026-08-28 with ADR-102 Phase 4 | **ours** |
| `Actor` · `Activation` · `Worker` · `Shard` · `Mailbox` · `ActorRef<A>` · `Policy` | **Retired vocabulary** — none of these names are used anywhere in AgentEngine's codebase today (historical: this was Quark's runtime vocabulary, used verbatim and unchanged, before `decisions/ADR-037-remove-quark-as-core-runtime.md` removed Quark as a dependency entirely; there is no actor model left to name) | Quark (historical) |
| `ae::task<T>` · `ae::result<T>` | Coroutine return type · `std::expected<T, error>` | `agentengine::rt::` (historical: `task<T>` originated as a `quark::task<T>` alias before ADR-037; `core/task.hpp` now defines it directly, zero Quark dependency) |

The trust and isolation names are ours because **MAF has no capability model, no sandbox seam, and
no worktree**. That is the substantive difference between the two systems, and it deserves its own
vocabulary rather than being smuggled into MAF words. `EffectClass` is ours for the same reason one
level down: MAF has no exactly-once effect model at all — a method call either ran or it didn't, and
nothing in MAF asks whether re-running it is safe — so there is no existing name to adopt.

## 5. The collision register

Words that mean different things depending on which specification you are reading. Every one of
these has caused a real bug or a real misunderstanding somewhere in the ecosystem.

| Word | In AgentEngine core | Elsewhere | Rule |
|---|---|---|---|
| **`Resource`** | **Nothing.** There is no `Resource` type in core (historical: this used to be Quark's — a dependency with a lifetime scope, resolved at activation, Quark 004 — before ADR-037 removed Quark as a dependency; the word is free in core prose again, though the collision-avoidance habit below is worth keeping for readers coming from Quark) | **MCP**: server-exposed content addressed by URI. **MAF**: `SkillResource`, a bundled file | MCP's is `mcp::Resource`, never imported unqualified. Skill files are `SkillResource`. |
| **`Task`** | **Nothing.** There is no `Task` type in core | `ae::task<T>` is the coroutine type. **A2A**: `Task`, a unit of delegated work = our `Run`. **MCP**: the `tasks` extension | Lowercase `task<T>` is the coroutine and only that. `a2a::Task` and `mcp::Task` stay namespaced. **Never declare a bare `Task` type.** |
| **`Skill`** | A `SKILL.md` bundle (§3) | **A2A**: `AgentSkill`, a discovery record in an Agent Card — no instructions body, no files, no progressive disclosure. Functionally closer to a tool listing | `a2a::AgentSkill` is never abbreviated to `Skill`, in code or prose. |
| **`Plugin`** | A signed WASM component package (009) | **Semantic Kernel**: a group of functions ≈ our tool set | Do not re-import the SK meaning. |
| **`Session`** | `AgentSession` — durable conversation state | **MCP ≤ 2025-11-25**: a transport-level session, **removed** in `2026-07-28` | Never use "session" for a connection or transport concept. That word is now free precisely because MCP gave it up. |
| **`Executor`** | A workflow graph node (014) | historical: **Quark** informally used it for the worker currently holding an activation — moot now, AgentEngine has no actor model post-ADR-037 | In AgentEngine prose "executor" always means the graph node. **Code/shell execution units (010) are deliberately named `Runner`, not `Executor`**, precisely to avoid a second meaning of a word this table already has to disambiguate. |
| **`Context`** | Four distinct types | `SessionContext` (per-run session view), `WorkflowContext` (per-executor), `MessageContext` (ambient per-message stop token, deadline, trace id — historical: Quark's concept before ADR-037), `EffectContext` (attribution for an effect) | Never write bare `Context`. Each is spelled in full at every use. |
| **`Provider`** | `ContextProvider`, `HistoryProvider`, `SkillsProvider` | Colloquially, "provider" often means the *model vendor* | The inference seam is `ChatClient`, never `Provider`. **This supersedes RFC 004's use of "provider".** |
| **`Content`** | One element of a message (§2) | **A2A**: `Part`. **MCP**: content block | `Content` in core; `Part` appears only in `a2a::` mapping code. |
| **`Tool`** | Same concept everywhere | MCP `Tool`, MAF `FunctionTool`/`AITool` | No collision. Use freely. |

## 6. Namespaces

```
agentengine                                       (alias ae)   the public core
agentengine::detail                                            internals, not user-facing
agentengine::trust  ::sandbox  ::workflow                      public core, split by module —
                                                                same §2-4 vocabulary rules as bare
                                                                agentengine, not a separate namespace
agentengine::schema  ::json  ::yaml  ::response_format_codec   core/ format-handling helpers —
                                                                same §2-4 vocabulary rules as bare
                                                                agentengine
agentengine::mcp  ::a2a  ::agui  ::openai  ::anthropic         protocol wire types and mappings
                                                                only — own wire-format vocabulary,
                                                                exempt from §2-4
agentengine::pal                                               a small, self-contained, vendored
                                                                socket PAL (historical: "nothing —
                                                                platform code lives in Quark's pal"
                                                                before ADR-037 removed Quark)
agentengine::rt                                                the runtime substrate: async mutex,
                                                                thread pool, session/append-log
                                                                store, circuit breaker,
                                                                channel/stream backend (ADR-037;
                                                                historical: this row used to read
                                                                "quark — the runtime, used verbatim"
                                                                — there is no `quark` namespace in
                                                                this codebase anymore)
```

**The boundary rule from CONVENTIONS restated as a naming rule:** a `mcp::` or `a2a::` type never
appears in `agentengine::core`. Translation happens at the protocol boundary. This is what keeps the
collision register short — most collisions cannot reach the core because the type system stops them.

**Corrected 2026-08-10 (ADR-025):** this diagram previously listed only `::detail`/`::mcp`/`::a2a`/
`::agui`/`::pal`, omitting `::trust`, `::sandbox`, `::workflow`, `::schema`, `::json`, `::yaml`,
`::response_format_codec`, `::openai`, and `::anthropic` even though all nine were already real,
in-tree namespaces. `tools/naming_lint.py` (G1) had been built against this incomplete picture — it
matched only the bare string `agentengine`, so every declaration inside a C++17 nested-namespace
module (`trust`, `sandbox`, `workflow`, `schema`, `json`, `yaml`, `response_format_codec`) was
silently invisible to the gate, never reported as a violation or a suppression. Fixed alongside this
diagram correction; see ADR-025 for the full finding and falsifiable before/after evidence.

## 7. Terminology debt

Names settled here that existing RFCs had not yet adopted, applied in a follow-up commit
(2026-07-31) — this table is the record of what changed and why, not a live TODO list:

| RFC | Change | Status |
|---|---|---|
| 003 | `Part` → `Content` (kept as `Part` only in `a2a::` mapping prose, 012) | Applied |
| 004 | "Provider" / "Model Provider Plane" → `ChatClient`; retitled, including the `ChatClientId<"vendor:model">` policy tag (002) | Applied |
| 005 | `MemoryProvider` → `ContextProvider`, memory as a kind (§3) | Applied |
| 005, 001 | `Session` → `AgentSession` for the type; "session" stays fine in prose | Applied |
| 026 | `agent.memory` reviewed against `ContextProvider` naming | No rename needed — it is a Python-facing module name describing a task, not a competing type; 029 §4–5 grounds what it actually reads |
| 006 | `EffectClass<C>` / `effect_class` (019 §3's `pure`/`idempotent`/`at_most_once`) — implemented (Milestone 4 Phase F4, `tool.hpp`/`tool_pipeline.hpp`) before 006 §1's declaration example or this RFC's tables named it | Applied |

**G3 is satisfied**: the table is empty of unresolved rows.

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

- ~~**Q1** — `ChatClient` is MAF's name and it is slightly wrong for a seam that also covers
  embeddings and non-chat completion; MAF handles that with a separate `BaseEmbeddingClient` and
  `Supports*` capability protocols. Adopting that split wholesale is probably right and is not yet
  decided.~~ **Resolved, No, keep one `ChatClient` seam (2026-08-04):** 004 §2's capability bitset
  already covers this (`multimodal_in`/`multimodal_out` and similar bits let a backend declare what
  it doesn't support), and nothing in this project's current scope needs embeddings as a first-class
  separate seam — no embedding-consuming feature exists anywhere in the spec yet. Splitting
  speculatively, with no concrete driver, is the premature abstraction CLAUDE.md warns against. If a
  concrete embedding-consuming feature is designed later (e.g. a retrieval provider needing
  embeddings, 029), that's the point to decide whether it needs its own seam — evidence-driven, not
  speculative.
- ~~**Q2** — MAF's capability-detection pattern (`SupportsCodeInterpreterTool`,
  `SupportsWebSearchTool`, `SupportsMCPTool`…) is very close to RFC 004's capability bitset. Whether
  to mirror the protocol-per-capability shape in C++ concepts, or keep the bitset, is open.~~
  **Resolved, keep the bitset (2026-08-04):** this is a language-idiom difference, not a concept
  difference, so §1's naming rule doesn't actually push toward MAF's shape here. A `constexpr` bitset
  checked once at registration time is more aligned with this project's own established CRTP-policy
  idiom (002's whole authoring model) than porting C#/Python's structural-typing `Supports*` pattern
  would be, and it's trivially composable (query/iterate/log several capabilities at once) in a way N
  separate protocol-conformance checks aren't as naturally. 004 §2 stays as specified.
- ~~**Q3** — Whether `Run` should be renamed to `Task` to match A2A, accepting the collision with
  `ae::task<>`. Current answer is no, emphatically, but it will be asked again.~~ **Resolved, No,
  formalizing what §2 already argues (2026-08-04):** the collision with `ae::task<>` (§5's "never
  declare a bare `Task` type") and MAF having no first-class run object at all (§2: "inventing `Run`
  is cheaper than overloading `AgentResponse` with a lifecycle it does not have") are structural
  reasons, not preferences — this was never actually open, only flagged as likely to be re-asked,
  which this entry now states explicitly rather than leaving the question looking undecided.
  Revisiting it needs an ADR (CLAUDE.md's locked-decision discipline), not a re-ask.
- ~~**Q4** — Whether the declarative format (015) should use these names verbatim as its YAML keys
  (probably yes — one vocabulary, two syntaxes).~~ **Resolved, Yes, same vocabulary, YAML-idiomatic
  spelling — confirming what 015's own examples already do (2026-08-04):** "verbatim" was never fully
  literal — `MaxTurns<N>` isn't valid YAML key syntax to begin with, so the real question was always
  "same concept, what casing," and 015's examples already answer it (`max_turns`, `capabilities`,
  `sandbox.profile` map 1:1 to `MaxTurns<N>`/`Capabilities<Cs...>`/`SandboxProfile<P>` in ordinary
  snake_case). Stated now as a rule rather than left implicit: a new policy name introduced under
  §8 rule 3 gets its YAML key as the same name in snake_case, in the same commit.
