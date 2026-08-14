# External process hooks — gap and design questions

**Status:** Pre-milestone scoping note, not a stage-4 work breakdown — same rationale as
`agent-as-workflow-executor-gap.md` and `batch-inference-coalescing-gap.md`: no milestone currently
owns this. Written from a real user observation (2026-08-14): AgentEngine has no equivalent of Claude
Code's Hooks feature, and MAF (the framework this project's developer model is shaped on) has no such
concept either — confirmed, not assumed (see Research below). **Document only — no code written this
pass**, matching the standing project-owner posture already applied to the two sibling gap docs above;
whether to implement is a decision for whoever picks this up, informed by the design draft's punch
list.

**RFCs:** `002-Agent-Model-and-Authoring.md` §5 (`Middleware<Ms...>`, the closest existing concept);
`006-Tool-and-Function-Plane.md` §3 (the ten-step tool pipeline); `008-Sandbox-and-Isolation.md` §3
(sandbox profiles); `009-Plugin-and-Extension-System.md` §5 (the plugin capability surface, which
explicitly excludes subprocess); `013-UI-and-Streaming-Surfaces.md` (the `RunEvent` stream).
**Research:** `docs/research/2026-08-14-claude-code-hooks-mechanics.md` — Claude Code's and the OpenAI
Agents SDK's real "Hooks" mechanics, fetched and cited directly (not from memory, per `CLAUDE.md`'s
research discipline).

## What's real today

- **`Middleware<Ms...>`** (002 §5) declares four interception points — run, turn, model call, tool
  call. Only the **model-call** point is wired to a real consumer:
  `middleware.hpp`'s `ModelCallContext`/`run_before`/`run_after`/
  `enforce_backend_tool_call_provenance`, consumed by `MiddlewareModelCallGateway<Inner, Ms...>`
  (`core/model_call_gateway.hpp`, ADR-036). The **run/turn/tool-call** points are declared vocabulary
  only — `middleware.hpp`'s own file-top comment: *"The run/turn/tool-call interception points are NOT
  wired into `AgentSession`'s turn loop."*
- **`ApprovalDecider`** (`core/tool_pipeline.hpp:273`) is a real, already-wired, synchronous decision
  seam at exactly the tool-call point: `std::function<bool(tool_name, canonical_args_json)>`, consulted
  at `invoke_tool()`'s step 5 (`tool_pipeline.hpp:411-420`) and called from `AgentSession::run_rounds()`
  (`rt/agent_session.hpp:999`). This is, structurally, already a "PreToolUse"-shaped gate — allow/deny,
  synchronous, no capability access of its own — just narrower than a full hook (boolean only, no
  content-rewrite, no other event types).
- **`RunEvent`/`emit_run_event`** (`core/run_event.hpp`, `rt/agent_session.hpp:817-829`) is a real,
  typed, already-firing **observation-only** stream: `run_event_kind` already enumerates 20 values
  covering almost the same lifecycle Claude Code names — `run_started`/`run_finished`/`run_failed`/
  `run_canceled`, `turn_started`/`turn_finished`, `model_call_started`/`model_call_finished`,
  `tool_call_started`/`tool_call_finished`, `approval_requested`/`approval_resolved`,
  `input_required`/`input_resolved`, `warning`. `emit_run_event` pushes into
  `run_event_producer_` (a stream, `rt/agent_session.hpp:822,828`) and returns immediately — nothing
  reads a return value, nothing can block on it. Real events already fire from inside `run_rounds()`
  at every one of these points (`rt/agent_session.hpp:901-1007` — cited directly, not inferred).
- **009 §5** states plainly, for the WASM Component Model plugin surface (tools, skills, providers,
  memory stores, filters, codecs — every existing extension point): *"Not available in any world: raw
  sockets, subprocess, nested sandbox creation, arbitrary filesystem, host environment, or any handle
  that outlives the invocation."* No existing extension mechanism in this codebase execs a host
  process.
