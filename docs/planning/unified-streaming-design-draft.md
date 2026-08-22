# Unified streaming design — gateway `call_stream`, tool-call argument/result streaming, `Bundle::ask_stream()`

**JUDGED (2026-08-22) — see `decisions/ADR-073-unified-streaming-gateway-call-stream-tool-argument-
result-streaming-ask-stream.md` for the compressed, judged record.** This document remains the full,
uncompressed working history (every finding from all five red-team rounds, every revision) — the ADR is
the authoritative decision record; this doc is its evidence trail.

**Status:** Design pass, revised seven times. **Rev 2 deliberately drops the "stay additive, avoid touching
existing call sites" constraint Rev 1 imposed on itself** — project owner directive (2026-08-22):
prioritize the clean, coherent shape even where it means changing an existing type and naming a real
migration, not working around it to minimize diff. **Rev 3 folded in a first adversarial red-team pass
against §1/§2/§5** — 7 MUST-FIX findings, 2 confirmed-clean, 2 minor. **Rev 4 folded in a SECOND pass
attacking Rev 3's own corrections** — 4 more MUST-FIX (14, 17, 24, 25), one citation nit (21), 8
confirmed-clean. **Rev 5 folds in a THIRD pass attacking Rev 4's corrections** (a third fresh agent,
independent of the first two, same ADR-060 §4 discipline) — found real, NEW problems again, not
convergence: the most significant, Findings 26/27, found Piece B's entire producer-side design was scoped
to the OpenAI backend only, with Anthropic's own independent `StreamingUpdateAccumulator` never named
anywhere through three rounds — now extended to cover both backends, with `is_final` semantics
correspondingly split per-backend (Anthropic's answer is now known; OpenAI's remains open). Plus Finding
28 (a sequencing fix — the `chat_recording.hpp` codec can't be written until per-backend `is_final` lands),
Finding 34 (3 more missed `ModelDelta` sites — the same recurring blast-radius-completeness defect a third
time), and two minor corrections (32, 35). All folded in below. §3/§4 (Pieces A/D) still have **not** been
red-teamed at all. **Rev 5's own corrections have not been re-attacked** — three consecutive rounds have
each found real problems in the previous round's fixes; the recurring "incomplete migration list" pattern
across all three rounds is judged, at this point, to be the kind of defect real compilation ("prove") will
catch mechanically and exhaustively — more reliably than a fourth read-only pass — so the plan going
forward is to move to `prove` next, not run a fourth red-team round on migration-list completeness
specifically, while treating any genuinely architectural finding (the Finding 26/27 class) as still worth
a targeted look if `prove` surfaces something similar.

**`prove` (2026-08-22): Pieces B, C, and E are implemented against the real tree and compile clean.**
Real, not self-assessed: the full test/tools/examples build (818 targets, MSVC) succeeded with zero
failures after every edit below; the 6 directly-exercised test binaries (`test_rt_agui_projection`,
`test_mcp_progress`, `test_a2a_streaming`, `test_agent_session_tool_call_progress`,
`test_rt_agent_session_background_task`, `test_rt_agent_session_streaming_and_events`) run clean (0
`FAIL:` lines). Real compilation caught what three read-only red-team rounds did not, exactly as
predicted: one MORE `report_progress` bind site with the old `std::string_view` lambda signature
(`test_agent_session_tool_call_progress.cpp:361`, inside a hazard-regression test, not a migration-list
grep target), and one place where the design's own sketch would have double-moved a `ToolResult` (both
the `tool_call_finished` emission AND the tool's own `results.push_back()` needed the same value — fixed
by copying into the event, moving into history, at all three affected sites). `test_openai_chat_client_
translation.cpp`'s D2-R4 case needed a real behavioral update (3→5 expected updates), not just a
compile fix, since Piece B is exactly a new, real capability that test exercises.

**Residual, honestly disclosed, not solved here:** `test_openai_chat_client_translation.cpp` and
`test_anthropic_chat_client_translation.cpp` are gated behind `AGENTENGINE_VENDOR_MBEDTLS`, not enabled
in the build used for `prove` — those two edits (the OpenAI/Anthropic `StreamingUpdateAccumulator`
companion-emission paths, and the D2-R4 test rewrite above) were traced by hand against the real JSON
fixtures instead of compiler-verified. They reuse the identical `json::Value` accessor patterns already
proven elsewhere in those same files, and the type-level plumbing they depend on (`ChatResponseUpdate::
tool_call_argument_chunk`, `ToolCallArgumentChunk`) is the same type real compilation DID verify at
every other call site — but this is a real, named gap in `prove`'s coverage, not silently claimed as
covered. Enabling `AGENTENGINE_VENDOR_MBEDTLS` to close it is separate follow-up, not done in this pass.

**Rev 6 (2026-08-22): Pieces A and D redesigned after their FIRST adversarial red-team pass** — B/C/E had
three rounds before implementation; A/D had none, and the first one found their Rev 2-5 sketch could not
have been implemented as written (a referenced type, `ChatEvent`, that does not exist anywhere in the
tree; a real deadlock risk in `call_stream()`'s sketch with no described draining mechanism; and Piece D's
`std::jthread` "resume loop" reintroducing a `session_mutex_` double-resume hazard `session_builder.hpp`'s
own top comment already documents finding and fixing once). §3/§4 below are fully rewritten: `call_stream()`
now returns `stream<ChatResponseUpdate>` backed by a detached thread, mirroring `ChatClient::chat_stream()`'s
own already-proven contract shape instead of a new `task<>`-based one; `ask_stream()`'s driver reuses
`ask()`'s own proven bounded-single-`resume()` contract instead of a loop, paired with a second relay
thread; `call_stream()` becomes an optional, separately-checked concept rather than widening
`ModelCallGatewayLike`; and the I5/cost-attribution claims are narrowed to what's actually real in this
tree today rather than citing machinery (`StreamAttemptRecord`) that doesn't exist.

**Rev 7 (2026-08-22): a 5th adversarial pass (second independent agent) reviewed Rev 6 and CONFIRMED its
central architecture** — the `chat_stream()`-shaped `call_stream()`, the separate
`ModelCallGatewayStreamLike` concept, and the narrowed I5/cost claim all re-verified correctly against real
source, not re-trusted. It also found **one guaranteed, reproducible compile break** in Rev 6's own
dispatch sketch (a runtime `if` nested inside `if constexpr` does not prune the unreached branch — real,
currently-passing tests instantiate the exact `ChatClientT`s that would break), a stale warning event Rev 6
never touched (`start_run()`'s own gateway warning, a separate emission site from the dispatch fix — the
same "recurring incomplete migration list" defect class found four times now across this whole document),
and an unstated `CircuitBreaker` cross-thread-ownership invariant. All three folded into Rev 7 below,
plus a reasoning correction (not an architecture change) to Piece D's `ask_mutex_` explanation. **Per the
5th pass's own explicit recommendation — these are narrow, mechanical fixes, not a redesign — the plan is
to move to `prove` next for Pieces A/D, not run a 6th broad round.**

**`prove` (2026-08-22): Piece A is implemented and compiles clean; Piece D is implemented but NOT
compiler-verified — a real, disclosed gap, not silently claimed as covered.** `core/model_call_gateway.hpp`
(`call_stream()`, `stream_tier<Tier>`, `stream_attempt_with_retry()`), `core/chat_client.hpp`
(`ModelCallGatewayStreamLike`), and `rt/agent_session.hpp` (the corrected nested-`if constexpr` dispatch,
`drain_streaming_response()` shared by both the gateway and non-gateway streaming paths, `start_run()`'s
split warning) all compile clean — confirmed by a real rebuild (308 targets touched, zero failures) and by
running exactly the test binaries the 5th red-team pass named as the ones Rev 6's own dispatch bug would
have broken (`test_rt_agent_session_content_replay`, `test_rt_agent_session_turn_and_replay_composition`,
`test_model_call_gateway`, `test_middleware_model_call_gateway`, `test_content_replay_gateway`,
`test_rt_model_call_gateway_session`, `test_rt_agent_session_streaming_and_events` — all pass, 0
`FAIL:` lines). **Two real, additional findings surfaced during this implementation that neither red-team
pass caught**, both fixed in code and folded back into §3/§4 above: (1) `call_stream()`'s detached thread
capturing `this` has its own use-after-free-adjacent lifetime contract (disclosed, matching `background_
task()`'s own established `weak_ptr` pattern isn't applicable here without a larger ownership change); (2)
Piece D's own declaration-order text (originally written into this doc) had `ask_stream_driver_`/`event_
stream_`'s order backwards relative to what "destroy the driver before the stream it reads from" actually
requires — corrected in both the code and this doc's own §4 text, plus a second, new `Bundle`-move lifetime
hazard the driver's own `this`-capture creates, disclosed the same way.

**Update (2026-08-22, later the same day): every previously-disclosed compiler-verification gap in this
whole document is now closed, including a real, live-network test.** A pre-existing `build-https` tree
(`-DAGENTENGINE_WITH_HTTPS=ON -DAGENTENGINE_VENDOR_MBEDTLS=ON`, MbedTLS already vendored and built there
from an earlier session) turned out to already exist — no new vendoring needed, just a reconfigure. This
closed three residuals at once:

- `core/session_builder.hpp` (`Bundle::ask_stream()`) **now compiles clean** — `test_session_builder_
  prototype` passes, all 14 checks. **A real bug surfaced in the process, caught by neither red-team pass
  nor hand-tracing**: `ask_stream()` never called `session_->set_stream_model_calls(true)`, so
  `stream_model_calls_` would have stayed at its default `false` — `run_model_call()`'s dispatch would
  never have reached `call_stream()` at all, and `ask_stream()` would have silently returned an EMPTY text
  stream regardless of what the model said. Fixed in `session_builder.hpp` before any test exercised it.
- `tests/test_openai_chat_client_translation.cpp` **compiles and passes clean** (the D2-R4 fix from the
  earlier B/C/E `prove` pass held up).
- `tests/test_anthropic_chat_client_translation.cpp` **found a real, second bug** the hand-trace missed:
  E-STREAM-R1's expected update count was still the OLD number. Traced `sandbox::SseEventFramer::feed()`
  directly (splits on every `"\n\n"`/`"\r\n\r\n"` — one block per SSE record, confirmed by reading the
  framer's own source, not assumed) to compute the real expected sequence (6 updates, not 3: a text delta,
  three companion argument-chunk updates in wire order, a second text delta, then the assembled tool_use).
  Corrected; now passes.

**Then a genuinely new live test was written and run against real OpenRouter**
(`tests/test_session_builder_openrouter_live_e2e.cpp`, `live-network` label, env-var-gated per 018 §4,
mirroring this suite's established live-test pattern) — the first test ever to exercise `Bundle::ask()`/
`Bundle::ask_stream()` end to end, not just compile them:
- **QS-1** (`Bundle::ask()`): succeeds against real OpenRouter — `QuickstartSessionBuilder` → `Bundle` → a
  real `ModelCallGateway<OpenAIChatClient<...>>` → `AgentSession`, over real DNS/TLS. Reply: `"pong"`.
- **QS-2** (`Bundle::ask_stream()`, Piece D's own core claim): succeeds, reaches a clean terminal, and
  **4 real live text chunks** were pushed through the driver/relay thread pair (joined: `"one two three
  four"`) — direct, live proof that `set_stream_model_calls(true)` reaches `run_model_call()`'s dispatch
  and `call_stream()` genuinely gets used, not a silent fallback to the buffered path.
