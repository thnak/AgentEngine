# ADR-073 — Unified streaming: gateway `call_stream()`, tool-call argument/result streaming, `EffectContext::report_progress` widening, `Bundle::ask_stream()`

**Status:** Judged (design → red-team → prove phases complete; implemented and tested, including one
live-network exchange; approved by project owner, 2026-08-22).

**Relates to:** `docs/planning/unified-streaming-design-draft.md` (the full working document — five
adversarial red-team rounds, seven revisions, every finding and its fix in detail; this ADR is the
compressed, judged record, not a replacement for it), `docs/research/2026-08-22-tool-call-streaming-
validation-landscape.md` (external survey this design maps against), `decisions/ADR-034-agentsession-
streaming-turn-loop.md` (`stream_model_calls_`, the existing toggle this design extends rather than
replaces), `decisions/ADR-036-model-call-gateway.md` (`ModelCallGateway`/`ModelCallGatewayLike`, the
type this design adds `call_stream()`/`ModelCallGatewayStreamLike` to), `decisions/ADR-060-tool-call-
progress-reporting.md` (`EffectContext::report_progress`, the field this design widens from
`std::string_view` to `ContentItem`, and the methodology model — numbered findings, severity, direct
source reading — every red-team round in this pass followed), `013-UI-and-Streaming-Surfaces.md` (the
`RunEvent` vocabulary this design's producers/projections extend).

## 1. The question

**Stated so it has a wrong answer:** commit a30747d and the streaming-gap tracking doc named four
independent streaming limitations — a gateway-typed `AgentSession` cannot stream at all, a tool call's
arguments are invisible while the model is still generating them, a tool call's result content never
reaches the live event stream, and the quickstart `Bundle` facade has no streaming entry point.
Alongside them, a fifth, related gap: `EffectContext::report_progress` could only carry plain text, with
no way for a tool author or host app to push structured or app-defined content through the same channel.
Can all five be closed as one coordinated design, reusing this codebase's own established idioms
(discriminated unions for "one of several kinds," the `output_schema_strategy` degradation pattern,
`stream<T>`/`stream_producer<T>`'s existing channel machinery) rather than five separate, uncoordinated
patches — without reopening I2/I3, without silently substituting a mid-stream backend the way 004 §4
forbids, and without weakening any invariant three prior ADRs (034/036/060) already established?

## 2. The design (five pieces)

Full type-level detail lives in `docs/planning/unified-streaming-design-draft.md` §1-§5; summarized here:

- **Piece B — tool-call argument streaming.** `run_event_payload::ModelDelta` becomes a real
  discriminated union (`std::variant<ModelTextDelta, ModelToolCallArgumentDelta>`), matching this
  codebase's own `ContentItem::value` idiom rather than a bolted-on optional field.
  `core::ChatResponseUpdate` gains an additive `std::optional<ToolCallArgumentChunk>
  tool_call_argument_chunk` field (following that type's own established `usage`-field precedent). Both
  the OpenAI and Anthropic backends' `StreamingUpdateAccumulator`s push a companion update as a tool
  call's arguments grow, alongside their existing full-buffer accumulation — the fragment is never
  engine-side repaired or parsed; `finish()` remains the one real `json::parse`, unchanged. Anthropic's
  wire protocol gives a real per-index completion boundary (`content_block_stop`), so its fragment-level
  `is_final` is a resolved answer; OpenAI's has no equivalent, left open.
- **Piece C — tool-call result streaming.** `run_event_payload::ToolCallFinished` is widened to carry
  the real `agentengine::ToolResult` (`ok` now derived as `!result.is_error`) instead of a bare bool —
  merged into the existing event rather than adding a sibling, since both would fire at the identical
  instant from the same call site.
- **Piece E — `EffectContext::report_progress` widening.** The field's signature changes from
  `std::function<void(std::string_view)>` to `std::function<void(ContentItem)>`, giving a tool author
  the engine's whole existing content vocabulary (plain text, a structured `Data` fact, or a namespaced
  `Custom` payload for anything app-specific) rather than a bespoke shape. Every real bind site forces
  `tainted = true` recursively (through any nested `ToolResult`) before constructing the event — a tool
  never gets to mark its own pushed content trusted.
- **Piece A — `ModelCallGateway::call_stream()`.** A new method mirroring `ChatClient::chat_stream()`'s
  own contract exactly (`stream<ChatResponseUpdate>`, a plain non-coroutine function backed by a
  detached thread), not a `task<>` the caller awaits while pushes happen on the same chain. Internally:
  a commit-gated tier loop — chunks are pushed live to the caller as they arrive; `any_pushed` tracks
  whether anything has been shown yet; a failure before that stays invisible (retry/failover proceeds
  exactly as `call()`'s own buffered path), a failure after is terminal (no retry, no fallback tier),
  matching 004 §4's "no silent mid-stream backend substitution" rule via a different, streaming-capable
  path rather than relaxing it. `ModelCallGatewayStreamLike` is a new, separate, optional concept
  (`ModelCallGatewayLike` itself is unchanged) — `ModelCallGateway` satisfies both;
  `MiddlewareModelCallGateway`/`ContentReplayGateway` satisfy only the original, a named, disclosed
  residual (neither gains streaming in this pass). `AgentSession::run_model_call()`'s dispatch and
  `start_run()`'s own gateway warning are both gated on `stream_model_calls_ &&
  ModelCallGatewayStreamLike<ChatClientT>`, nested via `if constexpr` (a runtime `if` inside `if
  constexpr` does not prune the unreached branch — a real, caught-before-ship compile break, §4).
- **Piece D — `Bundle::ask_stream()`.** Reuses `Bundle::ask()`'s own proven bounded-single-`resume()`
  contract (never a resume loop — a resume loop is what reintroduced the exact `session_mutex_`
  double-resume hazard this file already found and fixed once for `ask()`), moved onto a background
  driver thread, paired with a second relay thread that drains the session's one persistent
  `enable_event_stream()` consumer (created lazily, cached — that API is single-call, a real design
  point the original sketch never named) and relays `ModelTextDelta` fragments into the caller-facing
  `stream<std::string>`, stopping on its own run's terminal event rather than the shared stream's
  `.done()` (which never fires — confirmed by grep, that producer is session-scoped and outlives any
  single run).

## 3. The red-team attack

**Five independent adversarial rounds ran against this design before any of it was judged** — three
against Pieces B/C/E, two against Pieces A/D — each a fresh agent with no authorial stake in the prior
round's fixes, grounded in direct reads of the real, current tree rather than trusting either the design
doc's own citations or the previous round's corrections. Full finding-by-finding detail lives in
`docs/planning/unified-streaming-design-draft.md`; the pattern and the load-bearing findings are recorded
here.

**Round 1 (Pieces B/C/E, Rev 3):** 7 MUST-FIX findings. The headline: the draft's own "confirmed by
direct grep" blast-radius claims were incomplete in three separate places (real production emission
sites and protocol projectors missed), one design gap was genuinely unresolved (`ChatResponseUpdate`
plumbing for Piece B had no real vessel for an in-progress tool-call fragment), and one invariant was
asserted in prose but not enforced in code (Piece E's taint rule).

**Round 2 (Pieces B/C/E, Rev 4 — attacking Round 1's own fixes):** 4 more MUST-FIX findings, not
convergence. The Round-1 fix for the `ChatResponseUpdate` plumbing had no `is_final` slot, making it
structurally dead; the Finding-7 taint fix (`item.tainted = true`) didn't recurse into a nested
`ToolResult`; 5 more test call sites were missed for `ToolCallDelta`'s type change.

**Round 3 (Pieces B/C/E, Rev 5 — attacking Round 2's own fixes):** the most architecturally significant
finding of the whole design: Piece B's entire producer-side design had been silently scoped to the
OpenAI backend only, through two full prior rounds — Anthropic's own independent
`StreamingUpdateAccumulator` was never named. Extended to cover both backends; Anthropic's real
`content_block_stop` completion signal resolved its own `is_final` question in the process. Plus 3 more
missed `ModelDelta` call sites — the identical "incomplete migration list" defect class, a third time.
**Decision made here**: stop running further read-only rounds chasing migration-list completeness
specifically (that class recurred three times and is exactly what real compilation catches
mechanically and exhaustively) — move to `prove` for B/C/E, while still treating any genuinely
architectural finding (the Piece-B-Anthropic-gap class) as worth a targeted look if `prove` surfaces one.

**Round 4 (Pieces A/D — their first pass, none before this):** found the pre-`prove` sketch could not
have been implemented as written. Three MUST-FIX findings: `call_stream()`'s own sketch signature named
a type (`ChatEvent`) that does not exist anywhere in the tree; the sketch had `call_stream()` as a
`task<>` the caller awaits while pushing chunks synchronously on the same chain — a real deadlock risk
past the channel's default 256-item capacity, since `push()` genuinely blocks until a consumer drains it,
with no described draining mechanism on a different thread; Piece D's `std::jthread` "resume loop" sketch
reintroduced the exact `session_mutex_` double-resume hazard `Bundle::ask()` was built to avoid. Plus two
real, unresolved concept-widening questions (does `call_stream()` become required or optional on
`ModelCallGatewayLike`, given two existing wrapper conformers with no `call_stream()` at all) and one
scope-inflation finding (the I5-recording/cost-attribution claims cited a `StreamAttemptRecord` mechanism
that does not exist anywhere in the real tree).

**Round 5 (Pieces A/D, attacking Round 4's fixes — a second, independent agent):** confirmed the central
architectural fixes hold up under direct re-verification (the `chat_stream()`-shaped `call_stream()`, the
separate `ModelCallGatewayStreamLike` concept, and the narrowed I5/cost claim). Found one real,
reproducible compile break in the redesigned dispatch (the `if constexpr` nesting bug named in §2 above)
against real, currently-passing tests; one stale warning-emission site the dispatch fix never touched
(`start_run()`'s own gateway warning is a separate call site — fixing only `run_model_call()`'s dispatch
would have left it asserting the old, now-false "no live model_delta events" claim forever); and one
unstated `CircuitBreaker` cross-thread-ownership invariant (real, but almost certainly already safe under
I1's own single-executor guarantee — named explicitly rather than left implicit). All three fixed and
folded back in; the reviewer's own recommendation — narrow, mechanical fixes, not a redesign — moved
straight to `prove` rather than a sixth round.

## 4. Executed evidence

**Compile.** Every piece compiles clean against the real tree. Pieces B/C/E: a full test/tools/examples
rebuild (818 targets, MSVC/Ninja) succeeded with zero failures. Piece A: a targeted rebuild (308 targets
touched) succeeded with zero failures, including the exact test binaries Round 5 named as the ones the
dispatch bug would have broken (`test_rt_agent_session_content_replay`,
`test_rt_agent_session_turn_and_replay_composition`, `test_model_call_gateway`,
`test_middleware_model_call_gateway`, `test_content_replay_gateway`, `test_rt_model_call_gateway_session`,
`test_rt_agent_session_streaming_and_events`). Piece D required a second build tree
(`-DAGENTENGINE_WITH_HTTPS=ON`, MbedTLS vendored) — closing that gap also closed two other previously-
disclosed compiler-verification gaps in the same pass (the OpenAI/Anthropic translation tests).

**Real bugs `prove` caught that five red-team rounds did not:**
- A stale `report_progress` lambda signature (`std::string_view`, pre-dating Piece E's widening) in a
  concurrency-hazard-regression test, not a migration-list grep target.
- A double-move-of-`ToolResult` hazard in the original Piece C sketch — both the `tool_call_finished`
  event and the tool's own history record needed the same value; fixed by copying into the event, moving
  into history.
- `Bundle::ask_stream()` never called `session_->set_stream_model_calls(true)` — without it,
  `run_model_call()`'s dispatch would never reach `call_stream()` at all, and `ask_stream()` would have
  silently returned an empty text stream regardless of what the model said. Found and fixed before any
  test exercised the method — its own body had never been compiler-instantiated before this pass, since
  C++ template member functions are lazily instantiated and no prior test called it.
- `test_anthropic_chat_client_translation.cpp`'s E-STREAM-R1 case had a stale expected-update count from
  an earlier, hand-traced (not compiler-verified) edit. Traced `sandbox::SseEventFramer::feed()`'s real
  framing behavior directly (splits on every `"\n\n"`/`"\r\n\r\n"` — one block per SSE record) to compute
  the correct sequence; corrected, now passes.

**New tests, not just non-regression.** `test_model_call_gateway.cpp` gained G9-G11: G9 proves
`call_stream()`'s basic live-push success path reconstructs identically to `call()`'s own buffered path;
G10 proves the commit gate's invisible-retry half (mirrors G2, via the streaming entry point); G11 is the
commit gate's own core claim, previously unproven by any test — a primary attempt that pushes one live
chunk and THEN fails is terminal, the fallback is NEVER attempted, proven with a real assertion
(`fallback.call_count() == 0`) that would fail if the gate didn't actually gate.
`test_rt_model_call_gateway_session.cpp` gained G6: the same gateway type, `stream_model_calls_(true)`,
proving real `model_delta` events fire end-to-end through `AgentSession`'s corrected dispatch, not just
that it compiles.

**Live network.** A new test, `tests/test_session_builder_openrouter_live_e2e.cpp` (`live-network` label,
env-var-gated per 018 §4, credentials never compiled in), is the first test ever to exercise
`Bundle::ask()`/`Bundle::ask_stream()` at all — run against real OpenRouter:
- QS-1 (`Bundle::ask()`): succeeds end to end — `QuickstartSessionBuilder` → `Bundle` → a real
  `ModelCallGateway<OpenAIChatClient<...>>` → `AgentSession`, over real DNS/TLS. Reply: `"pong"`.
- QS-2 (`Bundle::ask_stream()`, Piece D's own core claim): succeeds, reaches a clean terminal, 4 real
  live text chunks pushed through the driver/relay thread pair (`"one two three four"`) — direct,
  live confirmation that `call_stream()` genuinely gets used, not a silent fallback to the buffered path.
- QS-3 (positive control): a syntactically valid but wrong API key is rejected by the real service,
  classified `failure_class::policy`, surviving translation through the full
  `ModelCallGateway::call()` → `AgentSession::run_model_call()` → `Bundle::ask()` stack.

All three pass. Full `ctest` sweep (default `build/` tree, non-HTTPS): 196/196 passed, 0 failed.

## 5. Named residuals (disclosed, not solved by this ADR)

- **OpenAI's per-fragment `is_final` for tool-call arguments** remains genuinely open — no equivalent
  completion boundary exists on that backend's wire protocol (only an unparsed `index`-transition
  convention or a trailing `finish_reason`).
- **`chat_recording.hpp` does not round-trip `tool_call_argument_chunk`** — sequenced after the OpenAI
  `is_final` question above is settled, not written until then. A separately-confirmed, pre-existing gap
  in the same file (`ChatResponseUpdate::usage` also doesn't round-trip) is named but not fixed here.
- **`MiddlewareModelCallGateway`/`ContentReplayGateway` do not gain `call_stream()`** in this pass — a
  session built on either wrapper keeps today's exact behavior (the original blanket warning, no live
  `model_delta`) when streaming is requested. Whether either wrapper needs its own forwarding
  `call_stream()` is a real product question, not decided here.
- **I5 replay / cost-attribution for `call_stream()` specifically** is scoped to the one concrete
  guarantee that already exists (`call()`'s own fail-closed-on-missing-usage rule, applied identically).
  Per-tier attempt cost ledgers and `RecordingChatClient`-style replay support for the streaming gateway
  path do not exist — whether `call()` itself already has an equivalent gap is a real question for a
  later pass to check, not assumed either way here.
- **`call_stream()`'s detached thread captures `this`** — a real lifetime contract, disclosed rather than
  structurally enforced (the established `weak_ptr` fix used elsewhere in this codebase for the identical
  hazard class does not apply without a larger ownership change to `ModelCallGateway`): the caller must
  ensure the gateway (and its owning `AgentSession`) outlives every in-flight `call_stream()` call.
- **A `Bundle` must not be moved while `ask_stream()` is active on it** — the driver/relay threads
  capture `this`; moving the `Bundle` elsewhere leaves them holding a dangling pointer once the old
  object is destroyed. Narrow in practice (`Bundle` has no move-assignment; real usage is "build once,
  keep in place"), disclosed rather than prevented.
- **006-Tool-and-Function-Plane.md §6a's `report_progress(ProgressUpdate)` naming** does not match this
  ADR's `ContentItem` choice — a documentation-only RFC amendment or a real scope question, not resolved
  here.
- **A tool that pushes `Media{raw_bytes}` (not a `BlobRef`) as progress content** gets those bytes
  base64-inlined into the live event stream — pre-existing codec behavior, not new to this design, named
  as authoring guidance (prefer `BlobRef` for large media in a progress channel).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| Pieces B/C/E's redesigned types are correct and complete against the real tree | **CORRECT, after 3 rounds of correction** | Rounds 1-3 findings, all fixed; full 818-target rebuild, 0 failures |
| Piece A's `call_stream()` commit gate actually gates (no fallback once anything has been shown) | **CORRECT** | `test_model_call_gateway.cpp` G11: `fallback.call_count() == 0` after a post-commit failure, a real, falsifiable assertion |
| Piece A's dispatch reaches `call_stream()` at runtime, not just compiles | **CORRECT** | `test_rt_model_call_gateway_session.cpp` G6: real `model_delta` events observed, joined text matches accumulated result |
| Piece D's `ask_stream()` is safe against the double-resume hazard `ask()` was built to avoid | **CORRECT** | Reuses the identical bounded-single-`resume()` contract, proven safe under contention by `ask()`'s own prior evidence; architecture confirmed by Round 5 |
| Piece D actually streams live text through the full quickstart stack | **CORRECT** | `test_session_builder_openrouter_live_e2e.cpp` QS-2, live network: 4 real chunks, clean terminal |
| No regression elsewhere in the tree | **CORRECT** | Full `ctest`, 196/196 (default tree); `build-https` tree's own targeted suite passes; both translation tests pass after their real, `prove`-caught fixes |

**Overall verdict: this design, as corrected across five adversarial rounds and proven against the real
tree — including one genuine live-network exchange — is CORRECT and BUILDABLE.** The residuals in §5 are
real and disclosed, not blocking: none of them were found to compromise I2/I3/I5 or reopen a hazard any
prior ADR (034/036/060) had already closed.
