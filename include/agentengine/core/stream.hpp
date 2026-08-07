#pragma once
// Implements 004-Model-Provider-Plane.md §1's `ae::stream<T>` — the AgentEngine-side adapter over
// Quark's already-Accepted ReplyStream/StreamChannel credit-controlled ring (Quark RFC 024, ADR-018).
//
// Milestone 5 Phase B4b (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, decision
// 3): "ae::stream<T> is an AgentEngine-side wrapper over Quark's already-Accepted ReplyStream/
// StreamChannel primitives... not a new Quark-side ask... only the adapter shaping it as 'one HTTP SSE
// chunk per credit-controlled item' is missing."
//
// Two adaptations over the raw Quark primitive, both deliberate and named rather than silently done:
//
// (1) BOXING. Quark's ring is inline-slot only today: `quark::StreamChannel<F>::static_assert`
//     requires F to be trivially copyable; `StreamMode::ZeroCopyRetained` (the by-reference regime) is
//     a declared 019/003 seam, "stubbed, never wired" (stream_channel.hpp's own top comment).
//     `ChatResponseUpdate` carries a `ContentItem` (a `std::string`-backed `Text`), so it is NOT
//     trivially copyable and cannot ride the ring directly. `stream<T>`/`stream_producer<T>` box each
//     item on the heap (one alloc on push, one free on drain) behind a raw owning pointer — itself
//     trivially copyable (a pointer is POD) — so arbitrary movable T can flow through the ring. This
//     trades away ADR-018's own measured "0 per-item heap" property for non-trivial T ONLY; named here
//     rather than silently claimed. A trivially-copyable specialization that skips the box is future
//     reach, not built here since no caller needs it yet.
//
// (2) NO ACTOR ADDRESSING. `chat_stream()` is a plain, synchronous, in-process call — 004 §1's literal
//     signature is `ae::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&)`, NOT
//     `ae::task<...>` — so there is no cross-actor OPEN-handshake race to arbitrate the way Quark's own
//     `ask_stream<F>` must (caller and callee may be different actors on different shards/nodes).
//     `make_stream<T>` below still reuses Quark's PROVEN `make_ask_stream`/`StreamResponder::accept`/
//     `block_on_open` sequence rather than hand-rolling a second construction path around
//     `ReplyStreamState` directly — `accept()` resolves the OPEN cell synchronously and
//     `block_on_open()` returns immediately after, on the SAME thread, with no actor dispatch or
//     `Activation::complete_parked()` inline-resume anywhere in the call chain. This deliberately
//     avoids the documented reentrancy hazard named in ADR-018's residual risks (accept()'s OPEN
//     resolve can run the caller's ENTIRE resumed continuation inline when it crosses an actor
//     boundary) — that hazard is specific to route (a), draining a stream opened via a real
//     `ask_stream<F>` actor ask; route (b), used here, never crosses an actor boundary at all.
//     `std::monostate` is a throwaway `make_ask_stream` query payload; no real conformer ever sees it.
//
// `ReplyStream<F>::next()` is poll-only, never `co_await`-able per item (Quark supplies no per-item
// consumer suspension) — `stream<T>::next()` inherits that same poll contract; a caller drains with
// `while (auto x = s.next()) { ... }` then inspects `done()`/`terminal()`.

#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>
#include <variant>

#include "quark/core/reply_stream.hpp"

namespace agentengine {

// The producer side — held by a `ChatClient` conformer's `chat_stream()` implementation, or by
// whatever background execution context (thread, detached task) it hands production off to, since
// `chat_stream()` itself returns synchronously and cannot keep producing after it returns. Move-only
// (the ring's single-writer token guards a second bind).
template <class T>
class stream_producer {
public:
    stream_producer() noexcept = default;
    explicit stream_producer(quark::ReplyStreamProducer<T*> inner) noexcept : inner_(std::move(inner)) {}

    stream_producer(const stream_producer&) = delete;
    stream_producer& operator=(const stream_producer&) = delete;
    stream_producer(stream_producer&&) noexcept = default;
    stream_producer& operator=(stream_producer&&) noexcept = default;

