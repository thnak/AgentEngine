# MAF's four "background"/"long-running" concepts — none of them are batch inference

**Date:** 2026-08-21. **Sources:** `https://learn.microsoft.com/en-us/agent-framework/agents/background-agents`
(`ms.date` 2026-07-29, updated 2026-08-10), `https://learn.microsoft.com/en-us/agent-framework/agents/background-responses`
(`ms.date` 2026-05-27, updated 2026-08-12), `https://learn.microsoft.com/en-us/agent-framework/agents/agent-hooks`
(`ms.date` 2026-08-07, updated 2026-08-14), `https://learn.microsoft.com/en-us/agent-framework/agents/planning-and-todos`
(`ms.date` 2026-07-29, updated 2026-08-10) — all fetched live 2026-08-21. Per `CLAUDE.md`'s research
discipline: fetched, not recalled. Written to close out the "do we have something like background
agent provider like MAF have?" question from earlier this session with primary sources instead of the
name-based guess that started it.

MAF ships four distinct things that all touch "the agent doesn't finish in one synchronous call."
None of them is a vendor Batch API. Confirms and sharpens the conclusion already reached earlier this
session (before these pages were fetched): MAF has no batch-mode toggle anywhere.

## 1. Background Agents (`BackgroundAgentsProvider`) — multi-agent task delegation

A parent agent hands independent tasks to named **child agents**, each running in its own child
session concurrently. The provider adds six model-facing tools: `background_agents_start_task`,
`background_agents_wait_for_first_completion`, `background_agents_get_task_results`,
`background_agents_get_all_tasks`, `background_agents_continue_task`,
`background_agents_clear_completed_task`. Task status: `running | completed | failed | lost` (lost =
in-process handle didn't survive a restart/session-restore). No cancellation tool exists by design —
you let a task reach a terminal state, then clear it. `LoopAgent` + `BackgroundTaskCompletionLoopEvaluator`
(.NET) / `AgentLoopMiddleware` + `background_tasks_running()` (Python) add automatic re-polling.
Explicitly **marked experimental** on the page. .NET and Python only — not in Go.

This is fan-out-to-sub-agents, the same shape as AgentEngine's own multi-agent workflow story
(RFC on multi-agent orchestration), not a model-call scheduling primitive at all.

## 2. Background Responses (`AllowBackgroundResponses` / continuation tokens) — what was already found

Confirms the earlier finding exactly. `AgentRunOptions.AllowBackgroundResponses = true` (.NET) /
`options={"background": True}` (Python) / `agent.AllowBackgroundResponses(true)` (Go). A run either
completes immediately or returns a `ContinuationToken`/`continuation_token` the caller polls
(non-streaming) or resumes a dropped SSE stream with (streaming) — `null`/`None` token means done.

**Provider-gated, stated plainly on the page:** *"Currently, only agents that use the OpenAI Responses
API support background responses: OpenAI Responses Agent and Azure OpenAI Responses Agent."* This is
OpenAI/Azure's own server-side async-response feature exposed through MAF's run options — not a
MAF-authored mechanism, and structurally unrelated to OpenRouter's (or OpenAI's own file-upload) Batch
API: one continuation token tracks *one in-flight chat turn*, not a job containing N independent
requests submitted together for later collection. "Some agents may decide autonomously whether to
initiate a background response... regardless of the setting" — i.e. even within its own gated
provider set, the toggle is advisory, not a hard mode switch.

## 3. Agent Hooks — governance control plane, unrelated to scheduling

`AGENT-HOOKS-0.1`-conformant interception contract: `agent_startup → input → pre_model_call →
post_model_call → pre_tool_call → post_tool_call → output → agent_shutdown`, each point returning an
`allow | deny | transform` verdict, fail-closed, buffered-streaming (no partial output crosses the
`output` gate until the full response passes). This is a policy/approval/redaction layer analogous to
AgentEngine's own capability-gate + provenance seams (I2/I3/I4), not a background-execution or
batching concept — included here only because the user's link set named it alongside the other three;
it does not change the batch/background comparison. **Python only, explicitly experimental** ("isn't
yet available for .NET [or Go]", `ExperimentalWarning` on first use).

## 4. Planning and Todos (`TodoProvider` / `AgentModeProvider`) — plan/execute state tracking

Two context providers: a todo list (`todos_add/complete/remove/get_remaining/get_all` tools, injected
into context each run so an agent can resume outstanding work across turns) and a `plan`/`execute`
mode switch (`mode_get`/`mode_set`, `plan` is interactive/clarifying, `execute` is autonomous and
works the todo list down). `TodoCompletionLoopEvaluator` / `todos_remaining()` drive a bounded loop
(`LoopAgent`, `MaxIterations`) until the todo list is empty. Session-scoped state (`.NET`:
`AgentSession.StateBag`; Python: `TodoSessionStore` or a pluggable `TodoFileStore`). Again: state
tracking and loop-continuation, not model-call batching or async dispatch. .NET/Python only.

## Net conclusion

None of MAF's four "long-running work" primitives is a vendor vendor Batch API concept. The one that
sounds closest — Background Responses — is a single-turn async/poll wrapper around a
provider-specific (OpenAI/Azure Responses API only) server-side feature, not a multi-request job
queue. Background Agents is multi-agent delegation. Agent Hooks and Planning/Todos are governance and
state-tracking respectively, unrelated to either axis.

This leaves AgentEngine's actual position as already stated earlier this session, now with primary
sources behind all four MAF comparisons rather than three: a real, live-verified vendor Batch API path
(`tools/batch_infer.cpp`, OpenRouter's `/api/beta/batches`, deliberately standalone and NOT wired into
`AgentSession`'s run loop per `docs/planning/batch-inference-coalescing-gap.md`'s standing gate), a
real tool-scoped background primitive (`StandingEffect` / `AgentSession::start_background_task()`,
`include/agentengine/rt/agent_session.hpp:1163`, `standing_effect_kind`: `schedule_wakeup,
watch_resource, background_task`), and a real resilience/failover gateway
(`ModelCallGateway<Primary, Fallback...>`) — three real primitives on a different axis each from all
four of MAF's, none of which is "sync vs. batch as a per-call toggle" the way the user's original
question framed it.
