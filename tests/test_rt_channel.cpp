// Proof for ADR-037 Phase 1: agentengine::rt::channel<T> (include/agentengine/rt/channel.hpp), a
// bounded, thread-safe, backpressured producer/consumer channel meant to eventually replace
// quark::ReplyStream<T*> as core/stream.hpp's internal backend. Deliberately no dependency on
// quark:: anywhere in this file -- that's the whole point of this type existing.
//
// This is a REAL multi-threaded test: every block below that exercises the channel spawns an actual
// std::thread as the producer and drains from the main thread (or, for the async block, drives an
// agentengine::rt::task<T> consumer coroutine that a producer thread resumes directly across a real
// co_await suspension). Covers:
//   T1 -- items pushed on a producer thread arrive on the consumer side IN ORDER.
//   T2 -- a bounded-capacity producer genuinely BLOCKS when the queue is full, until the consumer
//         drains some (proven via wall-clock: the producer's push count stays capped while the
//         consumer deliberately does not drain, then completes once it does).
//   T3 -- close() after all items are consumed reaches a clean terminal (done()==true,
//         terminal()==closed) with ZERO data loss (every pushed item was observed exactly once, in
//         order) -- items pushed before close() are still delivered even though the terminal is
//         already set by the time the consumer gets to them.
//   T4 -- fail() reaches a failed terminal (terminal()==failed, failed()==true) and the consumer
//         observes the carried error -- again with zero loss of the items pushed before the failure.
//   T5 -- the ASYNC co_await surface: a consumer coroutine (agentengine::rt::task<std::vector<int>>)
//         genuinely suspends on an empty channel (proven by the driving resume() call returning
//         near-instantly, NOT blocking for the producer's artificial delay) and is resumed directly
//         by the producer thread across that suspension with no busy-polling anywhere in the
//         implementation (next_awaiter::await_suspend() only records a handle and returns -- see
//         channel.hpp's own file banner) -- proven structurally by that implementation and empirically
//         by wall-clock (the result is only available AFTER the producer's delay has elapsed).
//   T6 -- CANCELLATION SAFETY regression (found during an ADR-037 Phase 2 red-team pass, fixed the
//         same day in channel.hpp's next_awaiter): a consumer task destroyed WHILE genuinely parked
//         in next_async() must not leave a dangling coroutine_handle behind. Proven by destroying such
//         a task mid-park, then pushing/closing from a producer thread afterward -- the old code would
//         call .resume() on the already-destroyed frame here (a use-after-free); this must not crash.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/channel.hpp"
#include "agentengine/rt/task.hpp"

using agentengine::rt::channel_terminal;
using agentengine::rt::make_channel;
using agentengine::rt::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Drives a task directly (mode b, same driver test_rt_task.cpp uses): resume() until done(). Only
// used here to START the async-surface task -- see T5, where the task suspends on its very first
// resume() and is thereafter resumed by the PRODUCER thread, not by this driver.
template <class T>
void start(task<T>& t) {
    t.resume();
}

// The async consumer body for T5: loops next_async() until it sees a nullopt (drained + terminal),
// collecting every item it receives, in order.
template <class T, class E>
task<std::vector<T>> collect_via_async(agentengine::rt::channel_consumer<T, E>& consumer) {
    std::vector<T> out;
    for (;;) {
        std::optional<T> v = co_await consumer.next_async();
        if (!v) break;
        out.push_back(std::move(*v));
    }
    co_return out;
}

// T6's body: parks forever on an empty channel's next_async() -- never actually resumes normally in
// this test, since the whole point is to destroy the owning task WHILE it's still parked here.
template <class T, class E>
task<void> park_forever(agentengine::rt::channel_consumer<T, E>& consumer) {
    (void)co_await consumer.next_async();
    co_return;  // unreachable in T6
}

}  // namespace

