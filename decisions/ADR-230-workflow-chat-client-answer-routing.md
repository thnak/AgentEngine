# ADR-230: Answering a `WorkflowChatClient` request port from behind an outer `AgentSession` (issue #44)

**Status: Proposed (2026-10-01).** Designed, self red-teamed (findings below, all MAJOR ones fixed before
commit), implemented and proven by real test execution with mutation positive controls. Awaiting project-owner
Judge. Amends ADR-162 (its §4a/§9-item-1 descoping of compositions (a) and (b)).

## 1. The question

`WorkflowChatClient` (`include/agentengine/rt/workflow_as_chat_client.hpp`, ADR-162) lets a workflow stand in
for a model. When the wrapped workflow pauses on a `request_port`, the adapter pushes a
`Custom{"agentengine.workflow_request_port", ...}` item. ADR-162 deliberately chose `Custom` over a MAF-style
`ToolCall` because the `ToolCall` encoding let `AgentSession`'s turn loop answer the paused port with a
fabricated tool result. That fix prevented corruption but left two compositions descoped (design draft §4a,
§9 item 1):

- (a) a caller behind an OUTER `AgentSession` bound to the adapter can see the ask but cannot answer it: it holds
  no reference to the adapter, and the only answer channel was a `Custom` resume item in the history of a direct
  `chat_stream()` call;
- (b) an outer session with `set_output_schema()` armed fails the pause as
  `run.output_schema_validation_failed` (the `Custom`-only response has no text to validate).

How does the outer session's caller answer, without letting anything a model writes open, answer or route the
interaction (I3), and how do #155 route refusals, #157 per-delivery ids and ADR-214 cancel compose with it?

## 2. Decision

### 2.1 The pause is read from the client's state, never from response content

New optional concept `InteractiveChatClient` (`core/chat_client.hpp`), duck-typed like `HasProducerChatClientId`:

```cpp
std::vector<ClientInteractionAsk> pending_client_interactions() const;  // {client_interaction_id, ask Message}
void cancel_client_interactions() const noexcept;
```

`WorkflowChatClient` implements it from the supervisor's own `open_interaction_asks()` / `cancel()`.
`AgentSession` (template hook `bound_pending_client_interactions()`, `if constexpr` on the concept; nullopt for
every other client, so nothing changes for them) asks the client after EVERY completed model call. If the client
is waiting, the round suspends:

- BEFORE the tool-call loop (nothing in a paused response is dispatched) and BEFORE output-schema validation
  (closes (b): a pause is not a final answer);
- one `Interaction` per pending client interaction, new reason `interaction_reason::client_input` (codec string
  `client_input`); already-open ones keep their ids across re-suspension, ones the client no longer holds close;
- `input_required` for every open one; the run returns the sentinel `run.suspended_for_client_input`
  (`AgentSessionCore::kSuspendedForClientInput`), the same "never fold, never fabricate" shape as
  `run.suspended_for_approval`;
- the caller reads each ask with `AgentSession::client_input_ask(id)`.

A model-shaped ask in response content is never consulted: it cannot open an interaction (O3), and it cannot
hide one either.

### 2.2 The answer is the host's, and travels out of band

The caller answers with the ordinary `resolve_interaction()` (admission and per-request authority run first,
exactly as for every other reason): `answer` (text shorthand, a user message) or the new
`ResolveInteraction::client_answer` = `ClientInputAnswer{response Message, routes}` — exactly one. The session
records it and puts it on the NEXT model call's new `ChatRequest::client_interaction_answers` field (set in one
place, `run_rounds_body()`; a turn middleware sees a `ContextContribution`, never the request). The adapter:

- treats out-of-band answers as the ONLY answers for that call — the history is not scanned at all;
- refuses (`chat_client.workflow_chat_client.answer_not_open`) an answer naming an interaction that is not open,
  and never starts a fresh run in place of an answer;
- passes `routes` through to `ResumeWorkflow` (they were hard-coded `{}` before, so a switch_case port could
  not be answered through the adapter at all);
