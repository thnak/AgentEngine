// Proof for ADR-037 Phase 2: agentengine::rt::AsyncMutex (include/agentengine/rt/async_mutex.hpp),
// the local replacement for I1 ("one session, one executor") now that AgentSession will stop being a
// quark::Actor (whose mailbox + Sequential dispatch used to guarantee this structurally). Covers:
//   M1 -- uncontended lock() resolves without ever suspending (await_ready() takes it immediately).
//   M2 -- a contended lock() genuinely suspends and is woken exactly once the holder releases.
//   M3 -- REAL mutual exclusion under genuine cross-thread contention: many real std::thread callers
//         race to increment a plain (non-atomic) counter inside the critical section; if AsyncMutex
//         ever let two holders overlap, this would corrupt the count (a torn read-modify-write) -- the
//         final count matching exactly, every time, across repeated runs, is the actual proof, not a
//         logical inference from the code alone. Driven with EXACTLY ONE resume() call per job from
//         its starting thread, then left alone -- calling resume() again from a naive polling loop
//         while a coroutine is legitimately parked waiting for an EXTERNAL wake (mtx.lock(), unlike a
//         nested task<T> await, suspends until a *different* thread's unlock() resumes it) is a real
//         double-resume race (undefined behavior) -- found the hard way while first drafting this
//         test against `rt::ThreadPool`, whose own drive loop assumes only nested-task<T> suspension.
//         The correct pattern, matching how channel<T>'s own async-surface test (test_rt_channel.cpp
//         T5) already drives things: resume() ONCE to start, then let the primitive's own internal
//         hand-off (AsyncMutex::unlock() -> next.resume(), exactly like channel<T>'s push()/close() ->
//         waiter.resume()) carry it the rest of the way.
//   M4 -- FIFO ordering: N queued waiters are released in the exact order they queued.
//   M5 -- CANCELLATION SAFETY: a queued (not yet woken) lock() task destroyed mid-park does not
//         corrupt the waiter queue -- the OTHER still-pending waiters are still served correctly, in
//         order, afterward, and no later unlock() crashes.
//   M6 -- move semantics: moving a Guard transfers ownership; the moved-from Guard does not
//         double-release when it is later destroyed.
//
// EVERY coroutine below is a genuinely NAMED FREE FUNCTION, never an immediately-invoked lambda
// (`[&]() -> task<T> { ... }()`) -- found the hard way (a 100%-reproducible hang/segfault while first
// drafting M3/M4/M5) that this is a real, well-documented C++ coroutine pitfall, not a hypothetical
// one: a lambda's `operator()` converted to a coroutine stores only a POINTER to the closure object in
// the coroutine frame (the implicit object parameter), not a copy of it. Calling the lambda as a
// temporary (`[...]() -> task<T> {...}()`) means that closure is destroyed at the end of the full
// expression -- long before a coroutine that suspends (parks on a contended lock, or on next resume
// from a different loop iteration) actually finishes running its body. Two DIFFERENT scratch repros
// (D:\...\scratchpad\repro_mutex.cpp using this pattern vs. repro_mutex2.cpp using named functions)
// confirmed this directly: identical AsyncMutex logic, identical workload, the lambda version
// hung/crashed at every job count from 2 to 200, the named-function version passed cleanly up to 500.
// Ordinary function parameters (by reference or by pointer) have no such lifetime hazard -- they are
// correctly captured into the coroutine frame per the standard, which is why every job below takes its
// shared state as explicit function parameters instead of lambda captures.

#include <atomic>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

using agentengine::rt::AsyncMutex;
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

// ---- M1/M6 helpers ---------------------------------------------------------------------------

task<void> acquire_and_check_held(AsyncMutex& mtx, char const* label) {
    AsyncMutex::Guard g = co_await mtx.lock();
    check(g.held(), label);
    co_return;
}

// ---- M2/M4/M5 helper: acquire and hand the Guard itself back out ----------------------------

task<AsyncMutex::Guard> acquire(AsyncMutex& mtx) { co_return co_await mtx.lock(); }

// ---- M2 helper ---------------------------------------------------------------------------------

task<void> wait_then_check(AsyncMutex& mtx, char const* label) {
    AsyncMutex::Guard g = co_await mtx.lock();
    check(g.held(), label);
    co_return;
}

// ---- M3: the mutual-exclusion critical-section job ---------------------------------------------

