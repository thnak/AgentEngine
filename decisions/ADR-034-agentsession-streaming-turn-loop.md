# ADR-034 — AgentSession's opt-in streaming turn loop

**Status:** Proposed (2026-08-12). Designed, red-teamed (independent pass via the `Agent` tool,
general-purpose reviewer), implemented, and proven (real code + deterministic offline tests, §6);
awaiting the project owner's explicit "Judged" sign-off per this project's governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project owner is the ADR
judge, never an AI).

**Relates to:** `decisions/ADR-027-agent-session-tool-call-loop.md` (Judged — the internal
multi-round tool-call loop this ADR's streaming path runs inside, unmodified in its own control
flow); `013-UI-and-Streaming-Surfaces.md` §1 (the real-event-stream vocabulary this ADR gives its
first real `model_delta` emitter); `004-Model-Provider-Plane.md` §1/§5 (TokenBudget<N>, the
guarantee this ADR's fail-closed usage rule protects).

## 1. The question

The project owner asked for real token-by-token streaming rendered live in `tools/cli_chat.cpp`.
`AgentSession::handle()`'s real, Judged internal tool-call loop (ADR-027) calls the blocking
`ChatClientT::chat()` once per round — the model's whole reply arrives at once, however long
generation takes, with no visibility into it until it's fully done. `013-UI-and-Streaming-
Surfaces.md` §1 already names `run_event_kind::model_delta` and its `ModelDelta{text_delta}`
payload in `run_event.hpp`'s real vocabulary — declared since Milestone 7 Phase A, with zero real
emitter anywhere in this codebase until this ADR.

## 2. The design considered and rejected

**First draft:** make `handle()`'s core loop call `chat_stream()` unconditionally, for every
`AgentSession<ChatClientT, ...>` instantiation, with no opt-in.

**Rejected by red-team, fatally, on three independent grounds** (an agent dispatch that read the
real backend implementations directly, not a summary):

- **Fatal 1 — failover silently defeated.** `FailoverChatClient::chat_stream()`
  (`include/agentengine/core/failover_chat_client.hpp`) unconditionally calls the primary backend
  and never tries a fallback — a named, deliberate scoping decision in that file's own top comment
  ("failover is `chat()`-only this phase"). `AgentSession<ChatClientT, ...>` is the one call path
  every real caller uses; making `chat_stream()` the *only* path would make 004 §4's failover
  guarantee (Milestone 5 Phase F3, real and tested) unreachable for every caller composing
  `FailoverChatClient` underneath a session, not just `cli_chat.cpp`.
- **Fatal 2 — circuit-breaker feedback silently defeated.** `ResilientChatClient::chat_stream()`
  (`include/agentengine/core/resilient_chat_client.hpp`) never reports an outcome to its own
  circuit breaker — only `chat()` does, by that file's own explicit comment. Under an unconditional
  streaming core loop, 022 §3's breaker degrades to admission-checks-only forever, for every
  session, the moment any caller composes `ResilientChatClient`.
- **Fatal 3 — ADR-023's confused-deputy scan has no streaming equivalent.**
  `apply_response_format_scan` (`include/agentengine/protocol/openai/chat_client.hpp`) — the
  mechanism that catches a serving layer leaking raw Harmony/DeepSeek/Hermes/`<think>` tokens and
  forces a smuggled tool call down to `call_provenance::text_derived` before it can reach
  `invoke_tool`'s trust gate — runs only from `chat()`, gated by `scan_response_format_leaks`.
  `StreamingUpdateAccumulator` never calls it. An unconditional streaming core loop would silently
  drop this protection for any operator who armed it (including `cli_chat.cpp` itself, which does)
  — exactly the class of bug ADR-027 itself had to find and fix once already, reopened one layer
  down.

**Accepted design:** streaming is a per-session **opt-in**
(`AgentSession::set_stream_model_calls(bool)`, default `false`), the same shape
`suspend_for_approval_` (ADR-029) already established. Every existing and future caller that never
calls it keeps calling `chat()` exactly as ADR-027 shipped it, bit-for-bit — proven in §6 by
re-running `test_agent_session_tool_call_loop.cpp`'s full R1-R6 suite unmodified. A caller that
opts in accepts losing failover/breaker-feedback/leak-scanning **for that session**, as a
documented tradeoff rather than a silent one: a `run_event_kind::warning` fires once per run
naming exactly what's traded away, visible to any consumer of the event stream.

## 3. Token-budget correctness — the must-fix that shapes the mechanism

`ChatResponseUpdate` (`core/chat_client.hpp`) had no usage field at all — `chat_stream()`'s real
backend implementations never surfaced per-call token counts. Silently treating "the backend
reported nothing" as "this call cost nothing" would let 004 §5's `TokenBudget<N>` (Milestone 5
Phase F4, real and tested) silently stop being enforced the moment a caller streamed — a real,
exploitable regression, not a cosmetic gap.

**Fixed two ways:**

- `ChatResponseUpdate` gains `std::optional<Usage> usage = std::nullopt` (additive, appended last —
  003 §6's field-ordering convention; every pre-existing `ChatResponseUpdate{delta, is_final}` call
  site keeps compiling unchanged).
- `OpenAIChatClient`'s streaming path requests OpenRouter/OpenAI's `stream_options.include_usage`
  and captures the vendor's trailing usage-only chunk (`parse_usage_object`, factored out of the
  non-streaming parse path so both share one implementation of the wire contract, not two that
  could drift — this file's own established D2 precedent). `AnthropicChatClient`'s streaming path
  is **not** wired this pass (named residual, §7) — its `chat_stream()` still reports no usage,
  which the rule below makes safe rather than silently wrong.
- `AgentSession::run_model_call()` (the new private helper `handle()`'s core loop calls instead of
  `chat()` directly) **fails the run closed** — the ask never resolves, `run.usage_unavailable`,
  the same "never fabricate a response" shape every other fail-closed branch in this loop already
  uses — whenever a stream completes with no `Usage` on its terminal update. A backend that hasn't
  been wired for usage-in-streaming (Anthropic, today) is therefore unusable in streaming mode, but
  safely so: it fails loudly and immediately, never silently undercounts.

## 4. Message reconstruction

Every delta's `ContentItem` (`Text` fragments, mapped 1:1 to the vendor's own SSE chunks; an
assembled `ToolCall`, emitted whole at the end — real backend behavior, a partial JSON-argument
fragment is never a valid `ToolCall::arguments_json`) is accumulated, in order, into one `Message`.
No merge step is needed for this to be equivalent to `chat()`'s own one-shot parse:
`tool_call_extraction.hpp`'s `tool_calls_of()`/`text_of()` — what `handle()`'s loop and every
downstream reader actually use — are genuinely position/count-agnostic across however many
separate content items a `Message` carries (confirmed by the red-team pass, and by this ADR's own
S1 test asserting `text_of()` on a Message built from three separate Text deltas). Verified
downstream too: `openai::translate_message()` already concatenates multiple `Text` content items
into one wire `content` string — a reconstructed multi-item history entry round-trips correctly
into the next round's outbound request.

## 5. The drain loop itself

`run_model_call()` polls the returned `stream<ChatResponseUpdate>` (`next()` is poll-only per
`core/stream.hpp`'s own documented contract — no coroutine suspension protocol exists for it) with
a bounded `sleep_for(5ms)` between empty polls, not a bare spin — real backends run their blocking
HTTP/SSE read loop on a **detached background thread** (confirmed in both `OpenAIChatClient::
chat_stream()` and `AnthropicChatClient::chat_stream()`), so this is genuinely waiting on I/O, the
same kind of cost the existing blocking `chat()` call already pays, not a new one. Each Text delta
fires `run_event_kind::model_delta` before being accumulated.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict |
|---|---|---|
| C1 | Default (`stream_model_calls_ == false`) behavior is unchanged, bit-for-bit. | **CORRECT** — `test_agent_session_tool_call_loop.cpp`'s full R1-R6 suite (14 assertions) re-run unmodified against the changed `agent_session.hpp`, all pass. |
| C2 | A streamed, no-tool-call response fires one `model_delta` per pushed Text delta, in order, and reconstructs the same final text a non-streaming parse would have produced. | **CORRECT** — new test S1. |
| C3 | A streamed multi-round tool-call conversation (a round that streams a Text delta then an assembled `ToolCall`, followed by a converging round) still resolves correctly through the unmodified tool-invocation pipeline. | **CORRECT** — new test S2. |
| C4 | A stream whose terminal update carries no `Usage` fails the run closed — the ask never resolves — rather than silently proceeding as a zero-cost call. | **CORRECT** — new test S3. |
| C5 | `set_stream_model_calls()`/`stream_model_calls()` default false and round-trip. | **CORRECT** — new test S4. |
| C6 | The real `OpenAIChatClient` backend now supplies real usage on its streaming path. | **CORRECT (backend-level)** — verified live against OpenRouter (`api-test.txt`), through `cli_chat.cpp`: streamed turns completed without ever hitting `run.usage_unavailable`, meaning the trailing `stream_options.include_usage` chunk was received and parsed for real. |

## 7. What this ADR does not claim

- `AnthropicChatClient`'s `chat_stream()` is **not** wired for usage capture this pass — streaming
  against it fails closed (§3), it does not silently work with wrong accounting. A real follow-up,
  not attempted here: capture `usage.input_tokens` from `message_start` and `usage.output_tokens`
  from `message_delta` events, the same class of change as the OpenAI side.
- `apply_response_format_scan`/`scan_response_format_leaks` (ADR-023's confused-deputy detector,
  and `cli_chat.cpp`'s own "[thinking]" display, which depends on it) does **not** run on the
  streaming path — named, not silently degraded (the per-run warning event says so). Extending the
  scan to run once over the reconstructed Message (after the stream completes, before any tool call
  is trusted) is real, scoped follow-on work, not attempted here.
- `tools/cli_chat.cpp`'s migration off `quark::TestKit` onto a real, single-worker `quark::Engine`
  (needed for the CLI to genuinely print deltas as they arrive, not just batched after `ask()`
  returns) is a change to a demo tool's own hosting shape, not to any tested production surface —
  its correctness rests on `test_agent_session_suspend_resume.cpp`'s own real-Engine precedent for
  hosting an `AgentSession` outside `TestKit`, and on the live run in §6/C6, not on a new
  regression-tested harness of its own.
- Failover and circuit-breaker composition ARE still real and unaffected for every session that
  does not opt into streaming — this ADR narrows what streaming itself can offer, it does not
  narrow the engine's baseline guarantees.

## 8. Files changed

- `include/agentengine/core/chat_client.hpp` — `ChatResponseUpdate::usage` (additive).
- `include/agentengine/protocol/openai/chat_client.hpp` — `parse_usage_object()` (factored out,
  reused by both the non-streaming parse and the new streaming capture);
  `build_request_body()` sends `stream_options.include_usage` when streaming;
  `StreamingUpdateAccumulator` captures the trailing usage chunk and attaches it (and handles the
  edge case of a genuinely empty completion that still carries real usage, via a synthetic empty
  final update rather than dropping it).
- `include/agentengine/core/agent_session.hpp` — `run_model_call()` (new private helper: routes to
  `chat_stream()` when opted in, drains it, emits `model_delta`, fails closed on missing usage);
  `set_stream_model_calls()`/`stream_model_calls()` accessor pair; `stream_model_calls_` member;
  the per-run `warning` event when streaming is engaged.
- `tests/test_agent_session_streaming_model_calls.cpp` (new) — S1-S4, 16 checks, a real
  `chat_stream()`-backed scripted `ChatClientT` built on `core/stream.hpp`'s real `make_stream<T>`.
- `tools/cli_chat.cpp` — migrated off `quark::TestKit` onto a real single-worker `quark::Engine`
  (the construction shape `test_agent_session_suspend_resume.cpp` already established for hosting
  `AgentSession` outside `TestKit`); `set_stream_model_calls(true)` engaged; a per-turn
  `std::jthread` drain thread prints `model_delta` text as it arrives and every other event
  immediately after, with a `mid_line` tracker so structural event lines never glue onto in-flight
  token text; the thread's own destructor (`request_stop()` + `join()`) runs before `resp` is read,
  on every exit path including an exception out of `block_on()`.

Full regression suite: **206/206** pass (200 pre-existing + the new
`test_agent_session_streaming_model_calls` + the live-network label, exercised live in §6/C6).
