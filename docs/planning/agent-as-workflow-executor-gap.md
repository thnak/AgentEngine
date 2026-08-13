# `executor_kind::agent` has no runtime bridge — residual status

**Status:** Pre-milestone scoping note, not a stage-4 work breakdown — same rationale as
`backgroundable-standingeffect-gap.md`: no milestone currently owns this (M6, which built the graph
model this gap lives in, is complete and explicitly fenced the gap off rather than closing it — see
below; M7 owns protocol conformance, not this). This note exists so the gap is tracked in one place,
with a real design reference (MAF's own shipped shape), ready to seed a real ADR once a milestone
claims it. **Do not implement from this doc directly** — per explicit project-owner direction
(2026-08-13): document the gap and study the reference design first, no code yet.

**RFC:** 014 (Workflow and Orchestration) §3/§7. **Research:**
`docs/research/2026-08-13-maf-agent-as-workflow-executor.md` — MAF's (.NET) real, shipped design for
the equivalent bridge, read directly from `D:\GitSrc\agent-framework`, cited file:line. AgentEngine is
deliberately shaped after MAF's vocabulary and mechanics (`CLAUDE.md`: "The developer model is
MAF-shaped"); this gap should be closed by following MAF's real shape, not by inventing an
incompatible one.

**Design draft, already red-teamed once:** `agent-as-workflow-executor-design-draft.md`. The first
draft's "no core-seam change needed" claim turned out FALSE against real code — a genuine checkpoint/
resume amnesia bug and a genuine concurrent double-resume race, both FATAL as originally scoped. Read
that doc before starting any implementation; it also names one small, independently-landable
prerequisite (a real test for `check_workflow_executable()`'s runtime refusal — see "What's real
today" below, last paragraph) that doesn't depend on the rest of the design.

## What's real today

`executor_kind` (`include/agentengine/workflow/graph.hpp:89`) is a closed enum: `agent, function,
sub_workflow, request_port`. All four are real at the DECLARATION layer — `Executor{id, kind, ...}`,
`validate_workflow()`, the YAML compiler (`workflow/yaml_compiler.hpp`), and the Mermaid/Graphviz
renderer (`workflow/introspection.hpp:88,174`) all handle `agent`-kind nodes correctly as data.

**Deliberately fenced off at the EXECUTION layer, not silently mishandled.** M6 (Phase E) found that
`WorkflowSupervisor` asks every non-port node through `FunctionExecutor` — so an `agent`-kind node
would silently *run as a plain function* if nothing stopped it, "plausible output from a graph whose
behaviour differs from what its author declared"
(`docs/planning/milestone-6-multi-agent-orchestration-breakdown.md:263-266`). M6 built
`check_workflow_executable()` (`workflow/graph.hpp:431`) specifically to refuse this: it rejects any
`Workflow` containing an `agent`- or `sub_workflow`-kind executor outright, and
`WorkflowSupervisor::initialize()` calls it alongside `validate_workflow()` before marking itself
`valid_` (`rt/workflow_supervisor.hpp:489-491`) — so today, a graph naming an `agent`-kind executor
validates as well-formed DATA but refuses to RUN. The file's own comment states the design intent
directly: *"Delete a kind's branch when that kind lands. A refused graph is recoverable; a quietly
reinterpreted one is not."* (`graph.hpp:429-430`). This is the same "unforgeable by construction, not
merely by convention" discipline this project applies everywhere else (ADR-009, ADR-028's
`captures_session_state` guard, etc.) — the gap is real, but nothing about it is a silent
correctness hazard today.

**A separate, small, already-confirmed test-coverage gap on the fence itself.** Verified this session
(build+run a throwaway program, since deleted): `run_workflow()` on a graph with an `agent`-kind
executor genuinely returns `workflow_status::invalid` — the fence works. But `check_workflow_executable()`
is called from exactly one place in the whole tree (`rt/workflow_supervisor.hpp:490`) and is otherwise
untested — `tests/test_workflow_graph_validation.cpp:177-178`'s own comment claims the runtime refusal
"is tested in `test_workflow_request_port.cpp`," a file that no longer exists under that name. Only
`validate_workflow()`'s ACCEPTANCE of an agent-kind graph as well-formed data is tested (line 179-183);
the runtime REFUSAL to run one is not. Small, independently-landable fix, doesn't depend on anything
else in this doc.

**What the live examples do instead, admittedly.** `examples/16_group_chat_live.cpp` and
`17_planner_live.cpp` (real `OpenAIChatClient` calls against OpenRouter) both work around the fence by
declaring their nodes `executor_kind::function` and calling `ChatClient::chat()` directly inside the
`ExecutorBody` closure — `16_group_chat_live.cpp`'s own comment: *"`agent`-kind executors aren't
runnable yet... this is what a model-backed node looks like TODAY, as an ordinary `function`-kind
executor whose body happens to call a real `ChatClient`."* (lines 8-11). This is narrower than a real
agent-in-workflow: it bypasses `AgentSession` entirely — no tool-calling loop, no capability-gated
tools, no context providers, no skills — only a raw chat completion.

## What MAF actually built (full detail in the research doc)

Summary (see the research doc for file:line citations): MAF wraps a real `AIAgent` in
`AIAgentHostExecutor`, produced via an implicit `ExecutorBinding(AIAgent) => agent.BindAsExecutor()`
conversion so graph-building code can pass a raw agent as a node. It holds one `AgentSession` (MAF's
term for per-run conversation state) per WORKFLOW RUN — lazily created, reused across turns within
that run, never shared across runs (`declareCrossRunShareable: false`, explicit comment: "we maintain
turn state on the instance"). Isolation across runs comes from minting a fresh executor instance per
run (`ExecutorBinding.CreateInstanceAsync(sessionId)`), not from any per-call reset. The agent's
response is filtered (strip non-portable content) and re-sent as the next message, which IS the
routing mechanism (MAF routes by message type). A pending tool/approval request withholds the
outgoing turn-completion signal — the same shape as AgentEngine's own `Interaction`/suspend-for-
approval (ADR-029). Streaming and non-streaming are two distinct code paths in the SAME bridge
function, both surfacing through the workflow's one event stream. Session state checkpoints through
the SAME mechanism every other stateful executor uses, not a special case.

**What MAF's design does NOT answer for AgentEngine.** MAF has no in-process capability/authority
system analogous to `CapabilitySet`/`EffectContext::capabilities` — `IWorkflowContext` carries nothing
like it. MAF's own design therefore gives zero precedent for the one question that actually matters
most for AgentEngine's I2 invariant ("no ambient authority"): **where does an agent-executor's
`CapabilitySet` come from** — the workflow's own declared capability ceiling (analogous to how a tool's
`capability_ceiling` is declared on `Tool<...>`), a per-executor binding-time grant (mirroring
`AIAgentHostOptions`, supplied once when the node is wired into the graph), or something derived from
the invoking run's own principal? This is real AgentEngine-specific design work a future ADR has to
do — not something "port MAF" resolves by itself.

## What a future ADR needs to resolve (not designed here — scoping only)

Mapping MAF's shape onto AgentEngine's existing vocabulary, in rough correspondence:

| MAF | AgentEngine equivalent (existing, real) |
|---|---|
| `AIAgentHostExecutor` | A new `rt::ExecutorBody`-producing adapter, not yet built |
| `AIAgent`/`AgentSession` (MAF's) | `agentengine::rt::AgentSession<ChatClientT, ...>` (real, ADR-037) |
| `AIAgent.RunAsync`/`RunStreamingAsync` | `AgentSession::start_run()` (real, `rt::task<result<AgentResponse>>`) |
| `ExecutorBinding.CreateInstanceAsync(sessionId)` (fresh session per run) | Needs a decision: does `WorkflowSupervisor` mint a fresh `AgentSession` per `run_workflow()`, matching MAF exactly? |
| `IWorkflowContext.SendMessageAsync` (routing = re-send as next message) | AgentEngine already routes via `ExecutorOutcome{payload, routes}` — a real, different, arguably simpler shape (explicit route list, not implicit-by-message-type); does NOT need to copy MAF's routing mechanism, only the agent-hosting pattern |
| `TurnToken` withheld while `HasOutstandingRequests` | AgentEngine's own `Interaction`/ADR-029 suspend-for-approval — likely reusable as-is, needs a real design pass to confirm the workflow-suspend and session-suspend mechanisms compose correctly |
| `AIAgentHostOptions` (binding-time config) | No AgentEngine equivalent yet — **this is where the `CapabilitySet` question above needs to land** |
| Checkpoint via `_agent.Serialize/DeserializeSessionAsync`, same mechanism as any stateful executor | `AgentSession::to_record()`/`restore_from_record()` already exist (real); `WorkflowSupervisor::to_record()`/`restore_from_record()` already exist (real) — plausible these compose directly, needs verification, not assumed |
| `drive<T>()`-style bridge from async agent call into a synchronous `ExecutorBody` | Already proven working in AgentEngine's own tree: `examples/16_group_chat_live.cpp`'s `drive<T>()` (lines 54-58) already drives a real `ChatClient::chat()` call from inside a synchronous `ExecutorBody` — the same pattern applies unchanged to `AgentSession::start_run()`, since ADR-037 made both the same `rt::task<T>` shape |

No new technology needed to close this (per explicit project-owner direction: this is a MAF port with
extensions, not a place to reach for something exotic) — every primitive on the AgentEngine side of
the table above already exists and is independently proven; what's missing is the adapter function
itself and a real answer to the `CapabilitySet`-sourcing question.

## Where this was NOT deferred, for the record

Unlike `backgroundable-standingeffect-gap.md`, this was not passed over unnamed — M6 named it directly
(the `check_workflow_executable` design decision, above) and fenced it off deliberately rather than
building it or forgetting it. ADR-032 (worktree scoping for workflow executors) and 014's own RFC text
both separately reconfirm the fence is still in place as of 2026-08-11/13
(`decisions/ADR-032-workflow-executor-worktree-scoping.md:41-42`,
`014-Workflow-and-Orchestration.md:44`) — nothing has silently drifted since M6 closed.
