#pragma once
// ADR-037 Phase 1: `agentengine::rt::channel<T>`, the second piece of AgentEngine's own runtime
// substrate (after `rt::task<T>`, `rt/task.hpp`). Eventually replaces `quark::ReplyStream<T*>` as
// `core/stream.hpp`'s internal backend (item 4, ADR-037 §4) -- lives under `agentengine::rt`, a NEW
// namespace, deliberately NOT wired into `core/stream.hpp` yet, so nothing in the live, Quark-based
// build is touched by this file existing. Zero `quark::` dependency anywhere below -- that is the
// entire point of this migration.
//
// `core/stream.hpp`'s existing public contract is the design target this type is built to eventually
// power: a producer (a plain worker thread -- real backend HTTP/SSE code runs its blocking read loop
// on a detached thread TODAY, not a coroutine) pushes items and terminates cleanly (`close()`) or with
// an error (`fail()`); a consumer polls non-blockingly (`next()`-shaped) and checks `done()`/
// `terminal()`. This type is a genuinely new, self-contained design, NOT a port of
// `quark::ReplyStream`'s internals (those are Quark-internal and not something this migration can
// reason about the correctness of from outside) -- built from first principles below, with two
// consumer-side surfaces instead of one:
//
//   - SYNCHRONOUS: `try_pop()` -- non-blocking, `std::optional<T>`, exactly like
//     `quark::ReplyStream<T>::next()`'s existing poll contract. `core/stream.hpp::stream<T>::next()`
//     can wrap this verbatim once Phase 2 wires this type in.
//   - ASYNCHRONOUS: `next_async()` -- `co_await`-able, so a coroutine consumer suspends until an item
//     is available instead of spin-polling `try_pop()` in a loop. This is genuinely new relative to
//     `quark::ReplyStream<T>`, which supplies no per-item consumer suspension at all (`core/
//     stream.hpp`'s own top comment names this as an inherited limitation). BOTH halves are fully
//     implemented and tested below (see `tests/test_rt_channel.cpp`), not just the synchronous one.
//
// THREADING MODEL. Producer and consumer genuinely run on different threads (a worker thread pushing,
// a coroutine-driving thread consuming) -- this is not a place to reach for lock-free cleverness; one
// `std::mutex` + one `std::condition_variable` guards the bounded queue, correct-by-construction, and
// throughput is not the goal a first implementation optimizes for. Two different wake mechanisms are
// used deliberately, for two different kinds of waiter:
//
//   - `push()` blocks a real OS THREAD when the queue is at capacity -- an OS thread is exactly what a
//     `std::condition_variable` is for, so it waits on one (`not_full_`), woken by `try_pop()`/
//     `next_async()`'s drain or by any terminal transition.
//   - `next_async()` suspends a COROUTINE, not an OS thread -- parking it on a `condition_variable`
//     would need a second, dedicated waiter thread to eventually notify it (busy machinery this design
//     avoids entirely). Instead, `await_suspend()` does nothing but record the coroutine handle under
//     the same mutex and return -- no loop, no poll, no cv wait. Whichever producer call
//     (`push()`/`close()`/`fail()`) next makes an item or a terminal state available finds that
//     recorded handle and calls `.resume()` on it DIRECTLY, from the producer's own thread, before
//     that producer call returns. The suspended coroutine's remaining body (up to its next suspension
//     or completion) therefore runs momentarily on the producer's thread -- a well-understood,
//     intentional thread hop used by essentially every hand-rolled minimal async-channel/generator
//     (this is not novel research; it is the same trick `cppcoro`-style async generators use). This is
//     why there is no `condition_variable` on the async path: a suspended coroutine has already handed
//     its thread back to whoever resumed it, so "waking" it is just a direct function call once the
//     producer has something to hand over, not a park-and-notify.
//
// SINGLE-CONSUMER. At most one coroutine may have an outstanding `next_async()` await at a time
// (`waiting_consumer_` holds a single handle, not a set) -- matches `core/stream.hpp::stream<T>`'s own
// single-owner drain-handle shape. Calling `next_async()` again before the previous await resolves is
// a caller bug (last-registration-wins, not detected/asserted here -- Phase 1 keeps this type minimal;
// a debug-only assert is easy future-add if a real caller trips it). Multiple PRODUCERS are similarly
// unsupported (`channel_producer<T,E>` is move-only, matching `ae::stream_producer<T>`'s own shape) --
// a real fan-in point, if ever needed, is a caller-side concern, not this channel's.
//
// BACKPRESSURE CHOICE: `push()` BLOCKS (not fails) when full. A chat backend's SSE read-loop thread --
// the real caller this exists for -- has no good fallback if refused: dropping an item silently loses
// data, and a caller-side retry loop would just busy-poll in the caller instead of this type. Blocking
// on a `condition_variable` costs nothing but that one thread's own stack while it waits, and is woken
// promptly the moment the consumer drains -- so "block" is strictly better than "refuse" for this
// type's one real, currently-known caller shape.
//
// ERROR TYPE: a template parameter (`E`, default `std::string`), not a fixed AgentEngine-wide error
// type -- this header intentionally has no dependency on anything from `core/` (that would reintroduce
// exactly the kind of upward coupling ADR-037 is trying to shed at the substrate layer). A future
// `core/stream.hpp` rewrite over this type picks whatever concrete `E` it needs (e.g. `ae::error`) at
// the call site.

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace agentengine::rt {

// The three states a channel can be in. `open` is the only state new items may still be pushed in;
// `closed`/`failed` are both terminal but distinguishable (mirrors `quark::ReplyStreamTerminal`'s own
// closed-vs-failed split, reproduced here rather than assumed, per this file's own "not a port" rule).
enum class channel_terminal { open, closed, failed };

namespace detail {

// The one heap-allocated block shared (via `shared_ptr`) between a `channel_producer<T,E>` and its
// paired `channel_consumer<T,E>` -- it must outlive whichever of the two handles is dropped first,
// which is exactly what `shared_ptr` is for here (no other AgentEngine machinery is available to this
// header by design -- see file banner).
template <class T, class E>
struct channel_state {
    std::mutex m;
    // Guards `push()` when the bounded queue is at capacity. NOT used by the async consumer path --
    // see file banner's "two wake mechanisms" note.
    std::condition_variable not_full;
    std::deque<T> queue;  // a size-checked bounded queue, not a preallocated ring -- see push() below
    std::size_t const capacity;
    channel_terminal terminal = channel_terminal::open;
    std::optional<E> error;
    // At most one coroutine may be parked here at a time (single-consumer, see file banner). Whichever
    // producer call next makes progress possible (`push()`/`close()`/`fail()`) takes this handle,
    // clears the slot, and resumes it directly -- see file banner for why this is not a cv wait.
    std::coroutine_handle<> waiting_consumer;

    explicit channel_state(std::size_t cap) noexcept : capacity(cap) {}
};

}  // namespace detail

// The producer side. Move-only (mirrors `ae::stream_producer<T>`) -- a single logical writer owns the
// push sequence; two live copies pushing concurrently on the SAME logical stream would defeat the
// "one writer" assumption the channel's terminal-transition bookkeeping relies on.
template <class T, class E = std::string>
class channel_producer {
public:
    enum class push_result { ok, terminated };

    channel_producer() noexcept = default;
    explicit channel_producer(std::shared_ptr<detail::channel_state<T, E>> state) noexcept
        : state_(std::move(state)) {}

    channel_producer(channel_producer const&) = delete;
    channel_producer& operator=(channel_producer const&) = delete;
    channel_producer(channel_producer&&) noexcept = default;
    channel_producer& operator=(channel_producer&&) noexcept = default;

    // A producer dropped without an explicit close()/fail() (an early return, an exception unwinding
    // the worker thread) still reaches a clean terminal -- mirrors `ae::stream_producer<T>`'s own
    // fire-default (itself mirroring `quark::ReplyStreamProducer`'s). Without this, a consumer parked
    // in `next_async()`/looping on `try_pop()` could be left waiting forever for a producer that will
    // never call anything again.
    ~channel_producer() {
        if (state_) close();
    }

    // Blocks the calling (real OS) thread while the queue is at capacity, waking only when the
    // consumer drains an item or the channel reaches ANY terminal state -- see file banner for why
    // blocking (not refusing) is this type's deliberate choice. Returns `terminated` (the value is
    // NOT enqueued) if the channel was already closed/failed, or became so, before room existed --
    // e.g. a consumer that cancels by dropping its `channel_consumer` while the producer is mid-block
    // (dropping the consumer's shared_ptr does not itself signal termination in THIS type -- that is a
    // future caller-side concern; here `terminated` covers the already-closed/failed case only).
    [[nodiscard]] push_result push(T value) {
        std::unique_lock lock(state_->m);
        state_->not_full.wait(lock, [this] {
            return state_->queue.size() < state_->capacity || state_->terminal != channel_terminal::open;
        });
        if (state_->terminal != channel_terminal::open) return push_result::terminated;
        state_->queue.push_back(std::move(value));
        // Hand off directly to a parked async consumer, if any -- see file banner. Cleared under the
        // same lock that guarded the push, so a second concurrent producer call (a caller bug, per the
        // move-only/single-writer contract above) cannot double-resume the same handle.
        std::coroutine_handle<> waiter = std::exchange(state_->waiting_consumer, {});
        lock.unlock();
        if (waiter) waiter.resume();
        return push_result::ok;
    }

    // Clean, in-band end-of-stream. Idempotent -- a second call (or the destructor's own fire-default)
    // after an explicit close()/fail() is a harmless no-op, never overwrites an already-set terminal.
    void close() noexcept { finish(channel_terminal::closed, std::nullopt); }
    // Failed terminal, carrying `error` to the consumer.
    void fail(E error) noexcept { finish(channel_terminal::failed, std::move(error)); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
    void finish(channel_terminal which, std::optional<E> err) noexcept {
        if (!state_) return;  // moved-from producer -- destructor's fire-default is a no-op here
        std::coroutine_handle<> waiter;
        {
            std::lock_guard lock(state_->m);
            if (state_->terminal == channel_terminal::open) {
                state_->terminal = which;
                state_->error = std::move(err);
            }
            waiter = std::exchange(state_->waiting_consumer, {});
        }
        // Wakes any thread blocked in push() with a full queue -- close()/fail() are the only OTHER
        // way (besides draining) that push()'s wait predicate can become true.
        state_->not_full.notify_all();
        if (waiter) waiter.resume();
    }

    std::shared_ptr<detail::channel_state<T, E>> state_;
};

// The consumer side. Move-only (mirrors `ae::stream<T>`).
template <class T, class E = std::string>
class channel_consumer {
public:
    channel_consumer() noexcept = default;
    explicit channel_consumer(std::shared_ptr<detail::channel_state<T, E>> state) noexcept
        : state_(std::move(state)) {}

    channel_consumer(channel_consumer const&) = delete;
    channel_consumer& operator=(channel_consumer const&) = delete;
    channel_consumer(channel_consumer&&) noexcept = default;
    channel_consumer& operator=(channel_consumer&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

    // SYNCHRONOUS half. Non-blocking -- std::nullopt means "nothing buffered right now", NOT
    // "finished"; drain to empty then check done()/terminal() to tell the two apart (mirrors
    // `quark::ReplyStream<T>::next()`'s contract, which `ae::stream<T>::next()` already inherits
    // verbatim -- see `core/stream.hpp`).
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock lock(state_->m);
        if (state_->queue.empty()) return std::nullopt;
        T v = std::move(state_->queue.front());
        state_->queue.pop_front();
        lock.unlock();
        state_->not_full.notify_one();  // may unblock a producer parked on a full queue
        return v;
    }

    // True once the queue is drained AND a terminal state has been reached -- false while items
    // remain buffered even after close()/fail(), so a consumer never loses data that was already
    // handed to the channel before termination (zero-data-loss contract).
    [[nodiscard]] bool done() const noexcept {
        std::lock_guard lock(state_->m);
        return state_->queue.empty() && state_->terminal != channel_terminal::open;
    }

    [[nodiscard]] channel_terminal terminal() const noexcept {
        std::lock_guard lock(state_->m);
        return state_->terminal;
    }

    [[nodiscard]] bool failed() const noexcept { return terminal() == channel_terminal::failed; }

    // Valid once terminal() == failed. Returned by value (not reference) under lock -- simple and
    // correct over clever; the error is written once at the terminal transition and this type does
    // not try to hand out a reference whose lifetime story would need separate reasoning.
    [[nodiscard]] std::optional<E> error() const {
        std::lock_guard lock(state_->m);
        return state_->error;
    }

    // Current buffered item count -- diagnostic/test use (e.g. proving push() genuinely blocked at
    // capacity), not part of the drain protocol itself.
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(state_->m);
        return state_->queue.size();
    }

private:
    // ASYNCHRONOUS half's awaiter. See file banner for the full design rationale (no cv on this path
    // -- the producer resumes this directly from its own thread).
    struct next_awaiter {
        channel_consumer* self;
        std::optional<T> value;
        bool have_value = false;

        [[nodiscard]] bool await_ready() {
            std::unique_lock lock(self->state_->m);
            if (!self->state_->queue.empty()) {
                value = std::move(self->state_->queue.front());
                self->state_->queue.pop_front();
                lock.unlock();
                self->state_->not_full.notify_one();
                have_value = true;
                return true;  // resume immediately, no suspension needed
            }
            if (self->state_->terminal != channel_terminal::open) {
                return true;  // drained AND terminal -- resume immediately with a nullopt result
            }
            return false;  // must suspend
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard lock(self->state_->m);
            // Re-check under lock: a push()/close()/fail() may have landed in the gap between
            // await_ready()'s unlock and this lock -- if so, don't suspend at all (returning `false`
            // tells the compiler-generated machinery to resume the coroutine immediately instead).
            if (!self->state_->queue.empty() || self->state_->terminal != channel_terminal::open) {
                return false;
            }
            self->state_->waiting_consumer = h;
            return true;  // genuinely suspend -- a producer call resumes `h` directly, later
        }

        [[nodiscard]] std::optional<T> await_resume() {
            if (have_value) return std::move(value);
            // Either await_ready()'s second branch fired (drained+terminal, no value), or a producer
            // call woke us after enqueuing something -- check the queue once more under lock.
            std::unique_lock lock(self->state_->m);
            if (!self->state_->queue.empty()) {
                T v = std::move(self->state_->queue.front());
                self->state_->queue.pop_front();
                lock.unlock();
                self->state_->not_full.notify_one();
                return v;
            }
            return std::nullopt;
        }
    };

public:
    // Returns an awaiter -- `co_await consumer.next_async()` from inside any coroutine (e.g.
    // `agentengine::rt::task<T>`; this type has no dependency on `task.hpp` at all, since the standard
    // awaiter protocol below needs no `await_transform` customization to be `co_await`-able from a
    // `task<T>` coroutine body). `std::nullopt` from the resulting `co_await` means "drained and
    // terminal", exactly like `try_pop()` plus a `done()` check, but without the caller ever having to
    // poll: `await_suspend()` above does nothing but record a handle and return -- no loop, no cv wait
    // on this thread. See file banner for the full rationale.
    [[nodiscard]] next_awaiter next_async() noexcept { return next_awaiter{this}; }

private:
    std::shared_ptr<detail::channel_state<T, E>> state_;
};

template <class T, class E = std::string>
struct channel_pair {
    channel_producer<T, E> producer;
    channel_consumer<T, E> consumer;
};

// Constructs a fresh, directly-connected producer/consumer pair over a bounded queue of capacity
// `capacity` (must be >= 1 -- a zero-capacity channel could never admit a single push() and is almost
// certainly a caller bug, not a real use case, so this is left as an unchecked precondition rather
// than an added runtime branch every push() would otherwise pay for).
template <class T, class E = std::string>
[[nodiscard]] channel_pair<T, E> make_channel(std::size_t capacity) {
    auto state = std::make_shared<detail::channel_state<T, E>>(capacity);
    return channel_pair<T, E>{channel_producer<T, E>(state), channel_consumer<T, E>(state)};
}

}  // namespace agentengine::rt
