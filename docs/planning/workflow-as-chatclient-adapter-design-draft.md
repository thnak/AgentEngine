# Design draft: a `Workflow`-as-`ChatClient` adapter (closes issue #35)

**Status: revised after red-team rounds 1-10. Round 10 reports the design has no remaining MUST-FIX
defect** — the first round to give that signal, after finding only 2 stale documentation bullets (§9
items 5/7, since fixed) left behind by the editing pass that produced round 9's structural change. Per
this repo's `design → red-team → prove → judge` discipline (CLAUDE.md), this document is still formally
the design step — no code exists yet — but it is genuinely implementation-ready: the next phase is
"prove" (write the code and the tests §11 already names), not another design-level red-team round unless
implementation surfaces something these ten rounds didn't. §10 records all ten rounds' findings in full;
§9 is the current, accurate summary of every resolved design decision and accepted scope boundary.

## 1. The question

Issue #35: AgentEngine has no adapter that lets a whole built `agentengine::rt::WorkflowSupervisor`
(graph + bodies + contexts) satisfy the `ChatClient` concept (`core/chat_client.hpp`), so it can be
reused anywhere a `ChatClient`/model-backed agent is expected — a sub-agent inside another agent's
tool set, one participant in an outer agent-to-agent composition, or a direct standalone call —
mirroring MAF's `workflow.as_agent()`.

**This is not the same gap #36/ADR-150 closed.** ADR-150 built `workflow_as_executor_body()`
(`include/agentengine/rt/workflow_as_executor.hpp`) — a whole `Workflow` reused as an `ExecutorBody`
participant *inside another workflow's own builder graph*. That adapter refuses any wrapped graph
containing a `request_port` node outright, because `ExecutorBody`'s contract is one synchronous call
with no "still pending" concept at all. `ChatClient` is a different embedding: `chat()`/`chat_stream()`
is the literal contract every model-backed agent already satisfies, multi-turn, and — as this draft
found by reading MAF's own reference implementation rather than assuming — MAF's real `as_agent()`
does NOT refuse a paused inner workflow. It has a genuine, working answer for that case that
`workflow_as_executor_body()` structurally cannot reuse. That is the actual, harder mechanism this
draft designs.

## 2. Grounding: what MAF's real implementation does

Read directly against the local `agent-framework` checkout, commit `4c0bff8` (same commit issue #35
cited) — `python/packages/core/agent_framework/_workflows/_workflow.py` (`Workflow.as_agent()`) and
`_agent.py` (`WorkflowAgent`), not asserted from memory.

- **`as_agent()`** (`_workflow.py:1195`) requires the wrapped workflow's start executor to accept
  `list[Message]` — the *entire* normalized input (a bare string, one `Message`, or a list of either)
  becomes a message list handed to `start`, not just the newest turn.
- **Every call re-runs the whole workflow from `start`** (`WorkflowAgent._run_impl`,
  `_agent.py:242`): it combines session history (via an `AgentSession`/`HistoryProvider`, when the
  caller opted into one) with the new input into one `session_messages` list, then drives
  `self._run_core(session_messages, ...)` — a fresh graph execution, not a resumed live instance, for
  the *ordinary* (non-paused) case. This matches the sibling ADR-150 adapter's own "fresh run every
  call" contract and answers this draft's own open question 1 (multi-turn semantics) the same way:
  **re-run from `start`, not a stateful "turn" concept of its own.**
- **A paused workflow is NOT refused.** `WorkflowAgent._process_request_info_event`
  (`_agent.py:701`) converts a `request_info` workflow event (AgentEngine's `request_port` firing) into
  a **`function_call`-shaped `Content` item** on the returned `AgentResponse` — `call_id` = the
  request's own id, `name` = the class constant `REQUEST_INFO_FUNCTION_NAME = "request_info"`,
  `arguments` = `{request_id, request_event}` (the request event carries the actual ask payload,
  serialized). The caller answers by sending a `function_result`/`function_approval_response` content
  item back with a matching `call_id` on its *next* call; `WorkflowAgent._extract_function_responses`
  (`_agent.py:726`) scans the next call's input messages for exactly that, keyed by id, and feeds the
  answers back into the (separately checkpoint-restored) workflow run.
- This is a genuinely different, harder answer than ADR-150's "refuse the graph shape" — MAF encodes a
  suspended `request_port` interaction as an ordinary tool-call turn, using the SAME vocabulary a model
  already uses to call a real tool. This closes this draft's own open question 2 with a real reference
  answer, not an invented one.
- MAF has no I2/capability-ceiling concept at all — this draft's open question 3 (capability sourcing)
  has no MAF reference answer and stays AgentEngine's own call (§6).

## 3. What's real in AgentEngine today (the pieces this adapter composes)

- **`ChatClient` concept** (`core/chat_client.hpp:206`, post-ADR-035 Phase 3) requires only
  `capabilities() -> ChatClientCapabilities` and `chat_stream(ChatRequest const&, EffectContext&) ->
  stream<ChatResponseUpdate>`. `chat()` (`task<result<ChatResponse>>`) is optional — required only by
  the stricter `LegacyChatClient` concept, which nothing new needs to satisfy.
- **`WorkflowSupervisor::run_workflow(RunWorkflow{Message input}) -> task<WorkflowResult>`**
  (`rt/workflow_supervisor.hpp:870`) takes exactly ONE `agentengine::Message`, not a list — narrower
  than MAF's `list[Message]`-typed start executor. `WorkflowResult{status, rounds, output, partial,
  failed_executor, open_interactions, unopened_ports}`.
- **`WorkflowSupervisor::resume_workflow(ResumeWorkflow{interaction_id, response, routes}) ->
  task<WorkflowResult>`** (`:892`) resolves exactly ONE open port/interaction per call; if others
  remain open it returns `suspended` again with the still-open set in `open_interactions()`.
- **`open_interactions()`** (`:810`) returns `vector<Interaction>` — `{interaction_id, run_id, reason,
  opened_at_ns, expires_at_ns}` (`core/interaction.hpp:57`) only. **It does not expose the ask
  payload.** Traced directly: the port's ask (`d.payload`, the `Message` delivered to the `request_port`
  node) is stored internally in `OpenPort::response` (`:1125`) *before* resolution — reused as the same
  field the real answer overwrites after `resume_workflow()` — but `open_interactions()` strips it,
  returning only the bare `Interaction`. **This is a real gap this adapter depends on closing** — see
  §5 and §9.
- **`ContentItem::value`** is `variant<Text, Reasoning, Media, Data, ToolCall, ToolResult, Citation,
  Error, Custom>` (`core/content.hpp:153`). `ToolCall{call_id, tool_name, arguments_json, origin,
  provenance}` and `ToolResult{call_id, content, is_error}` are the exact AgentEngine-native vocabulary
  MAF's `function_call`/`function_result` maps onto — no new content kind is needed.
- **`agentengine::Agent<Derived, Policies...>`** (`core/agent.hpp:120`) is compile-time CRTP
  authoring scaffolding — `register_agent<Derived>()` resolves it to metadata at startup. It is not a
  runtime interface a `WorkflowSupervisor` instance can be made to satisfy; there is no "wrap this
  instance as an `Agent`" operation to build. The only buildable half of issue #35 is the `ChatClient`
  conformer below — an author who wants a wrapped workflow reachable through the ordinary CRTP
  authoring surface writes an ordinary `Agent<Derived, ChatClientId<"...">>` whose bound backend
  happens to be this adapter instance, exactly like binding any other backend to a `ChatClientId`. This
  draft states that explicitly rather than silently dropping the `Agent` half of the issue.

## 4. The proposed adapter

New type, `WorkflowChatClient` (`include/agentengine/rt/workflow_as_chat_client.hpp`, new namespace
member of `agentengine::rt`), constructed from `std::shared_ptr<WorkflowSupervisor> inner` — the
shared_ptr-primary lesson ADR-150 §10 finding 4 already settled for the sibling adapter (a
`WorkflowSupervisor` is immovable, embeds an `AsyncMutex`, so a reference-capturing overload invites
the same dangling/reallocation traps found there). No reference overload is proposed here — unlike
ADR-150's adapter, this one must outlive arbitrarily many calls across a real multi-turn conversation,
so the "already independently guaranteed stable" escape hatch is a much weaker excuse to offer.

**Plain constructor, no refusal case — revised after red-team (§10 finding 7).** The first pass of this
draft marked the constructor `// may fail — see §5` without ever naming a real failure condition; §5
never described one either. Unlike ADR-150's sibling adapter, which refuses `request_port` graphs
outright, **this adapter's entire purpose is to support `request_port` graphs** — there is no graph
shape it needs to reject at construction time. The comment is dropped; construction is infallible.

**Stated invariant — one conversation per instance, for its whole lifetime (new after red-team, §10
finding 4).** `run_workflow()` unconditionally clears `ports_`/`pending_sub_workflows_` at its own top
(`workflow_supervisor.hpp:883-884`) on every fresh call, and §4a's own fail-closed contract-mismatch
check (below) refuses a second, unrelated fresh conversation while an earlier one on the same instance
is still paused. `WorkflowChatClient` is therefore NOT a stateless, freely-shared `ChatClient` the way
`OpenAIChatClient` is — one instance serves exactly one caller-visible conversation for as long as that
instance lives. This must be documented on the type itself, the same way `AgentSession` documents I1
("one session, one executor"), not left implicit.

```cpp
class WorkflowChatClient {
public:
    explicit WorkflowChatClient(std::shared_ptr<WorkflowSupervisor> inner);

    [[nodiscard]] ChatClientCapabilities capabilities() const;   // streaming=false, tool_calling=false — see §7 (revised)
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) const;
    // DELIBERATELY NO chat() — removed after round 9 (§10 round 9 finding 1). §4b has the full
    // rationale: this adapter can never report honest, non-fabricated Usage (WorkflowSupervisor has no
    // usage-tracking mechanism at all), and a chat()-shaped method that fails every call to avoid
    // silently fabricating a zero (round 8's own fix) turned out to fail EVERY caller, not just a
    // budget-reliant AgentSession binding — including this adapter's own headline "direct caller" use
    // case, which chat() has no way to distinguish from a budget-sensitive one (§6: the EffectContext
    // this adapter receives is unused, carrying no such signal either way). ChatClient (core/chat_client.
    // hpp:205-209) already makes chat() OPTIONAL — chat_stream() is the only required method, and the
    // concept's own comment states this relaxation exists specifically so a backend can implement
    // chat_stream() alone. Omitting chat() here is that mechanism used as designed, not a workaround:
    // it pushes ONE ChatResponseUpdate PER ContentItem in the resulting Message's `content`, IN ORDER,
    // `is_final=true` only on the LAST one (§4b), with usage left honestly `nullopt` on every push
    // (never fabricated) — a direct caller drains this stream and gets real content with no failure at
    // all; a caller that specifically needs a synchronous chat()-shaped call, or specifically needs
    // usage accounting, drains it themselves and hits the SAME honest nullopt AgentSession's own
    // pre-existing chat()-absent fallback (`agent_session.hpp:1730-1756`) already handles correctly,
    // reusing already-audited engine machinery instead of this adapter inventing a second, narrower
    // fail-closed rule that (as round 9 found) ends up blocking more than intended.
private:
    std::shared_ptr<WorkflowSupervisor> inner_;
    std::shared_ptr<std::mutex>         call_mutex_;   // ADR-150 §10 finding 1's lesson, reused — see §8
};
```

### 4a. The read-then-act algorithm — runs on `chat_stream()`'s detached worker thread, revised after
round 3 (§10 round 3 finding 3)

**Corrected after round 3**: this algorithm (previously mislabeled "`chat()` algorithm," describing
`call_mutex_` as locked "for the full call" of `chat()`) actually runs entirely on the DETACHED WORKER
THREAD `chat_stream()` spawns (§4b) — not synchronously inside whichever method the caller invoked.
Round 3 found the stale framing was not just cosmetic: if an implementer followed the old text and
acquired `call_mutex_` synchronously inside `chat_stream()` before detaching, contention on that lock
would block the CALLING thread — silently reintroducing the exact "the calling thread is never blocked
by `chat_stream()` itself" regression §4b's fix exists to close. **`call_mutex_` is acquired on the
detached worker thread, for the full body below, once that thread starts** — correctness (mutual
exclusion between two racing detached workers against the same `inner_`) is unaffected by which thread
holds it; only the caller-blocking property depends on WHERE it's acquired.

1. Lock `call_mutex_` for the full body below (on the worker thread — see above, not the caller's own
   thread).
2. Inspect `inner_->open_interactions()`. Two cases:
   - **Empty (no paused run):** this is a fresh call. Collapse `request.messages` into ONE input
     `Message` per the flattening design below (open question A, RESOLVED) and call
     `drive(inner_->run_workflow(RunWorkflow{flattened}))`.
   - **Non-empty (a paused run is waiting):** scan `request.messages` for the resume-signal content
     items described below, matching against the CURRENT `open_interactions()` set. For each match found
     (open question B, RESOLVED below — the resume loop and partial-answer handling are now fully
     specified), build a `ResumeWorkflow{interaction_id, response, routes={}}` and call
     `drive(inner_->resume_workflow(...))`, re-checking `open_interactions()` before each next call in
     the loop. If `request.messages` carries NO matching resume signal for ANY currently-open
     interaction, this is a **contract-mismatch error** (RESOLVED, was open question 4) —
     `failure_class::contract`, never silently discarding the paused run or silently starting a second
     concurrent one.

