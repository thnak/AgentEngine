# ADR-017 — Propagating consumer cancellation into a stream producer's blocking I/O

**Status:** Accepted — 2026-08-07
**Closes:** the Milestone 5 Phase J3 residual, "`chat_stream()`'s underlying fetch has no cancellation
wiring from the consumer" (`docs/planning/milestone-5-providers-identity-secrets-breakdown.md`).
**Relates to:** 004-Model-Provider-Plane.md §7 G2, ADR-018 (Quark's credit-controlled ring),
ADR-011/ADR-013 (the egress read loop and its `stop_token` cancellation).

## 1. Context

004 §7 G2 requires that "cancellation mid-stream releases the connection within a bounded time and
leaves no orphaned socket or partial state." Phase J3 built the proof for that gate and, in doing so,
found the property did not hold — and said so rather than claiming the gate covered:

> `run_stream_worker` calls `perform_provider_https_exchange(..., /*stop_token=*/std::nullopt, ...)`
> at BOTH the OpenAI and Anthropic `chat_stream()` call sites — dropping the consumer's `stream<T>`
> cancels the RING but has no wiring back into the underlying HTTP fetch's own cancellation at all.

Quantified at the time: against a server dripping its SSE body over ~900ms, cancelling ~20ms in did
not shorten the connection's real lifetime by any measurable amount. Release was bounded only by the
coarse 10s `kIoTimeoutMs` stall detector.

The root cause is a genuine gap in the ring's shape, not an oversight at the call site. A
`stream_producer<T>` learns the consumer is gone **only by attempting a `push()`** and getting
`Terminated` back. That is precisely useless for a producer that is blocked in a long I/O call and has
nothing to push yet — which is exactly what `run_stream_worker` is for the entire duration of a fetch.

Phase J3 deferred the fix deliberately, as "an invariant-adjacent, hot-path concurrency change that
belongs behind CLAUDE.md's design → red-team → prove → judge cycle, not an ad-hoc patch." This ADR is
that cycle's output.

## 2. Options considered

**(a) Poll `ReplyStreamProducer::terminal()` from a watcher thread.** `terminal()` is already a
thread-safe accessor, so a second thread per stream could poll it and request stop. Rejected: a thread
per in-flight stream is a real cost on a hot path, the poll interval becomes a new latency floor, and
it adds a second lifetime to reason about on a path that already uses a *detached* worker.

**(b) Push a zero-sized probe item to sense `Terminated`.** Rejected outright: it corrupts the item
stream to ask a control question, and `push()` blocks on credit anyway, so it can stall exactly when
the answer matters most.

**(c) Extend Quark's `ReplyStreamState` with a native cancellation callback.** The cleanest layering
in the abstract, but it is an upstream change to a security- and concurrency-critical primitive
(ADR-018) for a need that is currently AgentEngine-side only. Deferred, not rejected: if a second
consumer of this pattern appears, this becomes the right answer.

**(d) Share one `std::stop_source` between the two halves. — CHOSEN.**

## 3. Decision

`make_stream<T>` constructs one `std::stop_source` and gives a **copy** to each half.
`std::stop_source`'s copy constructor shares the associated stop-state rather than duplicating it, so
this costs no extra allocation and needs no plumbing between the halves.

- `stream<T>::cancel()` — cancels the ring, then requests stop.
- `~stream<T>()` — requests stop (the "caller just stopped caring" shape).
- `stream_producer<T>::stop_token()` — the matching token, for handing to blocking work.

Both `chat_stream()` backends pass that token to `perform_provider_https_exchange`, reaching the
mid-flight cancellation Phase C2 already proved at the raw exchange layer (`test_provider_http_client
.cpp`'s P2 case, ~440ms, `std::stop_token`-driven). That mechanism was always real; what was missing
was a caller threading a token through it.

### Properties this deliberately preserves

- **The ring is unchanged.** This is a second, *parallel* signal for the I/O layer, not a replacement
  for `push()`'s `Terminated`. A producer that ignores `stop_token()` behaves exactly as before, so
  every existing `stream<T>` user is unaffected.
- **Ordering.** `cancel()` tears down the ring *before* requesting stop, so a producer that observes
  the stop and then attempts one final `push()` still sees `Terminated`, never a half-torn ring.
- **Lifetime.** `std::stop_token` keeps the shared stop-state alive independently of both handles, so
  the token stays valid after the consumer *and* the producer are destroyed. That is what makes it
  safe on the detached worker thread both backends use.
- **A moved-from `stream<T>` cannot cancel the live one it was moved into.** `std::stop_source`'s move
  constructor leaves the source with no stop-state, so `~stream()` on the husk is a no-op.

### One sharp edge, called out because it is invisible at the call site

Argument evaluation order is unspecified in C++. Written inline, `pair.producer.stop_token()` next to
`std::move(pair.producer)` in the same call may legally be evaluated *after* the move — reading a
moved-from producer whose `stop_source` is empty, yielding a token that never fires. The bug would be
silent: cancellation would simply stop working, exactly the regression this ADR exists to fix. Both
call sites therefore bind the token to a local first, with a comment saying why.

## 4. Falsifiable gates

| # | Claim | What would falsify it |
|---|---|---|
| G1 | Cancelling a slow in-flight stream cuts the connection short. | The slow-drip server completing its full paced send after a cancel (`completed_fully()` true). |
| G2 | Release is bounded by the cancellation, not the peer's pacing. | The server delivering more than ~2 of its 6 chunks — i.e. the connection outliving ~2 drip intervals. |
| G3 | The common (fast-provider) case is unchanged. | J3-R1/R2 regressing: a fast stream failing to reach `Closed`, or exceeding 2s end to end. |
| G4 | Existing `stream<T>` semantics are untouched. | Any pre-existing stream test changing behaviour — `test_chat_client_stream.cpp`, the backpressure and cancel cases in both `*_chat_client_live.cpp`, or the live E2E streaming cases. |

Measured on acceptance: **1 of 6 chunks** delivered before teardown, against 6 of 6 before this
change. G1–G4 all hold.

### What the harness cannot measure, stated rather than glossed

Two available timestamps are misleading and are printed as context but asserted on nowhere:

- **The consumer's scope exit.** `cancel()`/`~stream()` are non-blocking and always were, so timing
  the scope passes identically with and without this change. A check that cannot fail proves nothing.
- **The server's `finished_at()`.** Measured ~2150ms after the cancel — but that is the test server's
  own 2000ms BIO timeout elapsing on a write to a departed peer. It says when the *server noticed*,
  not when the *client released*. Asserting a millisecond bound on it would be asserting on an mbedTLS
  timeout constant.

The quantity that is both meaningful and precisely measurable is how far the server got through its
own paced send: each delivered chunk costs a known 150ms, so "1 of 6" is a real time bound expressed
in the peer's own units.

## 5. Consequences

- 004 §7 G2's "bounded time" now holds for `chat_stream()`'s genuinely-in-flight case, not only for
  the trivial fast-provider case where the fetch had already finished.
- Any future `stream<T>` producer that drives blocking work gets the same signal for free — this is
  the reason it lives on `stream_producer<T>` rather than being open-coded in the two backends.
- The value grows when Phase C's other named gap (no incremental read loop) closes: today's
  non-incremental fetch means most real responses finish before cancellation matters, which is
  exactly why J3 could only find this by deliberately testing a slow server.
- Option (c) remains the better layering if a second use appears. Revisit then, not before.