struct M3Context {
    AsyncMutex mtx;
    long counter = 0;  // deliberately NOT atomic -- a torn update would prove a real bug
    std::atomic<int> finished{0};
};

task<void> m3_job(M3Context* ctx) {
    AsyncMutex::Guard g = co_await ctx->mtx.lock();
    long const before = ctx->counter;
    // A deliberate window for another thread to race in, if the mutex ever failed to exclude -- a
    // read, a yield, then a write, maximizing the chance a real bug shows up.
    std::this_thread::yield();
    ctx->counter = before + 1;
    g = AsyncMutex::Guard{};  // release explicitly, before signaling completion below
    ctx->finished.fetch_add(1, std::memory_order_release);
    co_return;
}

// ---- M4/M5: FIFO-order-recording job -------------------------------------------------------

struct OrderContext {
    AsyncMutex& mtx;
    std::vector<int>& order;
};

task<void> record_order_job(OrderContext* ctx, int id) {
    AsyncMutex::Guard g = co_await ctx->mtx.lock();
    ctx->order.push_back(id);
    co_return;
}

// ---- M6: move-semantics job -----------------------------------------------------------------

task<void> move_semantics_job(AsyncMutex& mtx) {
    AsyncMutex::Guard g1 = co_await mtx.lock();
    AsyncMutex::Guard g2 = std::move(g1);
    check(!g1.held(), "M6: after move, the source Guard no longer holds the mutex");
    check(g2.held(), "M6: the destination Guard now holds it");
    co_return;  // only g2 (and the already-empty g1) destruct here -- must release exactly once
}

task<void> probe_job(AsyncMutex& mtx) {
    AsyncMutex::Guard g = co_await mtx.lock();
    check(g.held(), "M6: the mutex is cleanly available again -- no double-release corruption");
    co_return;
}

}  // namespace