**Open question A — the flattening strategy. RESOLVED once (round 3's own resolution pass), that
resolution found UNSOUND by round 4, now RESOLVED A SECOND TIME with a different mechanism.**
`run_workflow()` takes exactly one `agentengine::Message`; `ExecutorBody`'s real runtime signature is
fixed as `Message const&` regardless of what a `Workflow`'s compile-time-only `MessageTypeId`/
`TypedExecutor<In,Out>` labels claim (round 1 finding 6) — there is no way to pass "a list of Messages"
into `run_workflow()` at all, unlike MAF's own `list[Message]`-typed start executor.

**The first resolution — concatenate every message's `content` items into one `Message`, relying on
per-item `ContentItem::origin` to preserve turn attribution instead of the outer `Message::role` — is
WRONG, per round 4 (§10 round 4 finding 1).** Traced directly against BOTH real backends' wire
translation: `OpenAIChatClient`'s `translate_message()` (`protocol/openai/chat_client.hpp:94-141`) and
`AnthropicChatClient`'s equivalent (`protocol/anthropic/chat_client.hpp:177-223`) both key an outbound
wire message SOLELY on `role_to_wire(m.role)` and concatenate every `Text` item's string into ONE blob —
**neither reads `ContentItem::origin` at all**. A flattened `Message` reaching either real backend (the
mainline case issue #35 exists for: a wrapped workflow whose own executors eventually call a real
model) produces either a garbled, unattributed text blob (the model can no longer tell its own prior
answer from the new question) or, when a `ToolResult` item ends up under the wrong outer role after
flattening, an outright WIRE-PROTOCOL-INVALID request (`tool_call_id` on a non-`tool`-role OpenAI
message; a `tool_result` block in a non-`user`-role Anthropic message). `origin` genuinely is read
elsewhere in this codebase (`context_assembly.hpp:243-284`), but for a trust/provenance check, not as a
substitute for `Message::role`-keyed turn segmentation — no code in this repo treats it that way, so the
first resolution's central citation was doing the wrong job. Because this adapter re-runs the whole
workflow from `start` on EVERY call (§2), this is not a rare edge case — it corrupts every caller-visible
turn of an ordinary multi-turn conversation the moment the wrapped workflow reaches a real backend.

**Fixed by NOT physically merging content items at all — encode the ordered message list as one opaque,
structured envelope instead, using this codebase's own existing message codec.** `rt/message_codec.hpp`
already provides `message_to_json(Message const&) -> json::Value` / `message_from_json(json::Value) ->
result<Message>` (`:393`, `:419`) — the real, existing JSON codec for `Message`, not a new format this
draft invents. **Decision**: build a JSON array by calling `message_to_json()` on every message in
`request.messages`, in order, and wrap the dumped array as ONE `Message{role::user, content=
[ContentItem{Custom{type_id="agentengine.workflow_chat_client_history", payload_json=<dumped array>},
origin=content_origin::external, tainted=<true if ANY source message carried tainted content>}]}` — a
single opaque item, never a physical merge of heterogeneous `ContentItem`s into one vector.

This closes round 4's finding structurally, not just by disclosing it harder: nothing about this envelope
type-checks as an ordinary single-turn `Message` a naive executor body could forward unmodified into a
real backend's `translate_message()` and have it "look correct" — a `start` executor wrapped by this
adapter MUST explicitly call `message_from_json()` in a loop to recover the real `list<Message>` before
doing anything with it (matching MAF's own requirement that a wrapped workflow's start executor be
*purpose-built* for this calling convention, `as_agent()`'s own doc: "otherwise initialization will fail
with a ValueError" — this is the AgentEngine-native version of that same requirement, now actually
enforced by the envelope's own opacity rather than merely documented). A `start` executor that decodes
this envelope internally reconstructs full role/origin fidelity from the real, unmangled `Message` list
and is free to build a correctly-shaped `ChatRequest.messages` for whatever real backend it eventually
calls — the corruption path round 4 found is eliminated because there is no longer a plausible "just
forward this Message as-is" naive path that produces a wire-plausible-looking but wrong result.

**The envelope has a real size ceiling, added after round 5 (§10 round 5 finding 2) — this draft's first
pass had none, contradicting this codebase's own established, fail-closed convention for exactly this
problem.** Because this adapter re-runs the whole workflow from `start` on EVERY call (§2), an unbounded
`payload_json` grows with the WHOLE conversation on every turn — including any `Media` bytes
base64-inlined by `message_to_json()`'s own `Media` handling (confirmed correct and lossless by round 5,
`rt/message_codec.hpp:230-244`/`:311-336` — the risk here is unbounded SIZE, not data loss). This is
unbounded growth with no cap, directly at odds with I8 ("budgets are enforced") and with this codebase's
own real precedent for exactly this class of problem: `tool_pipeline.hpp`'s `normalize_success()`
(`:421-461`) promotes an oversized `Data` payload to an out-of-line `Media{BlobRef}` once it crosses
`ctx.tool_result_byte_threshold`, and FAILS CLOSED (`tool.result_oversized_no_sink`, `:442-450`) — never
silently inlining an oversized result — when no `ctx.blob_sink` is configured to promote it. **Fixed by
applying the same rule to the built envelope**: before wrapping `payload_json` in the `Custom` item,
check its byte length against `tool_result_byte_threshold`/`blob_sink` read off the OWNED, thread-local
`EffectContext` copy §8 now passes into the detached worker (`ctx_copy` → `ctx` inside `run_worker()`,
per round 6's own fix — never the caller's original `EffectContext&`, for the same dangling-reference
reason §8 already establishes for `cancellation`); under threshold (or no threshold configured), inline
the `Custom` item as designed above; at or above threshold, promote to a `ContentItem{Media{BlobRef}}` via
`ctx.blob_sink` instead of `Custom`, with the SAME fail-closed refusal
(`chat_client.workflow_chat_client.history_oversized_no_sink`) when no sink is configured — never falling
back to inlining anyway. A `start` executor wrapped by this adapter must therefore be documented to accept
EITHER a `Custom{type_id="agentengine.workflow_chat_client_history"}` item OR a promoted `Media{BlobRef}`
item carrying the same encoded history, resolving the `BlobRef` through whatever store 019's blob-store
seam provides — the same two-shape contract `normalize_success()`'s own callers already live with.

**Documentation note, added after round 5 (§10 round 5 minor finding 4)**: because only the fresh-call
branch (this section) flattens, and the flattening happens on the FULL accumulated `request.messages`
history, a conversation that has already been through one suspend/resume round-trip will have this
adapter's OWN `Custom{type_id="agentengine.workflow_request_port"/"...port_response"}` bookkeeping items
(§4a's ask/resume-signal mechanism) sitting inside the history a LATER fresh call re-flattens — nested one
level inside the new outer envelope (round 5 confirmed this round-trips correctly through
`message_to_json()`/`message_from_json()` with no corruption, `message_codec.hpp:270-275`/`:372-376`,
since `Custom`'s own codec treats `type_id`/`payload_json` as opaque strings, never re-parsing). A `start`
executor decoding the envelope will see these adapter-internal items mixed into what it otherwise treats
as ordinary history; it is documented to either ignore unrecognized `Custom` items it doesn't understand
(round 4's own trace of `translate_message()` found real backends already silently skip unrecognized
`Custom` items rather than corrupting the wire request) or explicitly filter this adapter's own two
`type_id` values before forwarding history onward — a one-line documentation note, not a design change.

**Open question B — multiple simultaneously-open interactions, RESOLVED (both halves).**
- **"One message with N items, or N separate messages"**: settled by a hard type constraint, not
  preference — `ChatResponse` carries exactly ONE `Message message` (singular, `core/chat_client.hpp:103`),
  not a list the way MAF's own `AgentResponse.messages: list[Message]` is. N separate messages are not
  even expressible at this boundary. **Decision**: one `Message{role::assistant, content}` holding one
  ask-signal `ContentItem{Custom{...}}` per entry in `open_interactions()`.
- **Partial answers**: the resume loop (step 2 above) only ever matches resume-signal items against the
  CURRENT `open_interactions()` set, re-checked before each `resume_workflow()` call in the loop — so a
  caller answering M of N open interactions (M < N) is handled by construction, with no special-casing
  needed: the loop resolves the M it can match, and if `open_interactions()` is still non-empty afterward
  (the N−M unanswered ones, plus any newly-opened ones a resolved answer's own continuation produced),
  step 3 folds that into a fresh `suspended` `ChatResponse` listing the CURRENT open set — exactly the
  same shape as the original suspend. A resume-signal naming an interaction_id that is NOT in the current
  open set (e.g. a stale answer to an already-resolved interaction, replayed because `request.messages`
  carries full accumulated history) is silently skipped by the same match-against-current-set logic — not
  a special case, a natural consequence of matching against `open_interactions()` fresh each time rather
  than against some remembered "what was originally asked" set. **Confirmed by round 4** against
  `resume_workflow()`'s real body (`workflow_supervisor.hpp:892-1029`): resolving one interaction never
  mutates or invalidates any OTHER currently-open interaction's id (ordinary-port resolution only flips
  that one port's own `resolved` flag, `:1004-1006`; nested resolution only touches the ONE key being
  resumed, `:909`/`:942-946`), and `execute()` — the only path that could open brand-new interactions or
  complete the run — is reached only once EVERY currently-open interaction has been resolved in that call
  (`:975-988`/`:1010-1028`), so a genuine M-of-N partial batch can never trigger a premature `execute()`
  mid-loop regardless of processing order. This holds PROVIDED the implementation genuinely re-fetches
  `open_interactions()` fresh before every single `resume_workflow()` call in the loop, rather than
  matching once up front against a stale snapshot taken before the loop started — stated correctly above,
  but worth its own direct test (a caller answering 2-of-3, where resolving the first causes the second to
  re-suspend via nested resolution, is a good targeted case per round 4).

**Note on step 3 below**: when the resume loop (step 2) processes M > 1 resume signals in one call, only
the LAST iteration's `WorkflowResult` is what step 3 folds into the returned `ChatResponse` — every
earlier iteration in the loop necessarily returned an intermediate `suspended` result (superseded by
construction, since `execute()` cannot fire until every interaction in that call is resolved, per the
paragraph above) and is discarded, not accumulated. Clarified per round 4 finding — no logic change, the
design already implied this, it was just never stated explicitly.

**Contract-mismatch vs. silent-abandon (was open question 4), RESOLVED — fail-closed, confirmed as the
right choice, not just the safer-sounding default.** Silently treating a paused run with zero matching
answers as "abandoned" and starting a fresh run over it would (a) silently discard live interaction state
the SAME way ADR-150's own sibling adapter refused to risk for its own, simpler `ExecutorBody` embedding
(that adapter's finding 3, round 1 of ITS OWN red-team process — refusing `request_port` graphs outright
specifically to avoid exactly this class of silent loss), and (b) gives the caller no attributable signal
for why their paused interaction vanished — a direct I4 concern ("every effect is attributable"). A
`failure_class::contract` error, naming the mismatch explicitly, is strictly more debuggable and costs the
caller nothing they weren't already going to need (they must resend a matching answer either way).
**Honesty correction per round 4 (§10 round 4 minor finding 1)**: the ADR-150 analogy is real in
MECHANISM (the same `run_workflow()`-clears-`ports_`-on-every-fresh-call code path,
`workflow_supervisor.hpp:883-884`, is what both adapters guard against) but NOT equivalent in STRENGTH.
ADR-150 makes the hazard structurally IMPOSSIBLE by refusing the graph shape at construction time — never
reachable at all. This adapter's fix is a RUNTIME check, re-derived from matching resume-signal content
against `open_interactions()` on every single call for the instance's whole lifetime — a weaker guarantee
in kind, not just degree, since it depends on that matching logic being correct on every call rather than
being foreclosed once. The protection is real, just enforced differently; not claimed as equally strong.
3. Fold the resulting `WorkflowResult` into a `ChatResponse`:
   - `status == completed` → `ChatResponse{Message{role::assistant, <output's content>}, ...}`. The
     `output` `Message`'s own content/`origin`/`tainted` metadata passes through unchanged (mirrors
     ADR-150 §3's own "ROUTING NOTE" I3 discipline: nothing here re-derives authority for content the
     inner workflow already produced).
   - `status == suspended` → build ONE `Message{role::assistant, content}` where `content` holds one
     ask-signal `ContentItem` (below) per entry in `open_interactions()` (open question B, RESOLVED above
     — one message, N items, settled by `ChatResponse.message`'s own singular type, not a preference
     call). This is a **successful** `ChatResponse`, not an error — a paused workflow mid-conversation is
     an expected,
     not a failure, outcome for this adapter.
   - any other status (`bound_*`, `routing_failed`, `merge_conflict`, `invalid`) → `co_return
     std::unexpected(error{failure_class::contract, ..., "chat_client.workflow_chat_client.inner_run_not_completed." + status_tag(r.status)})`
     — same per-status error-code discipline ADR-150 §4 established for its own adapter.

**The ask/resume signal is `Custom`, NOT `ToolCall`/`ToolResult` — reversed after red-team (§10 finding
1).** The first pass of this draft encoded a suspended interaction as a `ContentItem{ToolCall{call_id=
interaction_id, tool_name="workflow_request_port", ...}}`, mirroring MAF's `function_call` envelope
literally. Round-1 red-team traced this end to end against `rt/agent_session.hpp`'s real turn loop and
found it is not merely risky — **it deterministically and silently corrupts the paused interaction the
first time this adapter is bound as an ordinary agent's `ChatClientId` backend**, one of issue #35's own
named use cases (§10 finding 1 has the full trace). `AgentSession::tool_calls_of()` extracts every
`ToolCall` item with no name filtering; an unrecognized tool name still reaches `invoke_tool()`, which
returns a normal (non-crashing) `ToolResult{is_error=true, ...}` carrying the SAME `call_id` — i.e. the
real `interaction_id` — and `AgentSession` folds that fabricated result back into history and calls
`chat()` again automatically, *inside the same `AgentSession::run()`*, with no external caller ever
seeing the ask. This adapter's own §4a step 2 then matches that fabricated `ToolResult` against the open
interaction and resumes it with garbage — the human-in-the-loop question is answered by
`AgentSession`'s own approval machinery's absence, not by anyone who was actually asked.

**Fixed by changing the wire shape, not by scoping around the bug**: the ask is
`ContentItem{Custom{type_id="agentengine.workflow_request_port", payload_json=
{"interaction_id": ..., "ask": ... /* from open_interaction_asks(), §5 */}}}`; the resume answer is
`ContentItem{Custom{type_id="agentengine.workflow_request_port_response", payload_json=
{"interaction_id": ..., "response": ...}}}`. `AgentSession::tool_calls_of()` only extracts the `ToolCall`
variant (confirmed, `core/tool_call_extraction.hpp:34-40` per §10 finding 1) — a `Custom` item is
invisible to it, so an outer `AgentSession` wrapping this adapter sees an ordinary response with an
empty tool-call set and returns it to ITS OWN caller as a normal final answer, `Custom` payload and all,
**PROVIDED the outer session has no `output_schema_validate_` armed — narrowed after round 2, §10 round
2 finding 1**: `run_rounds()`'s own "no tool calls → final" branch (`agent_session.hpp:2320-2359`)
validates `text_of(response->message)` against any schema installed via `set_output_schema()`
(`:769-776`) before accepting the round as terminal; `text_of()` extracts only `Text` items
(`tool_call_extraction.hpp:44-50`), so a `Custom`-only response validates against an EMPTY string,
which fails essentially any real schema and turns the whole outer `AgentSession::run()` into a failed
call (`"run.output_schema_validation_failed"`) instead of surfacing the ask — the opposite of "returns a
normal final answer." This is the same outcome MAF's own `_process_request_info_event` reaches for its
"specialized, already-recognized request" branch (§2) by a different route, but ONLY for an outer
session with no structured-output contract of its own. **This does not, by itself, give a
human-in-the-loop caller sitting behind an outer `AgentSession` a way to ANSWER the paused interaction**
— that caller now needs to recognize the `Custom` payload and drive a fresh, direct call against this
`WorkflowChatClient` (or some higher-level convention not designed here) to supply the response. Named
explicitly as unsolved, not silently papered over: the "sub-agent inside another agent's own tool set"
use case from issue #35's own References section, AND an outer session with `set_output_schema()`
armed, are **descoped** for the request_port case until a real answer-routing mechanism through an
intermediate `AgentSession` is designed — this adapter is proven safe only for a caller that talks to it
directly, or an outer `AgentSession` with no output-schema contract of its own.

### 4b. `chat_stream()` — rewritten after red-team round 2 (§10 round 2 finding 2)

**The first pass's "thin wrapper: drive `chat()` to completion, then push" description is not
expressible as stated, and was never real design.** Round 2 traced this codebase's own `chat_stream()`
contract directly: it must be an ordinary, non-coroutine function that returns `stream<ChatResponseUpdate>`
**synchronously** — confirmed against two real conformers, not asserted. `ReplayChatClient::chat_stream()`
(`core/replay_chat_client.hpp:169-192`)'s own file banner states `chat_stream()` "must return the drain
handle to the caller synchronously and cannot keep producing after it returns" (`:21`);
`OpenAIChatClient::chat_stream()` (`protocol/openai/chat_client.hpp`) performs its blocking HTTPS
exchange **on a detached background thread** for exactly that reason. `chat()` here returns
`task<result<ChatResponse>>` — a lazy `agentengine::rt::task<T>` that only advances when pumped (the
whole reason `drive()`'s "resume until done" loop exists, per `workflow_supervisor.hpp:1229`'s own
comment) — so "drive `chat()` inline, then push" as literally written would block the CALLING thread for
the wrapped workflow's entire run before `chat_stream()` even returns, the opposite of what every real
conformer in this codebase does, and risks a self-deadlock if the calling thread happens to be one of
`WorkflowSupervisor`'s own `pool_` workers (the same nested-worker-budget hazard class ADR-157's
`split_worker_budget()` mechanism exists to manage for nested `WorkflowSupervisor` trees — never
analyzed here because this adapter was never considered as a potential NESTED participant, only a
top-level one).

**Fixed by giving it a real worker, mirroring the two real conformers above**: `chat_stream()` spawns a
detached thread (or schedules onto an explicit worker, matching `run_stream_worker`'s own pattern in
`protocol/openai/chat_client.hpp`) that runs §4a's actual algorithm (`drive(inner_->run_workflow(...))`/
`resume_workflow(...)`, under `call_mutex_`) and, once §4a step 3 produces the resulting `Message`,
pushes ONE `ChatResponseUpdate` PER `ContentItem` in that `Message.content`, in order, `is_final=true`
only on the last — **corrected after round 5 (§10 round 5 finding 1)**: the original text here said
"pushes the resulting single `ChatResponseUpdate`," which cannot represent a `Message` with more than one
content item (the suspended case, per §4a's own resolved open question B, produces exactly one item PER
open interaction, N ≥ 1). `ChatResponseUpdate::delta` is a singular `ContentItem`
(`core/chat_client.hpp:150-152`); `ReplayChatClient::run_replay_worker`
(`core/replay_chat_client.hpp:93-114`) is the real precedent this section already claimed to mirror and
already pushes one update per chunk in a loop — the fix brings this section's own text in line with that
precedent, not a new mechanism. `chat_stream()` itself still returns the `stream<ChatResponseUpdate>`
consumer immediately, exactly like `ReplayChatClient`/`OpenAIChatClient` already do. **`chat_stream()` is
now this adapter's ONLY method — round 9 (below) removes `chat()` entirely** rather than keeping it as a
"thin wrapper," which is where earlier rounds (2 through 8) left this paragraph. `ChatClient`
(`core/chat_client.hpp:205-209`) already makes `chat()` optional, precisely for backends like this one.

