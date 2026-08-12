#pragma once
// Implements 004-Model-Provider-Plane.md §1's `ae::stream<T>`. ADR-037: this file's INTERNAL backend
// is now `agentengine::rt::channel<T, quark::error>` (rt/channel.hpp) instead of Quark's actor-
// integrated `ReplyStream`/`StreamChannel`/ask-stream OPEN-handshake machinery -- the mailbox-adjacent
// runtime dependency ADR-037 exists to remove. Milestone 5 Phase B4b's own original design note
// ("ae::stream<T> is an AgentEngine-side wrapper over Quark's already-Accepted ReplyStream/
// StreamChannel primitives") is superseded by this migration; the paragraphs below replace it.
//
// SCOPE, DELIBERATELY NARROW -- named, not silently oversold. This swaps the INTERNAL backend only.
// The PUBLIC API keeps its EXACT existing shape: `terminal()` still returns `quark::ReplyStreamTerminal`,
// `fail_error()` still returns `quark::error`, `push()` still returns `quark::ReplyPush`. Every existing
// call site (`core/chat_stream_drain.hpp`, `core/recording_chat_client.hpp`, `core/replay_chat_client.hpp`,
// both `AgentSession` variants, every live ChatClient backend and its tests) needed ZERO source changes
// as a result of this migration -- confirmed by rebuilding and running the full suite unchanged. This is
// a deliberate choice, not an oversight:
//
//   `quark::error`/`quark::ReplyStreamTerminal`/`quark::ReplyPush` (quark/core/error.hpp,
//   quark/core/reply_stream.hpp's own enums) are plain, trivially-copyable value types with NO actor/
//   mailbox/scheduler coupling of their own -- reusing them here costs nothing at the "no more Quark
//   RUNTIME dependency" level ADR-037 actually cares about. What is REMOVED by this migration is the
//   genuinely actor-integrated machinery: `quark::ReplyStream<T*>`/`ReplyStreamProducer<T*>`/
//   `StreamChannel<F>` (the credit-controlled ring itself, tied to Quark's own scheduler/dispatch
//   assumptions) and the `make_ask_stream`/`StreamResponder::accept`/`block_on_open` OPEN-handshake
//   ceremony (built to arbitrate a cross-actor ask that never applied to this file's own "plain,
//   synchronous, in-process `chat_stream()` call" usage in the first place -- see the retired design
//   note's own point (2), preserved below since the reasoning still holds against `rt::channel<T>` too).
//
//   Migrating the PUBLIC types as well (`quark::error` -> `agentengine::error`, `ReplyStreamTerminal`
//   -> a native enum) is real, separately-scoped follow-up work this pass deliberately does NOT
//   attempt, because it is entangled far beyond this file: `core/chat_stream_drain.hpp`'s
//   `classify_drained_failure()` and `ModelCallGateway`'s own retry logic are keyed DIRECTLY on
//   `quark::errc` values fed by HTTP-status classification in `protocol/openai/chat_client.hpp`/
//   `protocol/anthropic/chat_client.hpp`, and `core/replay_chat_client.hpp`/`core/recording_chat_client.hpp`
//   round-trip `quark::error`/`quark::ReplyStreamTerminal` through the PERSISTED recording format
//   itself (`terminal_to_quark_error()`, the wire-string mapping switch). Rewriting that whole error
//   vocabulary is a real, valuable task -- Phase 4's eventual submodule removal still needs it done
//   before `quark::error`/`quark::ReplyStreamTerminal` can stop existing at all -- but it is NOT a
//   "swap the backend" change, and bundling it into this pass would have meant a much larger, riskier
//   edit across the entire model-call/streaming error-classification surface for one migration. Named
//   here as the real residual it is, not silently narrowed away.
//
// (Renumbered from the retired design note, preserved verbatim where the reasoning is unchanged)
//
// (1) BOXING -- GONE. Quark's ring was inline-slot only (`StreamChannel<F>` required F trivially
//     copyable), which forced every non-trivially-copyable T (e.g. `ChatResponseUpdate`, carrying a
//     `ContentItem` with a `std::string`-backed `Text`) to be boxed on the heap (`new T`/`delete`) and
//     pushed as a raw pointer. `rt::channel<T,E>`'s queue is a `std::deque<T>` holding T BY VALUE
//     directly -- no such constraint exists, so the box-on-push/unbox-on-drain dance is simply gone.
//     ADR-018's own "0 per-item heap" measurement, previously traded away for non-trivial T only, is
//     restored as a side effect of this migration, not something it had to separately earn.
//
// (2) NO ACTOR ADDRESSING (unchanged reasoning, new backend). `chat_stream()` is a plain, synchronous,
//     in-process call -- there is no cross-actor OPEN-handshake race to arbitrate the way Quark's own
//     `ask_stream<F>` had to. `rt::make_channel<T,E>` reflects this directly: a channel pair is
//     constructed and handed to both sides with no handshake step at all, not merely a construction
//     path that happens to avoid one (as `make_stream`'s own retired implementation did, by reusing
//     Quark's `make_ask_stream`/`accept`/`block_on_open` sequence specifically to sidestep the
//     documented ADR-018 reentrancy hazard that applies only to a REAL cross-actor `ask_stream<F>`
//     open, never to this file's own always-local usage).
//
// `rt::channel<T,E>::try_pop()` is poll-only, never `co_await`-able through THIS wrapper -- `stream<T>`
// inherits that same poll contract (`next_async()` exists on the underlying `channel_consumer<T,E>` for
// a caller that constructs one directly, but `stream<T>` itself does not expose it, matching the
// original's own poll-only public surface exactly). A caller drains with
// `while (auto x = s.next()) { ... }` then inspects `done()`/`terminal()`.
//
// (3) AN OUT-OF-BAND CANCELLATION SIGNAL, alongside the channel's own (ADR-017) -- UNCHANGED in
//     intent, now backed by `rt::channel<T,E>::cancel()` (its own ADR-037 addition, this same phase)
//     instead of `quark::ReplyStream::cancel()`. `channel_producer<T,E>::push()` blocking a real OS
//     thread on a full queue with nobody left to drain is EXACTLY the deadlock that addition closes --
//     `stream<T>::cancel()` (and its destructor) call the channel's own `cancel()` for that reason, in
//     the same "ring first, then the external stop signal" order the original established (a producer
//     that observes the stop token first and then attempts a final push must still see `Terminated`,
//     never a half-torn-down channel) -- unified here through one explicit `cancel()` method rather
//     than the original's implicit-via-member-destruction-order design, so both the explicit-call path
//     and the destructor path get the identical, correctly-ordered sequence rather than two different
//     orderings that happened to both work.
//
// `gap_detected()` -- ALWAYS FALSE, now a real (not merely observed) invariant rather than a runtime
// check: `rt::channel<T,E>` has no credit/retry/dedup concept to gap in the first place -- `push()`
// BLOCKS rather than drops or retries with a re-offered identity (`StreamChannel<F>`'s own
// `producer_seq`/dedup machinery, which is what `gap_detected()` originally inspected), so a delivered
// sequence structurally cannot skip an item. Kept as a method (not removed) purely for source
// compatibility with existing callers that check it (`core/chat_client.hpp`'s tests) -- always
// returning `false` is not a narrowing of behavior those callers could ever observe as different, since
// nothing in this codebase's real usage ever produced a gap through this path even under the old
// backend either.

