# MAF workflow-vs-agent split and human-in-the-loop model — source-grounded notes

**Date:** 2026-08-03 · **Source:** fresh clone of Microsoft Agent Framework
(`https://github.com/microsoft/agent-framework`, commit `f5dfb1413ebb5f8be6e344e211d50c5061eb66f2`,
2026-08-03; the prior `D:\GitSrc\agent-framework` checkout used by
[2026-maf-provider-concepts.md](2026-maf-provider-concepts.md) no longer exists on this machine).
File paths and excerpts below are verbatim from that tree, Python package only (`python/packages/`).
This note resolves `OpenQuestions.md` OQ-2 and OQ-4.

## 1. Is a single-agent run a workflow-graph-of-one, or a separate code path?

**Answer: separate code path, and the dependency direction is the opposite of "agent is a
special-cased workflow."** `Workflow`/`Executor` is optional multi-node machinery a caller opts
into; a plain agent run never touches it.

- `python/packages/core/agent_framework/_agents.py` has **no import of `_workflows`** anywhere in
  the file (confirmed by grep — the only hit is a docstring mention). `run()`'s implementation
  (`_agents.py:1055-1067`) goes straight to the chat client:
  ```python
  async def _run_non_streaming() -> AgentResponse[Any]:
      ctx = await _prepare_run_context()
      response = await self._call_chat_client(ctx, stream=False)
      return await self._parse_non_streaming_response(ctx, response)
  ```
  No graph, no executor registry, no superstep loop.
- The `run()` docstring (`_agents.py:1009-1013`) confirms the relationship explicitly: *"Since you
  won't always call `agent.run()` directly (it gets called through workflows)..."* — workflows call
  into agent.run, not the reverse.
- `AgentExecutor` (`python/packages/core/agent_framework/_workflows/_agent_executor.py:119-134`) is
  the adapter that lets an agent participate as **one node** in a graph: *"built-in executor that
  wraps an agent for handling messages."* Its handler (`:424`, `:480`) calls
  `self._agent.run(...)` — the identical method from `_agents.py`. The import direction is
  `_workflows → _agents`, never the reverse.
- `Workflow` itself (`_workflows/_workflow.py:208-217`) is a general Pregel-style engine: *"A
  graph-based execution engine that orchestrates connected executors... running in supersteps until
  the graph becomes idle. Workflows are created using the WorkflowBuilder class - do not instantiate
  this class directly."*

**Conclusion:** MAF does not unify "agent" as a degenerate workflow. It keeps a lightweight direct
call path for the common case and layers a separate, heavier graph engine on top, with `AgentExecutor`
as the bridge when an existing agent needs to become a node in that graph.

## 2. Human-in-the-loop / long-running correlation

**Mechanism:** `WorkflowContext.request_info()` emits a `request_info` event carrying a
`request_id: str` (UUID unless the caller supplies one); the workflow reaches
`IDLE_WITH_PENDING_REQUESTS`; the caller resumes via `workflow.run(responses={request_id: value})`.

- `_workflows/_workflow_context.py:403-434`:
  ```python
  async def request_info(self, request_data: object, response_type: type, *, request_id: str | None = None) -> None:
      """... request_id: Optional unique identifier for the request. If not provided,
      a new UUID will be generated. This allows executors to track requests and responses."""
      ...
      request_info_event = WorkflowEvent.request_info(
          request_id=request_id or str(uuid.uuid4()),
          source_executor_id=self._executor_id,
          request_data=request_data,
          response_type=response_type,
      )
  ```
- Resume-side correlation, `_workflows/_workflow.py:1011-1030` (`_send_responses_internal`):
  ```python
  pending_requests = await self._runner.context.get_pending_request_info_events()
  for request_id, response in responses.items():
      if request_id not in pending_requests:
          raise ValueError(f"Response provided for unknown request ID: {request_id}")
      pending_request = pending_requests[request_id]
      response = try_coerce_to_type(response, pending_request.response_type)
  ```
  The `request_id` is the entire correlation mechanism — a flat string key checked against a
  pending-requests dict.

**Checkpointing ties to the same identity.** `_workflows/_checkpoint.py:30-98`
(`WorkflowCheckpoint`): keyed by `workflow_name` + `graph_signature_hash` (not a specific workflow
*instance*), chained via `previous_checkpoint_id`, and its `pending_request_info_events: dict[str,
WorkflowEvent[Any]]` field is indexed by that same `request_id`. `_workflow.py:1037-1041` notes a
checkpoint is taken *at the moment responses are delivered*, "before the runner processes them in
the next superstep... so a human-in-the-loop continuation is fully replayable." Resume from a
checkpoint (`_workflow.py:987-1009`) restores this dict, then validates/delivers `responses` exactly
as the non-checkpoint path does.

**Protocol projection precedent (AG-UI).** `python/packages/ag-ui/agent_framework_ag_ui/_workflow_run.py`
shows MAF's own answer to "one internal identity, several protocol identities":
- `:149-165` — the internal `request_id` becomes AG-UI's interrupt `"id"` directly, with the full
  internal tuple (`request_id`, `source_executor_id`, `request_type`, `response_type`) preserved
  underneath an `"agent_framework"` metadata key rather than discarded.
- `:845-848` — the *same* `request_id` is reused a second time as AG-UI's `tool_call_id` when the
  pending request is surfaced as a tool call — one internal token, two protocol-facing aliases,
  simultaneously.
- On resume (`:826-831`), incoming AG-UI payloads are mapped by `request_id` back into the
  `responses` mapping before it reaches `_send_responses_internal` — the alias is stripped back off
  before it touches the correlation mechanism in §2 above.

## 3. What this settles for AgentEngine

- **OQ-2 (001 Q1 / 014 Q1) resolves to: do not unify.** Keep the single-agent turn loop (001 §3) as
  its own lightweight path, separate from the `Workflow` graph engine (014), matching MAF's actual
  split rather than the "uniformity" framing OQ-2 was originally posed under. An agent becomes a
  workflow *node* only through an explicit adapter (AgentExecutor's role) when a caller opts into a
  multi-node graph — never implicitly.
- **OQ-4 (001 §2 / 014 §4-5)**: our internal `InputRequired`/`Suspended` shape should carry a single
  flat correlation token (MAF's `request_id` equivalent) as the one source of truth, with each
  protocol's own correlation identity (MCP `requestState`, A2A `taskId`, AG-UI `interruptId`) as a
  *projection* of that token at the protocol boundary — including MAF's demonstrated pattern of the
  same token aliasing to more than one protocol-facing field (AG-UI's interrupt `id` and
  `tool_call_id` both being the same `request_id`) without conflict, because the projection layer,
  not the token itself, is protocol-specific.