**`Usage` — new, was never addressed by any prior round, added after round 6 (§10 round 6 finding 1).**
`ChatResponseUpdate::usage` (`core/chat_client.hpp:159`) defaults to `nullopt`, and its own doc comment is
explicit that `nullopt` on the terminal update means "this backend/call provided none," and "a caller that
needs usage... must treat nullopt as a hard failure, never silently as zero cost." This is not a
hypothetical caller: `AgentSession`'s real streaming-consumption path,
`detail::drain_streaming_response()` (`rt/agent_session_trust.hpp:92-136`), enforces exactly this —
`agent_session.hpp:1755-1756` reaches it directly whenever `stream_model_calls_` is `true` (a real, opt-in,
already-supported configuration for any `AgentSession` this adapter could be bound to, per §9 item 1's own
"sub-agent" composition), and its own code (`:128-134`) `co_return std::unexpected(error{...,
"run.usage_unavailable"})` if `usage` is unset on the terminal update. Leaving `usage` at its default
`nullopt` (what an implementer following the design up to this point would naturally do) would make
**every** streamed call through `WorkflowChatClient` hard-fail the entire outer `AgentSession::run()` the
moment that composition is used — not a degraded state, a total break.

**Decision, REVERSED after round 7 (§10 round 7 finding 1) — zeroing was wrong, not just undocumented.**
The first pass of this fix chose a deliberate zeroed `Usage{}` on both fields, reasoned as an honest
disclosure of "this boundary doesn't meter." Round 7 traced the REAL budget-summation code and found this
is not honest disclosure — it is exactly the silent budget-evasion the `nullopt` contract exists to
prevent, and it applies MORE broadly than the streaming path alone: `agent_session.hpp:2311-2317` sums
`run_tokens_consumed_ += response->usage.input_tokens + response->usage.output_tokens` on EVERY
`run_model_call()` round-trip — streaming or not — and fails the run once `token_budget_` (an ordinary,
independent `AgentSession` config knob, unrelated to `stream_model_calls_`) is exceeded. A zeroed
`Usage{}` makes `run_tokens_consumed_` never increase for this adapter's calls, REGARDLESS of how many
real, metered inner model calls the wrapped workflow's own `agent`-kind nodes actually made — a caller who
configures `.token_budget(N)` expecting it to bound the whole run's real cost gets a silently inert cap.
This is not a rare case; it is the default effect of the zeroing fix on any budgeted session, and I8
("budgets are enforced") names exactly this failure mode. There is also no existing usage-aggregation
mechanism to report instead: a full grep of `workflow_supervisor.hpp` for "usage"/"Usage" returns zero
matches — `WorkflowSupervisor` tracks no token usage at all today, so "report the wrapped run's real
summed usage" is real, unbuilt, nontrivial engine-level work (aggregating across however many `agent`-kind
nodes fired, recursively through nested sub-workflows), not something this draft can invent in place.