#include <memory_resource>
#include <optional>
#include <stop_token>
#include <utility>

#include "agentengine/rt/channel.hpp"

#include "quark/core/error.hpp"
#include "quark/core/reply_stream.hpp"

namespace agentengine {

// The producer side — held by a `ChatClient` conformer's `chat_stream()` implementation, or by
// whatever background execution context (thread, detached task) it hands production off to, since
// `chat_stream()` itself returns synchronously and cannot keep producing after it returns. Move-only
// (the channel's single-writer contract, rt/channel.hpp, guards a second bind).
template <class T>
class stream_producer {
public:
    stream_producer() noexcept = default;
    explicit stream_producer(rt::channel_producer<T, quark::error> inner,
                              std::stop_source stop = {}) noexcept
        : inner_(std::move(inner)), stop_(std::move(stop)) {}

    stream_producer(const stream_producer&) = delete;
    stream_producer& operator=(const stream_producer&) = delete;
    stream_producer(stream_producer&&) noexcept = default;
    stream_producer& operator=(stream_producer&&) noexcept = default;

    // Pushes `value` directly -- no boxing (see file banner) -- blocking (losslessly) until the
    // channel has room or reaches ANY terminal state. `Terminated` means the consumer cancelled/
    // dropped its `stream<T>`, or the channel was already closed/failed -- stop producing.
    [[nodiscard]] quark::ReplyPush push(T value) {
        auto const r = inner_.push(std::move(value));
        return r == rt::channel_producer<T, quark::error>::push_result::ok ? quark::ReplyPush::Ok
                                                                             : quark::ReplyPush::Terminated;
    }

    // In-band EoS. Idempotent; also fires automatically (as Closed) if the producer is dropped without
    // an explicit close()/fail() (rt::channel_producer's own destructor fire-default).
    void close() noexcept { inner_.close(); }
    // Mid-stream failure terminal — carries the error to the consumer.
    void fail(quark::error e) noexcept { inner_.fail(std::move(e)); }
    [[nodiscard]] bool valid() const noexcept { return inner_.valid(); }

