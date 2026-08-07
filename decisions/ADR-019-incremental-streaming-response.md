# ADR-019 — Incremental streaming of a provider response

**Status:** Accepted — 2026-08-07
**Closes:** the Milestone 5 residual "threading `task<T>`/a live producer into
`sandbox/provider_http_client.hpp`'s SSE read loop and a real Phase D/E backend's own
`chat()`/`chat_stream()` bodies", and the Phase C/D2 gap named in both backends' file banners
("`perform_provider_https_exchange` has no incremental/chunked-transfer read loop yet").
**Relates to:** 004-Model-Provider-Plane.md §7 G3, ADR-011 (the egress read loop), ADR-013 (TLS),
ADR-016 (the plaintext transport), ADR-017 (consumer cancellation).

## 1. Context

`chat_stream()` was streaming in shape only. `run_stream_worker` performed one **complete blocking
fetch**, then walked the already-received event list pushing one `ChatResponseUpdate` per event. Both
backends' banners said so plainly rather than claiming otherwise:

> `perform_provider_https_exchange` has no incremental/chunked-transfer read loop yet — it blocks
> until the full HTTP response is buffered, then returns.

So the vendor's chunk boundaries were preserved in delivery **order** (004 §7 G3's own words:
"identical chunk boundaries") but not in **time**. A consumer saw nothing until the entire completion
had arrived. For a real completion that is not a subtlety — it is the difference between tokens
appearing as the model produces them and the whole answer landing at once.

It also blunted two things that had just been built. ADR-017's cancellation could only ever fire
during a fetch the consumer had no visibility into; and the credit-controlled ring's backpressure
regulated a replay of already-received data rather than a live producer.

## 2. Decision

Three layers, each independently testable.

**(1) Framing decoders (`sandbox/incremental_http_body.hpp`).** `ChunkedBodyDecoder` and
`SseEventFramer`: stateful, pure `(state, bytes-in) -> (bytes/blocks-out)`, no I/O. They accept
arbitrary slices — a chunk-size line split mid-digit, an event terminator split across two reads — so
they can be tested against a byte-at-a-time feed with no server involved.

The framer returns raw event **blocks**, not parsed fields. Parsing here would mean baking one
vendor's dialect into shared framing: the OpenAI-compatible backend only needs `data:`, Anthropic's
named-event shape also needs `event:`. Blocks are exactly what both already know how to read.

**(2) Streaming transport.** `perform_http_exchange_streaming` / `perform_https_exchange_streaming`,
and `perform_provider_streaming_exchange` over them: identical to the buffering pair in every respect
— same request building, same byte-cap enforcement (ADR-011 C8), same no-redirect posture (C10), same
`stop_token` cancellation — except body bytes reach an `on_body` sink as they arrive. The returned
`NetEgressResponse` carries status and headers only; its `body` is empty.

Separate entry points, not a nullable sink parameter on the existing ones: the return value *means*
something different, and a caller who silently got an empty body because of a sink they had forgotten
would be badly served by a shared name.

**(3) Backend accumulators.** Each backend gains a `StreamingUpdateAccumulator`: `feed(bytes)` returns
whichever updates are complete now, `finish()` flushes the rest.

### Two design points worth stating

**One-item hold-back, to keep `is_final` honest.** `is_final` marks the last update of a stream, and
"last" is unknowable until the stream ends. Rather than weaken that contract or emit a trailing empty
update to carry the flag, the accumulator holds exactly one completed update: `feed()` releases update
N when N+1 arrives, and `finish()` flushes the held one with `is_final` set. One item of lag —
invisible beside a model's own inter-token latency — in exchange for `is_final` meaning precisely what
it meant before.

**The one-shot parser is now implemented on top of the incremental one.**
`parse_streaming_response_into_updates` used to be the only decoder; it is now a thin wrapper around
`StreamingUpdateAccumulator`. These are two views of one wire contract, and leaving them as
independent implementations is exactly the drift this project's conventions warn about. It also means
the large existing offline translation suites re-prove the incremental decoder for free — they passed
unchanged, which is the strongest available evidence that this refactor is behaviour-preserving.

**Tool calls still emit at the end**, unchanged. A tool call's arguments arrive as JSON-string
fragments across many chunks; a partial fragment is not a valid `ToolCall::arguments_json`. Text
deltas map 1:1 to the vendor's own chunks and stream immediately; tool calls (and Anthropic's
`thinking` blocks) are assembled and appended by `finish()` — the same ordering and content as before.

### One inference, named because it is the least obvious part

The sink never sees the response head, so it cannot read `Transfer-Encoding`. Whether the body is
chunked is inferred from the **first fragment**: a chunked body begins with a hex chunk-size line, an
unchunked SSE body begins with `data:` / `event:` / a `:` comment. Cheap, consulted once. A
non-2xx response is an error document rather than SSE, so it yields no events and nothing bogus is
pushed; the worker fails the stream after the exchange returns with the real status.

## 3. Falsifiable gates

| # | Claim | What would falsify it |
|---|---|---|
| G1 | The OpenAI backend delivers every event exactly once. | A delta lost or duplicated across a read boundary. |
| G2 | Its first item arrives long before the stream ends. | `to_first / total` approaching 1.0 — which is what a buffering implementation produces *by construction*. |
| G3 | The Anthropic named-event decoder is incremental too. | The same ratio failing on its structurally different decoder. |
| G4 | The framing decoders are correct across arbitrary split points. | A byte-at-a-time feed losing a block, missing the terminal chunk, or accepting malformed input silently. |
| G5 | Existing behaviour is unchanged. | Any pre-existing translation or live test changing result. |

Measured on acceptance: **OpenAI first item 131 ms, total 751 ms** (server pacing 600 ms);
**Anthropic first item 263 ms, total 883 ms**. G5 holds — both offline translation suites and both
canned-server live suites passed unchanged, as did the live OpenRouter and llama.cpp runs.

G2 is deliberately stated as a *ratio* against the stream's own duration rather than an absolute
millisecond bound. An absolute bound would be measuring the test machine; the ratio measures the
design, and is ~1.0 for the implementation this ADR replaces.

## 4. Consequences

- ADR-017's cancellation now matters in the common case, not only against a deliberately slow server:
  a consumer that stops reading halfway through a long completion releases a connection that is
  genuinely still in flight.
- The ring's backpressure now regulates a live producer rather than a replay, which is what the
  credit-controlled design was for.
- The `on_body` sink runs on the reading thread, so it must not block long — the read loop is stalled
  while it runs. Both backends only decode and `push()` there, and `push()` blocking on ring credit
  *is* the intended backpressure.
- The buffering exchange functions remain, and remain the right choice for `chat()`: a non-streaming
  response genuinely is fully in hand, and nothing is gained by decoding it in pieces.