    // Boxes `value` on the heap and blocks (losslessly) until the ring has credit or the stream is torn
    // down. `Terminated` means the consumer cancelled/deadlined (or dropped the stream) — stop
    // producing; the box is freed right here, so a Terminated push never leaks.
    [[nodiscard]] quark::ReplyPush push(T value) {
        T* boxed = new T(std::move(value));
        quark::ReplyPush outcome = inner_.push(boxed);  // push the POINTER (F = T*), not the pointee
        if (outcome != quark::ReplyPush::Ok) delete boxed;  // never delivered -- free it here, not the ring
        return outcome;
    }

    // In-band EoS. Idempotent; also fires automatically (as Closed) if the producer is dropped without
    // an explicit close()/fail() (quark::ReplyStreamProducer's own destructor fire-default).
    void close() noexcept { inner_.close(); }
    // Mid-stream failure terminal — carries the error to the consumer.
    void fail(quark::error e) noexcept { inner_.fail(e); }
    [[nodiscard]] bool valid() const noexcept { return inner_.valid(); }

private:
    quark::ReplyStreamProducer<T*> inner_;
};

// The consumer-side drain handle — 004 §1's literal `ae::stream<T>` return type of `chat_stream()`.
// Move-only; dropping it cancels the stream (mirrors `quark::ReplyStream<T*>`'s own destructor).
template <class T>
class stream {
public:
    stream() noexcept = default;
    explicit stream(quark::ReplyStream<T*> inner) noexcept : inner_(std::move(inner)) {}

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;
    stream(stream&&) noexcept = default;
    stream& operator=(stream&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return inner_.valid(); }

    // Pulls the next item, taking ownership of the boxed payload and freeing the box. std::nullopt when
    // nothing is buffered right now -- drain to empty, then check done() (mirrors
    // quark::ReplyStream<T>::next()'s own contract verbatim).
    [[nodiscard]] std::optional<T> next() {
        std::optional<T*> boxed = inner_.next();
        if (!boxed) return std::nullopt;
        std::unique_ptr<T> owned(*boxed);
        return std::optional<T>(std::move(*owned));
    }

    [[nodiscard]] bool done() const noexcept { return inner_.done(); }
    [[nodiscard]] quark::ReplyStreamTerminal terminal() const noexcept { return inner_.terminal(); }
    [[nodiscard]] quark::error fail_error() const noexcept { return inner_.fail_error(); }
    [[nodiscard]] bool gap_detected() const noexcept { return inner_.gap_detected(); }
    // Caller-initiated teardown; also happens implicitly on destruction.
    void cancel() noexcept { inner_.cancel(); }

private:
    quark::ReplyStream<T*> inner_;
};

template <class T>
struct stream_pair {
    stream_producer<T> producer;
    stream<T> consumer;
};

// Just `capacity`/`low_watermark` (quark::StreamChannel<F>::Config) -- a caller tuning ring capacity
// never needs to know the ring's actual frame type is boxed (`T*`, not `T`).
template <class T>
using stream_config = typename quark::ReplyStreamState<T*>::Config;

// Constructs a fresh, directly-connected producer/consumer pair over a credit-controlled ring of
// capacity `cfg.capacity` (default 256, Quark's own default) -- no actor addressing, no OPEN-handshake
// race (see file banner). `mr` backs the ring's slot array (an array of `T*`, not of `T` -- boxed items
// are plain `new`/`delete`) and must outlive both handles.
template <class T>
[[nodiscard]] stream_pair<T> make_stream(std::pmr::memory_resource* mr, stream_config<T> cfg = {}) {
    auto req = quark::make_ask_stream<std::monostate, T*>(std::monostate{}, mr, cfg);
    quark::ReplyStreamProducer<T*> producer = req.envelope.respond.accept();
    quark::result<quark::ReplyStream<T*>> opened = quark::block_on_open(std::move(req.future));
    // accept() resolves the OPEN cell synchronously, immediately above, on this same thread -- no actor
    // dispatch, no suspension, nothing that could race or fail in between. Cannot fail.
    return stream_pair<T>{stream_producer<T>(std::move(producer)), stream<T>(std::move(*opened))};
}

}  // namespace agentengine