- **QS-3** (positive control): a syntactically valid but wrong API key is rejected by the real service,
  classified `failure_class::policy`, surviving translation through the full `ModelCallGateway::call()` →
  `AgentSession::run_model_call()` → `Bundle::ask()` stack.

All three pass. **Every piece of this design (A, B, C, D, E) is now implemented, compiler-verified, and
has passing tests — including one real live-network exchange for Piece D specifically.** No disclosed
compiler-verification gap remains in this document.

Still **not** the full `design → red-team → prove →
judge` cycle CLAUDE.md requires before ANY of this becomes code -- `judge` (project owner sign-off) has
not happened.

**Supersedes/merges:** `docs/planning/quickstart-builder-streaming-gap.md`, `docs/planning/model-call-
gateway-routing-design-draft.md` Finding 2, `docs/planning/tool-call-argument-streaming-gap.md`,
`docs/planning/tool-call-result-streaming-gap.md`, `docs/planning/tool-call-argument-streaming-validation-
design-draft.md`, and (Piece E, added after Rev 2) `decisions/ADR-060-tool-call-progress-reporting.md`
§7's named "structured/typed progress payloads" residual. **Research:** `docs/research/2026-08-22-tool-
call-streaming-validation-landscape.md`.

**Five pieces now:** A (`ModelCallGateway::call_stream()`), B (tool-argument display streaming), C
(tool-call result content in `ToolCallFinished`), D (`Bundle::ask_stream()`), E (`EffectContext::report_
progress` becomes `ContentItem`-typed).

## Rev 2 — what changed from Rev 1, and why each change is now cleaner, not just different

| Rev 1 (additive/reuse) | Rev 2 (redesigned) | Why Rev 2 wins on its own merits, not just "less compat baggage" |
|---|---|---|
| `ModelDelta` kept `text_delta: string` unchanged, added `tool_call_delta: optional<...>` beside it | `ModelDelta` becomes a real discriminated union: `std::variant<ModelTextDelta, ModelToolCallArgumentDelta>` | Matches this codebase's OWN established idiom for "one of several kinds of thing" — `ContentItem::value` is already exactly this shape (`content.hpp:146`). A struct with an optional field bolted on is a weaker, ad hoc version of a pattern the codebase already has a strong idiom for. |
| New sibling `run_event_kind::tool_call_result`, `ToolCallFinished{call_id, ok}` left untouched | `ToolCallFinished` widened to carry the full `ToolResult` (`ok` derived as `!result.is_error`); no sibling kind | `ToolCallFinished` and the "sibling" fired at the **identical instant**, from the **same call site**, every time — two wire events for one moment isn't the `ModelDelta`/`ToolCallDelta` pattern (which are legitimately split because they fire at genuinely different times); it's unnecessary ceremony. Confirmed while re-grounding this pass: `protocol/agui/projection.hpp:111-117`'s own comment already names the gap this closes — *"No ToolCallResult: `ToolCallFinished`'s payload carries only `{call_id, ok}` ... honest scoping, not a dropped event"* — merging fixes that file's own named limitation in the same stroke a sibling event would have needed a second pass for. |
| Gateway warning made "conditional on entry point used" (`call` vs `call_stream`), a new ad hoc concept | `stream_model_calls_` (the flag that ALREADY exists and already governs streaming for non-gateway `ChatClientT`) becomes the single, uniform toggle for gateway-typed sessions too | There was already a real, working runtime toggle for "does this session want streaming" — non-gateway sessions have used it since ADR-034. Rev 1 invented a parallel "which method got called" framing instead of asking whether the EXISTING flag should simply extend to the gateway case. It should. This removes "no `model_delta` events, full stop" as a permanent structural fact for gateway-typed sessions, rather than reframing the same limitation with better words. |
| Piece D's background-thread/cancellation risk left as "flagged, not verified" | Resolved: `rt::channel<T,E>`'s `push()` provably unblocks on consumer `cancel()` (confirmed against `rt/channel.hpp`, not assumed) | Re-reading the actual channel implementation (not previously opened in this design series) answers the question outright instead of deferring it. See §5. |
| `ChatClientCapabilities`'s flat-bool shape — reused without re-examination | Reconsidered, kept as-is, on its own merits | Not every reuse is a compromise. A flat bool per independent yes/no backend fact is already the clean shape for that data — redesigning it would cost a large blast radius (every backend's `capabilities()`, every reader) for no coherence gain. Named explicitly so this isn't silent inertia. |

## 1. Piece B (redesigned) — `ModelDelta` as a real discriminated union

```cpp
// core/run_event.hpp — replaces the current struct ModelDelta { std::string text_delta; }
namespace run_event_payload {

struct ModelTextDelta {
    std::string text;
};

struct ModelToolCallArgumentDelta {
    std::string call_id;
    std::string tool_name;           // present on the fragment that opens the call
    std::string arguments_fragment;  // raw bytes received in THIS delta, not the accumulated total
    bool        is_final = false;    // true on the fragment that completes the call
};

struct ModelDelta {
    std::variant<ModelTextDelta, ModelToolCallArgumentDelta> value;
};

}  // namespace run_event_payload
```

The fragment stays raw (no engine-side best-effort JSON repair) — presentation is a projection concern,
per the research doc's own conclusion (class B: `jiter`-style partial parsing is a display-layer helper,
never baked into the producer).

**Real, named migration cost — CORRECTED after red-team (Findings 1-3): Rev 2's claim of "one call site,
the entire blast radius" was false.** A fresh, independent grep of the whole tree (not trusting the
earlier citation) found:

1. `tools/cli_chat.cpp:927-928` — the test-rendering consumer already named.
2. **[Finding 1, MUST-FIX] `rt/agent_session.hpp:1497`** — the REAL, shipped, production emission site
   (ADR-034's non-gateway streaming drain loop, inside `run_model_call()`), missed entirely by Rev 2:
   `emit_run_event(run_event_kind::model_delta, run_event_payload::ModelDelta{t->text});`. This is the
   actual producer for every real streaming run today, not `cli_chat.cpp` — Rev 2's claim that
   `StreamingUpdateAccumulator` is "where the new producer side gets wired" skipped over the fact that
   there is already a real, working (text-only) producer, and it lives here, not in the accumulator.
3. **[Finding 2, MUST-FIX] `protocol/agui/projection.hpp:87-91`** — a real, shipped protocol projector
   (`RunEventProjector::project()`'s `model_delta` case), reading `p.text_delta` directly. Not in Rev 2's
   list at all.
4. **[Finding 3, MUST-FIX] Two test files**: `tests/test_rt_agent_session_streaming_and_events.cpp:383`
   (a real behavioral test, S1, exercising the production path in point 2 above and asserting on
   `.text_delta`) and `tests/test_a2a_streaming.cpp:112-113` (hand-constructs `ModelDelta{"text"}` as an
   A2A projection fixture). Neither was in Rev 2's list.
5. **[Finding 34, MUST-FIX, from the third red-team pass] Three MORE sites, missed by both prior rounds —
   the same recurring defect class a third time.** `tests/test_mcp_progress.cpp:73`,
   `tests/test_rt_agui_projection.cpp:195`, `tests/test_rt_agui_projection.cpp:209` — all positional
   `ModelDelta{"some text"}` constructions. Notably, two of the three are in files this draft is ALREADY
   touching for other pieces (Piece E's own migration list already names `test_mcp_progress.cpp` lines
   35/47/49/61, and Piece C/E name `test_rt_agui_projection.cpp` lines 225/111-117) — yet their `ModelDelta`
   constructions were still missed for Piece B specifically. Under the new `variant<ModelTextDelta,
   ModelToolCallArgumentDelta>` shape, a bare positional string literal does not compile unchanged.

All four sites get the same mechanical fix shape (switch a direct `.text_delta` read for a
`std::get_if<ModelTextDelta>(&value)` check), but the CLAIM that this was a one-site change was wrong —
four real sites, all found by direct re-grep, not by trusting the earlier draft's own citation.

**[Finding 17, MUST-FIX, from the second red-team pass] A fifth site, of a different kind: `core/chat_
recording.hpp` does not round-trip this data at all, and the "safe, established `usage`-precedent"
argument this whole correction leans on does not actually hold up at that seam.** `chat_recording.hpp:
482-498`'s `chat_response_update_to_json`/`_from_json` serialize `delta`/`is_final` only — **`usage` is
never round-tripped either**, confirmed by direct read, and `tests/test_chat_recording_codec.cpp:211-244`'s
own streaming round-trip test never sets or asserts it. This is a real, pre-existing, independently-
confirmed gap (not introduced by this design) feeding a real path: `write_chat_call_recording`/`read_
chat_call_recording` (`chat_recording.hpp:654-681`) round-trip `RecordedChunk`s to `ReplayChatClient`
(`replay_chat_client.hpp:93`). Piece A's own I5 discipline names exactly this seam ("nondeterminism
crosses a recorded seam") — a new `tool_call_argument_chunk` field would silently repeat the same gap:
recorded/replayed sessions would drop tool-call-argument-streaming events with no error. **Corrective
action, now part of this piece's migration** — **[Finding 28, MUST-FIX, from the third red-team pass]
sequencing correction: this fix is NOT actionable yet, not just unsketched.** `chat_response_update_to_
json`/`_from_json` must be extended to round-trip `tool_call_argument_chunk`, but a real codec can't be
meaningfully written or tested until the per-backend `is_final` semantics (Findings 26/27's split open
question) are settled — a codec written today would have to invent transitions to round-trip-test against
that the design hasn't decided yet. **Ordering, not scope, is the fix**: this codec work is sequenced
AFTER the per-backend `is_final` wiring lands, not before or alongside it, despite being listed in this
piece's own migration set. (Also disclosing, not silently fixing as a drive-by, the pre-existing `usage`
round-trip gap this pass found while checking the precedent this correction cited — confirmed by the
third red-team pass as a real oversight in `chat_recording.hpp`, not a deliberate choice, and separately
actionable outside this design's scope.)

**Wiring — CORRECTED after red-team (Finding 4, MUST-FIX): Rev 2's sketch was not type-correct; this was a
real, unresolved plumbing gap, not a paraphrase issue.** `ChatResponseUpdate::delta` (`core/chat_client.hpp:
124`) is declared `ContentItem`, not `RunEventPayload::ModelDelta` — there is no way to "push a
`ChatResponseUpdate` carrying `ModelDelta{...}`" as Rev 2 literally proposed; those are different types at
different layers. Tracing the REAL translation: `StreamingUpdateAccumulator::items_from_block()`/`finish()`
(`openai/chat_client.hpp:586-695`) only ever construct `ContentItem`s (`Text` for text fragments, `ToolCall`
once complete) inside `ChatResponseUpdate`s; the actual `RunEvent`-emitting step is
`run_model_call()`'s drain loop (`agent_session.hpp:1492-1502`), which today only branches on
`std::get_if<Text>(&upd->delta.value)` — there is no existing vessel anywhere in this path for a growing,
INCOMPLETE tool-call-argument fragment (`ContentItem::ToolCall.arguments_json` is documented and used
everywhere else as a COMPLETE, parseable buffer; reusing it for a partial fragment would be exactly the
kind of "type lie" this design series has avoided elsewhere, e.g. for `ModelDelta`/`ToolCallFinished`).

**[Findings 26/27, MUST-FIX, from the third red-team pass] This whole "Wiring" section — including the
first two correction rounds — scoped the producer side to `openai/chat_client.hpp` ONLY. AgentEngine has
TWO real backends, and Anthropic's is structurally unrelated code, never named anywhere in this draft
until this pass.** `protocol/anthropic/chat_client.hpp:713-925` declares its own, independent
`StreamingUpdateAccumulator` (a `PendingBlock`/SSE-named-events vocabulary — `content_block_start/delta/
stop`, `message_stop` — not OpenAI's `PendingToolCall`/JSON-array vocabulary). Shipping Piece B as drafted
through Rev 4 would silently give tool-call-argument streaming to OpenAI-backed sessions only, with
nothing anywhere disclosing that scope limit. **This needed a real design extension, not just a disclosed
residual — folded in below:**

- The companion emission path described above (push a `ChatResponseUpdate{.tool_call_argument_chunk =
  {...}}` as a tool call's arguments grow, leave `delta` at its no-op default) applies **identically in
  shape** to Anthropic's accumulator — the drain loop in `agent_session.hpp` is already backend-agnostic
  (it only reads `ChatResponseUpdate::tool_call_argument_chunk`, regardless of which backend produced it).
  What differs is only WHERE inside each backend's own accumulator the emission gets added — `anthropic/
  chat_client.hpp`'s `items_from_block()` needs the same companion push, keyed off its own `PendingBlock`
  vocabulary instead of OpenAI's `PendingToolCall`.
- **The `is_final` open question is answered differently per backend, not left uniformly open** (this
  pass's own finding): Anthropic's real wire protocol sends an explicit, unambiguous `content_block_stop`
  event per index — confirmed at `anthropic/chat_client.hpp:883-885`'s own comment, *"content_block_stop/
  message_stop carry no content item of their own; block completion is settled from `pending_by_index_`
  at `finish()`"* — a real per-tool-call completion boundary the accumulator already parses and currently
  discards rather than using to set `ToolCallArgumentChunk.is_final`. **OpenAI's stream has no equivalent
  explicit boundary event** (only an unparsed `index`-transition convention or a trailing `finish_reason`
  field) — genuinely still open for that backend specifically. Open question 6 (below) is now split
  per-backend rather than left as one undifferentiated deferral.
- Migration list for Piece B gains: `protocol/anthropic/chat_client.hpp`'s `StreamingUpdateAccumulator`
  (the companion emission path, symmetric with the OpenAI-side change) — named explicitly now, not a
  disclosed-but-unscoped gap.

**The real fix — additive, and correct, because it follows `ChatResponseUpdate`'s OWN already-established
extension precedent.** That type's own comment (`chat_client.hpp:126-131`) documents exactly this pattern
already: `usage` was added as an appended, optional field specifically so "every pre-existing
`ChatResponseUpdate{delta, is_final}` call site keeps compiling unchanged" — an explicit, deliberate,
author-endorsed strategy for widening this type, not a compromise being avoided elsewhere in this draft.
`ChatResponseUpdate` is a wide-blast-radius envelope (every `ChatClient` backend's `chat_stream()`
implementation constructs it) — restructuring it into a full discriminated union, the way `ModelDelta`/
`ToolCallFinished`/`ToolCallDelta` were redesigned, would mean touching every backend's streaming
implementation for no coherence gain the additive path doesn't already give; this is the one place in the
series where following the type's OWN established idiom (additive) is the clean choice, not a fallback:

```cpp
// core/chat_client.hpp — additive, following the SAME precedent `usage` already set
struct ToolCallArgumentChunk {
    std::string call_id;
    std::string tool_name;           // present on the fragment that opens the call
    std::string arguments_fragment;  // raw bytes received in THIS update, not the accumulated total
    bool        is_final = false;    // CORRECTED (Finding 14) — see below; was missing entirely
};