**Fixed by reverting the value, not by building the aggregation**: `ChatResponseUpdate::usage` stays
`nullopt` on the terminal update — the TECHNICALLY CORRECT signal ("unknown," not "zero") — which means a
caller with `stream_model_calls_(true)` correctly HARD-FAILS via the pre-existing `run.usage_unavailable`
refusal (round 6's original, correct discovery, never actually a defect to "fix" by faking a number). A
loud failure is strictly better than a silent budget bypass.

**Round 8 tried to fix `chat()`'s mandatory-`Usage` gap by making `chat()` itself fail closed on missing
usage — round 9 found this broke the adapter's own headline use case, and REMOVES `chat()` instead of
patching it a third time (§10 round 9 finding 1).** Round 8's own reasoning was sound as far as it went
(§9's earlier §10 entries preserve the full trace: `ChatResponse::usage` is mandatory, `AgentSession`'s
DEFAULT non-streaming branch feeds it into `run_tokens_consumed_` unconditionally, so leaving it
undecided would silently reproduce round 6's defeated-budget defect) — but the FIX landed in the wrong
place. A `chat()` that fails every call because `usage` is always `nullopt` has no way to tell an
`AgentSession` binding (which needs the budget guarantee) apart from a caller talking to this adapter
DIRECTLY (§1/§9's own headline use case, which never asked for or needed usage accounting at all) —
`chat()`'s signature carries no such signal, and §6 already establishes the `EffectContext&` it receives
is unused. The result: `chat()` as round 8 specified it fails EVERY caller unconditionally, including
§11's own planned example (`examples/28_workflow_as_chat_client.cpp`, "driven through two `chat()`
calls") — the fix broke the exact "safe for a direct caller" property every round since round 1 had
assumed.

**Fixed structurally, not with a third runtime rule**: `chat()` is removed from this type entirely (§4's
class declaration). `ChatClient` (`core/chat_client.hpp:205-209`) already makes `chat()` optional — its
own comment states the relaxation exists specifically so a backend can conform via `chat_stream()` alone.
This isn't a workaround; it's that exact, already-provided mechanism. The consequences, worked through:
- **A direct caller** (this adapter's own headline use case) drains `chat_stream()` and gets real
  `ContentItem`s with an honestly-`nullopt` usage on the terminal push — no failure, ever, for a caller
  who never asked for budget accounting. This restores the property round 8 accidentally broke.
- **An `AgentSession` binding** reaches this codebase's OWN pre-existing, already-audited fallback:
  `run_model_call()` (`agent_session.hpp:1730-1756`) already checks `if constexpr` whether the bound
  `ChatClientT` has a `chat()` method at all; when it doesn't, it falls through UNCONDITIONALLY (regardless
  of `stream_model_calls_`) to `detail::drain_streaming_response(chat_client_->chat_stream(...), ...)`,
  which already fails closed on missing usage (`agent_session_trust.hpp:128-134`,
  `"run.usage_unavailable"`) — the exact same protection round 8 tried to build bespoke, now reached
  through machinery this codebase already has, tested, and trusts, with no new adapter-side error code
  needed.
- The `ChatResponse::usage`-is-mandatory tension that drove rounds 6, 7, and 8 disappears structurally —
  there is no `ChatResponse` this type ever constructs, so there is no mandatory field left to
  misrepresent by omission, zero, or a third patched-on runtime rule.

**This is therefore named as a THIRD explicitly descoped composition, extending §9 item 1's existing
two, corrected after round 9 to describe WHERE the block comes from**: an outer `AgentSession` still
cannot successfully complete a call bound to this adapter — the total-block OUTCOME round 8 wanted is
unchanged — but the block now comes from `AgentSession`'s own pre-existing `chat()`-absent fallback
correctly refusing genuinely-unknown usage, not from a bespoke rule inside this adapter that (as round 9
found) reached further than intended. This remains real, unbuilt, prerequisite follow-on work — until
`WorkflowSupervisor` exposes real, aggregate `Usage` tracking (zero mentions of "usage" in
`workflow_supervisor.hpp`, unchanged since round 7's own citation) — but it is no longer a claim this
adapter's own `chat()` needs to enforce, because this adapter no longer has one.

**The nested-worker-budget question this surfaced, worked through (revised before round 3, SIMPLIFIED by
round 9 removing `chat()`)**: once `chat_stream()` is fixed to spawn its own detached thread and return
immediately, the CALLING thread is never blocked by `chat_stream()` itself — so if the caller of
`chat_stream()` happens to be one of another `WorkflowSupervisor`'s own `pool_` workers, that worker
returns to the pool right away, exactly as it would after dispatching any other non-blocking call. This
eliminates the specific self-deadlock shape round 2 flagged for `chat_stream()`. Rounds 3-8 carried an
open question here about whether a caller driving `chat()` directly (blocking its own thread for the
wrapped workflow's entire run, the same cost any long-running `function`-kind `ExecutorBody` already
imposes on its own pool per ADR-150 §6's own precedent) could starve a pool worker's budget under some
`AgentSession` call pattern — **moot after round 9**: `chat()` no longer exists on this type, so there is
no direct-call path left to trace or worry about; every caller, `AgentSession` included, reaches this
adapter exclusively through `chat_stream()`'s own non-blocking, detached-worker design. §9 item 5 is
resolved by construction, not by finishing the trace rounds 3-8 left open.

## 5. The `open_interactions()` gap this adapter depends on (real, upstream, scoped after red-team)

To fill in a `Custom` ask payload (§4a) that actually tells the caller WHAT is being asked — the
`WorkflowChatClient`-side equivalent of MAF's `RequestInfoFunctionArgs.request_event` — the caller
needs the request_port node's own ask `Message`, not just a bare `interaction_id`. `open_interactions()`
does not expose it today (§3).

**Descoped to `OpenPort` only — revised after red-team (§10 finding 2).** The first pass of this draft
proposed one accessor covering every open interaction uniformly and called it "genuinely additive." Round
1 traced the `OpenPort` half and confirmed it directly (`workflow_supervisor.hpp:1748`: `d.payload`, the
ask, sits in `OpenPort::response` pre-resolution exactly as claimed, and `open_interactions()`, `:810-820`,
strips it). But the `PendingSubWorkflow` half (ADR-157's nested sub-workflow mechanism) is NOT a simple
superset: `PendingSubWorkflow` (`:1141-1145`) carries only `{interaction, executor_index,
inner_interaction_id}` — no ask `Message` at all. The real ask lives inside the wrapped nested
`WorkflowSupervisor` (`sub_workflows_[executor_index]`, private, `:2106`), reachable only by recursively
querying that inner instance for the entry matching `inner_interaction_id` — which may itself be another
`PendingSubWorkflow` several levels deep, bounded only by `kMaxNestingDepth` (`:667`). A one-shot,
provably-terminating recursive walk across an arbitrary nesting depth is real, separate design work this
draft does not attempt.

**Fixed by narrowing the claim, not by building the recursion**: this draft now proposes the accessor
ONLY for `OpenPort` entries; a `PendingSubWorkflow` entry gets the already-named `ask = Message{}`
placeholder (an empty ask, honestly disclosed, not a silently wrong one) until the recursive case is
designed as its own follow-on. This is still additive to `workflow_supervisor.hpp` — `open_interactions()`
itself is unchanged, every existing caller (ADR-157's nested work, ADR-159's cancellation work) keeps
compiling — just narrower in what it actually resolves than the first pass claimed:

```cpp
struct InteractionAsk {
    agentengine::Interaction interaction;
    agentengine::Message     ask;   // OpenPort's own d.payload; Message{} placeholder for a
                                     // PendingSubWorkflow entry until the nested case is designed
};
[[nodiscard]] std::vector<InteractionAsk> open_interaction_asks() const;   // new, alongside open_interactions()
```

## 6. Capability sourcing (I2)

No MAF reference answer exists (§2). This draft proposes the SAME answer ADR-150 §5 already settled
for the sibling adapter, for the same reason: the outer `EffectContext&` `chat_stream()` receives is
**unused** — the wrapped workflow's own executors run under whatever `EffectContext`s the
caller already supplied to `inner->initialize(..., contexts, ...)`, entirely decoupled from whatever
capabilities the outer caller (an `AgentSession`, a direct caller, or an outer orchestration) happens
to hold. Consistent with `check_workflow_executable()` never engine-enforcing `capability_ceiling` for
anything but `agent`-kind nodes today (ADR-150 §5's own citation) — this is not a new hole, it is the
same pre-existing shape restated for a new embedding.

**Confirmed by red-team round 1 (§10 finding 8)**: the citation (`workflow/graph.hpp:529-548`) was
independently re-derived and transfers cleanly — `sub_workflow`- and `request_port`-kind executors both
pass `check_workflow_executable()` with zero engine-side capability enforcement, exactly like
`function`-kind nodes. No change from the first pass.

## 7. `capabilities()` — resolved after red-team (§10 finding 1)

The first pass of this draft flagged, but did not trace, what `AgentSession`'s own turn loop does with
a `ToolCall` naming no declared tool, and left `tool_calling = true` as the "honest" capability
declaration pending that trace. Round 1 traced it (§4a's rewritten "ask/resume signal" paragraph has the
full call chain) and found the real behavior is worse than any outcome this draft had speculated about:
a synthetic `ToolCall` is silently picked up by `AgentSession::tool_calls_of()`, routed through
`invoke_tool()`'s unknown-tool path, and fed back as a fabricated `ToolResult` carrying the real
`interaction_id` — which this adapter's own resume logic would then treat as a genuine answer, silently
corrupting the paused interaction.

Fixed by removing the cause, not the symptom: §4a no longer encodes the ask/resume signal as
`ToolCall`/`ToolResult` at all — it uses a dedicated `Custom` content kind, which
`AgentSession::tool_calls_of()` never extracts (confirmed, `core/tool_call_extraction.hpp:34-40`). With
that change, this adapter never emits a genuine `ToolCall`, so `capabilities().tool_calling` is now
`false` — an honest declaration, not a scope-driven downgrade. `capabilities().streaming` stays `false`
for the same reason as the first pass (no real incremental streaming exists). Both bits were traced
against their real readers (`select_output_schema_strategy()`, `core/chat_client.hpp:298-303`, and
`detect_undeclared_tool_call_leak()`, `core/response_format_leak_scan.hpp:111-135`) and found currently
inert either way — no other behavior in this codebase reacts to these two bits for this adapter's shape
today, so the correction has no other downstream consequence to re-check.

## 8. Concurrency, lifetime, and cancellation

Same shape ADR-150 §6 already proved necessary for the sibling adapter, for the identical underlying
reason: `drive()`'s hand-rolled "resume until done" loop has no idea what `WorkflowSupervisor`'s
`AsyncMutex`-based `run_mutex_` is actually suspended on, so a second concurrent call against the same
`inner_` risks resuming another call's already-parked coroutine handle. `call_mutex_` (an adapter-owned
`std::mutex`, independent of `run_mutex_`) is held for the ENTIRE read-then-act body — not just the
`drive()` call — because this adapter's own correctness now depends on a read-then-act sequence
(`open_interactions()` snapshot, then `run_workflow()` or `resume_workflow()` against that snapshot)
that a second concurrent caller must not be allowed to interleave with. **Corrected after round 3 (§10
round 3 finding 3): acquired on `chat_stream()`'s detached WORKER thread (§4a/§4b), never synchronously
inside `chat_stream()` itself before detaching** — the latter would block the calling thread on
contention, reintroducing the exact caller-blocking regression §4b's fix exists to close; correctness
(mutual exclusion between two racing workers against the same `inner_`) holds regardless of which
thread acquires it, only the caller-blocking property depends on where. `inner_` is captured by
`shared_ptr` (§4), so no separate lifetime contract is needed — construction never fails on lifetime
grounds the way ADR-150's reference overload could.

**New gap, found by round 2, unaddressed by any pass of this draft until now (§10 round 2 finding 3):
`EffectContext`'s deadline/cancellation are structurally unreachable.** `run_workflow()`/
`resume_workflow()` (`workflow_supervisor.hpp:870`, `:892`) take no `EffectContext` at all — there is no
parameter through which `chat_stream()`'s own `EffectContext&` could reach them, regardless of
what this adapter's own code does. `EffectContext::deadline`/`::cancellation` are real, actively
enforced elsewhere in this codebase — `model_call_gateway.hpp:376-382`/`:544-549` both bail out once
`now >= ctx.deadline`, `tool_pipeline.hpp:667-668` refuses a call past deadline — so a caller that binds
a bounded `EffectContext` to this adapter (the ordinary case for a sub-agent call, or any caller trying
to cancel a hung run) gets no way to interrupt a hung or looping wrapped workflow through the
`ChatClient` contract at all; both fields default to unset, so this only bites a caller that actually
sets them, which is the common case for anything but an unbounded top-level call.
`WorkflowSupervisor` already has its own real, independent cancellation mechanism (ADR-159's
`cancel_source_`/`cancellation_token()`/`cancel()`, e.g. `:780`, `:1364`, `:1476`) — §6's own framing of
`EffectContext&` as merely "unused" was scoped only to I2 capability sourcing and never considered
deadline/cancellation as a separate concern at all.

**Designed, not just flagged (revised before round 3; the snippet itself corrected AFTER round 3 — see
below)** — grounded directly against `cancel()`'s own comment (`:766-776`): `void cancel() noexcept {
cancel_source_.request_stop(); }` is EXPLICITLY documented as "callable from ANY thread while
`run_workflow()`/`resume_workflow()`/`continue_workflow()` is in flight on another," requires no lock,
and never touches `state_`/`ports_`/`rounds_` — exactly the shape a `std::stop_callback` invoked from an
arbitrary thread needs. `EffectContext::cancellation` (`effect_context.hpp:167`) is confirmed to be a
plain `std::stop_token` — its own file comment even already documents the SAME cancel_source_/stop_token
pairing for the opposite direction of composition (populated FROM a `WorkflowSupervisor`'s own
`cancel_source_` INTO the `EffectContext` each dispatched executor body receives, `:1476`) — so bridging
the caller's OWN cancellation, the reverse direction, into `inner_->cancel()` is the same mechanism this
codebase already trusts, not a new primitive.

**Corrected after round 3 (§10 round 3 finding 1): the ORIGINAL snippet read `ctx.cancellation` lazily
from inside the detached worker thread — a real dangling-reference hazard.** `chat_stream()` (§4b) returns
to its caller immediately, before the worker even starts; nothing in the `ChatClient` contract obligates
a caller to keep the `EffectContext&` it passed alive past that point. Round 3 confirmed neither real
conformer this draft's §4b claims to mirror does this: `OpenAIChatClient::chat_stream()`
(`protocol/openai/chat_client.hpp:1019-1040`) resolves everything it needs from `ctx` SYNCHRONOUSLY,
before spawning the thread.

**Widened after round 6 (§10 round 6 finding 2): a two-field pick-list (`cancellation`/`deadline`) already
missed two MORE fields the envelope size-check (below) needs (`tool_result_byte_threshold`/`blob_sink`),
reintroducing round 3's own already-fixed dangling-reference hazard for those two fields specifically.
Rather than extend a hand-picked field list a THIRD time the next time some new logic needs one more field
off `ctx`, this now copies the WHOLE `EffectContext` by value before detaching — matching
`tool_pipeline.hpp::background_task()`'s own real, established precedent for exactly this class of
problem** (`tool_pipeline.hpp:701-707`/`:726-727`: takes `EffectContext ctx` BY VALUE before ever
detaching its own `std::thread`, precisely because of an earlier ADR-060 red-team finding about this same
hazard class; `normalize_success()` — the envelope size-check's own cited precedent, below — reads
`ctx.tool_result_byte_threshold`/`ctx.blob_sink` off exactly that owned, thread-local copy, never the
caller's original reference, `:430-461`).

**Corrected AGAIN after round 7 (§10 round 7 finding 2): a bare whole-struct copy is not what
`background_task()` actually does, and copying the struct alone does nothing for its raw, non-owning
pointer fields.** `EffectContext` (`core/effect_context.hpp`, read in full for this correction) carries
several fields explicitly documented "borrowed, never owned here" — `bound_capabilities`
(`vector<BoundCapability> const*`, `:53`) and `sandbox_fs` (`FileSystemAdapter*`, `:211`) — where copying
the enclosing struct copies the POINTER's bit pattern, not what it points at; and three `std::function`
fields explicitly call-scoped, bracketed open-then-reset around each real call site elsewhere in this
codebase — `report_progress` (`:115`), `agent_turn_sink` (`:140`), `moderator_delta_sink` (`:145`).
`background_task()`'s REAL pattern (not just its by-value parameter) is copy-THEN-SANITIZE, not
copy-and-trust: it explicitly resets `ctx.report_progress = [](ContentItem){};` (`:775`, citing ADR-060 §4
by name for exactly this dangling-into-a-detached-thread hazard class) and `ctx.sandbox_fs = nullptr;`
(`:784`, "same hazard class... same fix"), and re-points `ctx.bound_capabilities` at a vector the WORKER
ITSELF owns (`:824`, `bound` moved into the worker's own closure at `:821`) — never trusting the caller's
original pointer on the detached thread at all. This is a LIVE hazard for this adapter specifically, not
theoretical: a `Tool<>::invoke()` body is free to hold a `WorkflowChatClient` and call `chat_stream()`
(nothing in this codebase prevents it, and every round since round 2 has insisted `chat_stream()` must
return without blocking its caller) — `invoke_tool()`'s own real bracket (`tool_pipeline.hpp:674-676`)
sets `ctx.bound_capabilities = &bound` around one synchronous call and resets it to `nullptr` immediately
after, with `bound` itself a stack-local destroyed on return; a bare `ctx_copy = ctx` would carry that
about-to-dangle pointer's bit pattern onto the detached worker, live past the stack frame that owned it.
**Fixed**: the copy is sanitized field-by-field, on the SAME local copy, before detaching — closely
mirroring `background_task()`'s real pattern, ONE deliberate deviation noted below (round 8's own finding
3):

```cpp
// Inside chat_stream(), BEFORE spawning the worker — mirrors OpenAIChatClient::chat_stream()'s own
// "resolve everything needed from ctx synchronously" discipline (protocol/openai/chat_client.hpp:
// 1019-1040), widened per round 6 to a whole-context copy, corrected per round 7 to sanitize it like
// tool_pipeline.hpp::background_task() (:775, :784, :824) actually does — not a bare copy.
// ctx itself (the caller's reference) is NEVER captured or read from the detached thread.
agentengine::EffectContext ctx_copy = ctx;
ctx_copy.bound_capabilities  = nullptr;                 // borrowed, never owned (effect_context.hpp:53) —
                                                         // matches §6's own "zero implicit capability
                                                         // flow across this boundary" design already
ctx_copy.capabilities        = nullptr;                 // DEVIATES from background_task() (round 8
                                                         // finding 3) — that precedent deliberately KEEPS
                                                         // this field (a refcounted shared_ptr, safe to
                                                         // copy per ADR-061 §20.3, tool_pipeline.hpp:
                                                         // 709-712). Nulled here out of EXTRA caution
                                                         // beyond what the precedent itself requires,
                                                         // never incorrect (nulling a shared_ptr copy is
                                                         // never UB) — since §6/§8 already establish this
                                                         // adapter never reads ctx.capabilities at all
                                                         // (run_workflow()/resume_workflow() take no
                                                         // EffectContext parameter), there is nothing
                                                         // this null could break.
ctx_copy.sandbox_fs          = nullptr;                 // borrowed, never owned (effect_context.hpp:211)
ctx_copy.report_progress     = [](ContentItem) {};      // call-scoped reverse channel (ADR-060 §4)
ctx_copy.agent_turn_sink     = [](RunEvent const&) {};  // call-scoped reverse channel (ADR-152)
ctx_copy.moderator_delta_sink = [](std::string const&) {};  // call-scoped reverse channel
// cancellation/deadline/tool_result_byte_threshold/blob_sink are KEPT — real value types or a
// worktree-scoped callback this adapter's own design genuinely needs on the worker thread (see below).
// principal/trace_id/span_id/run_id/turn_index/codeact_preseeded_answers are also carried across
// UNCHANGED — confirmed by round 8 (full field-by-field audit of effect_context.hpp) to be plain value
// types with no aliasing/call-scoped character (Principal is three strings/an enum/a uint32_t, the rest
// are std::string/uint64_t/vector<string> — none borrow anything past this call), named explicitly here
// so this list is a complete accounting of EVERY field on EffectContext, not an implicit "everything
// else is presumably fine" the way earlier rounds' narrower lists silently were.
std::thread(&workflow_chat_client_detail::run_worker, inner_, call_mutex_, std::move(ctx_copy),
            std::move(pair.producer))
    .detach();
```

```cpp
// run_worker(), on the detached thread — call_mutex_ acquired HERE (§4a's own correction), not
// synchronously inside chat_stream() before detaching. `ctx` here is the SANITIZED owned copy passed in
// above — every borrowed pointer/call-scoped callback is already nulled/no-op, so nothing this function
// reads off `ctx` can dangle, regardless of what the caller's own frame does after chat_stream() returns.
std::lock_guard<std::mutex> guard(*call_mutex);
// registering a stop_callback on an ALREADY-stopped token invokes the callback immediately and
// synchronously (std::stop_callback's own standard guarantee), so a caller whose EffectContext was
// already cancelled before this call even started correctly cancels the inner run at the first
// opportunity, not just for a stop requested mid-flight. No lock-ordering hazard against call_mutex_
// itself: cancel()'s own body (cancel_source_.request_stop()) takes no lock (round 3 re-confirmed).
std::stop_callback bridge(ctx.cancellation, [inner] { inner->cancel(); });
WorkflowResult r = drive(inner->run_workflow(...));   // or resume_workflow(...) — §4a's algorithm,
                                                       // including the envelope size-check against
                                                       // ctx.tool_result_byte_threshold/ctx.blob_sink
```

A cancelled run surfaces through the ordinary `workflow_status::cancelled` status (`:298-304`, ADR-159) —
already one of the "any other status" cases §4a step 3 folds into a `failure_class::contract` error via
the existing per-status error-code discipline (`chat_client.workflow_chat_client.inner_run_not_completed.cancelled`);
no new status-handling branch is needed, only the bridge itself.

**`ctx.deadline` gets a pre-call check only, deliberately not a live mid-run poll**: before each
`run_workflow()`/`resume_workflow()` call, if `deadline` (the copy extracted above) is set (non-default)
and already past, fail closed immediately, mirroring `model_call_gateway.hpp:376-382`/`:544-549`'s own
"bail out once `now >= ctx.deadline`" pattern — the SAME live wall-clock read that layer already performs
for exactly this purpose, so no new precedent is set. **Corrected after round 3 (§10 round 3 finding 2):
the first pass of this design invented a `failure_class::deadline_exceeded` that does not exist** — the
real enum (`core/error.hpp:12-18`) is `{transient, policy, contract, resource, fatal}`, and even the
cited precedent (`model_call_gateway.hpp`) never synthesizes a deadline-specific class — on
`now >= ctx.deadline` it stops retrying and re-returns the underlying attempt's own error via
`drained_failure_to_agent_error(...)`. **Fixed**: this adapter's own pre-call deadline check uses
`failure_class::resource` ("budget, quota, or limit exceeded" — the closest real fit) with a real code
string (`chat_client.workflow_chat_client.deadline_exceeded`), not an invented enumerator.

**Deliberately NOT wired as a mid-run enforcement mechanism** (e.g. a timer thread that calls
`inner_->cancel()` once the deadline passes) in this pass: `core/routing_model_call_gateway.hpp` excludes
`deadline` from `SelectFn`'s own visible `EffectContext` specifically because "a live read of it is not
reproducible across a replay boundary" (`effect_context.hpp:156-163`'s own comment, cited directly) — an
I5 concern this draft takes seriously rather than casually adding a second live-clock consumer inside a
composition (this adapter, wrapping a `WorkflowSupervisor`) that ADR-157/ADR-159's own nested-run
machinery already has enough clock-sensitive moving parts in. A pre-call check is a single, one-shot read
at a well-defined boundary (matching `model_call_gateway`'s own precedent exactly); a mid-run poll would
be new, ongoing, per-round wall-clock consumption this draft is not prepared to sign off on without its
own dedicated I5 analysis. Named explicitly as a deliberate scope boundary, not an oversight — a mid-run
deadline poll is real, separate future work if ever needed.

The wrapped workflow's own internal bounds (`max_rounds`/`deadline_ms`, workflow-author-set at
construction) remain a different, orthogonal knob from the CALLER's own `EffectContext.deadline` and are
not conflated by either mechanism above — the caller's deadline governs whether THIS `chat_stream()` call
is worth starting/continuing at all; the workflow's own bounds govern how far a single accepted run is
allowed to go internally, exactly as they already do with no caller `EffectContext` involved.

**Recording and replay (I4/I5) — was §9 item 7, RESOLVED for the general claim, NARROWED after round 9's
removal of `chat()` (§10 round 9 finding 1).** `ChatCallRecording`/`ReplayChatClient` (004 §6) capture
whatever ONE `ChatClient` conformer's `chat_stream()` call actually returned at that conformer's own
boundary — the mechanism is agnostic to how much internal work produced that return value;
`ReplayChatClient` itself (`core/replay_chat_client.hpp`) plays back a recorded chunk sequence with no
knowledge of, or dependency on, what the ORIGINAL backend did internally to produce it. `WorkflowChatClient
::chat_stream()` (§4a/§4b) runs the WHOLE wrapped-workflow call — `run_workflow()`/`resume_workflow()`,
any live model/tool calls the wrapped workflow's own executor bodies make — entirely SYNCHRONOUSLY WITHIN
one adapter call, by construction (the read-then-act algorithm never suspends the outer call across
multiple `chat_stream()` invocations to represent one logical wrapped-workflow step). So recording
`WorkflowChatClient` as "the backend" a caller is bound to is, IN PRINCIPLE, architecturally identical to
recording `OpenAIChatClient` as that backend — the recording captures "what this backend returned for
this request," regardless of internal complexity, the same guarantee every other composed/multi-step
`ChatClient` conformer in this codebase already relies on. **The one real, orthogonal residual, explicitly
not a defect**: if the WRAPPED workflow's OWN executor bodies want independent recording/replay of THEIR
OWN inner model calls (e.g. an `agent`-kind node's own bound `ChatClient`), that is a wholly separate
recording concern belonging to whatever `ChatClient` those inner bodies are bound to — unaffected by, and
requiring no coordination with, whether the OUTER `WorkflowChatClient` itself is being recorded. Whether
`WorkflowSupervisor`'s own execution is otherwise deterministically reproducible (e.g. `mint_interaction()`'s
naming, confirmed deterministic — `run_id_ + ":port:" + executor_id + ":" + round`, no randomness,
`workflow_supervisor.hpp:1845-1852`) is a pre-existing property of that class, unrelated to and unchanged
by wrapping it in a `ChatClient` adapter.

**Real, disclosed narrowing introduced by round 9, not present in round 4's original confirmation**:
round 4 confirmed `RecordingChatClient<WorkflowChatClient>`'s own `static_assert` passes because
`RecordingChatClient<Inner>` (`core/recording_chat_client.hpp:140-266`) gates on `LegacyChatClient`
(`capabilities()`+`chat()`+`chat_stream()` with exact signatures, `core/chat_client.hpp:228-234`), which
`WorkflowChatClient` satisfied AT THE TIME because it still had a `chat()` method. **Round 9 removed
`chat()` — `WorkflowChatClient` now satisfies plain `ChatClient` (`chat_stream()`-only) but NOT
`LegacyChatClient`, so `RecordingChatClient<WorkflowChatClient>` no longer compiles at all.** The general
architectural claim above (recording is agnostic to internal complexity) still holds in principle, but the
ONE EXISTING mechanism this codebase has for exercising it (`RecordingChatClient`) cannot be instantiated
over this adapter until either `RecordingChatClient` itself is widened to accept a plain-`ChatClient`
`Inner` (real, separate, unbuilt follow-on work, unrelated to this adapter's own design), or a
narrower, `chat_stream()`-only recording wrapper is built. Named here explicitly as a real, disclosed
consequence of removing `chat()` — this draft trades a working `chat()` path that broke the "direct
caller" composition (round 9's own finding) for a `chat_stream()`-only design that is safe for every
composition but currently unrecordable through the one existing mechanism this codebase has for it.

**Explicitly rejected, per round 10: adding back a stub `chat()` purely to satisfy `LegacyChatClient`'s
type-level requirement.** This is not just "probably worse" — it is mechanically provable to reintroduce
round 8's exact, already-rejected defect. `AgentSession::run_model_call()`'s dispatch on whether to call
`chat()` is a pure TYPE-LEVEL `if constexpr (requires(...) { c.chat(r,e) ... })`
(`agent_session.hpp:1730-1733`) — it never inspects what `chat()` actually does. Re-adding any `chat()`,
however it's stubbed (throws, returns a sentinel error, whatever), flips that check back to `true`, and
`AgentSession` unconditionally PREFERS calling it (`:1735`) over the safe `chat_stream()` fallback this
whole round-9 fix relies on — silently undoing the fix by construction, not by anyone's mistake. `chat()`
staying absent is load-bearing, not merely tidy.

## 9. Design decisions and accepted scope boundaries (all prior open questions resolved; round 4 partially
validated — one MUST-FIX found and fixed, see §10 round 4)

Rounds 1, 2, and 3's own punch lists are folded into §10 below. **Every design question this draft raised
across three rounds has now been resolved** (open questions A and B, the contract-mismatch choice, and
the I4/I5 recording/replay question — all resolved in-place in §4a/§8 above, in the pass that produced
this revision). What remains is not open design work — it is either a deliberate, accepted scope
boundary, or plain implementation/testing follow-through:

1. **Deliberate scope boundary, not a defect — THREE descoped compositions, (c) STRENGTHENED after round
   8.** This adapter is proven safe only for a caller talking to it directly, OR an outer `AgentSession`
   with no `set_output_schema()` armed of its own (§4a/§4b, §10 round 1 finding 1, round 2 finding 1):
   (a) "sub-agent inside another agent's own tool set" (issue #35's own References section) — no designed
   answer-routing mechanism for the request_port case; (b) an outer session with a structured-output
   contract of its own — a `Custom`-only suspended response fails that session's own schema validation;
   (c) **binding this adapter as ANY `AgentSession`'s `ChatClientId` backend at all — the OUTCOME
   round 8 found is unchanged (still an unconditional block), but round 9 corrected WHERE it comes from,
   after round 8's own fix (making `chat()` itself fail closed) broke the direct-caller composition too.**
   Round 8 traced `AgentSession::run_model_call()`'s DEFAULT, non-streaming branch
   (`stream_model_calls_ == false`, `agent_session.hpp:2731`) and found it feeds `ChatResponse::usage`
   into `run_tokens_consumed_ += ...` (`:2311`) UNCONDITIONALLY — round 7's revert had only fixed the
   streaming/opt-in half. Round 8 fixed the gap by making `chat()` itself fail closed on missing usage —
   but round 9 found `chat()`'s signature has no way to tell an `AgentSession` binding (which needs the
   usage guarantee) apart from a caller talking to this adapter DIRECTLY (which never needed it), so a
   fail-closed `chat()` failed EVERY caller, including the "direct caller" use case §1/§11 treat as this
   adapter's headline scenario. **Fixed by removing `chat()` from this type entirely (§4/§4b)** — `chat_
   stream()` is this adapter's only method (`ChatClient` already makes `chat()` optional for exactly this
   reason). An `AgentSession` binding still cannot complete a call: `run_model_call()`'s own pre-existing,
   already-audited `chat()`-absent fallback (`agent_session.hpp:1730-1756`) unconditionally drains
   `chat_stream()` and fails closed on missing usage via `drain_streaming_response()`
   (`agent_session_trust.hpp:128-134`) — the SAME block, reached through engine machinery this codebase
   already trusts rather than a bespoke adapter-side rule. A DIRECT caller, meanwhile, drains
   `chat_stream()` itself and hits NO failure at all — the property every round since round 1 assumed,
   restored rather than left broken. All three descoped compositions are real, unbuilt follow-on work,
   deliberately out of THIS draft's scope, not defects in what it proposes — (c) still means this draft,
   as scoped, only delivers the "direct caller" half of issue #35's own use cases; the "reusable inside
   another agent's own composition" half needs the usage-aggregation prerequisite built first — but that
   direct-caller half is now actually SAFE, which it briefly was not between rounds 8 and 9.

   **Post-implementation update (ADR-163, not a further red-team round — this draft's own scope ended
   at round 10; see that ADR for its own verification discipline): the "usage-aggregation prerequisite"
   this bullet named as real, unbuilt, follow-on work has since been built.** `WorkflowSupervisor` now
   tracks real, cumulative `Usage` (`usage()`, sourced from `agent`-kind dispatches and resolved nested
   `sub_workflow`s). `AgentSession::run_model_call()`'s `chat()`-absent fallback no longer fails
   unconditionally for composition (c): a wrapped graph containing a real `agent`-kind node now reports
   its honest, non-fabricated cost and the outer `AgentSession` completes the call. The residual is
   narrower, not gone — a graph with ONLY `function`-kind nodes still fails closed (genuinely zero
   tracked cost is indistinguishable, by design, from "nothing reports usage at all"), and this bullet's
   compositions (a) and (b) are entirely untouched by ADR-163. Full detail: `decisions/ADR-163-
   workflow-supervisor-usage-tracking.md`.
2. **Open question A (message-flattening strategy) — RESOLVED in §4a, TWICE.** The first resolution
   (physically concatenate `request.messages`' content items, relying on per-item `origin` for turn
   attribution) was found UNSOUND by round 4 (§10 round 4 finding 1) — neither real backend
   (`OpenAIChatClient`/`AnthropicChatClient`) reads `ContentItem::origin` at all, so a flattened `Message`
   reaching either one produces a garbled or wire-invalid request. **Fixed**: the message list is now
   encoded as one opaque `Custom` envelope built from this codebase's own existing `message_to_json()`
   codec (`rt/message_codec.hpp:393`), which a wrapped `start` executor must explicitly decode via
   `message_from_json()` — no naive "forward the Message as-is" path exists that could silently corrupt
   real backend wire calls. Round 1's own finding (no runtime shape-check exists to catch a wrong design
   here, `workflow/graph.hpp:58-76`) still means the test named in §11 is the thing that ultimately proves
   this design correct in practice.
3. **Open question B (multiple simultaneously-open interactions) — RESOLVED in §4a, confirmed by round 4
   against the real `resume_workflow()` body.** One `Message`, N ask-signal items — settled by
   `ChatResponse.message`'s own singular type, not a preference call. Partial-answer handling was traced
   against `resume_workflow()`'s real code (`workflow_supervisor.hpp:892-1029`) and confirmed correct:
   resolving one interaction never mutates any other's id, and `execute()` cannot fire mid-loop until
   every interaction in that call is resolved — provided the implementation genuinely re-fetches
   `open_interactions()` fresh before each resume call (a specific, targeted test case is named in §4a).
   Partial-answer handling (a caller answering M of N) falls out of matching resume signals against the
   CURRENT `open_interactions()` set on each loop iteration, with no special-casing needed.
4. **Contract-mismatch vs. silent-abandon — RESOLVED in §4a.** Fail-closed, `failure_class::contract`,
   for the same "silent state loss" reason ADR-150's sibling adapter refused `request_port` graphs
   outright rather than risk it, plus a direct I4 (attributability) argument.
5. **`chat_stream()`'s worker-budget interaction — RESOLVED by round 3, MOOTED by round 9 (§10 round 9
   finding 1) — updated after round 10 found this bullet still described the pre-round-9 state.** Round 3
   traced `AgentSession::run_model_call()` calling `chat_client_->chat(request, ctx)` directly when the
   bound backend had one, and confirmed `AgentSession` genuinely runs as a `WorkflowSupervisor` pool worker
   via `agent_session_as_executor_body()` → `pool_.submit(run_executor_job(...))`
   (`rt/agent_workflow_executor.hpp:89-123`, `workflow_supervisor.hpp:1477-1479`). **This call path is now
   structurally unreachable for this adapter**: round 9 removed `chat()` entirely, so
   `run_model_call()`'s own `if constexpr` on `chat()`'s existence (`agent_session.hpp:1730-1733`) is
   always false for `WorkflowChatClient`, and execution falls straight through to draining `chat_stream()`
   (§4b) — matching what §4b's own text already says ("moot after round 9: `chat()` no longer exists on
   this type, so there is no direct-call path left to trace or worry about"). See item 8 for the
   worker-budget-ACCOUNTING residual (unrelated to `chat()`'s removal) this earlier trace surfaced.
6. **`EffectContext` deadline/cancellation bridging — designed, then corrected, by round 3 (§8, §10 round
   3 findings 1 and 2).** A `std::stop_callback` bridging `cancellation` to `inner_->cancel()`, with
   `cancellation`/`deadline` extracted synchronously before `chat_stream()` detaches (fixing round 3's
   dangling-reference finding), and a pre-call-only `deadline` check using the real `failure_class::resource`
   (fixing round 3's invented-enumerator finding). Round 4 should verify the corrected design, not the
   original.
7. **I4/I5 recording/replay compatibility — RESOLVED in §8 for the general claim; NARROWED after round 9,
   this bullet corrected after round 10 found it still read the pre-round-9 state.** `ChatCallRecording`/
   `ReplayChatClient` capture whatever this adapter's `chat_stream()` returns at ITS OWN boundary, agnostic
   to internal complexity — architecturally identical to recording any other `ChatClient` conformer, since
   the whole wrapped-workflow run happens synchronously within one adapter call. **But** round 9's removal
   of `chat()` means `WorkflowChatClient` no longer satisfies `LegacyChatClient`
   (`core/chat_client.hpp:228-234`), and `RecordingChatClient<Inner>` gates specifically on that stricter
   concept (`core/recording_chat_client.hpp:122-123`) — so `RecordingChatClient<WorkflowChatClient>` no
   longer compiles. Round 10 confirmed this is the COMPLETE consequence (grepped `LegacyChatClient`
   across the whole tree: only `RecordingChatClient`'s own definition/gate and one unrelated production
   call site, `tools/cli_chat.cpp:713-720`, wrapping `OpenAIChatClient`/`AnthropicChatClient` — both
   unaffected). The general architectural claim holds in principle; the ONE EXISTING mechanism this
   codebase has for exercising it does not apply to this adapter until `RecordingChatClient` is widened or
   a narrower recording wrapper is built (real, unbuilt, separate follow-on work) — a real, disclosed
   trade, not a silently-broken claim.
8. **Accepted, shared, pre-existing scope limitation, named by round 3 — not a blocker.**
   `WorkflowChatClient`'s `inner_` sits entirely outside ADR-157's `split_worker_budget()` nesting
   accounting (never passed through `bind_sub_workflow()`, the only mechanism that accounting tracks,
   `workflow_supervisor.hpp:2112-2118`), so nesting many wrapped agent nodes under one outer supervisor
   can oversubscribe host threads outside that mechanism's reach. Round 3 confirmed ADR-150's own sibling
   adapter has the IDENTICAL, unnamed gap — a shared property of "wrap a whole independent
   `WorkflowSupervisor`" as a pattern. Candidate follow-on work covering BOTH adapters together, not
   something this draft alone needs to solve before implementation.
9. **Implementation/testing follow-through, not a design question.** §5's narrowed
   `open_interaction_asks()` (`OpenPort`-only) is independently re-verified structurally sound across two
   rounds (round 1's construction, round 2's re-derivation) — it still needs to be BUILT and tested; the
   `PendingSubWorkflow` recursive case remains explicit, unbuilt follow-on work, not a defect in the
   narrowed version.

## 10. Red-team findings and resolutions

### Round 1

Independent red-team pass (fresh agent, zero prior context, briefed on this draft plus
`workflow_supervisor.hpp`, `chat_client.hpp`, `content.hpp`, `interaction.hpp`, `tool_pipeline.hpp`,
`agent_session.hpp`, `workflow_as_executor.hpp`/ADR-150, issue #35, and CLAUDE.md's I1–I8 invariants)
found 2 MUST-FIX findings and 7 MINOR/RESIDUAL findings.

1. **MUST-FIX, most severe** — the original §4a encoded a suspended `request_port` interaction as
   `ContentItem{ToolCall{call_id=interaction_id, tool_name="workflow_request_port", ...}}`, mirroring
   MAF's `function_call` envelope literally. Traced end to end against the REAL turn loop
   (`agent_session.hpp:2323-2359` round loop, `:2447-2450` approval-suspend pre-check gated on a
   resolved tool descriptor, `:2514-2544` unconditional `invoke_tool()` dispatch,
   `tool_pipeline.hpp:604-608` unknown-tool path returning `ToolResult{is_error=true,...}` with the
   SAME `call_id`, `:2617-2624`/`:1226-1233` auto-continuation): this is not merely risky, it
   **deterministically and silently resolves the paused human-in-the-loop interaction with a fabricated
   answer** the moment this adapter is bound as an ordinary agent's `ChatClientId` backend — one of
   issue #35's own named use cases. **Fixed**: the ask/resume signal is now a dedicated `Custom` content
   kind (§4a, §7), which `AgentSession::tool_calls_of()` (`core/tool_call_extraction.hpp:34-40`) never
   extracts — an outer `AgentSession` now sees an ordinary final answer instead of auto-consuming the
   ask. This narrows what the adapter can safely be composed with (§9 item 1, newly named) rather than
   fixing composability outright, but eliminates the silent-corruption hazard structurally.
2. **MUST-FIX** — §5's `open_interaction_asks()` was claimed to be a uniform, "genuinely additive"
   accessor over every open interaction. Traced directly: true for `OpenPort` (confirmed,
   `workflow_supervisor.hpp:1748`/`:810-820`), but FALSE for `PendingSubWorkflow`
   (`:1141-1145` carries no ask `Message` at all; the real ask requires recursing into a private, nested
   `WorkflowSupervisor` to an a-priori-unbounded depth, `:667`/`:2106`). **Fixed**: narrowed to
   `OpenPort` only, with an honestly-disclosed `Message{}` placeholder for `PendingSubWorkflow` entries
   (§5, rewritten) — the recursive case is named, explicit, unbuilt follow-on work, not silently assumed
   free.
3. MINOR — confirmed correct as cited: the draft's core claim that `open_interactions()` strips the ask
   `OpenPort::response` carries pre-resolution is accurate (evidence folded into finding 2 above).
4. MINOR, resolved into a stated design invariant — `run_workflow()` unconditionally clears
   `ports_`/`pending_sub_workflows_` at its own top (`:883-884`); combined with §4a's fail-closed
   contract-mismatch check, a second unrelated fresh conversation on a shared adapter instance while an
   earlier one is paused is hard-refused, not silently corrupted — but this means `WorkflowChatClient`
   supports exactly ONE caller-visible conversation per instance for its whole lifetime, unlike an
   ordinary stateless `ChatClient`. **Fixed**: stated explicitly as a design invariant on the type (§4),
   mirroring how `AgentSession` states I1.
5. MINOR — `capabilities().tool_calling`/`.streaming` traced against their real readers
   (`select_output_schema_strategy()`, `detect_undeclared_tool_call_leak()`) and found currently inert
   either way. Folded into §7's resolution — no separate action needed.
6. MINOR — open question A sharpened with evidence: `MessageTypeId` (`workflow/graph.hpp:58-76`) is a
   compile-time-only label with no runtime shape-checking, so nothing else in this codebase will catch a
   wrong message-flattening choice — raises the priority of the test named in §11. Carried into §9 item 2.
7. MINOR — the constructor comment `// may fail — see §5` never matched any real failure condition
   described anywhere in the draft. **Fixed**: dropped; construction is infallible (§4) — unlike ADR-150's
   sibling adapter, this one has no graph shape it needs to refuse.
8. MINOR, confirmed — the capability-sourcing citation (§6) transfers cleanly from ADR-150; independently
   re-derived, no change needed.
9. MINOR, resolved — `resume_workflow()`'s existing `pending_sub_workflows_`-before-`ports_` dispatch
   order (`:907-957`, ADR-157) already correctly resumes a nested sub-workflow's outer-visible
   interaction through the SAME `ResumeWorkflow{...}` shape §4a already builds, with no extra
   adapter-side branching needed. The real nested-case residual is finding 2 above
   (`open_interaction_asks()`'s missing ask payload), not the resume dispatch itself.

### Round 2

Independent red-team pass (fresh agent, zero prior context, briefed on the round-1-revised draft
including §10's own round 1 record, plus the same core files round 1 read, `core/tool_call_extraction.hpp`,
`core/stream.hpp`, and explicitly tasked with adversarially re-verifying round 1's two fixes rather than
trusting them). Found 3 MUST-FIX findings (one of which is a real gap in a round-1 fix's own supporting
claim, not a full reversal of the fix) and re-confirmed 2 of round 1's resolutions independently, with new
evidence.

1. **MUST-FIX** — round 1's `Custom`-content fix (finding 1 above) closes the SILENT-CORRUPTION hazard it
   was built to close (re-confirmed: `tool_calls_of()`, `core/tool_call_extraction.hpp:34-40`, still
   extracts only `ToolCall`), but §4a's further claim — that an outer `AgentSession` therefore "returns
   [the response] to ITS OWN caller as a normal final answer" — is FALSE whenever that outer session has
   `set_output_schema()` armed (`agent_session.hpp:769-776`). `run_rounds()`'s own "no tool calls → final"
   branch (`:2320-2359`) validates `text_of(response->message)` (`tool_call_extraction.hpp:44-50`, `Text`
   items only) against the installed schema before accepting the round as terminal; a `Custom`-only
   suspended response has no `Text` content, so validation runs against an empty string, fails essentially
   any real schema, and the WHOLE outer `run()` fails (`"run.output_schema_validation_failed"`) instead of
   surfacing the ask to anyone. **Fixed**: §4a's claim is now explicitly conditioned on "no
   `output_schema_validate_` armed," and an outer session WITH a structured-output contract is added as a
   second descoped composition (§9 item 1) alongside the already-named tool-set case — this is a real,
   unbuilt gap, not something the `Custom`-content fix was ever meant to solve.
2. **MUST-FIX** — §4b's original "`chat_stream()` is a thin wrapper: drive `chat()` to completion, then
   push... no new logic" was never real design. `chat_stream()` must return `stream<ChatResponseUpdate>`
   SYNCHRONOUSLY (confirmed against two real conformers' own file banners: `ReplayChatClient`,
   `core/replay_chat_client.hpp:21`, and `OpenAIChatClient`, whose blocking exchange runs on a detached
   background thread specifically so the synchronous return holds); `chat()` returns a lazy
   `agentengine::rt::task<T>` that must be pumped by something (`drive()`'s entire reason for existing,
   `workflow_supervisor.hpp:1229`). "Drive `chat()` inline, then push" as literally written blocks the
   CALLING thread for the wrapped workflow's entire run before `chat_stream()` even returns — the opposite
   of every real conformer here — and risks a self-deadlock if the calling thread is itself one of
   `WorkflowSupervisor`'s own `pool_` workers (the nested-worker-budget hazard class ADR-157 exists to
   manage, never previously considered for THIS adapter as a nested participant). **Fixed**: §4b rewritten
   — `chat_stream()` gets its own detached-thread/pool-scheduled worker mirroring the two real conformers,
   `chat()` becomes the thin wrapper instead (reversing which method round 1 called "the real logic"). The
   nested-worker-budget interaction itself is NOT resolved — carried forward as §9 item 5.
3. **MUST-FIX, entirely new** — `EffectContext::deadline`/`::cancellation` were never addressed by either
   the original draft or round 1; §6 only ever scoped `EffectContext&` to I2 capability sourcing.
   `run_workflow()`/`resume_workflow()` take no `EffectContext` parameter at all — structurally
   unreachable regardless of adapter-side code — while both fields are real, actively enforced elsewhere
   (`model_call_gateway.hpp:376-382`/`:544-549`, `tool_pipeline.hpp:667-668`), so a caller that binds a
   bounded `EffectContext` (the ordinary case for a sub-agent call, or any attempt to cancel a hung run)
   has no way to interrupt a hung/looping wrapped workflow through the `ChatClient` contract, even though
   `WorkflowSupervisor` already has its own independent, real cancellation mechanism (ADR-159's
   `cancel_source_`/`cancel()`) this adapter could bridge to. **Fixed**: §8 now names the gap explicitly and
   proposes a `stop_callback`-to-`inner_->cancel()` bridge plus a pre-call deadline check as the resolution
   direction — not yet designed in full; carried forward as §9 item 6.
4. MINOR, round 1's fix independently RE-CONFIRMED, not just re-cited — round 1's narrowed
   `open_interaction_asks()` (`OpenPort`-only) was traced a second time from first principles: the
   `!p.resolved` filter both `open_interactions()` (`:810-820`) and the proposed accessor share is, on its
   own, sufficient to prevent misattributing a resolved-but-not-yet-cleared port's stale ask to a live
   interaction, and `pending_sub_workflows_` is a wholly separate map walked independently, so no ordering
   hazard exists between the two. No change to §5 needed — the narrowed version holds.
5. MINOR, round 1's fix independently RE-CONFIRMED — `detect_undeclared_tool_call_leak()`
   (`response_format_leak_scan.hpp:111-135`) scans only `Text` items (`:115-116`), so it stays inert for a
   `Custom`-only ask/response exactly as §7 claims. No change needed.
6. MINOR — open questions A and B (message-flattening strategy, multiple simultaneously-open
   interactions) and the contract-mismatch-vs-abandon question (§9 items 2–4) were re-examined but not
   conclusively resolvable from evidence found this round — genuinely still open, not re-flagged for lack
   of trying.
7. MINOR, new and speculative, not asserted as a confirmed defect — whether a `WorkflowChatClient`-wrapped
   backend can be faithfully recorded/replayed through `ChatCallRecording`/`ReplayChatClient` (004 §6) when
   the real nondeterminism lives several layers inside the wrapped workflow's own round history rather than
   at the outer `ChatResponse` boundary the recording format captures. No code found proving this broken —
   named as a real question for the next round (§9 item 7), not manufactured to pad the findings list.

### Round 3

Independent red-team pass (fresh agent, zero prior context, briefed on the round-1-and-2-revised draft
including §10's own full record, explicitly tasked with adversarially verifying the two designs the
draft's author wrote in response to round 2 — §4b's detached-thread `chat_stream()` worker, §8's
`stop_callback` cancellation bridge and deadline pre-check — rather than trusting that citing real
evidence made them correct). Found 3 MUST-FIX findings, all in the two NEW designs themselves (both
fixed), confirmed the `chat()`-drains-`chat_stream()` shape is sound, and resolved §9 item 5 with hard
evidence.

1. **MUST-FIX** — §8's `stop_callback` snippet read `ctx.cancellation` LAZILY, from inside the detached
   worker thread, after `chat_stream()` had already returned to its caller — a genuine dangling-reference
   hazard, since nothing in the `ChatClient` contract obligates a caller to keep the `EffectContext&` it
   passed alive past the point `chat_stream()` returns (the entire premise of §4b's own fix). Neither real
   conformer §4b claims to mirror does this: `OpenAIChatClient::chat_stream()`
   (`protocol/openai/chat_client.hpp:1019-1040`) resolves everything it needs from `ctx` SYNCHRONOUSLY,
   before spawning the thread, and forwards only the stream's OWN `stop_token()` — never `ctx` itself —
   across the thread boundary. **Fixed**: §8's snippet now extracts `ctx.cancellation`/`ctx.deadline` into
   local values synchronously inside `chat_stream()`, before detaching, and passes the copies into the
   worker's closure by value, matching `OpenAIChatClient`'s own discipline exactly.
2. **MUST-FIX** — the deadline pre-check's proposed `failure_class::deadline_exceeded` does not exist.
   The real `failure_class` enum (`core/error.hpp:12-18`) is `{transient, policy, contract, resource,
   fatal}`; even the cited precedent (`model_call_gateway.hpp`) never invents a deadline-specific class —
   it re-returns the underlying attempt's own error on `now >= ctx.deadline` instead of synthesizing one.
   **Fixed**: the pre-call deadline check now uses the real `failure_class::resource` with a real code
   string, not a placeholder that has no backing enumerator.
3. **MUST-FIX** — §4a (describing the read-then-act algorithm) and §8 (the new cancellation snippet) were
   never reconciled after §4b's method-swap: §4a still read "Lock `call_mutex_` for the full call"
   describing `chat()`, and §8's snippet comment said the bridge is "held for the duration of one `drive()`
   call (`chat_stream()`'s worker thread)" — internally inconsistent about WHERE the lock is actually taken.
   If an implementer acquired `call_mutex_` synchronously inside `chat_stream()` before detaching (a
   plausible reading of the stale §4a text), contention on that lock would block the calling thread,
   silently reintroducing the exact caller-blocking regression §4b's fix exists to close. **Fixed**: §4a
   retitled and corrected to state explicitly that the whole read-then-act algorithm, including acquiring
   `call_mutex_`, runs on the detached worker thread — never synchronously inside whichever method the
   caller invoked; §8's prose updated to match.
4. CONFIRMED SOUND, not just re-cited — §4b's `chat()`-drains-`chat_stream()` design does not reintroduce a
   coroutine-level hazard. `stream<T>::next()` (`core/stream.hpp:170`) is a plain, non-blocking,
   non-suspending poll (`inner_.try_pop()`), never a `co_await` point; the one real drain loop in this
   codebase (`drain_streaming_response()`, `rt/agent_session_trust.hpp:92-122`) is an ordinary polling
   `while` loop with a real `sleep_for` between polls — genuine OS-thread blocking, not coroutine
   suspension, identical in character whether `chat()` is `co_await`ed or driven by a naive `while(!done())
   resume()` loop. The "every real conformer already blocks its caller the same way" acceptance claim
   holds for both call shapes.
5. CONFIRMED SOUND, once finding 1 above is fixed — `std::stop_callback`'s destructor-vs-`request_stop()`
   race was checked directly: the standard's own guarantee (synchronous invocation on an already-stopped
   token; the destructor blocks on a concurrently-running invocation from another thread, with no deadlock
   since `bridge`'s own destruction here never happens from inside its own callback) is correctly relied
   on, and `cancel()`'s body (`cancel_source_.request_stop()`, no lock, per its own documented contract) is
   fast enough that any blocking window is negligible. No lock-ordering hazard against `call_mutex_` found
   (private to the adapter, never exposed to `inner_`'s own executor bodies).
6. RESOLVED with hard evidence, narrower and more concrete than round 2 left it — §9 item 5 (now item 5,
   renumbered): `AgentSession::run_model_call()` DOES call a bound `ChatClient`'s `chat()` directly
   (`agent_session.hpp:1731-1735`, gated on `stream_model_calls_`, which defaults to `false`,
   `:2731`), and `AgentSession` genuinely runs as a `WorkflowSupervisor` pool worker via
   `agent_session_as_executor_body()` → `pool_.submit(run_executor_job(...))`
   (`rt/agent_workflow_executor.hpp:89-123`, `workflow_supervisor.hpp:1477-1479`) — the scenario is the
   DEFAULT wiring for an `AgentSession` bound to this adapter and run as an agent-kind node, not
   hypothetical. Deadlock-freedom confirmed; the residual cost is accepted, precedented behavior (ADR-150
   §6), except for one new dimension — see finding 7.
7. NEW, found by round 3 — `inner_`'s own thread pool sits entirely outside ADR-157's `split_worker_budget()`
   nesting accounting (`inner_` is never passed through `bind_sub_workflow()`, the only mechanism that
   accounting tracks, `workflow_supervisor.hpp:2112-2118`), so nesting many `WorkflowChatClient`-wrapped
   agent nodes under one outer supervisor can oversubscribe host threads outside that mechanism's reach.
   **Not treated as a defect unique to this draft**: round 3 confirmed ADR-150's own sibling
   `workflow_as_executor_body()` adapter has the identical gap, unnamed by ADR-150 itself — a shared
   property of "wrap a whole independently-constructed `WorkflowSupervisor`" as a pattern. Named as §9 item
   8, a candidate follow-on item covering both adapters together, not a blocker for this draft alone.
8. MINOR — open questions A and B (message-flattening strategy, multiple simultaneously-open interactions)
   were not pursued this round; round 3's effort was concentrated on the two required deep-dive
   verifications per its own task weighting, which is what surfaced findings 1-3. Still open for round 4.

### Round 4

Independent red-team pass (fresh agent, zero prior context, briefed on the full round-1/2/3-revised
draft, explicitly tasked with adversarially verifying the FOUR design questions the draft's author
resolved in one pass immediately before this round — message-flattening, multi-interaction resume,
contract-mismatch vs. abandon, and I4/I5 recording/replay — none of which had been red-teamed by anyone
before this round). Found 2 MUST-FIX findings (one of them a genuine, well-evidenced defect in the
mainline flattening design, not an edge case), confirmed 2 of the 4 resolutions hold with new evidence
traced against real code, and surfaced 2 MINOR findings.

1. **MUST-FIX, most severe of the whole review so far** — the message-flattening resolution ("concatenate
   content items, rely on per-item `ContentItem::origin` for turn attribution") is UNSOUND for the
   mainline multi-turn use case, not an edge case. Traced directly against BOTH real backends'
   `translate_message()`: `OpenAIChatClient` (`protocol/openai/chat_client.hpp:94-141`) and
   `AnthropicChatClient` (`protocol/anthropic/chat_client.hpp:177-223`) both key an outbound wire message
   SOLELY on `role_to_wire(m.role)` and concatenate every `Text` item into one blob — **neither reads
   `ContentItem::origin` at all**. A flattened `Message` reaching either backend produces a garbled,
   unattributed text blob, or — when a `ToolResult` item ends up under the wrong outer role after
   flattening — an outright WIRE-PROTOCOL-INVALID request (`tool_call_id` on a non-`tool`-role OpenAI
   message; a `tool_result` block in a non-`user`-role Anthropic message). The draft's own citation for
   `origin` being "already read throughout this codebase" (`context_assembly.hpp:243-284`) was real but
   doing the wrong job — that's a trust/provenance check, not a turn-boundary substitute; no code in this
   repo treats `origin` as a `Message::role` replacement. Because this adapter re-runs the whole workflow
   from `start` on EVERY call, this corrupts every caller-visible turn of an ordinary conversation once
   the wrapped workflow reaches a real backend — not a rare edge case. **Fixed**: the physical-merge
   design is replaced entirely — the message list is now encoded as one opaque `Custom` envelope built
   from `message_to_json()` (`rt/message_codec.hpp:393`), which a wrapped `start` executor must explicitly
   decode via `message_from_json()`. No naive "forward this Message as-is into a real backend" path exists
   that could silently produce a wire-plausible-looking but wrong result — the envelope's own opacity
   forces active decoding.
2. **MUST-FIX** — §4's own class-body code comment (the `WorkflowChatClient` interface declaration itself)
   still described the round-1 design round 2 found broken and §4b explicitly reversed — three rounds of
   red-team focused on §4a/§4b prose and missed that the literal interface block an implementer would copy
   verbatim still asserted the wrong design (`chat()` as "the real logic"). §4b's own text even said so
   ("reversing which of the two methods is 'the real logic' from what §4's class-body comment claimed")
   without anyone going back to fix the comment itself. **Fixed**: the comment now matches §4b —
   `chat_stream()` is the real logic, `chat()` is the thin wrapper.
3. CONFIRMED SOUND, with new evidence — multi-interaction resume / partial answers, traced against the
   REAL `resume_workflow()` body (`workflow_supervisor.hpp:892-1029`) rather than reasoned about
   abstractly: resolving one interaction never mutates or invalidates any other currently-open
   interaction's id (ordinary-port resolution only flips that one port's own flag, `:1004-1006`; nested
   resolution only touches the one key being resumed, `:909`/`:942-946`), and `execute()` is reached only
   once EVERY currently-open interaction in that call is resolved (`:975-988`/`:1010-1028`) — a genuine
   M-of-N partial batch cannot trigger a premature `execute()` mid-loop regardless of processing order.
   Holds PROVIDED the implementation re-fetches `open_interactions()` fresh before each resume call in the
   loop (stated correctly in the design; now flagged for its own targeted test).
4. CONFIRMED SOUND, against the actual CAPTURE-side code, not just the replay side already read by earlier
   rounds — I4/I5 recording/replay compatibility. `RecordingChatClient<Inner>`
   (`core/recording_chat_client.hpp:140-266`) makes no assumption about `Inner`'s internal threading model
   beyond the `LegacyChatClient` concept's surface; `WorkflowChatClient`'s declared signatures structurally
   satisfy that concept, so `RecordingChatClient<WorkflowChatClient>`'s own `static_assert` (`:122-123`)
   passes. Genuinely confirmed, not merely asserted.
5. MINOR — the contract-mismatch/ADR-150 analogy (§4a) overclaimed EQUIVALENCE in strength, not just
   mechanism: ADR-150 makes its hazard structurally IMPOSSIBLE (refuses at construction, never reachable);
   this adapter's fix is a RUNTIME check re-derived on every call — real mechanism-level parallel, weaker
   guarantee in kind. **Fixed**: §4a now states this distinction explicitly rather than claiming full
   equivalence.
6. MINOR — §4a step 3's "fold the resulting `WorkflowResult`" was ambiguous about which `WorkflowResult`
   is meant when the resume loop processes M > 1 signals in one call (answer: only the LAST iteration's,
   every earlier one is a superseded intermediate `suspended` result). **Fixed**: one clarifying paragraph
   added; no logic change, the design already implied this.

Round 4's own overall assessment: not clean on the first pass (finding 1 is real and mainline-severity),
but explicitly confirmed the other three resolutions (multi-interaction resume, contract-mismatch
mechanism, I4/I5 recording/replay) hold under direct adversarial scrutiny with real code evidence, and
recommended one more short, narrowly-scoped pass once findings 1-2 are fixed — which produced this
revision, now sent for round 5.

### Round 5

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 4's own
two fixes — the `Custom`-envelope/`message_to_json()` flattening mechanism, and the class-body comment
correction — rather than a full fresh sweep; explicitly tasked with checking whether the new envelope
mechanism defeats §4a's own resume-signal scan on the same call, among other risks). Found 2 MUST-FIX
findings (both new bugs introduced by round 4's own fixes, not reversals of round 4's core reasoning) and
4 MINOR findings, one of which was the specific interaction hazard the round was most worried about —
confirmed NOT to occur, with direct evidence.

1. **MUST-FIX** — round 4's rewritten §4 class-body comment (and §4b's own prose) claimed `chat_stream()`
   "pushes exactly ONE `ChatResponseUpdate`" for the resulting `Message` — but `ChatResponseUpdate::delta`
   is a SINGULAR `ContentItem` (`core/chat_client.hpp:150-152`), while `Message::content` is a
   `std::vector<ContentItem>` that §4a's own resolved open question B explicitly makes hold N ≥ 1 items
   for a suspended run (one ask-signal item per open interaction) — not an N>1 edge case, this breaks for
   ANY multi-item output, suspended or not. The real conformer this section claims to mirror does the
   opposite: `ReplayChatClient::run_replay_worker` (`core/replay_chat_client.hpp:93-114`) pushes ONE
   update PER CHUNK in a loop, `is_final` only on the terminal push. **Fixed**: both the class-body
   comment (§4) and §4b's own prose now describe pushing one `ChatResponseUpdate` per `ContentItem` in
   the resulting `Message.content`, in order, `is_final=true` only on the last — matching
   `ReplayChatClient`'s real pattern instead of contradicting it.
2. **MUST-FIX** — the new history-envelope (§4a, round 4's fix) had no size ceiling, contradicting this
   codebase's own established, fail-closed convention for exactly this problem:
   `tool_pipeline.hpp:421-461`'s `normalize_success()` promotes an oversized `Data` payload to an
   out-of-line `Media{BlobRef}` once it crosses `ctx.tool_result_byte_threshold`, and FAILS CLOSED
   (`tool.result_oversized_no_sink`, `:442-450`) rather than silently inlining an oversized result when no
   `ctx.blob_sink` is configured. The draft's envelope instead unconditionally `json::dump`s the ENTIRE
   `request.messages` history — including any base64-inlined `Media` bytes — into one `payload_json`
   string on EVERY fresh call (§2's "re-run from `start` every call" contract means this grows unbounded
   across a long conversation), with no threshold check and no promotion path — a direct I8 ("budgets are
   enforced") gap. **Fixed**: the SAME `ctx.tool_result_byte_threshold`/`ctx.blob_sink` mechanism now
   gates the envelope — under threshold, inline as `Custom`; at/above threshold, promote to
   `Media{BlobRef}` via the same sink, with the same fail-closed refusal when no sink is configured. A
   wrapped `start` executor is documented to accept either shape.
3. CONFIRMED — round 5's core adversarial target (does the new envelope bury §4a's own resume-signal item
   inside itself on the SAME call, defeating the resume-signal scan?) does NOT occur. Traced directly:
   the fresh-call branch (envelope-wrap) and the paused-call branch (raw resume-signal scan against
   `request.messages`) are mutually exclusive per call in §4a step 2 — flattening only ever happens on the
   branch where no resume-signal scan runs at all. No interaction bug found between the two mechanisms.
4. CONFIRMED — `message_to_json()`/`message_from_json()` round-trip a `Custom` item (including one nested
   one level inside the new envelope, from a PRIOR turn's resume-signal bookkeeping re-flattened on a
   LATER turn) without corruption: the codec's own `Custom` arms (`message_codec.hpp:270-275`/`:372-376`)
   treat `type_id`/`payload_json` as opaque strings, never re-parsing, and the underlying JSON string
   escaping (`json_value.hpp:364-386`/`:223-282`) correctly round-trips nested quotes/backslashes. This
   is a REAL scenario (occurs on any conversation past its first suspend/resume round-trip), not
   hypothetical, and was traced with direct evidence rather than assumed safe.
5. CONFIRMED — `message_to_json()`'s `Media` handling (`message_codec.hpp:230-244`/`:311-336`) is correct
   and lossless for all three `Media` payload variants (raw bytes via base64, uri, `BlobRef`); the real
   cost of an unbounded envelope is SIZE (finding 2), not silent data loss — the draft's own implicit
   worry about `Media` being dropped was unfounded.
6. MINOR, documentation-only — once a stale internal `Custom` bookkeeping item gets re-embedded in a later
   envelope (per finding 4), a wrapped `start` executor decoding it sees this adapter's own housekeeping
   mixed into ordinary history. Round 4's own trace suggests real backends already silently skip
   unrecognized `Custom` items rather than corrupting the wire request, but this was never stated
   explicitly. **Fixed**: one documentation paragraph added to §4a; no design change.

Round 5's own overall assessment: not clean (2 real MUST-FIX gaps in the two things it targeted), but
explicitly confirmed the specific interaction hazard it was most worried about does not materialize, and
confirmed `Media` handling and nested-`Custom` round-tripping are both sound with direct evidence.

### Round 6

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 5's two
fixes — the multi-item `ChatResponseUpdate` push/reassembly design, and the envelope size-threshold fix —
rather than a full fresh sweep). Found 2 MUST-FIX findings (neither a reversal of round 5's core
reasoning — one is a genuinely new, load-bearing omission no prior round had touched; the other is a
narrower regression of round 3's own already-fixed dangling-reference bug, reintroduced by round 5's own
fix missing two of the four fields that class of bug applies to), confirmed the push-ordering design
sound with direct evidence, and named one test-coverage gap.

1. **MUST-FIX, the most severe single finding across all six rounds** — `Usage` (`ChatResponseUpdate::usage`
   and `ChatResponse::usage`) was never addressed by ANY prior round; a full-document grep confirmed zero
   mentions before this round. `ChatResponseUpdate::usage` (`core/chat_client.hpp:159`) defaults to
   `nullopt`, documented as meaning "this backend/call provided none" — and `AgentSession`'s own real
   streaming-consumption path, `detail::drain_streaming_response()` (`rt/agent_session_trust.hpp:92-136`),
   enforces this literally: `co_return std::unexpected(error{..., "run.usage_unavailable"})` (`:128-134`)
   whenever the terminal update's `usage` is unset. `run_model_call()` reaches this path directly whenever
   `stream_model_calls_` is `true` (`agent_session.hpp:1755-1756`) — a real, already-supported
   configuration for exactly the "sub-agent" composition issue #35/§9 item 1 describe. Left at its
   undecided default, EVERY streamed call through `WorkflowChatClient` would hard-fail the entire outer
   `AgentSession::run()` the moment that composition is used — not degraded, a total break of a
   already-existing code path. **Fixed**: §4b now states a documented, deliberate zeroed `Usage{}` on
   both fields, with an explicit rationale (this adapter performs no metered model call of its own at the
   `ChatClient` boundary; the wrapped workflow's own `agent`-kind nodes meter their own calls
   independently) rather than an undocumented default that happens to hard-fail one real consumer.
2. **MUST-FIX** — round 5's envelope size-check (§4a) read `ctx.tool_result_byte_threshold`/`ctx.blob_sink`
   off the SAME `EffectContext&` `chat()`/`chat_stream()` receive, described as running inside §4a's
   algorithm — which §8 already established runs on the detached worker thread. §3's own round-3 fix only
   ever extracted `cancellation`/`deadline` synchronously before detaching; the two NEW fields round 5's
   fix needed were never added to that extraction, silently reintroducing round 3's own already-fixed
   dangling-reference hazard (finding 1, this time for two different fields). Confirmed both fields are
   safe to extract synchronously (`tool_result_byte_threshold` is `std::optional<std::uint64_t>`,
   `blob_sink` is a `std::function<...>` — both trivially copy-by-value, `effect_context.hpp:181`/`:190-191`)
   and confirmed this codebase's own real precedent for running tool-pipeline-shaped logic (including a
   `normalize_success()`-style size-check) from a detached thread already solves this by copying the WHOLE
   `EffectContext` by value before detaching (`tool_pipeline.hpp::background_task()`, `:701-707`/`:726-727`,
   itself the fix for an earlier ADR-060 finding about this same hazard class). **Fixed**: §8's snippet now
   copies the WHOLE `EffectContext` by value before detaching (matching `background_task()`'s own
   established pattern) rather than a hand-picked field list that has already needed extending once and
   would predictably need extending again the next time some new logic needs one more field off `ctx`.
3. CONFIRMED SOUND, with direct evidence — the multi-item push/reassembly ordering design (round 5's
   fix). `stream<T>::next()` (`core/stream.hpp:170`) is `inner_.try_pop()` against `rt::channel`'s strict
   FIFO queue (`push_back`/`pop_front`, `rt/channel.hpp:200`/`:291-298`), single producer/single consumer
   — no possibility of pushed `ContentItem`s coming back out of order. `drain_streaming_response()`
   (`rt/agent_session_trust.hpp:92-136`), the one real incremental consumer in this codebase, never acts
   on an individual update as a terminal signal — it only returns once the stream reaches a real terminal
   state with the queue fully drained, appending each item to `accumulated.content` along the way; no
   caller acts on "ask-signal item 1 of N" before N is fully pushed. This part of round 5's design holds
   exactly as designed.
4. MINOR — §11's planned test list named no explicit test item for round 4/5's own mechanisms (the
   multi-push/reassembly ordering, the envelope size-threshold/promotion path, or the
   `stream_model_calls_(true)`/usage regression this round's finding 1 would have caught). **Fixed**: five
   concrete test cases added to §11, naming each round's own finding as the thing each test proves.

Round 6's own overall assessment: not clean (2 real MUST-FIX gaps, one of them the single most severe
finding of the whole review — a load-bearing omission that would hard-break a real, already-existing
consumption path), but explicitly confirmed the push-ordering design itself is sound with direct evidence,
and both new findings are narrowly scoped to what round 5 touched rather than reopening earlier rounds'
work.

### Round 7

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 6's two
fixes — the zeroed-`Usage{}` decision, and the whole-`EffectContext`-copy fix — rather than a full fresh
sweep). **Neither of round 6's fixes held.** Found 2 MUST-FIX findings, both real: one a genuine, silent
I8 violation the round-6 fix itself introduced; the other a wider reopening of the exact dangling-reference
bug class round 3 (then round 6) both claimed to have closed, because round 6's own cited precedent
(`background_task()`) does field-by-field sanitization the round-6 snippet never actually performed.

1. **MUST-FIX, arguably the most consequential finding of the whole review — a silent violation, not a
   loud one.** Traced `AgentSession`'s real budget-summation code directly: `agent_session.hpp:2311-2317`
   sums `run_tokens_consumed_ += response->usage.input_tokens + response->usage.output_tokens` on EVERY
   `run_model_call()` round-trip (streaming or not) and fails once `token_budget_` — an ordinary,
   independent config knob, orthogonal to `stream_model_calls_` — is exceeded. Round 6's zeroed `Usage{}`
   means `run_tokens_consumed_` never increases for this adapter's calls REGARDLESS of how much the
   wrapped workflow's own inner model calls actually cost — a caller who configures `.token_budget(N)`
   expecting it to bound the whole run's real cost gets a silently inert cap, the default effect of round
   6's fix on any budgeted session, not an edge case. The doc-comment contract round 6 leaned on
   ("nullopt means hard failure, never silently as zero cost," `chat_client.hpp:156-158`) was misread —
   its subject is the cost of PRODUCING the response, not the literal wire cost of this one conformer;
   the wrapped workflow's inner nodes DID incur real cost, so reporting `Usage{}` is not honest disclosure
   of zero cost, it is exactly the "unknown real cost silently treated as zero" case the fail-closed
   `nullopt` path exists to refuse. Confirmed no aggregation mechanism exists to report instead
   (`workflow_supervisor.hpp` has zero mentions of "usage"). **Fixed**: reverted to `nullopt` on the
   terminal `ChatResponseUpdate` (restoring the correct, loud fail-closed signal round 6 had treated as a
   defect to paper over) and named the `ChatResponse::usage`/budget-reliance gap as a THIRD explicitly
   descoped composition (§9 item 1) rather than faking a number for a mandatory field with no
   fail-closed escape hatch of its own.
2. **MUST-FIX** — round 6's whole-`EffectContext`-copy fix (§8) was a bare `ctx_copy = ctx;` with no field
   hygiene. Read `effect_context.hpp` in FULL: `bound_capabilities` (`:53`) and `sandbox_fs` (`:211`) are
   raw pointers explicitly documented "borrowed, never owned here" — copying the struct copies the
   pointer's bit pattern, not what it points at; `report_progress`/`agent_turn_sink`/`moderator_delta_sink`
   (`:115`/`:140`/`:145`) are explicitly call-scoped `std::function`s, bracketed open-then-reset around
   each real call site elsewhere in this codebase. Round 6's OWN cited precedent, read in full rather than
   cited for its by-value PARAMETER alone, does NOT trust a bare copy: `background_task()` explicitly
   resets `ctx.report_progress`/`ctx.sandbox_fs` (`tool_pipeline.hpp:775`/`:784`, citing ADR-060 §4 by
   name for this exact hazard class) and re-points `ctx.bound_capabilities` at a vector the worker itself
   owns (`:824`) — copy-THEN-SANITIZE, never copy-and-trust. This is a LIVE hazard for this adapter, not
   theoretical: nothing prevents a `Tool<>::invoke()` body from holding a `WorkflowChatClient` and calling
   `chat_stream()` (every round since round 2 requires it not block), and `invoke_tool()`'s own real
   bracket (`tool_pipeline.hpp:674-676`) sets `ctx.bound_capabilities` to a STACK-LOCAL, reset to `nullptr`
   the instant the synchronous call returns — a bare struct copy would carry that about-to-dangle pointer
   onto the detached worker, live past the frame that owned it. **Fixed**: §8's snippet now sanitizes the
   copy field-by-field before detaching (`bound_capabilities`/`capabilities`/`sandbox_fs` → null,
   `report_progress`/`agent_turn_sink`/`moderator_delta_sink` → no-op), mirroring `background_task()`'s
   real pattern completely — keeping only the fields this adapter's own design genuinely needs on the
   worker thread (`cancellation`, `deadline`, `tool_result_byte_threshold`, `blob_sink`).

Round 7's own overall assessment: not clean — both round-6 fixes needed rework, and round 7 explicitly
noted neither "held." The `Usage` finding in particular reframes round 6's own "fix" as having been the
wrong direction entirely (papering over a correct fail-closed signal with a silent violation), not a
partial gap. Both are now fixed differently: `Usage` reverted to its pre-round-6 value with a new,
explicitly named scope boundary; the `EffectContext` copy kept round 6's whole-struct shape but added the
field-by-field sanitization round 6's own cited precedent already required.

### Round 8

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 7's two
fixes — the Usage revert to `nullopt`, and the `EffectContext` field-by-field sanitization). The
sanitization fix held up under a full field-by-field audit; the Usage fix did NOT — round 7 had only
fixed HALF of it.

1. **MUST-FIX** — round 7's Usage revert fixed `ChatResponseUpdate::usage` (the streaming/opt-in path)
   but left `ChatResponse::usage` (the plain `chat()` path) genuinely UNSPECIFIED, not merely descoped.
   Traced `AgentSession::run_model_call()`'s real non-streaming branch (`agent_session.hpp:1734`/`:1752`,
   reached whenever `stream_model_calls_` is `false` — that field's DEFAULT, `:2731`, not a narrower
   opt-in the way round 7's own framing implied): it feeds `response->usage` into
   `run_tokens_consumed_ += ...` (`:2311`) UNCONDITIONALLY, with no fail-closed gate, because
   `ChatResponse::usage` is mandatory (no `nullopt` available). Left undecided by round 7's own text, the
   only value an unguided `ChatResponse{...}` naturally produces is a default-constructed zero — silently
   reproducing round 6's exact defeated-budget defect, in `AgentSession`'s DEFAULT wiring, not confined to
   an opt-in. **Fixed**: `chat()` now adopts the identical fail-closed rule this codebase's own
   `ModelCallGateway::attempt_with_retry()` already established (`model_call_gateway.hpp:353-360`, "no
   reported usage is a failure, not a zero-cost success") — if the drained terminal update's `usage` is
   `nullopt` (which it always is, per the design above), `chat()` itself returns
   `std::unexpected(error{...})` rather than default-constructing a `ChatResponse`. §9 item 1(c) is
   correspondingly strengthened: this adapter cannot complete ANY call through an `AgentSession` binding,
   via either method, until real usage aggregation exists upstream — a materially bigger caveat than
   round 7 (or any earlier round) understood it to be.
2. CONFIRMED SOUND, with a full field-by-field audit, not a partial re-check — every field on
   `EffectContext` was read and cross-checked against the sanitization list: `principal`/`trace_id`/
   `span_id`/`run_id`/`turn_index`/`codeact_preseeded_answers` are plain value types (confirmed
   `Principal` itself, `trust/principal.hpp:27-56`, is three strings/an enum/a `uint32_t`, zero
   pointers) with no aliasing or call-scoped character — safe to carry across unchanged, and now named
   explicitly in §8's snippet rather than silently omitted from the accounting (round 8's own stated
   concern: an implicit "everything else is presumably fine" is exactly the pattern that produced the
   round-6→7 regression). No field fell through the cracks a third time.
3. MINOR — nulling `capabilities` actually DIVERGES from `background_task()`, the fix's own cited
   precedent: that precedent deliberately KEEPS this field (a refcounted `shared_ptr`, safe to copy per
   ADR-061 §20.3, `tool_pipeline.hpp:709-712`). Not incorrect (nulling a `shared_ptr` copy is never UB,
   and §6/§8 already establish this adapter never reads `ctx.capabilities`), but the draft's claim to
   "mirror `background_task()`'s real pattern completely" overstated it for this one field. **Fixed**:
   §8's snippet now states this is extra caution beyond what the precedent requires, not something the
   precedent demanded.
4. MINOR, confirmed sound — `blob_sink` (kept, needed for the envelope size-check) was checked against
   its real intended backing implementation, `FileWorktreeObjectStore::put_blob`
   (`core/file_worktree_object_store.hpp:93-96`), which is internally `std::mutex`-guarded with no
   documented thread-affinity requirement — safe to invoke from a detached worker thread, a genuinely
   different hazard class from `report_progress`'s (which was about reaching a SPECIFIC session's
   unlocked state from a foreign thread, not about the callable's own type). No production call site
   assigns `blob_sink` today (only two test fixtures), so this is confirmed against the intended real
   implementation, not a live production wiring site — the same "unbuilt follow-on work" category as
   finding 1's usage aggregation, not a defect in this draft.

Round 8's own overall assessment: not clean (the Usage gap was real and more consequential than round 7
believed it had fixed), but the `EffectContext` sanitization — the fix most rounds have now iterated on
— finally held up under a complete, field-by-field audit with no remaining gaps.

### Round 9

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 8's
`chat()`-fails-closed fix — the one remaining unverified change). Found round 8's fix, while correctly
motivated, was structurally in the wrong place, and recommended removing `chat()` entirely rather than
patching it a fourth time — a smaller, structurally cleaner resolution than any of rounds 6-8's own
attempts, adopted as-is.

1. **MUST-FIX, the round that finally closed the `Usage` saga (rounds 6-9) for good** — round 8's
   `chat()`-fails-closed fix has no way to distinguish an `AgentSession` binding (which needs the usage
   guarantee) from a caller talking to this adapter DIRECTLY (§1/§11's own headline use case, which never
   needed usage accounting at all) — `chat()`'s signature carries no such signal, and §6 already
   establishes the `EffectContext&` it receives is unused either way. The consequence: `chat()` as round 8
   specified it fails EVERY caller unconditionally, including §11's own planned example
   (`examples/28_workflow_as_chat_client.cpp`, "driven through two `chat()` calls") — round 8's fix broke
   the exact "safe for a direct caller" property every round since round 1 had assumed, a bigger
   regression than round 8's own text (scoped only to "AgentSession binding") suggested. Traced the cited
   precedent (`ModelCallGateway::attempt_with_retry()`) and found it does NOT actually transfer: that
   machinery is explicitly OPT-IN decorator code (its own file banner: "a caller wanting just retry+
   failover uses `ModelCallGateway<...>` directly" — a caller who doesn't want budget correctness never
   reaches the rule); `WorkflowChatClient::chat()` had no equivalent opt-in gate. **Fixed by removing
   `chat()` from the type entirely**, rather than adding a fourth patch: `ChatClient`
   (`core/chat_client.hpp:205-209`) already makes `chat()` optional specifically so a backend can conform
   via `chat_stream()` alone — this is that mechanism used as designed. A direct caller now drains
   `chat_stream()` and hits no failure, ever (usage honestly `nullopt`, never fabricated). An
   `AgentSession` binding still cannot complete a call — but now via `run_model_call()`'s own
   pre-existing, already-audited `chat()`-absent fallback (`agent_session.hpp:1730-1756`, unconditionally
   draining `chat_stream()` through `drain_streaming_response()`'s existing fail-closed check), not a
   bespoke adapter-side rule. §9 item 1(c) is corrected to describe the block as coming from THIS
   pre-existing machinery, and §11's test list/example are updated to reflect `chat_stream()` as the
   adapter's only entry point.
2. INVESTIGATED AND REJECTED, worth recording so a future round doesn't re-propose it — whether
   `WorkflowChatClient` could statically prove a genuinely honest zero `Usage` when its wrapped graph
   contains no `agent`-kind node reachable (avoiding both round 6's fabricated zero AND round 8's
   fail-every-call). Rejected for two independent reasons: (a) a `sub_workflow`-kind executor's own nested
   graph isn't inspectable without the exact recursive walk §5 already found to be real, unbuilt,
   `kMaxNestingDepth`-bounded work it explicitly does not attempt; (b) more fundamentally,
   `executor_kind::agent` is a capability-ceiling DECLARATION, not a soundness boundary — a `function`-kind
   node's `ExecutorBody` is arbitrary C++ free to hold and call a real `ChatClient` directly while declared
   `function`-kind, so "zero `agent`-kind nodes reachable" would not actually prove zero cost was incurred.
3. NEW, disclosed consequence of finding 1, not present before round 9 — `RecordingChatClient<Inner>`
   (`core/recording_chat_client.hpp`) gates on `LegacyChatClient` (requires `chat()`); `WorkflowChatClient`
   satisfied that concept through round 8 (when it still had `chat()`) but does NOT once `chat()` is
   removed — `RecordingChatClient<WorkflowChatClient>` no longer compiles. The general I4/I5 architectural
   claim (§8's recording/replay section) still holds in principle, but the ONE EXISTING mechanism this
   codebase has for exercising it over a `ChatClient` conformer cannot be instantiated over this adapter
   until `RecordingChatClient` itself is widened to accept a plain-`ChatClient` `Inner` (real, separate,
   unbuilt follow-on work) or a narrower recording wrapper is built. Named explicitly in §8 rather than
   left as a silently-broken claim — a real trade this draft accepts: a `chat_stream()`-only design that
   is safe for every composition, over a `chat()`-having design that was recordable but broke the direct-
   caller composition.

Round 9's own overall assessment: **the fix holds, and is explicitly smaller/cleaner than what it
replaces** — removing a method closes more ground than round 8's third attempt at patching its behavior
did, and restores a property (direct-caller safety) that had been true since round 1 and was only broken
between rounds 8 and 9. This is the first round since round 4 whose recommended fix REDUCED the design's
surface area rather than adding to it.

### Round 10

Independent red-team pass (fresh agent, zero prior context, narrowly scoped to verifying round 9's
structural fix — `chat()`'s removal — mechanically, and re-checking the `RecordingChatClient` disclosure
for completeness). **This is the first round to report the underlying DESIGN has no remaining MUST-FIX
defect.** It found 2 MUST-FIX documentation-consistency defects instead — stale bullets in §9's own
"resolved decisions" rollup that were never updated when §4a/§4b/§8 were fixed for round 9 — plus one
MINOR cosmetic note and one worthwhile strengthening, both addressed.

1. **MUST-FIX, documentation only** — §9 item 5 still described `AgentSession::run_model_call()` calling
   `chat_client_->chat(...)` directly as this adapter's own live call path, contradicting §4b's own text a
   few sections earlier ("moot after round 9: `chat()` no longer exists on this type"). Re-derived
   `agent_session.hpp:1690-1770` directly (not trusting either section's own citations) and confirmed the
   `if constexpr` gate on `chat()`'s existence is TYPE-LEVEL, always false for a `chat()`-less
   `WorkflowChatClient`, so execution unconditionally falls through to draining `chat_stream()` — §4b was
   right, §9 item 5 was stale. **Fixed**: item 5 rewritten to state the call path is now structurally
   unreachable, matching §4b.
2. **MUST-FIX, documentation only** — §9 item 7 (I4/I5 recording/replay) still read "RESOLVED... not
   broken" with no mention of round 9's real, disclosed `RecordingChatClient` narrowing that §8 itself
   already documents a few sections earlier — the same class of drift as finding 1, an editing pass that
   fixed the section describing the mechanism but not the section summarizing it. **Fixed**: item 7
   rewritten to match §8.
3. CONFIRMED SOUND, re-derived directly rather than trusted — `chat()`'s removal is mechanically correct:
   `WorkflowChatClient` (capabilities()+chat_stream() only) satisfies `ChatClient`'s exact `requires`
   clause (`core/chat_client.hpp:205-209`); `AgentSession`'s fallback to `drain_streaming_response()`
   triggers unconditionally regardless of `stream_model_calls_` (confirmed against
   `agent_session.hpp:1690-1770`/`agent_session_trust.hpp:87-136` directly); the `RecordingChatClient`
   consequence is the COMPLETE fallout of removing `chat()` (grepped `LegacyChatClient` across the whole
   tree — only `RecordingChatClient`'s own gate and one unrelated production call site wrapping
   `OpenAIChatClient`/`AnthropicChatClient`, both unaffected — no second missed mechanism).
4. MINOR — a vestigial "`AgentSession` calls `chat()` again automatically" phrase survives in §4a's
   historical explanation of the already-rejected `ToolCall`/`ToolResult` design (illustrating a hazard
   that would occur identically whether the continuation call is `chat()` or `chat_stream()`) — cosmetic,
   doesn't affect the finding's substance, left as-is.
5. Worthwhile strengthening, added to §8 — round 10 noted the draft never explicitly considered and
   rejected "add a stub `chat()` back just to satisfy `LegacyChatClient`," and that the reason is stronger
   than intuition: `AgentSession`'s dispatch is a pure type-level `if constexpr` that never inspects what
   `chat()` does, so ANY `chat()`, however stubbed, would flip `AgentSession` back to preferring it over
   the safe `chat_stream()` fallback — mechanically reintroducing round 8's exact defect, not just
   "probably worse." **Added**: two sentences to §8 making this explicit, so a future round doesn't
   re-propose it.

Round 10's own overall assessment, stated directly and without hedging: **the design has no remaining
MUST-FIX defect.** Ten rounds, 18 design-level MUST-FIX findings (all fixed), and both stale-documentation
findings this round found were fixed the same pass. This document is genuinely implementation-ready,
pending the usual "prove" phase (writing the code and the tests §11 already names) rather than further
design-level red-team rounds.

## 11. Files (planned, none written yet)

- **New**: `include/agentengine/rt/workflow_as_chat_client.hpp` (`WorkflowChatClient`)
- **New** (or an addition to `workflow_supervisor.hpp` directly): `open_interaction_asks()` (§5)
- **New**: `tests/test_rt_workflow_as_chat_client.cpp` — at minimum, per rounds 4-9's own findings (named
  here so they don't only live in test-writer's memory of §10): (a) the message-flattening envelope
  round-trips through `message_to_json()`/`message_from_json()` and a wrapped `start` executor actually
  receives what the caller intended (§9 item 2's own priority note — no runtime shape-check catches a
  wrong design here, this test is the only thing that does); (b) a suspended run with N > 1 open
  interactions produces N `ChatResponseUpdate` pushes that a direct caller draining `chat_stream()`
  correctly receives in order, reassembling one ordered `Message.content` if it chooses to (§10 round 6);
  (c) the envelope's size-threshold/`BlobRef`-promotion path actually fires and fails closed with no
  `blob_sink` configured (§10 round 5 finding 2/round 6 finding 2); (d) `WorkflowChatClient` bound to an
  `AgentSession` (any configuration — `stream_model_calls_`/`.token_budget()` no longer matter, per round
  9's removal of `chat()`) fails the whole `AgentSession::run()` via `run_model_call()`'s pre-existing
  `chat()`-absent fallback and `run.usage_unavailable` — this IS the intended, disclosed behavior (§9
  item 1's third descoped composition), not a regression to prevent; (e) a caller answering M of N open
  interactions in one call (partial answers, §4a's resolved open question B) resolves correctly across a
  suspend/resume round-trip; (f) a `ctx_copy`d `EffectContext` passed into `chat_stream()`'s detached
  worker (§8) never carries a live `bound_capabilities`/`sandbox_fs`/`report_progress`/`agent_turn_sink`/
  `moderator_delta_sink` from the caller's own frame — e.g. construct a `chat_stream()` call from inside a
  `Tool<>::invoke()` body (a real, supported pattern per §10 round 7 finding 2's own trace of
  `invoke_tool()`'s bracketing) with a stack-local `bound`/`FileSystemAdapter`, let the call return,
  destroy the stack-locals, and confirm the detached worker never dereferences anything stale;
  (g) **new after round 9, replaces the earlier round-8-shaped test** — a DIRECT caller (not through
  `AgentSession` at all) draining `chat_stream()` sees NO failure and NO fabricated `Usage` — every pushed
  `ChatResponseUpdate::usage` is honestly `nullopt`, and the call otherwise completes normally — the
  direct regression test for round 9's own finding 1 (round 8's fix had broken exactly this case).
- **New**: `examples/28_workflow_as_chat_client.cpp` (next free number as of this draft — confirm at
  implementation time) — a wrapped `Workflow`
  containing a `request_port` node, driven through two `chat_stream()` calls (open, then answer, each
  drained to its terminal update), proving the round-trip end to end, offline.
