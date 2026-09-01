# ADR-162: A `Workflow`-as-`ChatClient` adapter (closes issue #35)

## 1. The question

Can a whole, already-initialized `agentengine::rt::WorkflowSupervisor` satisfy this codebase's
`ChatClient` concept, so a built `Workflow` can be reused anywhere a model-backed agent backend is
expected — mirroring MAF's `workflow.as_agent()` (`python/samples/03-workflows/agents/
sequential_workflow_as_agent.py`, `agent-framework` clone, commit `4c0bff8`) — without silently
fabricating token usage, without corrupting a paused human-in-the-loop interaction the moment it is
composed with `AgentSession`'s own turn loop, and without any of the dangling-reference/thread-
boundary hazards a detached-worker design invites?

GitHub issue #35, left open by ADR-150 (issue #36, the OTHER direction — a `Workflow` embedded as an
`ExecutorBody` graph node). This ADR is the `ChatClient`-shaped adapter #35 actually asked for.

## 2. Scope

One piece: `agentengine::rt::WorkflowChatClient` (`include/agentengine/rt/workflow_as_chat_client.hpp`),
constructed from `std::shared_ptr<WorkflowSupervisor> inner` (already `initialize()`d), satisfying
`ChatClient` via `capabilities()` + `chat_stream()` — deliberately no `chat()` (§4). A small, additive
extension to `WorkflowSupervisor` itself: `open_interaction_asks()` (`workflow_supervisor.hpp`), an
`OpenPort`-only sibling of the existing `open_interactions()`, exposing what each open interaction is
actually asking, not just that one exists.

## 3. Design and red-team history

Ten independent red-team rounds ran against the design BEFORE any code existed — full detail in
`docs/planning/workflow-as-chatclient-adapter-design-draft.md` §10. 18 MUST-FIX findings across those
rounds, summarized by theme (each fully traced, with file:line evidence, in the design draft itself):

1. **The ask/resume signal is `Custom`, never `ToolCall`** (round 1). A `ToolCall`-shaped encoding
   would let `AgentSession::tool_calls_of()` silently pick up the ask, route it through
   `invoke_tool()`'s unknown-tool path, and auto-resolve the paused interaction with a fabricated
   answer — deterministic corruption, not a rare edge case, the moment this adapter is bound as an
   ordinary agent's `ChatClientId` backend.
2. **The message-flattening design was actively wrong, twice** (rounds 4 and 9's own follow-through).
   Physically concatenating `ChatRequest.messages`' content items and relying on per-item
   `ContentItem::origin` for turn attribution fails because neither real backend
   (`OpenAIChatClient`/`AnthropicChatClient`) reads `origin` at all — both key entirely on the outer
   `Message::role`. Fixed by encoding the whole message list as one opaque `Custom` envelope built from
   this codebase's own existing `message_to_json()`/`message_from_json()` codec, which a wrapped
   `start` executor must explicitly decode — no "just forward the Message" path exists that could
   silently corrupt a real backend's wire call.
3. **`Usage` cannot be honestly reported, and every attempt to paper over that broke something**
   (rounds 6, 7, 8, 9 — the single most-iterated finding of the whole review). A fabricated zero
   silently defeats real `.token_budget()` enforcement (a genuine I8 violation); a `chat()` that fails
   closed on missing usage instead breaks the adapter's own headline "direct caller" use case, because
   `chat()`'s signature cannot distinguish a budget-reliant `AgentSession` binding from a caller who
   never needed one. **Resolved structurally**: `chat()` is not implemented at all — `ChatClient`
   already makes it optional for exactly this reason. A direct caller drains `chat_stream()` and never
   sees a failure (usage honestly `nullopt`, never fabricated); an `AgentSession` binding still cannot
   complete a call, but via `AgentSession::run_model_call()`'s own pre-existing, already-audited
   `chat()`-absent fallback, not a bespoke rule this adapter invents.
4. **The `EffectContext` crossing the detached-thread boundary needed real sanitization, not a bare
   copy** (rounds 3, 6, 7, 8). A hand-picked field extraction missed fields as the design grew; a later
   whole-struct copy forwarded raw borrowed pointers (`bound_capabilities`, `sandbox_fs`) a real,
   already-established codebase precedent (`tool_pipeline.hpp::background_task()`) resets, not trusts.
   Fixed with `workflow_chat_client_detail::sanitize_for_detached_worker()` — nulls every borrowed
   pointer, resets every call-scoped reverse-channel callback to a no-op, keeps only what this
   adapter's own design genuinely needs (`cancellation`, `deadline`, `tool_result_byte_threshold`,
   `blob_sink`).
5. **The history envelope needed a real size ceiling** (round 5/6). Unbounded — the whole conversation
   re-encoded on every turn, since this adapter re-runs from `start` every call — contradicts this
   codebase's own established `tool_pipeline.hpp::normalize_success()` fail-closed-if-no-sink
   convention for exactly this problem. Fixed by applying the identical rule: promote to
   `Media{BlobRef}` via `ctx.blob_sink` once `payload_json` crosses `ctx.tool_result_byte_threshold`,
   failing closed with no silent fallback to inlining when no sink is configured.
6. **`chat_stream()`'s multi-item push design was under-specified, then wrong, then fixed** (round 5/6).
   A suspended run with N open interactions produces N `ChatResponseUpdate` pushes (one per ask), not a
   single push carrying a whole multi-item `Message` — `ChatResponseUpdate::delta` is a singular
   `ContentItem`. Matches `ReplayChatClient::run_replay_worker`'s own real chunked-push pattern.

Round 10 confirmed the design has no remaining MUST-FIX defect and found only two stale-documentation
bullets (fixed in the same pass) — the first round of the ten to report the design itself closed.

## 4. The accepted design

- `WorkflowChatClient(std::shared_ptr<WorkflowSupervisor> inner)` — infallible construction (unlike
  ADR-150's sibling adapter, this one has no graph shape it needs to refuse — supporting
  `request_port` graphs is its entire purpose).
- `capabilities()` returns `{streaming=false, tool_calling=false}` — both honest: no real incremental
  streaming exists, and this adapter never emits a genuine `ToolCall`.
- `chat_stream()` is the ONLY method (no `chat()` — §3 finding 3). Spawns a detached worker thread that
  runs the whole read-then-act algorithm and pushes the result; `chat_stream()` itself returns
  synchronously, exactly like `ReplayChatClient`/`OpenAIChatClient`.
- **Fresh call** (no open interactions): the caller's `ChatRequest.messages` are encoded via
  `message_to_json()` into a JSON array, wrapped as one `Custom{type_id=
  "agentengine.workflow_chat_client_history"}` item (or promoted to `Media{BlobRef}` if oversized), fed
  to `run_workflow()`.
- **Resume call** (open interactions exist): `request.messages` is scanned for
  `Custom{type_id="agentengine.workflow_request_port_response"}` items naming an interaction_id in the
  CURRENT `open_interactions()` set; each match resolves via `resume_workflow()`, re-checking
  `open_interactions()` before the next match — correctly handles a caller answering M of N open
  interactions in one call. No matching signal at all fails closed
  (`chat_client.workflow_chat_client.no_matching_resume_signal`) rather than silently discarding the
  paused run or starting a second concurrent one.
- **Completed**: one `ChatResponseUpdate` per `ContentItem` in the output `Message`, in order,
  `is_final` only on the last.
- **Suspended**: one ask-signal `Custom{type_id="agentengine.workflow_request_port"}` update per
  currently-open interaction (`WorkflowSupervisor::open_interaction_asks()`).
- **Any other terminal status**: fails closed with a status-specific error code
  (`chat_client.workflow_chat_client.inner_run_not_completed.<status>`).
- `EffectContext`: a `sanitize_for_detached_worker()`-cleaned copy crosses the thread boundary once,
  before detaching; `chat_stream()` never reads the caller's own `EffectContext&` from the worker.
  Cancellation bridges to `inner->cancel()` via `std::stop_callback`; deadline gets a pre-call-only
  check (deliberately not a mid-run poll — an I5 concern, matching
  `routing_model_call_gateway.hpp`'s own precedent for excluding `deadline` from a replay-sensitive
  boundary).
- One conversation per `WorkflowChatClient` instance, for its whole lifetime — not a stateless, freely
  shared `ChatClient` the way `OpenAIChatClient` is.

Full design and every rejected alternative: `docs/planning/workflow-as-chatclient-adapter-design-draft.md`.

## 5. What this ADR does not claim

- **Superseded by ADR-163, in full.** At the time this ADR was written, an `AgentSession` bound to
  this adapter could not complete a call at all, in any configuration, because `WorkflowSupervisor`
  had no usage-tracking mechanism to report honestly (this was the accepted cost of never fabricating
  a token count — the alternative, any value that lets the call silently succeed, is a worse, invisible
  I8 violation). ADR-163 closed that gap completely: `WorkflowChatClient::chat_stream()` now sets
  `usage` UNCONDITIONALLY on every terminal push (a real `agent`-kind node's own honest cost, or an
  honest zero for a graph with none), so an outer `AgentSession` bound to this adapter completes
  `start_run()` regardless of the wrapped graph's shape — proven by actually composing both shapes
  (ADR-163's own T12a/T12b), not merely reasoned about. The remaining, narrower residual is about WHAT
  is tracked, not whether the composition completes at all: an ordinary `function`-kind node's own
  direct `ChatClient` calls (bypassing this engine's tracking entirely) and a nested `sub_workflow`
  suspended mid-run (contributing zero to the total until it resolves) are both still real, disclosed
  gaps in the NUMBER reported, never in whether a bound `AgentSession` can complete. See ADR-163 §4 for
  the full, precise disclosure.
- Two further compositions are explicitly out of scope for the same reason ADR-150's own §5 named its
  own residuals: (a) "sub-agent inside another agent's own tool set" with an `AgentSession` in between
  — no designed answer-routing mechanism for surfacing a paused interaction THROUGH an intermediate
  session; (b) an outer session with `set_output_schema()` armed — a `Custom`-only suspended response
  fails that session's own schema validation.
- **`RecordingChatClient<WorkflowChatClient>` does not compile.** `RecordingChatClient<Inner>` gates on
  `LegacyChatClient` (requires `chat()`); this adapter deliberately has none. A real, accepted trade —
  see §3 finding 3 — not an oversight. NEVER add a `chat()` method back, even a stub, to work around
  this: `AgentSession`'s own dispatch is a pure type-level `if constexpr` on whether `chat()` exists,
  never on what it does, so any `chat()` at all silently reintroduces the exact defect closed here.
- `open_interaction_asks()`'s ask is real only for `OpenPort` entries; a `PendingSubWorkflow` entry
  (ADR-157's nested sub-workflow mechanism) gets an honestly-empty `Message{}` placeholder — the
  recursive walk needed to expose its real nested ask is real, unbuilt follow-on work.
- `WorkflowChatClient`'s own wrapped `WorkflowSupervisor` (`inner_`) sits entirely outside ADR-157's
  `split_worker_budget()` nesting accounting — a shared, pre-existing property of "wrap a whole
  independently-constructed `WorkflowSupervisor`" ADR-150's own sibling adapter has too, not unique to
  this one. Candidate follow-on work covering both adapters together.
- Does not touch issue #33 (`executor_kind::sub_workflow` as a graph node) or ADR-150/issue #36
  (`workflow_as_executor_body()`) — unrelated mechanisms.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | `capabilities()` reports `streaming=false`/`tool_calling=false`; the type has no `chat()` member. | CORRECT | `test_rt_workflow_as_chat_client.cpp` T1; `WorkflowChatClient`'s own `static_assert(ChatClient<WorkflowChatClient>)` |
| 2 | `sanitize_for_detached_worker()` nulls every borrowed pointer field and resets every call-scoped callback to a no-op, without mutating the caller's own `EffectContext`. | CORRECT | T2 |
| 3 | A fresh call's history envelope round-trips through `message_to_json()`/`message_from_json()`; the wrapped `start` executor decodes exactly the caller-supplied messages, in order. | CORRECT | T3 |
| 4 | A completed run's output pushes one `ChatResponseUpdate` per `ContentItem`, `is_final` only on the last, `usage` honestly `nullopt`. | CORRECT at the time written; narrowed by ADR-163 | T4 as it stood for this ADR's own "prove" phase. ADR-163 later gave `WorkflowSupervisor` a real usage-tracking mechanism, so `usage` is no longer always `nullopt` — T4 was updated in place to assert a real, tracked value (honest zero for a graph with no `agent`-kind node) rather than `nullopt`. The claim actually being tested here — never a *fabricated* nonzero value — still holds; only the specific "always `nullopt`" shape changed. |
| 5 | Reaching a `request_port` is a SUCCESSFUL suspended response (not an error) carrying a `Custom` ask-signal, never a `ToolCall`; resuming with a matching signal in the growing history completes the run with the port's own resolved response. | CORRECT | T5 |
| 6 | A signal-less call while a prior turn is paused fails closed with the documented contract-mismatch error code, never silently discarding the paused run. | CORRECT | T6 |
| 7 | Two simultaneously-open interactions produce two ask-signal updates; answering only one (a genuine partial answer) leaves the run suspended on exactly the other, unchanged interaction_id; answering the remainder completes the run. | CORRECT | T7 |
| 8 | An oversized history envelope fails closed with no `blob_sink` configured, and succeeds (promoted to `Media{BlobRef}`) once one is. | CORRECT | T8 |
| 9 | A non-completed, non-suspended terminal status (`bound_max_rounds`) fails closed with a status-specific error code. | CORRECT | T9 |
| 10 | The wrapped-`request_port` round-trip composes end to end through a real, standalone `chat_stream()` caller (not just unit-tested in isolation). | CORRECT | `examples/28_workflow_as_chat_client.cpp` — two real `chat_stream()` calls (open, then answer), `OK` |
| 11 | Every pre-existing workflow-family test still passes after this addition. | CORRECT | Full `ctest -R "workflow"`: 32/32 passed, zero regressions (includes the sibling ADR-150 adapter's own suite, unaffected by the additive `open_interaction_asks()` change) |
| 12 | The full project builds clean under this codebase's enforced `/WX`. | CORRECT | `cmake --build` (Debug, Visual Studio 18 2026, MSVC), zero errors, zero warnings, for the new header, the `workflow_supervisor.hpp` addition, the test, and the example |

## 7. Files changed

**New:**
- `docs/planning/workflow-as-chatclient-adapter-design-draft.md` (ten red-team rounds' full history)
- `include/agentengine/rt/workflow_as_chat_client.hpp`
- `tests/test_rt_workflow_as_chat_client.cpp`
- `examples/28_workflow_as_chat_client.cpp`

**Edited:**
- `include/agentengine/rt/workflow_supervisor.hpp` — additive: `WorkflowSupervisor::InteractionAsk`
  and `open_interaction_asks()`, alongside the existing `open_interactions()`. No existing method's
  behavior changed.
- `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new target registrations.

## Status

**Proposed — implemented, ten independent red-team rounds against the design before any code existed
(the tenth reporting no remaining MUST-FIX defect), all evidence executed and passing, pending
project-owner sign-off.** Full `cmake --build` (Debug, Visual Studio 18 2026, MSVC) and the full
workflow-family `ctest` suite (32/32) both clean; `examples/28_workflow_as_chat_client.cpp` run
directly, `OK`. Two real implementation bugs — a wrong assumption about `output_selection` merge
semantics with no `fan_in` edge, and an invalid `max_rounds=0` construction — were found and fixed
during this "prove" phase itself, by the tests catching them, not by further review.