    // ADR-017. Hand this to any blocking work this producer drives that would otherwise keep running
    // after the consumer has gone -- an HTTP read loop, a socket wait, a subprocess. Requested when
    // the consumer calls `cancel()` or drops its `stream<T>`, WITHOUT the producer having to attempt a
    // `push()` first (the whole point: a producer stalled in I/O has nothing to push yet).
    //
    // The token keeps the shared stop-state alive on its own, so it stays valid after the consumer --
    // and this producer -- are destroyed. Safe to hold on a detached thread, which is exactly how both
    // real `chat_stream()` backends use it.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_.get_token(); }

private:
    rt::channel_producer<T, quark::error> inner_;
    std::stop_source stop_;
};

// The consumer-side drain handle — 004 §1's literal `ae::stream<T>` return type of `chat_stream()`.
// Move-only; dropping it cancels the stream (mirrors `rt::channel_consumer<T,E>`'s own destructor).
template <class T>
class stream {
public:
    stream() noexcept = default;
    explicit stream(rt::channel_consumer<T, quark::error> inner, std::stop_source stop = {}) noexcept
        : inner_(std::move(inner)), stop_(std::move(stop)) {}

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;
    stream(stream&&) noexcept = default;
    stream& operator=(stream&&) noexcept = default;

    // Routed through the same explicit cancel() the caller-visible method uses -- see file banner's
    // point (3) for why this file no longer relies on member-destruction order to get the "channel
    // first, then the out-of-band stop signal" sequence right.
    ~stream() { cancel(); }

    [[nodiscard]] bool valid() const noexcept { return inner_.valid(); }

    // Pulls the next item directly -- no unboxing (see file banner). std::nullopt when nothing is
    // buffered right now -- drain to empty, then check done() (mirrors the original's own contract
    // verbatim).
    [[nodiscard]] std::optional<T> next() { return inner_.try_pop(); }

    [[nodiscard]] bool done() const noexcept { return inner_.done(); }

    [[nodiscard]] quark::ReplyStreamTerminal terminal() const noexcept {
        switch (inner_.terminal()) {
            case rt::channel_terminal::open:      return quark::ReplyStreamTerminal::Open;
            case rt::channel_terminal::closed:    return quark::ReplyStreamTerminal::Closed;
            case rt::channel_terminal::cancelled: return quark::ReplyStreamTerminal::Cancelled;
            case rt::channel_terminal::failed:    return quark::ReplyStreamTerminal::Failed;
        }
        return quark::ReplyStreamTerminal::Cancelled;  // unreachable -- channel_terminal is exhaustive
                                                        // above; see file banner on DeadlineExceeded
                                                        // never being reachable through this path
    }
    [[nodiscard]] quark::error fail_error() const noexcept { return inner_.error().value_or(quark::error{}); }
    // Always false -- see file banner: rt::channel<T,E> structurally cannot gap (blocks, never drops
    // or dedups a retried identity).
    [[nodiscard]] bool gap_detected() const noexcept { return false; }

    // Caller-initiated teardown; also happens implicitly on destruction. Tears down the channel AND
    // fires the out-of-band stop signal (ADR-017), in that order -- a producer that observes the stop
    // first and then attempts a final push must still see `Terminated`, never a half-torn-down channel.
    void cancel() noexcept {
        inner_.cancel();
        stop_.request_stop();
    }

private:
    rt::channel_consumer<T, quark::error> inner_;
    std::stop_source stop_;
};

template <class T>
struct stream_pair {
    stream_producer<T> producer;
    stream<T> consumer;
};

// Just `capacity` -- Quark's own `low_watermark` (a ring hysteresis re-arm threshold) has no
// `rt::channel<T,E>` equivalent (it blocks rather than using a credit/hysteresis scheme in the first
// place) and, confirmed by search, no real caller in this codebase ever set it -- dropped here as a
// genuine simplification, not a silently narrowed field. Still templated on `T` (even though nothing
// in the body depends on it) purely for source compatibility with existing call sites that write
// `stream_config<SomeType>`.
template <class T>
struct stream_config {
    std::size_t capacity = 256;  // matches Quark's own prior default
};

// Constructs a fresh, directly-connected producer/consumer pair over a bounded channel of capacity
// `cfg.capacity` -- no actor addressing, no OPEN-handshake (see file banner). `mr` is accepted for
// source compatibility with every existing call site (`std::pmr::get_default_resource()`, etc.) but is
// UNUSED: `rt::channel<T,E>`'s queue is a plain `std::deque<T>` with no custom allocator wired in --
// named here rather than silently ignored without comment.
template <class T>
[[nodiscard]] stream_pair<T> make_stream(std::pmr::memory_resource* /*mr*/, stream_config<T> cfg = {}) {
    auto pair = rt::make_channel<T, quark::error>(cfg.capacity);
    // ADR-017: ONE stop-state, shared by copy. `std::stop_source`'s copy constructor shares the
    // associated stop-state rather than duplicating it, so the consumer's `request_stop()` is visible
    // through the producer's `stop_token()` with no extra allocation and no plumbing between them.
    std::stop_source stop;
    return stream_pair<T>{stream_producer<T>(std::move(pair.producer), stop),
                          stream<T>(std::move(pair.consumer), stop)};
}

}  // namespace agentengine