int main() {
    // M1: uncontended lock() never suspends -- await_ready() alone is enough to take it.
    {
        AsyncMutex mtx;
        task<void> t = acquire_and_check_held(mtx, "M1: the awaited Guard genuinely holds the mutex");
        t.resume();
        check(t.done(), "M1: an uncontended lock() completes on the FIRST resume() -- no suspension");
    }

    // M2: a contended lock() genuinely suspends and is woken exactly once released.
    {
        AsyncMutex mtx;
        task<AsyncMutex::Guard> holder = acquire(mtx);
        holder.resume();
        check(holder.done(), "M2: setup: the first (uncontended) lock completes immediately");
        AsyncMutex::Guard held = holder.take_value();
        check(held.held(), "M2: setup: the first caller genuinely holds the mutex");

        task<void> waiter =
            wait_then_check(mtx, "M2: the SECOND caller eventually holds the mutex too, after waiting");
        waiter.resume();
        check(!waiter.done(), "M2: the second, contended lock() genuinely suspends (not done yet)");

        // Release the first holder -- this must resume the parked waiter directly.
        held = AsyncMutex::Guard{};  // moves-from destructs the old Guard -> releases -> resumes waiter
        check(waiter.done(), "M2: releasing the holder resumes the parked waiter -- it completes");
    }

    // M3: real mutual exclusion under genuine cross-thread contention (real OS threads).
    {
        M3Context ctx;
        constexpr int kJobs = 200;

        std::vector<task<void>> jobs;
        jobs.reserve(kJobs);
        for (int i = 0; i < kJobs; ++i) jobs.push_back(m3_job(&ctx));

        // Start every job with EXACTLY ONE resume() call, from real, genuinely concurrent OS threads
        // -- proves the mutex serializes REAL cross-thread races, not just single-threaded logical
        // suspension/resumption. Each starter thread's job is done the moment its single resume()
        // call returns (whether the job finished immediately, uncontended, or suspended waiting for
        // the mutex) -- it does NOT loop, matching this file's own driving-pattern rule above.
        {
            std::vector<std::thread> starters;
            starters.reserve(kJobs);
            for (auto& j : jobs) starters.emplace_back([&j] { j.resume(); });
            for (auto& th : starters) th.join();
        }

        // Jobs that suspended are resumed later by whichever thread's unlock() reaches them (possibly
        // several jobs deep in a single hand-off chain) -- wait for every job to signal completion
        // rather than polling task<void>::done() directly (racy to read while another thread may
        // still be resuming it).
        while (ctx.finished.load(std::memory_order_acquire) < kJobs) std::this_thread::yield();

        check(ctx.counter == kJobs,
              "M3: the counter reached EXACTLY kJobs -- real mutual exclusion held across 200 "
              "genuinely concurrent, real-OS-thread-started jobs, zero lost updates");
        // Deliberately NOT checking j.done()/j.faulted() here for every job: by the time finished's
        // acquire-load above is satisfied, the counter write is guaranteed visible (release/acquire
        // pairs with that specific write), but the coroutine's OWN done()-transition happens slightly
        // AFTER the release store in program order on whichever thread ran it last -- reading
        // coroutine_handle::done() from this (main) thread without its own synchronization edge to
        // THAT specific transition would be a genuine (if narrow) data race. `jobs` destructing at
        // scope exit is safe regardless: by then every real starter thread has long since joined, and
        // task<T>'s own destructor (`h_.destroy()`) tolerates a completed frame exactly as it
        // tolerates a never-started one.
    }

    // M4: FIFO ordering across several queued waiters.
    {
        AsyncMutex mtx;
        std::vector<int> order;
        OrderContext ctx{mtx, order};

        task<AsyncMutex::Guard> holder = acquire(mtx);
        holder.resume();
        AsyncMutex::Guard held = holder.take_value();

        constexpr int kWaiters = 5;
        std::vector<task<void>> waiters;
        waiters.reserve(kWaiters);
        for (int i = 0; i < kWaiters; ++i) {
            waiters.push_back(record_order_job(&ctx, i));
            waiters.back().resume();  // queues in order 0,1,2,3,4
        }
        // Release the original holder; each subsequent Guard destruction (as its task completes and
        // the Guard falls out of scope inside the coroutine) hands off to the next queued waiter.
        held = AsyncMutex::Guard{};
        for (auto& w : waiters) check(w.done(), "M4: every queued waiter eventually completes");
        std::vector<int> const expected = {0, 1, 2, 3, 4};
        check(order == expected, "M4: waiters are released in the EXACT order they queued (FIFO)");
    }

    // M5: cancellation safety -- destroying a queued (not yet woken) waiter mid-park does not corrupt
    // the queue; the OTHER waiters still get served correctly, in order, afterward.
    {
        AsyncMutex mtx;
        std::vector<int> order;
        OrderContext ctx{mtx, order};

        task<AsyncMutex::Guard> holder = acquire(mtx);
        holder.resume();
        AsyncMutex::Guard held = holder.take_value();

        task<void> waiter_a = record_order_job(&ctx, 1);
        waiter_a.resume();  // queues: [1]
        {
            task<void> waiter_b = record_order_job(&ctx, 2);
            waiter_b.resume();  // queues: [1, 2]
            check(!waiter_b.done(), "M5: setup: the soon-to-be-cancelled waiter is genuinely parked");
            // waiter_b is destroyed HERE, mid-park -- before the channel.hpp/async_mutex.hpp fix,
            // this would leave a dangling handle in the queue; a later unlock() resuming it would be
            // a use-after-free.
        }
        task<void> waiter_c = record_order_job(&ctx, 3);
        waiter_c.resume();  // queues: [1, 3] (2 was removed by its own destructor)

        // Release the original holder -- must not crash, and must serve 1 then 3, in that order,
        // skipping the cancelled slot entirely.
        held = AsyncMutex::Guard{};
        check(waiter_a.done() && waiter_c.done(),
              "M5: releasing after a mid-park cancellation does not crash -- the surviving waiters "
              "still complete");
        std::vector<int> const expected = {1, 3};
        check(order == expected,
              "M5: the surviving waiters are served in their original FIFO order, with the cancelled "
              "slot cleanly absent -- not skipped-with-a-gap, not double-served");
    }

    // M6: move semantics -- a moved-from Guard does not double-release.
    {
        AsyncMutex mtx;
        task<void> t = move_semantics_job(mtx);
        t.resume();
        check(t.done(), "M6: setup: the job completed");

        // Prove the mutex is genuinely free afterward (a double-release would have left internal
        // state corrupted, e.g. by handing off to a phantom waiter or leaving held_ permanently true).
        task<void> probe = probe_job(mtx);
        probe.resume();
        check(probe.done(), "M6: the probe acquires immediately (uncontended) -- proves single release");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_async_mutex: ALL PASS\n");
    return 0;
}