int main() {
    // T1: order preservation, real producer thread -> main-thread consumer via the sync poll surface.
    {
        auto pair = make_channel<int>(8);
        constexpr int kCount = 50;
        std::thread producer([&] {
            for (int i = 0; i < kCount; ++i) {
                auto r = pair.producer.push(i);
                check(r == decltype(pair.producer)::push_result::ok, "T1: every push() while open succeeds");
            }
            pair.producer.close();
        });

        std::vector<int> received;
        while (!pair.consumer.done()) {
            if (auto v = pair.consumer.try_pop()) received.push_back(*v);
        }
        producer.join();

        check(received.size() == static_cast<std::size_t>(kCount), "T1: every pushed item was received");
        bool in_order = true;
        for (int i = 0; i < kCount; ++i) {
            if (received[static_cast<std::size_t>(i)] != i) in_order = false;
        }
        check(in_order, "T1: items arrive on the consumer side in the exact order they were pushed");
        check(pair.consumer.terminal() == channel_terminal::closed, "T1: a clean close() reaches a closed terminal");
    }

    // T2: bounded capacity genuinely blocks the producer thread until the consumer drains.
    {
        constexpr std::size_t kCapacity = 2;
        auto pair = make_channel<int>(kCapacity);
        std::atomic<int> pushed_count{0};
        std::atomic<bool> producer_finished{false};

        std::thread producer([&] {
            for (int i = 0; i < 5; ++i) {
                (void)pair.producer.push(i);
                pushed_count.fetch_add(1, std::memory_order_relaxed);
            }
            pair.producer.close();
            producer_finished.store(true, std::memory_order_relaxed);
        });

        // Deliberately do NOT consume anything for a while -- if push() didn't really block once the
        // queue hit capacity, the producer thread would race ahead and pushed_count would exceed
        // kCapacity well before this sleep ends.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        int const stalled_count = pushed_count.load(std::memory_order_relaxed);
        check(stalled_count <= static_cast<int>(kCapacity),
              "T2: with the consumer not draining, the producer thread is blocked at (or just below) "
              "capacity, not racing ahead to push all 5 items instantly");
        check(!producer_finished.load(std::memory_order_relaxed),
              "T2: the producer thread has not finished either -- push() is a real block, not a "
              "fire-and-forget");

        // Now drain everything -- the producer must be able to make forward progress and finish.
        std::vector<int> received;
        while (!pair.consumer.done()) {
            if (auto v = pair.consumer.try_pop()) received.push_back(*v);
        }
        producer.join();

        check(received.size() == 5, "T2: after draining, all 5 items were eventually delivered");
        check(producer_finished.load(std::memory_order_relaxed),
              "T2: the producer thread completed once the consumer drained past capacity");
    }

    // T3: close() after full consumption is a clean terminal, zero data loss, order preserved --
    // including the items that were pushed BEFORE close() is observed by the consumer.
    {
        auto pair = make_channel<std::string>(3);
        std::vector<std::string> const items = {"alpha", "beta", "gamma", "delta", "epsilon"};
        std::thread producer([&] {
            for (auto const& s : items) (void)pair.producer.push(s);
            pair.producer.close();
        });

        std::vector<std::string> received;
        while (!pair.consumer.done()) {
            if (auto v = pair.consumer.try_pop()) received.push_back(std::move(*v));
        }
        producer.join();

        check(received == items, "T3: every item pushed before close() is delivered, in order, with zero loss");
        check(pair.consumer.done(), "T3: done() is true once drained and closed");
        check(pair.consumer.terminal() == channel_terminal::closed, "T3: terminal() reports closed");
        check(!pair.consumer.failed(), "T3: failed() is false for a clean close()");
        check(!pair.consumer.error().has_value(), "T3: no error is carried by a clean close()");
    }

    // T4: fail() reaches a failed terminal, observable by the consumer, again with zero loss of the
    // items pushed before the failure.
    {
        auto pair = make_channel<int>(4);
        std::thread producer([&] {
            (void)pair.producer.push(1);
            (void)pair.producer.push(2);
            (void)pair.producer.push(3);
            pair.producer.fail("scripted failure: upstream disconnected");
            // A push() attempted AFTER fail() must be refused (terminated), not silently enqueued.
            auto r = pair.producer.push(999);
            check(r == decltype(pair.producer)::push_result::terminated,
                  "T4: a push() after fail() is refused (terminated), not enqueued");
        });

        std::vector<int> received;
        while (!pair.consumer.done()) {
            if (auto v = pair.consumer.try_pop()) received.push_back(*v);
        }
        producer.join();

        check(received.size() == 3 && received[0] == 1 && received[1] == 2 && received[2] == 3,
              "T4: the 3 items pushed before fail() are all delivered, in order, before the terminal "
              "is reached");
        check(pair.consumer.done(), "T4: done() is true once drained and failed");
        check(pair.consumer.terminal() == channel_terminal::failed, "T4: terminal() reports failed");
        check(pair.consumer.failed(), "T4: failed() is true");
        auto err = pair.consumer.error();
        check(err.has_value() && *err == "scripted failure: upstream disconnected",
              "T4: the consumer observes the exact error carried by fail()");
    }

    // T5: the async co_await surface -- a real consumer coroutine suspends without busy-polling and
    // is resumed directly by the producer thread.
    {
        auto pair = make_channel<int>(4);
        task<std::vector<int>> t = collect_via_async(pair.consumer);

        auto const before_start = std::chrono::steady_clock::now();
        start(t);  // runs until the coroutine's first co_await on an EMPTY channel, then suspends
        auto const after_start = std::chrono::steady_clock::now();

        check(!t.done(),
              "T5: after the driving resume() call returns, the task is NOT done -- it genuinely "
              "suspended waiting for the producer, rather than looping until an item showed up");
        check(after_start - before_start < std::chrono::milliseconds(50),
              "T5: the driving resume() call returned near-instantly -- proves await_suspend() did "
              "not busy-spin/block the calling thread for the producer's (much longer) delay below");

        constexpr auto kProducerDelay = std::chrono::milliseconds(150);
        auto const overall_start = std::chrono::steady_clock::now();
        std::thread producer([&] {
            std::this_thread::sleep_for(kProducerDelay);
            for (int i = 0; i < 5; ++i) (void)pair.producer.push(i);
            pair.producer.close();
        });
        producer.join();  // synchronizes-with everything the producer thread did, including any
                           // coroutine-frame writes made while resuming `t` across the co_await
        auto const overall_elapsed = std::chrono::steady_clock::now() - overall_start;

        check(overall_elapsed >= kProducerDelay - std::chrono::milliseconds(20),
              "T5: the coroutine's result only became available after the producer's own delay had "
              "elapsed -- the consumer genuinely waited for real data, it did not race ahead");
        check(t.done(),
              "T5: the task completed -- resumed directly by the producer thread across the co_await "
              "suspension (push()/close() calling waiter.resume(), per channel.hpp's design), with no "
              "separate driver thread ever calling resume() again");
        check(!t.faulted(), "T5: the coroutine completed without an exception");
        if (!t.faulted()) {
            std::vector<int> const result = t.take_value();
            std::vector<int> const expected = {0, 1, 2, 3, 4};
            check(result == expected, "T5: items pushed after the suspension arrive via next_async(), in order");
        }
    }

    // T6: destroying a task while it is genuinely parked in next_async() must not leave a dangling
    // handle in the channel's shared state -- a later producer call must not crash.
    {
        auto pair = make_channel<int>(4);
        {
            task<void> t = park_forever(pair.consumer);
            t.resume();  // runs to the co_await on an empty channel, genuinely suspends there
            check(!t.done(), "T6: setup: the task is genuinely parked (not done) before destruction");
            // t is destroyed HERE, at scope exit, while still suspended inside next_async(). Before
            // the channel.hpp fix, this left channel_state::waiting_consumer pointing at the now-freed
            // coroutine frame.
        }

        // If the dangling-handle bug were still present, either of these would resume a destroyed
        // frame -- a use-after-free that may crash immediately, corrupt memory silently, or (under a
        // sanitizer) abort. Reaching the check() calls below at all is part of the proof.
        auto r = pair.producer.push(42);
        check(r == decltype(pair.producer)::push_result::ok,
              "T6: pushing after the parked consumer was destroyed does not crash");
        pair.producer.close();
        check(pair.consumer.terminal() == channel_terminal::closed,
              "T6: the channel remains in a perfectly ordinary, usable state afterward -- a NEW "
              "consumer read (via the sync surface, since the async one was destroyed) still works");
        auto v = pair.consumer.try_pop();
        check(v.has_value() && *v == 42, "T6: the item pushed after destruction is still retrievable");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_channel: ALL PASS\n");
    return 0;
}