struct ChatResponseUpdate {
    ContentItem delta;
    bool        is_final = false;
    std::optional<Usage> usage = std::nullopt;
    std::optional<ToolCallArgumentChunk> tool_call_argument_chunk = std::nullopt;  // NEW
};
```

**[Finding 14, MUST-FIX, from the second red-team pass] The first correction's `ToolCallArgumentChunk` had
NO `is_final` field at all — the drain loop's hardcoded `/*is_final=*/false` was not a deferred answer, it
was structurally dead: there was no channel to ever carry a real value even once the "which point sets
this" open question got answered.** Fixed above by adding the field to the struct itself. The open question
(now item 6 in "Open questions carried forward") is genuinely deferrable now — the channel exists, only the
producer-side logic that decides WHEN to set it is still unresolved.

`StreamingUpdateAccumulator::items_from_block()` gains a companion emission path — as
`PendingToolCall::arguments` grows, push a `ChatResponseUpdate{.tool_call_argument_chunk = {...}}` with
`delta` left at a recognizable no-op default, alongside (not instead of) continuing to accumulate into
`pending_by_index_` exactly as today. `finish()` is unchanged — same one real `json::parse`, same
authoritative buffer, same final `ContentItem{ToolCall{...}}` construction. Zero change to the dispatch-tier
invariant (`invoke_tool()`, `tool_pipeline.hpp:460`).

The drain loop (`agent_session.hpp:1492-1502`) gains a new branch, and **one real correctness requirement
this pass makes explicit rather than leaving implicit**: when `upd->tool_call_argument_chunk.has_value()`,
the loop's existing `accumulated.content.push_back(upd->delta)` must be SKIPPED for that update (guarded
explicitly on the new field being empty, not on `delta` merely happening to default to something harmless)
— otherwise every argument-chunk update silently appends a spurious placeholder `ContentItem` into the
accumulated message:

```cpp
// rt/agent_session.hpp — run_model_call() drain loop, sketch
while (std::optional<ChatResponseUpdate> upd = s.next()) {
    if (stream_model_calls_) {
        if (auto const* t = std::get_if<Text>(&upd->delta.value); t != nullptr && !t->text.empty()) {
            emit_run_event(run_event_kind::model_delta,
                            run_event_payload::ModelDelta{run_event_payload::ModelTextDelta{t->text}});
        } else if (upd->tool_call_argument_chunk.has_value()) {
            auto const& chunk = *upd->tool_call_argument_chunk;
            emit_run_event(run_event_kind::model_delta,
                            run_event_payload::ModelDelta{run_event_payload::ModelToolCallArgumentDelta{
                                chunk.call_id, chunk.tool_name, chunk.arguments_fragment,
                                chunk.is_final}});  // CORRECTED (Finding 14) — reads the chunk's own
                                                      // is_final now that the field exists; NOT the
                                                      // same thing as this loop's own upd->is_final
        }
    }
    if (!upd->tool_call_argument_chunk.has_value()) {
        accumulated.content.push_back(upd->delta);  // unchanged for text/final-content updates; SKIPPED
                                                       // for a pure argument-chunk update (new condition)
    }
    if (upd->is_final && upd->usage.has_value()) usage = upd->usage;
}
```

**Named, not resolved: `ModelToolCallArgumentDelta.is_final` (the fragment-level "this tool call's
arguments are now complete" signal) is a DIFFERENT thing from `ChatResponseUpdate::is_final` (the
stream-level "this is the last update in the whole response" signal) — conflating them would be wrong.**
Exactly which point in `items_from_block()`/`finish()` should set the fragment-level flag (a per-block
completion signal — e.g. the SSE stream's tool-call index changing, or `finish()` itself for whatever is
still pending) was not traced to the bottom in this pass — carried forward as an open question (below),
not guessed at.

**Validation strategy — unchanged from the prior pass, reconsidered and kept:** the `output_schema_
strategy{native, tool_shaped, parse_and_repair}` idiom (`chat_client.hpp:234-252`) is the right shape to
mirror for tool-argument enforcement, and reusing it here is not a compromise — it's the same problem
(schema conformance across heterogeneous backend capability) with a proven-clean existing answer:

```cpp
struct ChatClientCapabilities { /* existing bits ... */ bool tool_argument_strict_native = false; };
struct ToolDescriptor { /* existing fields ... */ bool strict_native_requested = false; };  // NOT on ChatRequest