- **008 §3**'s `native-jail` `SandboxProfile` is the one profile that *does* real OS-process execution
  — kernel-enforced (AppContainer+Job Object / namespaces+seccomp-BPF+cgroups), with capability
  enforcement per §4's table (`FsRead`/`FsWrite`/`NetOut` all mediated). This is the sandboxed path for
  running an external process under this project's own model, distinct from and much narrower than
  "exec an arbitrary host command with the invoking user's full permissions."
- **`ShellRunner`** (010 §1a/§2, ADR-001) is this project's own precedent for "dispatch to a named
  external capability without ever resolving or exec'ing an arbitrary host binary" — its own file-top
  comment: *"`ShellRunner` is engine-native code, not a wrapped shell, and it never resolves or exec's
  a real shell."*
- **ADR-039** (host-pluggable inbound transport, 2026-08-13) is the most recent, directly analogous
  precedent for the shape this gap needs: rather than AgentEngine building its own hardened
  infrastructure for something a host environment already has (there: a TLS/HTTP listener; here: a
  process-exec/dispatch mechanism), the engine defines a typed seam and the **host** supplies the real
  mechanism, already trusted at whatever tier the host chooses to run it at.

## The real finding: "Hooks" names two unrelated architectures, and only one of them is a gap

Confirmed by fetching both frameworks' real docs (`docs/research/2026-08-14-claude-code-hooks-
mechanics.md` §§2-4), not assumed from the shared name:

- **OpenAI Agents SDK's `RunHooks`/`AgentHooks`** are in-process, observation-only async method
  overrides — architecturally identical to 002 §5's `Middleware` idiom, and *narrower* than
  AgentEngine's own design on the one axis that matters (AgentEngine's model-call point can already
  deny/short-circuit; OpenAI's hooks cannot deny anything). **AgentEngine is not behind here** — it has
  the right shape already declared, three-quarters unwired.
- **Claude Code's Hooks** are out-of-process: declaratively configured, event-matched external
  commands/HTTP calls/MCP tool calls, with a real allow/deny/rewrite contract
  (`permissionDecision`/`updatedInput`/exit-code-2). This is the genuine gap — nothing in AgentEngine
  lets an *operator*, not a C++ author, attach an external, language-agnostic action to a lifecycle
  event without recompiling the engine.
- **The part of Claude Code's design that cannot transfer as-is**: its own docs state hook commands
  "run with the full permissions of the user running Claude Code" and are explicitly "not sandboxed or
  isolated." That is precisely the ambient-authority shape **I2** and 009 §5 were written to rule out.
  Copying the execution model, not just the event taxonomy, would be a structural regression against
  this project's own core invariant — the single most important finding this gap doc surfaces.

## Design questions a future ADR needs to answer (not answered here)

1. **Two different mechanisms for two different needs, or one unified one?** Observation-only hooks
   (SessionStart/SessionEnd/PostToolUse-as-notify-analogs) have a real, already-wired seam
   (`RunEvent`) that needs zero core change to use externally — a host just subscribes to the stream
   and does its own dispatch. Gating hooks (PreToolUse-block/UserPromptSubmit-modify/Stop-block
   analogs) need a synchronous decision seam, which today exists ONLY at the tool-call point
   (`ApprovalDecider`) and the model-call point (`Middleware`). Does closing this gap mean (a) finishing
   002 §5's declared-but-unwired run/turn points using the SAME chain machinery `middleware.hpp`
   already proves for model-call, (b) generalizing `ApprovalDecider`'s shape to the other three points
   instead, or (c) something else? The existing model-call implementation is the nearer, better-proven
   template — reusing it argues for (a).
2. **Where does "run an external process" actually live, if anywhere in first-party code?** Per
   ADR-039's precedent and 009 §5's exclusion, the answer this gap doc leans toward is: it doesn't —
   the engine hands a host-injected callback (matching `ApprovalDecider`/`MiddlewareTraceHook`/
   `WorkflowSupervisor::CheckpointHook`'s existing idiom) a typed, capability-free context object; the
   HOST's own callback implementation decides whether and how to reach an external process (spawn one
   directly under whatever authority the host trusts, dispatch through `native-jail`'s
   `SandboxBackend::exec` for a bounded/capability-scoped version, or make an HTTP call instead — an
   operator/deployer trust decision, the same category 008 §2a already assigns to a custom sandbox
   backend). Is a REFERENCE host-side dispatcher (matching ADR-039 §3e's "reference transport adapter,
   explicitly scoped as example, not hardened core") worth shipping, and if so under `examples/` only?
3. **What context does a hook get, and how does the I2/ADR-033 discipline apply to it?**
   `ModelCallContext` deliberately carries neither `EffectContext&` nor any capability type (002 §5:
   "a hook structurally cannot widen (or even read) a capability, because it is never handed one").
   Does a generalized `RunContext`/`TurnContext`/(possibly a new `ExternalHookContext` for the
   serialized-to-host-callback case) follow the identical discipline? And does ADR-033's fatal-finding
   fix (`enforce_backend_tool_call_provenance` — any `ToolCall` a hook returns/rewrites that isn't
   byte-identical to what the real backend produced gets downgraded to `text_derived` provenance)
   generalize to EVERY hook body, not just in-process C++ ones? Claude Code's own `PreToolUse`
   `updatedInput` field is precisely the content-rewrite shape ADR-033 already found fatal once — an
   external-process hook is, if anything, MORE likely to need this guard, not less, since a JSON
   round-trip through an arbitrary host process is a strictly less trustworthy path than an in-process
   C++ type the agent author wrote and compiled in.
4. **Event taxonomy: reuse `run_event_kind` as-is, extend it, or define a separate hook-event
   vocabulary?** `run_event_kind` already has 20 values covering most of Claude Code's per-run/turn/
   tool-call events. It has no analog for Claude Code's async standalone events (`FileChanged`,
   `ConfigChange`, `WorktreeCreate`) — none of which have an AgentEngine concept to hang off yet, since
   AgentEngine has no file-watching or config-reload subsystem today. Scoping to "the subset that maps
   onto real AgentEngine lifecycle points" (run/turn/model-call/tool-call/approval/input) versus trying
   to cover the whole Claude Code taxonomy up front is a real scope decision.
5. **Vocabulary mismatch to resolve explicitly, not silently.** AgentEngine's **Run** (001 §1: one
   `start_run()` call) is closer to Claude Code's session-scoped "one user turn, `UserPromptSubmit` to
   `Stop`" than to Claude Code's own word "run." AgentEngine's **Turn** (001 §1: "a segment of a run's
   coroutine between model calls") is an internal agentic-loop round Claude Code does not name as a
   distinct hook event at all (it only exposes per-tool-call and per-user-turn events, nothing at the
   round granularity). A future design must state this mapping explicitly rather than assume the words
   "run"/"turn" mean the same thing across the two systems — a silent mismatch here would misfire
   whichever hook a host attaches to `turn_started`, expecting Claude-Code's `UserPromptSubmit`
   cadence and getting one event per tool-calling round instead.
6. **Configuration surface — declarative like Claude Code, or C++-authored like `Middleware`, or
   both?** CLAUDE.md's own locked decisions: "v1 authoring surfaces are C++ CRTP and declarative
   YAML/JSON." A hook that dispatches to an external process is exactly the kind of thing an operator
   (not a C++ agent author) wants to attach without recompiling — the same audience 020
   (Configuration-and-Hosting) and 015 (Declarative-Agent-Format) already serve for other concerns.
   Does a hook configuration live in the same declarative surface as an agent's own YAML/JSON, or does
   it belong at the host/deployment layer (closer to how ADR-039 scoped transport as host-supplied,
   not agent-declared)?

## What NOT to design around (named, not solved)

Reproducing Claude Code's exact wire contract (`settings.json`'s `hooks` object, `matcher` regex
syntax, `permissionDecision`/`updatedInput`/exit-code-2 semantics) is explicitly out of scope for a
first design — AgentEngine is not a drop-in Claude Code hook host, and copying the CONTRACT SHAPE
(typed event context, allow/deny/rewrite outcome) does not require copying the SPECIFIC JSON schema or
config-file format. A first real design should scope to the mechanism (typed, host-injected,
capability-free decision seam; observation via the existing `RunEvent` stream) rather than
byte-for-byte protocol compatibility with any one product's hook config file.