- reports a workflow refusal (`invalid_routes`, `invalid`, `admission_denied`) as
  `chat_client.workflow_chat_client.answer_refused.<status>`.

Whether the answer was TAKEN is decided by the client's state after the call (does it still hold the id), never
by the error text. Taken: the interaction closes, `input_resolved{id, approver_id}` is emitted, the answer is
appended to history as the user's turn, then the response, and the round continues (it may pause again, or
finish and be schema-validated). Still held: `session.resolve_interaction.answer_refused` — the interaction stays
open, history is untouched, the turn index is given back, a `warning` and a fresh `input_required` are emitted,
and no `run_failed` (#155: refusal, not run outcome).

### 2.3 Lifecycle

- `start_run()` while the client holds an interaction is refused (`run.client_input_pending`) — a fresh call
  would either be refused by the adapter or reach its history scan. Interactions the client no longer holds are
  closed first (and, if a cancel ended them, the suspended run gets its `run_canceled`).
- `cancel()` now also calls the client's `cancel_client_interactions()`: a session suspended on `client_input`
  has no run in flight, so this is how the wrapped workflow ends `cancelled` immediately (ADR-214). A later
  resolve of the interaction ends `run.canceled`; a later `start_run()` runs fresh (O6).
- `run_rounds()` is now a wrapper: any run that ends other than by suspending for (or refusing an answer to) a
  client interaction leaves none open — what the client still holds is cancelled and the interactions closed. A
  run that failed after the host answered therefore never leaves the workflow paused on a conversation the
  session gave up on (which would wedge every later `start_run()`).
- A `client_input` interaction restored from an `AgentSessionRecord` carries no ask; resolving it is
  `session.resolve_interaction.round_not_recorded`, it closes, and the client is abandoned (ADR-196 §7's rule for
  every record-less interaction) (O8). `fork_from`/`clear_in_process_state`/`restore_from_record` drop the
  per-interaction records.

## 3. Self red-team (adversarial; design → prove)

Findings, each with what was done. "Proven" means a test that fails when the fix is removed (mutation run).

| # | Sev | Finding | Disposition |
|---|-----|---------|-------------|
| 1 | MAJOR | Detecting the pause from the response's `Custom` item (the obvious design) lets any content producer — a wrapped agent's output passed through as a completed answer — open an interaction the host then "answers" into nowhere, or hide a real one. | Rejected; the pause is read from the client's own state (§2.1). O3/O4 control. |
| 2 | MAJOR | Interaction ids are predictable (`<workflow>:run:<n>:port:<port>:<round>`, #157 made them distinct per delivery, not secret). A model can plant a response item naming the NEXT run's port id in a completed answer; the caller stores it in history. If the adapter scanned history as well as taking out-of-band answers, the planted item is processed first and the port resolves with the model's answer and route choice. | Fixed: out-of-band answers are exclusive. **Proven**: mutant M1 (scan history too) fails O3 "the port resolved with the HOST's answer". O3 control shows the planted item really is a working signal through the legacy scan, even aimed at the exact live id. |
| 3 | MAJOR (pre-existing, direct path) | The legacy history scan (direct callers) accepted response items of any origin, so the attack in #2 also works against a direct caller that re-sends the adapter's own output as history. | Defence in depth: the scan takes only caller-authored (`user`/`system` origin) items. **Proven**: mutant M3 fails O3-origin. Residual R1. |
| 4 | MAJOR | An answer arriving when nothing is open fell into the adapter's fresh-run branch (which ignores answers): the host believes it answered, the adapter starts a new workflow run on the history. | Fixed: `answer_not_open`, no run started (O1). |
| 5 | MAJOR | `start_run()` while the client is paused would reach the adapter's history scan with a model-writable history. | Refused, `run.client_input_pending` (O2). |
| 6 | MAJOR | A run that fails after the answer was delivered (token budget, max_turns, on_context failure) or a restore leaves the workflow paused forever; every later fresh call is refused by the adapter — the session is wedged. | `run_rounds()` settle step cancels the client and closes (§2.3); restore path cancels (O8). |
| 7 | MINOR | Refusal detection by matching error text is brittle and lets a client's message decide session state. | Decided by client state (still holds the id). **Proven**: mutant M5 (no refusal branch) fails 7 O5 checks. |
| 8 | MINOR | A refused attempt consumed a turn, so retrying a typo'd route burned `max_turns`. | Turn index restored on refusal (O5). |
| 9 | MINOR | `client_answer` on a non-`client_input` interaction silently ignored would let a caller believe it routed something. | Refused, `client_answer_not_applicable`. |
| 10 | NOTE | I2: does the new channel carry authority? | No capability flows: `resolve_interaction()` runs admission and `apply_dispatch_authority()` before the branch; the answer is data to the client; the adapter keeps ADR-162's "caller's EffectContext unused for capabilities". Routes are bounded by the workflow's own port validation (#155). |

### Residuals (honest, not fixed here)

- **R1** Legacy direct-caller scan: a caller that stores history in a form that loses `ContentItem::origin` (or
  rewrites it to `user`) re-opens finding #2 for itself. Direct callers should use
  `ChatRequest::client_interaction_answers`; the history signal is kept for compatibility.
- **R2** Concurrency: `WorkflowSupervisor::open_interactions()`/`open_interaction_asks()` take no lock, and
  `cancel()` settles a suspended run under `run_mutex_.try_lock()`. `cancel_client_interactions()` cancels under
  the adapter's `call_mutex_` when it is free, so a cancel cannot race a session's query in the common case; when
  a query or a worker holds it at that instant, the supervisor's cancel runs unlocked and can race that read. The
  window is the duration of one `open_interaction_asks()` copy. Pre-existing class (the adapter's worker already
  reads the same state while a cancel may land).
- **R3** No new run event carries the ask text: AG-UI's `Interrupt.message` for a `client_input` pause is empty;
  consumers read `client_input_ask()`. A projector change was out of scope.
- **R4** `input_resolved` is emitted after the client took the answer (after `model_call_finished`), not before
  the call as approvals announce theirs (ADR-183) — announcing earlier would announce answers the workflow then
  refuses.
- **R5** Each refused attempt still emits `turn_started`/`model_call_*` (no `turn_finished`), and charges whatever
  the adapter spent (zero for a refusal).
- **R6** A nested `sub_workflow`'s own ask is still surfaced as ADR-162's honestly-empty `Message{}` placeholder.
- **R7** A paused response's tool calls (none from this adapter) are recorded in history but never dispatched.
- **R8** The answer is appended to history verbatim (role forced to `user`); a host that puts model output into
  `client_answer` is making its own I3 decision.

## 4. Evidence

- `tests/workflow/test_rt_workflow_chat_client_outer_session.cpp` — O1..O8, **64/64 checks pass**:
  direct out-of-band answers; outer-session suspend/answer/complete with events and history; forged ask and
  forged answer (with positive controls); output schema; bad route refused and corrected; cancel; malformed
  resolves; restored record.
- Mutation positive controls (each run, then restored): M1 adapter also scans history → O3 fails (1 check);
  M3 drop origin filter → O3-origin fails (2 checks); M5 drop the session's refusal branch → O5 fails (7 checks).
- `tests/workflow/test_rt_workflow_as_chat_client.cpp` (ADR-162/163 T1-T12) unchanged and passing.
- Regression: see the commit message for the agent-session / workflow suite run.

## 5. Files

`include/agentengine/core/chat_client.hpp` (`ClientInteractionAsk`, `ClientInteractionAnswer`,
`ChatRequest::client_interaction_answers`, `InteractiveChatClient`), `core/interaction.hpp`
(`interaction_reason::client_input`), `rt/interaction_codec.hpp`, `rt/agent_session_core.hpp`,
`rt/agent_session.hpp`, `src/rt/agent_session_core.cpp`, `rt/workflow_as_chat_client.hpp`.