enum class tool_argument_enforcement_strategy { native, buffered_validate };
[[nodiscard]] inline tool_argument_enforcement_strategy select_tool_argument_enforcement_strategy(
        ChatClientCapabilities const& caps, bool strict_native_requested) noexcept {
    return (strict_native_requested && caps.tool_argument_strict_native)
        ? tool_argument_enforcement_strategy::native
        : tool_argument_enforcement_strategy::buffered_validate;
}
```

`buffered_validate`'s repair path reuses `ToolResult.is_error` (`content.hpp:118`, already set at
`tool_pipeline.hpp:374,585,754`) — on `finish()`'s accumulated arguments failing `json::parse`, synthesize
`ToolResult{call_id, content=[Error{"arguments did not parse as valid JSON"}], is_error=true}` and feed it
back as if the tool had run and failed, mirroring Anthropic's own documented `INVALID_JSON` recovery.
**Exact plug-in point (what `finish()`'s parse-failure path does today) still not confirmed — open
question, unchanged from the prior pass.**

## 2. Piece C (redesigned) — merge tool-call result content directly into `ToolCallFinished`

```cpp
// core/run_event.hpp — REPLACES struct ToolCallFinished { std::string call_id; bool ok = false; }
namespace run_event_payload {
struct ToolCallFinished {
    std::string             call_id;
    agentengine::ToolResult result;  // was: bool ok. ok is now `!result.is_error`.
};
}  // namespace run_event_payload
```

No new `run_event_kind`. `agentengine::ToolResult` (`content.hpp:115-121`, `{call_id, content, is_error}`)
is reused verbatim as the payload body — it already carries per-item taint (`ContentItem.tainted`) and
already supports out-of-line large content (`Media{BlobRef}`, `content.hpp:65-79`) if a tool populates its
result that way. No new mechanism for either concern (unchanged conclusion from the prior pass).

**Full, named migration — CORRECTED after red-team (Findings 5-6): Rev 2's own claim ("confirmed by direct
grep, not assumed") was itself not fully confirmed. A fresh, independent grep found one more real consumer
and one of the four "symmetric" emission sites is genuinely NOT mechanical.**

1. **Four emission sites**, `rt/agent_session.hpp:985-986, 1305-1306, 1614-1615, 1916-1917` — but these are
   NOT uniformly mechanical, corrected below:
   - **Sites 1, 3, 4** (`985-986`, `1614-1615`, `1916-1917`) each have a real `ToolResult` local already
     in scope (the direct return of `invoke_tool()` immediately above) — genuinely mechanical:
     ```cpp
     // before
     emit_run_event(run_event_kind::tool_call_finished,
                     run_event_payload::ToolCallFinished{audit.call_id, audit.ok});
     // after
     emit_run_event(run_event_kind::tool_call_finished,
                     run_event_payload::ToolCallFinished{audit.call_id, std::move(result)});
     ```
   - **[Finding 6, MUST-FIX] Site 2** (`1305-1306`, the background-task completion drain) is NOT
     symmetric with the other three — a real, separate migration, not a rename. Its `ToolResult` is
     currently DISCARDED, not merely absent: the production completion callback
     (`agent_session.hpp:1192-1199`, `start_background_task()`) is
     `[weak_queue, handle_id, call_id](ToolResult /*result_out*/, ToolInvocationAudit audit) mutable {
     ... q->pending.push_back(BackgroundTaskDone{handle_id, call_id, audit.ok}); }` — the `ToolResult`
     parameter is explicitly named-and-ignored, and `BackgroundTaskDone` (`agent_session.hpp:338-342`,
     `{handle_id, call_id, bool ok}`) has no field to carry it even if it weren't discarded. The real fix:
     ```cpp
     // agent_session.hpp:338-342 — widen, same convention as ToolCallFinished
     struct BackgroundTaskDone {
         std::string handle_id;
         std::string call_id;
         ToolResult  result;   // was: bool ok. ok is now !result.is_error.
     };
     // agent_session.hpp:1192-1199 — stop discarding the first callback parameter
     [weak_queue, handle_id, call_id = request.call_id](ToolResult result_out,
                                                          ToolInvocationAudit audit) mutable {
         if (auto q = weak_queue.lock()) {
             std::lock_guard<std::mutex> lock(q->m);
             q->pending.push_back(
                 BackgroundTaskDone{std::move(handle_id), std::move(call_id), std::move(result_out)});
         }
     }
     // agent_session.hpp:1305-1306 — the drain site itself, now has a real result to move
     emit_run_event_for(owner_run_id, run_event_kind::tool_call_finished,
                         run_event_payload::ToolCallFinished{m.call_id, std::move(m.result)});
     ```
     Not re-verified in this pass: whether any OTHER reader of `BackgroundTaskDone.ok` exists beyond this
     one drain site — named as unconfirmed, not assumed clear.
   `ToolInvocationAudit` (`tool_pipeline.hpp:344-351`, the separate audit-trail record) stays untouched at
   every site — a different concept from the live event payload, never conflated with it.
2. **`protocol/agui/projection.hpp:111-117`** — real, current consumer, and the direct beneficiary: its own
   comment (*"No ToolCallResult: ... honest scoping, not a dropped event"*) is exactly the gap this merge
   closes. Projection changes from `{ToolCallEnd{p.call_id}}` alone to also emitting AG-UI's own
   already-wire-typed `ToolCallResult` (per the original gap doc, `protocol/agui/types.hpp`) built from
   `p.result`.
3. **[Finding 5, MUST-FIX] `tools/cli_chat.cpp:796-799`** — missed by Rev 2 entirely. The CLI's own
   event-stream renderer: `case run_event_kind::tool_call_finished: { ... (p.ok ? "OK" : "FAILED") ... }` —
   reads `p.ok` directly; stops compiling once `ok` is gone. Fix: `!p.result.is_error ? "OK" : "FAILED"`.
4. **Two test files** — **[Finding 21, minor-correction, from the second red-team pass] one of these was
   mischaracterized.** `tests/test_rt_agui_projection.cpp` does construct `ToolCallFinished{...}` fixtures
   directly, as claimed. `tests/test_rt_agent_session_background_task.cpp` does NOT — it drives the real
   background-task path and reads `p->ok` off a genuinely produced `ToolCallFinished` event
   (`:263-264`, `finished_ok = p->ok;`), which needs updating to `!p->result.is_error` once the type
   widens. The migration cost was already correctly counted; the description of the mechanism was wrong —
   named here so this correction doesn't repeat the exact citation-imprecision class it exists to fix.

## 3. Piece A — Rev 7: a 5th adversarial pass confirmed Rev 6's architecture, found a real compile break in its dispatch sketch and one unstated concurrency invariant

**Rev 2-5's sketch was never implemented, and a fresh, independent adversarial pass (2026-08-22, the
first ever run against Piece A/D — B/C/E had three rounds by this point, A/D had none) found it could
not have been implemented as written.** Three MUST-FIX findings, not migration-list nits:

- **Finding 1**: `call_stream()`'s own sketch signature (`stream_producer<ChatEvent>&`) names a type,
  `ChatEvent`, that does not exist anywhere in this codebase (confirmed by full-tree grep) — a real,
  unresolved type gap, not a stale citation.
- **Finding 2**: the sketch had `call_stream()` as a `task<result<ChatResponse>>` the caller `co_await`s
  to completion, while pushing chunks into a channel from *inside* that same coroutine chain — but
  `channel_producer<T,E>::push()` (`rt/channel.hpp:194-199`) genuinely **blocks** the calling thread once
  the channel fills (default capacity 256, `core/stream.hpp:211`) until a consumer drains it. Nothing in
  either source doc said who drains the caller-facing stream, or on what thread, while the caller is
  blocked awaiting `call_stream()`'s own task — a real deadlock risk past 256 chunks, not a hypothetical.
- **Finding 3** (Piece D): the `std::jthread` "resume loop" sketch reintroduced a bug `session_builder.hpp`'s
  own top comment already documents finding and fixing once — a naive resume loop against a real
  `AgentSession` is only safe when `session_mutex_` (a real `rt::AsyncMutex` that PARKS coroutine handles,
  not a spinlock) is uncontended; `Bundle::ask()` is deliberately bounded to ONE `resume()`, fails closed
  otherwise, specifically to avoid this. Piece D's sketch explicitly rejected that bounded shape.

Plus two real, unresolved design questions (Findings 4/5) and one scope-inflation finding (6) — all
addressed below, not carried forward as open questions this time.

**Rev 6 (the redesign below, from that pass) then went through a 5th adversarial pass (2026-08-22, a
second fresh agent, independent of the 4th).** It CONFIRMED the central architectural fixes hold up under
direct re-verification (the `chat_stream()`-shaped `call_stream()`, the separate
`ModelCallGatewayStreamLike` concept, and the narrowed I5/cost claim — re-checked against real source, not
re-trusted) — but found **a guaranteed, reproducible compile break** in the dispatch sketch itself (Finding
4-new, below) against real, currently-passing test files, plus a stale warning event Rev 6 never touched
(Finding 5-new) and an unstated thread-safety invariant (Finding 6-new). Rev 7 (this text) folds all three
in directly, rather than listing them as open questions — the fixes are narrow and mechanical, per the 5th
pass's own recommendation to move to `prove` next rather than run a broad 6th round.

### The fix for Finding 1/2 — `call_stream()` gets `chat_stream()`'s OWN contract shape, not a new one

The deadlock only exists because the sketch tried to make ONE coroutine both (a) drive retry/failover to
completion and (b) synchronously push live chunks to a caller who is awaiting that same coroutine. Every
real `ChatClient::chat_stream()` backend in this tree already solves the identical problem a different
way: **it is not a coroutine the caller awaits at all** — it is a plain function that returns a
`stream<ChatResponseUpdate>` *immediately*, backed by a detached `std::thread` that does the real work and
pushes into the channel on its own thread, fully decoupled from whatever thread later calls `.next()`
(confirmed at `protocol/openai/chat_client.hpp:936-957`; `RecordingChatClient::chat_stream()`,
`core/recording_chat_client.hpp:167-266`, is a second, independent precedent for the exact "detached
thread relays one stream into a new one it returns synchronously" idiom this design now reuses).

`call_stream()` gets the identical shape:

```cpp
// core/model_call_gateway.hpp — ModelCallGateway<Primary, Fallback...>, NEW method
[[nodiscard]] stream<ChatResponseUpdate> call_stream(ChatRequest request, EffectContext& ctx);
```

Not `task<...>`, not `ChatEvent` (Finding 1 closed: reuse `ChatResponseUpdate`, the type every other
streaming path in this codebase already carries — including, since Piece B landed for real,
`tool_call_argument_chunk`, so a gateway-routed streaming call gets tool-call-argument fragments for
free, no separate design needed). Internals, on the detached thread:

1. Build `make_stream<ChatResponseUpdate>(...)` for the caller-facing pair; return the consumer
   synchronously, capture the producer by move into the detached thread's closure — exactly
   `RecordingChatClient::chat_stream()`'s own pattern.
2. Tier loop (Primary, then each Fallback, same recursion shape as today's `try_tier`/`attempt_with_retry`
   — no new tier-selection logic, only a new per-chunk delivery path inside it): for the current tier,
   call `backend.chat_stream(request, ctx)` and drain it. For each `ChatResponseUpdate` received, **push
   it onto the caller-facing producer immediately** (this is what makes it real streaming, not
   buffer-then-replay) and set `bool any_pushed = true` on the first successful push — this is Finding
   2's own original `any_pushed` commit gate from `model-call-gateway-routing-design-draft.md`, unchanged
   in spirit, just relocated onto a thread where blocking is safe.
3. If the tier's own stream ends `closed` with real usage (the same fail-closed-on-missing-usage rule
   `attempt_with_retry` already enforces for `call()`, `model_call_gateway.hpp:226-230` — unchanged, reused
   verbatim, not reinvented): success, stop, close the caller-facing producer.
4. If the tier's stream fails or ends without usage: apply the SAME breaker/backoff/retry-within-tier
   bookkeeping `attempt_with_retry` already has, gated on `any_pushed`: if `false`, nothing was shown yet
   — retry within the tier or fall through to the next tier, invisibly (no caller-visible failure — the
   caller's stream has received nothing at all so far, matching `call()`'s own behavior for this case
   exactly). If `true`, **terminal**: `producer.fail(...)` and stop — no retry, no fallback tier, matching
   `call()`'s existing single-attempt failure contract for that case, per a different, streaming-capable
   path (Finding 2's original framing, preserved).
5. Cancellation: the detached thread checks `producer.stop_token()` (armed automatically when the caller
   drops/cancels the returned `stream<T>`, `core/stream.hpp:130-138`) between chunks/tiers and stops early;
   a mid-`push()` cancel is separately, already safe by construction — `channel_consumer::cancel()`
   unblocks any thread parked in `push()` (`rt/channel.hpp:259-283`, re-confirmed by the 4th red-team pass
   as Finding 8, "confirmed-clean" — this specific claim from Rev 2-5 was correct, only the surrounding
   design around it was broken).

### The fix for `run_model_call()`'s dispatch — a real simplification, not just a wiring change

Because `call_stream()` now returns the SAME `stream<ChatResponseUpdate>` shape `ChatClient::chat_stream()`
already does, `run_model_call()`'s existing non-gateway streaming drain loop (the one Piece B already
extended — `agent_session.hpp`'s `while (!s.done()) { while (auto upd = s.next()) {...} }`) needs **no
gateway-specific branch inside it at all** — only the call that PRODUCES `s` differs:

**[Finding 4-new, MUST-FIX, from the 5th red-team pass] The nested-runtime-`if` sketch below does NOT
compile for `MiddlewareModelCallGateway`/`ContentReplayGateway`, and this is not hypothetical — it
breaks real, currently-passing tests.** A plain runtime `if`/`else` nested inside `if constexpr` does
**not** prune the unreached branch from template instantiation — every branch body must still be valid
code for the concrete `ChatClientT`, regardless of `stream_model_calls_`'s runtime value. Since
`MiddlewareModelCallGateway` and `ContentReplayGateway` genuinely have no `call_stream()` member,
`chat_client_->call_stream(request, ctx)` is a hard "no member named `call_stream`" error the instant
`AgentSession<ContentReplayGateway<...>>`/`AgentSession<MiddlewareModelCallGateway<...>>` is instantiated
— which real, in-tree tests do today (`tests/test_rt_agent_session_content_replay.cpp:133,167`,
`tests/test_rt_agent_session_turn_and_replay_composition.cpp:137`). **Fix: nest a second `if constexpr`**
— this codebase already has the working idiom for exactly this shape at `rt/agent_session.hpp:1721-1725`
(`if constexpr (HasProducerChatClientId<ChatClientT>) { if (chat_client_) {...} }` — the SAME precedent
Finding 4/5 itself cites for why `call_stream()` should be duck-typed in the first place; Rev 6 borrowed
the precedent's rationale but not its structure):

```cpp
// rt/agent_session.hpp — run_model_call(), Rev 7 dispatch — CORRECTED (Finding 4-new)
if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
    if constexpr (agentengine::ModelCallGatewayStreamLike<ChatClientT>) {  // compile-time, not runtime
        if (stream_model_calls_) {
            stream<ChatResponseUpdate> s = chat_client_->call_stream(request, ctx);
            // ... the SAME drain loop Piece B already wrote for the non-gateway case, unchanged ...
        } else {
            response = co_await chat_client_->call(request, ctx);  // unchanged
        }
    } else {
        if (stream_model_calls_) {
            // unchanged: today's blanket warning (see Finding 5-new below for the fix this warning
            // itself still needs) — this concrete ChatClientT is gateway-shaped but does not (and, per
            // Finding 4/5, may never) expose call_stream().
        } else {
            response = co_await chat_client_->call(request, ctx);  // unchanged
        }
    }
} else {
    // unchanged: the existing chat()/chat_stream() branch (Piece B already extended this one)
}
```

### Finding 4/5 — `call_stream()` is optional/duck-typed on the gateway shape, not a required member

The 4th pass found TWO existing `ModelCallGatewayLike` conformers beyond `ModelCallGateway` itself —
`MiddlewareModelCallGateway` (`model_call_gateway.hpp:306-325`) and `ContentReplayGateway`
(`content_replay_gateway.hpp:116-131`) — both `static_assert(ModelCallGatewayLike<Inner>, ...)`, both
expose only `call()`. Widening `ModelCallGatewayLike` itself to *require* `call_stream()` would break both
outright. The fix, mirroring a precedent already in the SAME file (`chat_client.hpp:239-245`'s duck-typed
`has_agent_description`-style detection for `Reasoning` provenance, chosen there for the identical
reason — "widening the concept would force every conformer to grow a method it has no real identity to
report"): a SEPARATE, optional concept, checked independently of `ModelCallGatewayLike`:

```cpp
// core/chat_client.hpp — NEW, alongside ModelCallGatewayLike, not a widening of it
template <class T>
concept ModelCallGatewayStreamLike = requires(T gateway, ChatRequest request, EffectContext& ctx) {
    { gateway.call_stream(request, ctx) } -> std::same_as<stream<ChatResponseUpdate>>;
};
```

`ModelCallGateway<Primary, Fallback...>` satisfies both concepts once `call_stream()` lands on it.
`MiddlewareModelCallGateway`/`ContentReplayGateway` satisfy only `ModelCallGatewayLike` — **named,
disclosed residual, not silently dropped**: a session built on either wrapper keeps today's exact
behavior (blanket warning, no `model_delta` events) when `stream_model_calls_` is set, until/unless a
later pass gives either wrapper its own forwarding `call_stream()`. This is explicitly the SAME class of
choice `chat_client.hpp:179-192`'s own comment records this codebase already made once before (Finding
9 from the 4th pass): the old `ChatClient` wrapper templates that could not cleanly support both a
buffered and a streaming method were resolved by DELETION in that case; here, because `ModelCallGateway`
itself is the only conformer either wrapper actually needs to forward to, duck-typed optionality is the
lower-cost fix — but the comparison is named explicitly, not silently assumed to be the same situation.

### Finding 6 — I5 recording / cost attribution: scope narrowed to what's real, not what was asserted

The prior sketch's `StreamAttemptRecord{tier_index, chunks_pushed, committed, outcome}` and its claimed
"extends 004 §5's cost seam" do not correspond to anything in the real tree — confirmed by grep,
`StreamAttemptRecord` appears nowhere outside the design docs themselves; `chat_recording.hpp`'s real
types (`RecordedChunk`/`ChatCallRecording`) have no tier/attempt/commit vocabulary to extend; the only
real cost-adjacent state anywhere is a plain running total (`run_tokens_consumed_`) and an empty tag
struct (`TokenBudget`) with no fields. **This piece does not introduce a new recording/cost mechanism.**
What it DOES carry over, real and unchanged: the fail-closed-on-missing-usage rule (§ above, step 3) —
the one concrete, already-implemented cost-safety guarantee `call()` has today, applied identically to
`call_stream()`. Per-tier attempt cost ledgers and I5 replay support for `call_stream()` specifically
(unlike `chat_stream()`, which `RecordingChatClient`/`ReplayChatClient` already wrap — `ModelCallGateway`
is deliberately NOT `ChatClient`-shaped, `chat_client.hpp:224-231`, so that machinery does not apply to it
directly) are a **named, disclosed gap**, not solved here — whether `call()` itself already has an
equivalent gap today is a real question for `prove` to check, not assumed either way.

### Finding 5-new [MUST-FIX, from the 5th red-team pass] — `start_run()`'s OWN gateway warning is a separate emission site the dispatch fix never touches, and it would ship stale/false

`rt/agent_session.hpp:826-832`'s `start_run()` fires this warning **unconditionally** for every
gateway-typed session, never gated on `stream_model_calls_`:

```cpp
if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
    emit_run_event(run_event_kind::warning,
        run_event_payload::Warning{"this run routes model calls through a ModelCallGateway (ADR-036): no "
            "live model_delta events fire for a gateway-routed round, ..."});
