#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.2/§4.4/§6.1 -- the I/O reactor
// seam (`pal::Reactor`), first slice: timers. Sockets, pipes and process waits extend this seam in later
// steps of the ADR's implementation order (§9 D2); the threading rules below already hold for them.
//
// The seam is std-only (CONVENTIONS: OS specifics and third-party code stay behind `pal`). The one
// production backend is standalone Asio, compiled once in src/backends/reactor_asio/ (§9 D1); no Asio type
// appears in this header. A manual, virtual-time implementation for tests lives in
// include/agentengine/testing/manual_reactor.hpp.
//
// THREADING CONTRACT (every implementation):
//   - A reactor owns one serial context, its "reactor thread" (the Asio backend: one thread running one
//     io_context). Initiation, completion and cancellation of every operation run on it -- callers on any
//     thread only POST to it (§4.4, round-2 M2: an I/O object is never touched from two threads).
//   - `ReactorOp::on_complete` runs on the reactor thread, exactly once per started operation, and must only
//     enqueue: it never runs engine coroutines inline (§4.2). `rt::sleep_until` (rt/sleep.hpp) is the
//     reference user of that rule.
//   - Cancellation is two-phase (§4.4, round-1 M1): `cancel()` records a sticky request and posts it; the
//     operation's own completion -- `completed` if it had already finished, `canceled` otherwise -- is what
//     the waiter is resumed from. A cancel that lands before the operation started makes the start complete
//     it immediately as `canceled` (round-2 M2: a stop is never lost between registration and initiation).
//   - Shutting a reactor down cancels every pending operation and delivers its completion before the
//     reactor thread is joined (§4.5 rule 4: no completion is dropped, no thread is detached).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace agentengine::pal {

// ae-naming-lint: allow op_status — ADR-237 §6.1: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class op_status : std::uint8_t {
    completed,  // the operation finished (a timer expired)
    canceled,   // stopped by a cancel request or by reactor shutdown before it finished
};

// One operation's record, shared between its waiter and the reactor (refcounted: whichever releases last
// frees it -- round-1 M1). The waiter derives from it to carry its own state (rt/sleep.hpp).
// ae-naming-lint: allow ReactorOp — ADR-237 §6.1: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class ReactorOp {
public:
    virtual ~ReactorOp() = default;
    ReactorOp(ReactorOp const&)            = delete;
    ReactorOp& operator=(ReactorOp const&) = delete;

    // Called once, on the reactor thread. Must only enqueue (see the contract above).
    virtual void on_complete(op_status status) noexcept = 0;

    // Sticky cancel request (round-2 M2). Set by `Reactor::cancel` on any thread; read by the backend on the
    // reactor thread when it starts the operation.
    [[nodiscard]] bool cancel_requested() const noexcept { return cancel_requested_.load(std::memory_order_acquire); }

protected:
    ReactorOp() noexcept = default;

private:
    friend class Reactor;
    std::atomic<bool> cancel_requested_{false};

public:
    // Backend-private state for this operation (the Asio backend keeps its timer here). Touched only on the
    // reactor thread. Public only so a backend in another translation unit can reach it; nothing else may.
    std::shared_ptr<void> backend_state;
};

// ae-naming-lint: allow Reactor — ADR-237 §6.1: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class Reactor {
public:
    using clock = std::chrono::steady_clock;

    virtual ~Reactor() = default;
    Reactor(Reactor const&)            = delete;
    Reactor& operator=(Reactor const&) = delete;

    // Starts a one-shot timer that completes at `deadline` (`completed`) or on cancel/shutdown
    // (`canceled`). Thread-safe: the start is posted to the reactor thread. `op->on_complete` is called
    // exactly once.
    virtual void start_timer(std::shared_ptr<ReactorOp> op, clock::time_point deadline) = 0;

    // Requests cancellation of `op`. Thread-safe, never blocks, may be called before the start has reached
    // the reactor thread (the request is sticky) or after the operation completed (no effect).
    void cancel(std::shared_ptr<ReactorOp> const& op) {
        op->cancel_requested_.store(true, std::memory_order_release);
        post_cancel(op);
    }

    // True on the reactor thread. Used to enforce "the reactor never runs engine code" (§4.2).
    [[nodiscard]] virtual bool on_reactor_thread() const noexcept = 0;

    // Count of wake-ups refused because the waiter had no home to be posted to (§4.2): a coroutine resumed
    // raw, outside block_on() and outside any ScopedResumer, that parked on this reactor. Such a waiter is
    // never resumed on the reactor thread. Diagnostic; monotonic.
    [[nodiscard]] std::uint64_t homeless_refusals() const noexcept {
        return homeless_refusals_.load(std::memory_order_relaxed);
    }
    void note_homeless_refusal() noexcept { homeless_refusals_.fetch_add(1, std::memory_order_relaxed); }

protected:
    Reactor() noexcept = default;
    // Posts the cancellation of `op` to the reactor thread. The sticky flag is already set.
    virtual void post_cancel(std::shared_ptr<ReactorOp> const& op) = 0;

private:
    std::atomic<std::uint64_t> homeless_refusals_{0};
};

// The production reactor (standalone Asio, one reactor thread). Defined in src/backends/reactor_asio/.
[[nodiscard]] std::unique_ptr<Reactor> make_default_reactor();

}  // namespace agentengine::pal