```

Once Piece A lands, a session bound to a real `ModelCallGateway<Primary>` with `stream_model_calls_ ==
true` genuinely DOES get live `model_delta` events (via `call_stream()`) — but this separate site would
keep asserting, every run, that it doesn't. This is the exact "recurring incomplete migration list"
defect class Findings 1-3/26-27/34 already found three times for Pieces B/C/E, now found once for A: the
dispatch fix in `run_model_call()` and this warning in `start_run()` are two different call sites, and
only the first was touched. **Fix: gate this warning identically to the dispatch condition**, narrowing
its text to match the real, narrower trade that remains (retry/failover only pre-commit — Finding 2's own
original framing) rather than the blanket "no model_delta events, full stop" claim:

```cpp
// rt/agent_session.hpp — start_run(), Rev 7 fix (Finding 5-new)
if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
    if constexpr (agentengine::ModelCallGatewayStreamLike<ChatClientT>) {
        if (!stream_model_calls_) {
            // no warning needed at all — call() is the buffered path, same as today
        }
        // stream_model_calls_ == true: no warning needed either — call_stream() genuinely streams now;
        // the only remaining named trade (retry/failover only pre-commit) is not a caller-visible defect
    } else if (stream_model_calls_) {
        emit_run_event(run_event_kind::warning,
            run_event_payload::Warning{"this run routes model calls through a ModelCallGateway (ADR-036) "
                "with a wrapper type (" /* MiddlewareModelCallGateway/ContentReplayGateway */ ") that does "
                "not yet support call_stream(): no live model_delta events fire for this gateway-routed "
                "round, ..."});
    }
}
```

### Finding 6-new [MUST-FIX, from the 5th red-team pass] — `call_stream()`'s detached thread shares `CircuitBreaker` state across threads, and the ownership invariant that makes this safe is never stated

`rt/circuit_breaker.hpp`'s own file banner: *"CONCURRENCY: single-writer, per-instance, no internal
synchronization... A caller sharing one instance across threads must serialize its own access; this type
does not attempt to."* `call_stream()`'s detached thread calls `breaker.on_send()`/`breaker.on_result()`
on the SAME `breakers_` vector (`model_call_gateway.hpp:293`) that `call()`'s own coroutine-driven
`attempt_with_retry()` also mutates — but where `call()`'s access happens synchronously on whatever
thread drives the session coroutine, `call_stream()` now does it from a brand-new, independently-scheduled
OS thread. **This is very likely safe** under I1 (one session, one gateway instance, never two
logically-concurrent model calls against the same gateway — `AgentSession`'s own `session_mutex_`
serializes every `run_model_call()` invocation against every other one, so at most one of `call()`/
`call_stream()`'s threads is ever actively touching `breakers_` at a time) — **but this ownership
invariant must be stated explicitly, not left implicit**, given `CircuitBreaker`'s own header explicitly
calls out that the CALLER (here, `ModelCallGateway`) is responsible for exactly this serialization.
**Named as a design-level invariant `prove`'s own tests must exercise directly** (e.g., a regression test
that a `call_stream()` in flight and a subsequent `call()`/`call_stream()` on the SAME gateway instance
never run concurrently) — not assumed safe by omission.

## 4. Piece D — Rev 7: bounded single-resume(), not a resume loop (architecture confirmed by the 5th pass, one reasoning correction)

**Requires Piece A's `call_stream()` real** for `QuickstartSessionBuilder`'s default gateway-typed
`ChatClientT` (`session_builder.hpp:234`, `ChatClientT = agentengine::ModelCallGateway<Primary>`, always
— confirmed by the 4th pass: Piece D is structurally blocked on Piece A specifically, not on
`ModelCallGatewayLike` in the abstract).

```cpp
[[nodiscard]] agentengine::result<agentengine::stream<std::string>> ask_stream(std::string text);
```

**Finding 3's fix: reuse `ask()`'s own bounded-single-`resume()` contract exactly, moved onto a background
thread — never a resume loop.** `Bundle::ask()` (`session_builder.hpp:177-195`) already has the safe
answer to "how do you drive a `task<T>` across a `co_await session_mutex_.lock()` without risking a
double-resume": call `resume()` exactly once; if `!t.done()` afterward, fail closed with a clear error
rather than resuming again. This works today because the whole coroutine chain, once past the ONE real
suspension point (`session_mutex_.lock()`, `agent_session.hpp:763`), runs to completion synchronously on
that single `resume()` call — the streaming drain loop inside it is a hand-rolled poll-with-`sleep_for`
(`agent_session.hpp`'s own drain loop), not a second coroutine suspension point, and tool invocation is a
plain synchronous call, not `co_await`ed. **`ask_stream()` needs exactly this same one-`resume()` call —
just moved onto a background thread, because it will legitimately block for the run's whole real-world
duration and the caller wants to consume events concurrently, not because it needs to resume more than
once.**

### The real design — TWO cooperating threads, not one loop

1. **Driver thread** (`std::jthread`, `Bundle::ask_stream_driver_`, same idiom as before —
   `cli_chat.cpp:915`'s `jthread` destructor precedent, unchanged): calls `session_->start_run(...)`,
   then `t.resume()` **exactly once**. If `!t.done()` afterward, `producer.fail(...)` the caller-facing
   stream with the identical honest message `ask()` already uses ("needed more than one resume()... most
   likely a concurrent caller"), rather than leaving the caller's stream hanging open forever.
2. **Relay thread** (a second thread, spawned by the SAME driver `jthread` BEFORE it calls `resume()` —
   ordering matters, see below): drains the session's event stream and translates `ModelTextDelta`
   fragments into pushes onto the caller-facing `stream<std::string>` producer.

**A real, NEW design point the prior sketch never named: `enable_event_stream()`
(`agent_session.hpp:681-686`) is a single-call API** — it move-assigns `run_event_producer_`, so a second
call would silently orphan any earlier consumer. `Bundle` must call it lazily ONCE (a new member,
`std::optional<agentengine::stream<agentengine::RunEvent>> event_stream_`, populated on the FIRST
`ask_stream()` call and reused by every later one) rather than per-call, as the original sketch implicitly
assumed. Confirmed by grep (`agent_session.hpp`): nothing ever calls `.close()`/`.fail()` on
`run_event_producer_` — it is session-scoped and outlives any single run, so the relay thread **cannot**
rely on the event stream's own `.done()` to know when to stop (it never becomes true); it must instead
recognize ITS OWN run's terminal event and stop there:

- `run_id` isn't known until `start_run()` actually begins executing (minted inside the coroutine,
  `effect_context_.run_id = ...`), so the relay thread starts with no filter, treats the FIRST
  `run_started` event it observes as establishing `current_run_id` for this call, then filters every
  subsequent event to that id, and stops (closing the caller-facing producer) on that run's own
  `run_finished`/`run_failed`/`run_canceled` — never on the shared event stream's own `.done()`.
- **Sequencing**: the relay thread must already be polling BEFORE the driver calls `resume()` (spawn
  relay first, THEN resume()) — `emit_run_event()` pushes onto a real channel with backpressure
  (default capacity 256, `core/stream.hpp:211`), so no event is ever silently dropped even if the relay
  is a few polls behind, but if the relay hadn't started polling AT ALL yet, the very first `run_started`
  event (needed to learn `current_run_id`) could theoretically sit unread indefinitely if nothing ever
  starts draining — spawning relay-before-resume removes any ambiguity about this.
- Translates `ModelDelta`s (§1's variant) the same way the existing drain loop does: `ModelTextDelta.text`
  pushed as-is; `ModelToolCallArgumentDelta` skipped — `ask_stream()`'s contract stays text-only, unchanged
  from the prior pass's own scoping, a richer variant remains a natural, separate follow-on.

### Why this closes Finding 3 without reopening it

Both threads now share the SAME bounded-single-`resume()` safety property `ask()` already has, proven
safe under contention today. A concurrent `Bundle::ask()` call while `ask_stream()`'s driver is in flight
is handled by the exact same mechanism that already handles concurrent `ask()`/`ask()`: whichever call's
task acquires `session_mutex_` first runs to completion; the second one's single `resume()` returns with
`!t.done()` (parked in `session_mutex_`'s `waiters_`) and fails closed immediately — a fast, honest error,
not a hang, not a race. **`ask_mutex_`'s real job, named explicitly for the first time**: it is NOT what
protects `session_mutex_` from concurrent access (that's `session_mutex_`'s own job, already proven) — it
exists to (a) give sequential `Bundle`-level calls a predictable, serialized UX (matching `ask()`'s
existing "serializes against itself" framing) and (b) protect the single new `event_stream_` consumer from
being read by two relay threads at once.

**[Finding 9-new, minor-correction, from the 5th red-team pass] The prior text claimed `ask_mutex_` "does
not need to be held for `ask_stream()`'s full duration" — this OVERSTATES what actually happens.** Neither
the driver thread (one blocking `resume()`) nor the relay thread ever checks its own `stop_token()` in this
design — nothing consumes it — so `std::jthread`'s move-assignment `request_stop()` call has no observable
effect; what actually makes a second overlapping `ask_stream()` call safe is `std::jthread::operator=`'s
OWN guaranteed behavior of calling `.join()` on the OLD thread object before installing the new one. That
join blocks for however long the OLD run+relay pair takes to finish naturally. So: **if the statement that
installs the new driver (`ask_stream_driver_ = std::jthread(...)`) runs while `ask_mutex_` is held, `ask_
mutex_` in practice IS held for up to the full remaining duration of a still-in-flight prior call** — just
via an implicit path (blocking inside a move-assignment), not a deliberate long hold. The NET observable
behavior (full serialization between overlapping `ask_stream()` calls, matching `ask()`'s own "serializes
against itself" framing) is fine and was always the intent — only the REASONING for why the lock's scope
looked short was wrong. Two honest ways to resolve this, either acceptable, decided during `prove`: (a)
keep `ask_mutex_` held across the whole install statement and document plainly that this makes overlapping
`ask_stream()` calls serialize for their full duration (simplest, matches current behavior exactly, no new
code); or (b) release `ask_mutex_` before the assignment and accept that two overlapping installs could
theoretically race each other's `std::jthread::operator=` call — needs its own real check against
`std::jthread`'s thread-safety guarantees for concurrent assignment to the SAME object before being chosen
over (a). No change to Piece D's own architecture either way.

**Declaration order — CORRECTED during implementation (`prove`), this text originally had it backwards.**
Members destroy in REVERSE declared order (the file's own existing rule for `session_`,
`session_builder.hpp:205-211`), so to get "the driver/relay threads are destroyed — and thus joined —
before `event_stream_` is torn down," `ask_stream_driver_` must be declared AFTER `event_stream_`, not
before: `..., ask_mutex_, event_stream_, ask_stream_driver_` (destruction order: `ask_stream_driver_`
first, `event_stream_` second, then the rest unchanged). Declaring it the other way around — as this
section's own prose first said — would tear down `event_stream_` while a thread might still be reading it.

**A second real lifetime hazard, found during implementation, that neither red-team pass named**:
`ask_stream()`'s driver/relay threads capture `this` (a `Bundle*`) to reach `session_`/`event_stream_` for
as long as they run — unlike `ask()` (purely synchronous, no cross-call lifetime concern at all), moving a
`Bundle` elsewhere while its `ask_stream_driver_`/relay pair is still active leaves that thread holding a
dangling `this` once the OLD `Bundle` object is destroyed. **Disclosed, not structurally prevented** — the
same class of choice `call_stream()`'s own `this`-capture residual makes (§3 above): a caller must not
move a `Bundle` after calling `ask_stream()` on it until that call's returned stream reaches a terminal
state. Narrow in practice (`Bundle` has no move-assignment, and real usage is "build once, keep in place"),
but real, and worth a regression test during `prove`'s own test-writing pass, not just this comment.

## 5. Piece E — `EffectContext::report_progress` becomes `ContentItem`-typed, a real app-defined extension point

**New piece, added after Rev 2.** Started as two separate ideas across this design series — a "structured
progress payload" follow-on to ADR-060 (§7 of that ADR names it as an explicit, deliberately out-of-scope
residual: *"Structured/typed progress payloads ... `ToolCallDelta`'s existing shape is `{call_id, text}`,
unchanged by this ADR"*), and a broader "let a consumer app define its own live UI update shape" ask.
Under the same directive as Rev 2 — clean over additive-safe — these are **the same underlying need**, and
unifying them is one redesign, not two mechanisms bolted on side by side.

**The redesign — replace the field's signature, don't add a second one alongside it:**

```cpp
// core/effect_context.hpp:102 — REPLACES std::function<void(std::string_view)> report_progress
std::function<void(ContentItem)> report_progress = [](ContentItem) {};

// core/run_event.hpp:81-84 — REPLACES struct ToolCallDelta { std::string call_id; std::string progress_text; }
struct ToolCallDelta {
    std::string call_id;
    ContentItem content;
};
```

`ContentItem::value` (`content.hpp:146`) is already `variant<Text, Reasoning, Media, Data, ToolCall,
ToolResult, Citation, Error, Custom>` — the tool author gets the engine's **entire existing content
vocabulary** for a mid-call update, not a bespoke one:

- Plain text (today's only case): `ctx.report_progress(ContentItem{Text{"25% done"}})`.
- A structured fact, e.g. a diff stat: `ContentItem{Data{R"({"files":1,"+":120,"-":10})", "diff_stat_v1"}}`.
- Anything genuinely app-specific, with no engine-side schema at all: `ContentItem{Custom{"myapp.
  file_preview.v1", payload_json}}` — reusing `content::Custom{type_id, payload_json}` (`content.hpp:137-
  142`), the SAME namespaced-escape-hatch idiom already established there, not a new one invented for this.

**This is not a purely internal redesign — it closes a real, confirmed connection gap between two escape
hatches that already existed independently and never met.** `run_event.hpp`'s own top comment states its
vocabulary is deliberately **closed** ("total... not a subset re-designed per consumer," `run_event.hpp:6-
9`) — there was no way for a host/tool author to emit a genuinely app-defined live signal through the
internal `RunEvent` pipe at all. Meanwhile, AG-UI's own projection was ALREADY reaching for exactly this:
`protocol/agui/projection.hpp:102-109`'s `tool_call_delta` case projects to AG-UI's `CustomEvent` (not
`TOOL_CALL_CHUNK`) **today**, with its own file-top comment (`projection.hpp:24-26`) already explaining why.
Piece E's redesign means that projection now has real, structured content to carry instead of only
`progress_text`, wired through the SAME `ContentItem` → JSON path `rt/message_codec.hpp` already implements
(reused, not reinvented) rather than a bespoke `{"progressText": ...}` shape:

```cpp
// protocol/agui/projection.hpp:102-109 — sketch, not implemented
case run_event_kind::tool_call_delta: {
    auto const& p = std::get<run_event_payload::ToolCallDelta>(ev.payload);
    return {CustomEvent{"ae:tool_call_progress",
                         json::Value::make_object(
                             {{"callId", json::Value::make_string(p.call_id)},
                              {"content", message_codec::to_json(p.content)}})}};  // reuses the existing codec
}
```

**Migration — confirmed, bounded, not open-ended:**

1. `EffectContext::report_progress`'s type (`effect_context.hpp:102`).
2. **The three real bind sites in `rt/agent_session.hpp` (978-981, 1601-1604, 1904-1907) — CORRECTED after
   red-team (Finding 7, MUST-FIX): Rev 2 asserted a taint invariant with no code enforcing it.** The
   original text claimed pushed content "gets `tainted = true` by the same convention `ToolResult` content
   already follows," but `tool_pipeline.hpp:594`'s `item.tainted = true` only fires where `invoke_tool()`
   builds ITS OWN return-value `ContentItem` — it has no bearing on a `ContentItem` a tool constructs
   itself and hands to `ctx.report_progress(...)`. As sketched, a tool calling
   `ctx.report_progress(ContentItem{Text{"x"}, origin, /*tainted=*/false})` would have that `false` flow
   untouched into `ToolCallDelta` and every downstream projection — the invariant was aspirational prose,
   not enforced. **Fix: each of the three closures forces it, unconditionally, before constructing
   `ToolCallDelta`** (a real, new line at each site, not inherited from anywhere):
   ```cpp
   effect_context_.report_progress = [this, call_id = req.call_id](ContentItem item) {
       force_tainted(item);  // CORRECTED (Finding 24) — was a flat item.tainted = true;
       emit_run_event(run_event_kind::tool_call_delta,
                       run_event_payload::ToolCallDelta{call_id, std::move(item)});
   };
   ```
   **[Finding 24, MUST-FIX, from the second red-team pass] A flat `item.tainted = true;` on the OUTER
   `ContentItem` does not reach a nested `ToolResult`'s own per-item taint flags — and this is
   independently observable downstream, not a theoretical gap.** `ContentItem::tainted` is non-recursive;
   `ContentItem::value` can itself hold a `ToolResult`, which nests its own `std::vector<ContentItem>`,
   each with an independently-settable `tainted`. `rt/message_codec.hpp:259,280`'s recursive serialization
   writes each nested item's OWN `tainted` field into the projected JSON, independently of the parent — so
   `ctx.report_progress(ContentItem{ToolResult{.content = {ContentItem{Text{"x"}, /*tainted=*/false}}}})`
   would have its outer wrapper correctly forced-tainted while the inner `Text` item's `tainted = false`
   (set by the tool itself) survives verbatim into AG-UI/MCP output — reopening, one level deeper, exactly
   the "a tool doesn't get to unilaterally mark its own content trusted" gap this correction exists to
   close. The pre-existing convention this correction cited (`tool_pipeline.hpp:594`) never had to handle
   recursion — it only ever wraps a flat `Data` item — so there was no real precedent to lean on here.
   **Fix: a recursive helper, not a single assignment:**
   ```cpp
   // rt/agent_session.hpp (detail-local) — NOT core/content.hpp, see placement note below
   void force_tainted(ContentItem& item) {
       item.tainted = true;
       if (auto* tr = std::get_if<ToolResult>(&item.value)) {
           for (ContentItem& child : tr->content) force_tainted(child);
       }
   }
   ```
   **[Finding 32, minor-correction, from the third red-team pass] Placement corrected.** The first sketch
   of this fix vaguely suggested "`core/content.hpp` or a nearby helper" — checked directly: `content.hpp`
   (189 lines) contains zero free functions with logic, only struct/enum declarations and defaulted
   `operator==`; a mutating recursive helper doesn't fit that file's established convention. It belongs
   next to its three actual call sites (`agent_session.hpp:978-981/1601-1604/1904-1907`) or as a detail
   helper adjacent to `tool_pipeline.hpp:679`'s existing reset choke point, not in the pure-data-vocabulary
   header. (Recursion-depth/cycle safety checked separately: `ToolResult`-within-`ToolResult` nesting is
   real and tool-author-controlled, but `message_codec.hpp`/`chat_recording.hpp`'s existing content-item
   codecs already recurse over the identical unbounded structure with no depth guard — `force_tainted()`
   inherits an existing, already-accepted risk class, not a new one.)
3. `tool_pipeline.hpp:679`'s reset-to-no-op line — same choke point, same fix, new signature.
4. `protocol/agui/projection.hpp:102-109` — swap `progress_text`/`"progressText"` for the reused
   `ContentItem` codec, as sketched above.
5. **`protocol/mcp/progress.hpp:56-63` (`McpProgressProjector::project()`) — found while resolving the
   open questions below, not in the original migration list.** Reads `p.progress_text` directly
   (`:62`) to build `ProgressNotification::message`; must be updated to the resolution in the box below.
6. **[Finding 25, MUST-FIX, from the second red-team pass; citation corrected by the third pass's Finding
   35] Four real test sites break under `ToolCallDelta`'s own type change** (corrected from the original
   five — see below), **none previously listed.** `ToolCallDelta{call_id, progress_text}` → `{call_id,
   content}` breaks: `tests/test_mcp_progress.cpp:35,47,49,61` (positional `ToolCallDelta{"call-1", "25%
   done"}` — a string literal no longer initializes a `ContentItem`), `tests/test_rt_agui_projection.cpp:
   225` (same pattern), and `tests/test_agent_session_tool_call_progress.cpp:276` (`delta_text = p->
   progress_text;` — field no longer exists). **[Finding 35, minor-correction] The original fix also cited
   `test_agent_session_tool_call_progress.cpp:329` as breaking — checked directly, it does not**: that
   line reads only `p->call_id` (unaffected by the `progress_text` → `content` rename) and
   `std::get_if<ToolCallDelta>` still compiles since the type's own name is unchanged; there is exactly
   one `progress_text` occurrence in that file (line 276), confirmed by grep. Citing a non-breaking site
   as breaking is the same citation-imprecision class Finding 21 exists to fix, recurring inside the very
   fix trying to avoid repeating it — corrected here, not left standing. This is the same defect class
   this very correction pass already fixed once for Piece B (Finding 3) and Piece C (Finding 5) —
   recurring, uncaught, inside this piece's own original "confirmed by grep" claim. All four (not five)
   need updating to construct/read the new `ContentItem`-typed `content` field.
7. Confirmed by grep before writing this: no production tool anywhere in the tree calls `report_
   progress()` today — only the five test files above (already folded into point 6) call or construct it.
   The "real tool body" migration cost this redesign would otherwise carry is effectively zero; only test
   fixtures need updating.

**Both open questions this piece originally left open are now resolved, against the real code (not
assumed):**

- **Does `rt/message_codec.hpp`'s `content_item_to_json()` (`:208-267+`) already handle every `ContentItem`
  variant, `Media`/`BlobRef` included, in a shape safe to embed directly in an AG-UI `CustomEvent`? Yes,
  confirmed by reading the function body.** All nine variants are handled exhaustively (`Text`,
  `Reasoning`, `Media`, `Data`, `ToolCall`, `ToolResult` — recursively, `Citation`, `Error`, `Custom`).
  `Media`'s three payload shapes are each handled distinctly: raw `std::vector<std::byte>` inlines as
  base64 (`:233-236`), a `uri` string passes through as-is (`:237-239`), and `BlobRef` serializes as a
  small `{digest, media_type, size, store}` reference object (`:241-243`, `mcd::blob_ref_to_json`) — **not**
  the referenced bytes themselves, so a tool that pushes `Media{BlobRef{...}}` progress content never
  inlines large payloads into the live event; this was already true everywhere else this codec is used
  (e.g. `ToolResult`), not a new property Piece E introduces. **Named residual, not a defect**: a tool
  author who instead pushes `Media{raw_bytes}` (not a `BlobRef`) as a *progress* update WOULD get real
  bytes base64-inlined into the live stream — pre-existing codec behavior, not specific to this piece, but
  worth naming as authoring guidance (prefer `BlobRef` for large media in a progress channel specifically,
  since progress updates are expected to be small and frequent). No new code needed in the codec itself —
  reuse it as-is.
- **Should MCP's progress projection also widen to carry structured content, or does MCP's own protocol
  shape cap it? Capped, confirmed by reading `protocol/mcp/progress.hpp`'s own file-top citation.**
  `ProgressNotification`'s real wire shape (`:41-47`) is exactly `{progressToken, progress: number,
  message: string}` — and the file's own comment (`:16-22`) already states `message` itself is *this
  project's own* addition, "not independently confirmed" against the cited MCP spec research
  (`docs/research/2026-mcp-protocol-detail.md §10`), which only guarantees `progressToken`/`progress`.
  There is no structured-content slot to widen into — MCP's real protocol genuinely caps this to text.
  **Resolution — TIGHTENED after red-team (Finding 11, minor-correction).** `McpProgressProjector::
  project()` keeps building `message` from `p.content`, branching on the variant instead of reading a
  no-longer-existent `.progress_text` field directly — `Text` content uses `.text` verbatim (identical
  output to today). The original resolution's fallback for every OTHER variant —
  `json::dump(message_codec::content_item_to_json(p.content))` — was found to be noisier than intended:
  `content_item_to_json()` already produces a JSON-STRING-valued `payload_json` field for `Custom` items
  (already escaped once), and wrapping the whole envelope (including irrelevant `origin`/`tainted`
  bookkeeping) double-escapes and adds noise no MCP client needs. **Corrected fallback: dump only the
  semantically relevant payload per variant** — `Custom{type_id, payload_json}` → `type_id + ": " +
  payload_json` (verbatim, no re-escaping); `Data{json, schema_id}` → `json` verbatim; anything else
  (`Media`, `ToolCall`, `ToolResult`, `Citation`, `Error`, `Reasoning`) → a short, variant-specific
  human-readable line rather than the full recursive codec dump. Still an honest degradation (structured
  content collapses to text over MCP, since MCP's real protocol has no structured slot), just a tighter
  one than Rev 2's first pass at this fallback.

**Thread-safety — inherits ADR-060 Finding 5's fix exactly, no new analysis needed.** The hazard (a
`Backgroundable` tool's `EffectContext` copy on `background_task()`'s detached thread observing a live,
`this`-capturing closure) is identical regardless of what `report_progress` carries — `tool_pipeline.hpp:
679`'s structural reset closes it the same way for a `ContentItem`-typed closure as it did for a
`string_view`-typed one. Re-verifying this is still true is a mechanical part of implementation, not a new
red-team finding.

**Taint.** A tool-pushed `ContentItem` through this channel gets `tainted = true` by the same convention
`ToolResult` content already follows (`tool_pipeline.hpp:594`, "external content and the primary
prompt-injection vector") — the caller (tool author) does not get to unilaterally mark its own content
trusted; that stays a pipeline-level default, consistent everywhere content originates from a tool.

**Scope, named explicitly:** this stays call-scoped to one `invoke_tool()` invocation, exactly like
today's `report_progress` — it does NOT make `EffectContext` a general-purpose extension bag reachable
from other surfaces (a `ContextProvider`, a workflow executor, a `ChatClient` middleware). Whether those
surfaces want an analogous reverse channel is a real, separate question, not assumed answered by this
piece just because the name "`EffectContext`" is shared.

**[Finding 12, informational — spec/code reconciliation needed before landing.]** `006-Tool-and-Function-
Plane.md:170` (the RFC this whole chain implements) already names this field `report_progress
(ProgressUpdate)` — a THIRD type name, matching neither today's shipped `std::string_view` nor this
piece's proposed `ContentItem`. Per CLAUDE.md's own rule ("when code and spec disagree, the spec wins; if
the spec is wrong, fix the spec first, with an ADR"), landing Piece E without addressing this leaves the
RFC and the implementation silently inconsistent. Two honest resolutions, not decided here: (a) treat
`ContentItem` AS the RFC's `ProgressUpdate` and update 006 §6a to say so explicitly (a documentation-only
ADR amendment, no new type), or (b) if the RFC's `ProgressUpdate` was meant to be something narrower than
the full content vocabulary, that needs surfacing and reconciling before this piece is implemented, not
discovered after.

## 6. Unified self-red-team (updated for Rev 2 + Piece E; superseded in part by the real adversarial pass above)

The bullets below are this draft's own self-review; the numbered Findings folded into §1/§2/§5 above came
from an INDEPENDENT adversarial pass (a fresh agent, not the author of this draft) and are the higher-
confidence source where the two might seem to overlap — self-review confirmed the invariants (I2/I3/cost)
hold, but self-review is exactly what missed Findings 1-7 in the first place; treat this section as
"nothing NEW broke I1-I8," not as a substitute for the adversarial pass.

- **I3.** Unchanged conclusion: nothing in `agent_session.hpp`/`tool_pipeline.hpp` treats any
  `RunEventPayload` as authorization input anywhere; widening `ToolCallFinished` and restructuring
  `ModelDelta` both stay strictly downstream of that boundary.
- **I5.** Same three pieces extend the `ChatClient`-seam recording contract (004 §6) — still needs one
  shared review of the recording-format change, `chat_recording.hpp` still not opened in this pass.
- **Cost (004 §5).** Only Piece A's attempt-retry accounting is new. Restructuring `ModelDelta`/
  `ToolCallFinished` adds no new backend calls — same conclusion as Rev 1, unaffected by the redesign.
- **Migration risk, named honestly (the actual cost of choosing clean over additive):** `ModelDelta` and
  `ToolCallFinished` are both real, already-shipped `RunEventPayload` variant members — any external
  consumer of the wire-level projections (AG-UI/MCP/A2A/OpenAI-compatible SSE, 013's four surfaces) that
  pattern-matches these two payload shapes directly needs updating. This pass confirmed the IN-TREE blast
  radius precisely (§1, §2's lists) — an external consumer outside this repository is out of this pass's
  visibility and is a real, disclosed residual, not something to claim as covered.
- **Concurrency (Piece D).** No longer an open risk — resolved against `channel.hpp`'s real, read
  semantics (§4).
- **Piece E, I2/I3.** `report_progress` stays a pure, one-directional PUSH onto an observational stream —
  confirmed (same conclusion as B/C, re-checked for this piece specifically) nothing consumes `RunEvent`
  as authorization input anywhere in `agent_session.hpp`/`tool_pipeline.hpp`. Widening what a tool may
  push (from a string to the full `ContentItem` vocabulary) does not change WHO can push or WHAT it can
  influence — a tool still only reaches its own call's `call_id`-scoped delta, never another call's, never
  a policy decision. `Custom{type_id, ...}`'s namespacing is the same collision-avoidance idiom
  `content::Custom` already relies on elsewhere — no new capability class needed.
- **Piece E, taint.** Explicit default `tainted = true`, matching `ToolResult` convention — named so this
  isn't silently weaker than the content vocabulary it reuses.
- **Piece E, scope discipline.** Explicitly NOT generalized into a cross-surface `EffectContext` extension
  bag in this pass (§5's own "Scope, named explicitly" — stays call-scoped to `invoke_tool()`, same as
  today). Naming this prevents scope creep from "one clean redesign" into "an open-ended extension
  mechanism nobody has red-teamed the full reach of."

## 7. Sequencing (updated for Piece E)

1. **B + C + E together** — B and C were already independent of each other and of A; E is independent of
   both too (different event kind, `tool_call_delta` vs `model_delta`/`tool_call_finished`) but shares the
   same design language (raw/reused content, no new taint mechanism) and the same near-zero production
   migration cost (E's confirmed-empty real-tool-caller list, §5 point 5). Natural to land as one change.
   Blocked on: `finish()`'s current parse-failure behavior (open question #2) for B/C specifically; E has
   no equivalent blocker.
2. **A** — self-contained, needed before D. Rev 6 redesign: `call_stream()` returns `stream<ChatResponseUpdate>`
   (mirroring `ChatClient::chat_stream()`'s own contract) backed by a detached thread, not a `task<>` the
   caller awaits while pushes happen synchronously on the same chain — closes the deadlock risk the 4th
   red-team pass found (Finding 2). `ModelCallGatewayStreamLike` is a new, separate, optional concept
   (not a widening of `ModelCallGatewayLike`) — `MiddlewareModelCallGateway`/`ContentReplayGateway` do not
   gain streaming in this pass (named residual, Finding 4/5).
3. **D** — last, gated on A. Rev 6 redesign: `ask_stream()`'s driver reuses `ask()`'s own bounded
   single-`resume()` contract (never a resume loop — the 4th pass's Finding 3 found the prior sketch
   reintroduced a `session_mutex_` double-resume hazard this codebase already found and fixed once), moved
   onto a background thread, paired with a second relay thread draining a lazily-created, Bundle-cached
   `event_stream_` (single-call `enable_event_stream()` API, confirmed by grep to never `.close()`).

## Open questions carried forward

1. Does the OpenAI backend's per-tool JSON schema emission already satisfy strict mode's
   `additionalProperties: false` + all-fields-`required` constraints, or does native per-tool strict need
   a schema-generation change too?
2. What actually happens today when `finish()`'s accumulated tool-call arguments fail `json::parse`?
   Must be answered before implementing §1's `is_error: true` recovery wiring.
3. Does llama.cpp's/Ollama's OpenAI-compat surface do anything with a tool-argument JSON Schema
   specifically — relevant to whether `tool_argument_strict_native` could ever be `true` there.
4. Does anything auto-promote oversized `ToolResult` content to `Media{BlobRef}` today, or is that
   entirely tool-author-manual?
5. Any external (out-of-repo) consumer of the current `ModelDelta{text_delta}` / `ToolCallFinished{call_id,
   ok}` / `ToolCallDelta{call_id, progress_text}` wire shapes is a disclosed, unverified residual of
   choosing to redesign rather than stay additive — named, not resolved.

Two prior open questions here (the `message_codec.hpp` coverage question and the MCP-widening question)
were resolved in §5 above, against the real code — removed from this list, not left duplicated.

**From the first red-team pass:**

6. **§1's exact fragment-completion point — SPLIT PER BACKEND after the third red-team pass (Findings
   26/27).** **Anthropic: answered.** `content_block_stop` (per-index, already parsed and currently
   discarded by `anthropic/chat_client.hpp`'s own accumulator) is the real completion signal — a real
   fix, not an open question, for that backend. **OpenAI: still genuinely open.** No equivalent explicit
   boundary event is parsed today (only an unparsed `index`-transition convention or a trailing
   `finish_reason` field) — must be answered before implementation for the OpenAI side specifically, not
   guessed at in code.
7. **§2 Finding 6's residual**: whether any consumer of `BackgroundTaskDone.ok` exists beyond the one
   drain site named in §2 — **RESOLVED (confirmed-clean) by the second red-team pass's Finding 18**: a
   full-tree grep found exactly one construction site and exactly one read site, both already accounted
   for. No longer open.
8. **§5 Finding 12**: reconcile `006-Tool-and-Function-Plane.md:170`'s `ProgressUpdate` naming against this
   piece's `ContentItem` choice — an RFC amendment (documentation-only ADR) or a real scope question,
   not decided here.

**From the second and third red-team passes:**

9. §1 Finding 17/28: the `chat_recording.hpp` round-trip codec for `tool_call_argument_chunk` is
   sequenced AFTER item 6 (per-backend `is_final` semantics) is settled — not written until then.
10. §1 Finding 17: the pre-existing `chat_recording.hpp` gap in round-tripping `ChatResponseUpdate::usage`
    (confirmed real, not deliberate, by the third pass's Finding 29) is a separately-actionable defect,
    named here so it isn't lost, not fixed by this design.

**From the 4th red-team pass (Piece A/D, 2026-08-22):**

11. §3 Finding 6: does `ModelCallGateway::call()` (the existing, already-shipped buffered path) have its
    own I5-recording/cost-ledger gap today, or does something already cover it that a grep for
    `StreamAttemptRecord`-shaped types missed? `call_stream()` inherits whatever the answer is rather than
    introducing a new double standard — needs a real check during `prove`, not assumed either way.
12. §3 Finding 4/5's own residual: whether `MiddlewareModelCallGateway`/`ContentReplayGateway` ever need
    their own forwarding `call_stream()` is a real product question (do any real sessions wrap a gateway
    in either type AND want streaming?), not decided here — named so it isn't silently forgotten the way
    Piece B's Anthropic-backend gap was for three rounds.
13. §4's relay-thread design assumes `emit_run_event()`'s default channel capacity (256) is always enough
    headroom between "relay thread spawned" and "relay thread's first successful poll" to never lose the
    `run_started` event needed to learn `current_run_id` — true given `push()`'s real backpressure
    semantics (confirmed, not assumed, in §4 above), but the exact spawn-before-resume ordering is a real
    implementation invariant `prove`'s own tests must exercise directly, not just assert in prose.

## What this draft is not

Not an implementation, not an ADR. Rev 2 made the shapes clean rather than additive-safe, per explicit
project-owner direction. **Rev 3 folds in a real adversarial red-team pass against §1/§2/§5** — the pass
this section previously said still needed to happen for `ModelDelta`/`ToolCallFinished`/`ToolCallDelta`'s
blast radius has now happened, found the draft's own "confirmed by direct grep" claims were incomplete in
three places (Findings 1, 2, 3, 5) and one design gap was genuinely unresolved (Finding 4, Piece B's
`ChatResponseUpdate` plumbing) and one invariant was asserted but not enforced (Finding 7, Piece E's
taint) — all now corrected above, not just noted as future work.

**Pieces B, C, E: `prove` complete (2026-08-22).** Real implementation landed against the full tree;
818-target build compiles clean, 6 directly-exercised test binaries pass with 0 failures. Two translation
tests (OpenAI/Anthropic backend accumulator changes) are gated behind `AGENTENGINE_VENDOR_MBEDTLS`, not
enabled in the build used for `prove` — hand-traced instead of compiler-verified, a named, disclosed
residual of `prove`'s own coverage, not silently claimed as covered.

**Pieces A, D: design-only, now through TWO independent adversarial passes.** The first pass (2026-08-22,
the first ever run against these two pieces — B/C/E had three rounds by the time A/D got its first) found
the Rev 2-5 sketch could not have been implemented as written: a referenced type that doesn't exist
(Finding 1), a real deadlock risk with no described draining mechanism (Finding 2), and a concurrency
hazard this exact codebase already found and fixed once, reintroduced (Finding 3) — plus two real
unresolved concept-widening questions (4/5) and one scope-inflation finding citing machinery that doesn't
exist in the tree (6). Rev 6 resolved all six directly against real source. **A second, independent pass
against Rev 6 CONFIRMED the central architecture holds up** (unlike Piece B's own history, where three
rounds each found the PREVIOUS round's fix still broken in a new way — this time the second round validated
the first round's fixes as sound, not merely different) — but found one guaranteed compile break in the
dispatch sketch's use of a runtime `if` where `if constexpr` was required (against real, currently-passing
tests), one stale warning-emission site the dispatch fix never touched, and one unstated cross-thread
ownership invariant for `CircuitBreaker` state. Rev 7 (this revision) folds in all three, plus a reasoning
correction to Piece D's `ask_mutex_` explanation (the observable behavior was already correct; the stated
reason for it was not). **Per the second pass's own explicit recommendation, these are narrow, mechanical
fixes — the plan is to move to `prove` next for Pieces A/D, not run a third broad round.**

**Still not done, named precisely rather than left implicit:**
- Three consecutive adversarial passes each found real, concrete problems in the previous pass's own B/C/E
  corrections (Rev 3 → 7 findings, Rev 4 → 4 more, Rev 5 → 3 more, including one genuine architectural gap
  — Piece B being OpenAI-only through three full rounds); B/C/E's own `prove` step then found two MORE real
  problems no red-team round had caught (a stale lambda signature, a double-move hazard) — real compilation
  remains the most reliable backstop for the defect classes read-only review keeps missing, for both A/D
  and B/C/E alike. A genuinely NEW architectural-class finding (the shape of Finding 26/27, or Piece A/D's
  own Finding 4-new/5-new/6-new) surfacing during `prove` should still get a targeted look, not be waved
  through just because red-team rounds are "done."
- `prove` (real implementation against the real tree, with real tests — matching ADR-060 §5/§6's bar: a
  test that can actually fail, not a vacuous one) is complete for Pieces B, C, E; has not started for Pieces
  A, D.
- `judge` (project owner sign-off) has not happened for any piece — this stays a design draft, not an ADR,
  until that happens.
