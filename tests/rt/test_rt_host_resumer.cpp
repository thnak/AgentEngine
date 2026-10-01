// Proof for decisions/ADR-219 (issue #79) -- a host's Resumer: an AgentEngine coroutine driven from a host's
// own executor, parked on an AsyncMutex or a channel<T>, is handed back to that executor when woken instead
// of being resumed on the waker's (foreign) thread (include/agentengine/rt/resume_home.hpp, task.hpp,
// async_mutex.hpp, channel.hpp).
//
// The "host executor" is a plain std::thread draining a queue of ParkedContinuations -- no QuarkCpp.
//
//   R1  -- AsyncMutex: a waiter started inside a ScopedResumer runs its critical section on the executor's
//          thread, not the thread that dropped the Guard. R1c is the positive control: the same program
//          without the scope runs it on the releasing thread (issue #79's observation).
//   R2  -- channel<T>: a consumer parked on next_async() inside a ScopedResumer is resumed on the executor's
//          thread after a producer thread pushes. R2c: without the scope, on the producer's thread.
//   R3  -- channel<T> close(): the terminal wake-up is handed over the same way.
//   R4  -- a resumed coroutine that parks AGAIN is handed back to the same resumer (two posts, both run on
//          the executor thread) -- the resumer travels with the continuation, not the thread.
//   R5  -- a host coroutine (not rt::task) co_awaiting an rt::task inside the scope: the rt::task's lock
//          continuation AND the host coroutine's code after the co_await run on the executor; while that
//          continuation is queued, the thread that opened the scope is NOT reported as the lock's holder
//          (the scope runs under a fresh holder id), and inside, the coroutine IS the holder.
//   R6  -- a resumer that drops every continuation: each is resumed inline by the drop, so the waiter runs
//          and the lock is released -- never held forever.
//   R7  -- a resumer whose post() throws: the waker survives, the continuation still runs, lock released.
//   R8  -- block_on() inside a ScopedResumer still owns what parks under it: resumed on the block_on()
//          thread, nothing posted to the resumer (ADR-175 unchanged).
//   R9  -- 2,000 waiters inside one scope: exact count, never two inside, every one resumed on the executor.
//   R10 -- a resumer that runs continuations INLINE in post(): 5,000 chained waiters complete on a default
//          (1 MiB on Windows) stack -- the hand-off runs inside AsyncMutex's trampoline, not nested.
//   R11 -- default behaviour unchanged: a waiter outside any scope records no resumer and the releaser runs
//          it, with no post to any resumer (also covered by R1c/R2c, stated here as its own check).
//   R12 -- self red-team finding 1: the host destroys a waiter whose lock grant is queued in its executor.
//          The lock it was granted is released (another thread acquires it), and the queued continuation
//          does nothing when the executor gets to it (under ASan, a use-after-free if it resumed).
//   R13 -- the same for a channel consumer: never resumed, and the item stays in the channel.
//
// MEMORY/TIME: capped at 512 MiB (tests/support/memory_cap.hpp); a watchdog ends the process if a check
// hangs. No daemon, network or credentials.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/channel.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"
#include "../support/crt_fail_fast.hpp"
#include "../support/memory_cap.hpp"

using namespace agentengine::rt;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 10s) {
    auto const end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// The fake host executor: one std::thread draining a FIFO of continuations. Can be paused to keep a
// continuation queued while the test inspects state.
class ThreadResumer final : public Resumer {
public:
    ThreadResumer() : worker_([this] { loop(); }) {}
    ~ThreadResumer() override { stop(); }
    ThreadResumer(ThreadResumer const&)            = delete;
    ThreadResumer& operator=(ThreadResumer const&) = delete;

    void post(ParkedContinuation c) override {
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(c));
            ++posted_;
        }
        cv_.notify_one();
    }
    void pause(bool p) {
        {
            std::lock_guard lock(m_);
            paused_ = p;
        }
        cv_.notify_one();
    }
    // Must be called from a thread other than the worker, before the last shared_ptr is released.
    void stop() {
        {
            std::lock_guard lock(m_);
            stop_   = true;
            paused_ = false;
        }
        cv_.notify_one();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
    std::thread::id id() const { return worker_.get_id(); }  // valid until stop()
    int posted() {
        std::lock_guard lock(m_);
        return posted_;
    }
    // Waits until `n` continuations have been resumed AND returned on the worker.
    bool wait_ran(int n) { return wait_until([&] { return ran_.load() >= n; }); }

private:
    void loop() {
        for (;;) {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || (!paused_ && !q_.empty()); });
            if (q_.empty()) return;  // stop_ and drained
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
            ran_.fetch_add(1);
        }
    }

    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    int                            posted_ = 0;
    bool                           paused_ = false;
    bool                           stop_   = false;
    std::atomic<int>               ran_{0};
    std::thread                    worker_;  // last: started after every member above exists
};

// Drops every continuation it is given (a host shutting down).
class DroppingResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override {
        ++dropped;
        (void)c;  // destroyed at the end of this call: resumes inline, here
    }
    std::atomic<int> dropped{0};
};

// Throws from post() after taking the continuation (a host queue push that fails).
class ThrowingResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override {
        ++attempts;
        ParkedContinuation taken = std::move(c);
        throw std::runtime_error("queue full");
    }
    std::atomic<int> attempts{0};
};

// Runs every continuation inline inside post() (an "inline executor").
class InlineResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override {
        ++posted;
        c.resume();
    }
    std::atomic<int> posted{0};
};

task<AsyncMutex::Guard> acquire(AsyncMutex& m) { co_return co_await m.lock(); }

// Holds `m` on a dedicated thread until released; the Guard is dropped ON THAT THREAD.
class ExternalHolder {
public:
    explicit ExternalHolder(AsyncMutex& m) {
        std::promise<void> held;
        auto held_f = held.get_future();
        thread_ = std::thread([&m, &held, this] {
            auto g = block_on(acquire(m));
            id_ = std::this_thread::get_id();
            held.set_value();
            release_.get_future().wait();
        });
        held_f.wait();
    }
    void release() { release_.set_value(); }
    void join() {
        if (thread_.joinable()) thread_.join();
    }
    std::thread::id id() const { return id_; }
    ~ExternalHolder() { join(); }

private:
    std::promise<void> release_;
    std::thread        thread_;
    std::thread::id    id_{};
};

struct Observed {
    std::thread::id   ran_on{};
    bool              held_inside = false;
    std::atomic<bool> done{false};
};

task<void> lock_and_record(AsyncMutex& m, Observed& o) {
    auto g        = co_await m.lock();
    o.ran_on      = std::this_thread::get_id();
    o.held_inside = m.is_held_by_current_thread();
    o.done        = true;
}

task<void> consume_record(channel_consumer<int>& c, std::optional<int>& out, Observed& o) {
    out      = co_await c.next_async();
    o.ran_on = std::this_thread::get_id();
    o.done   = true;
}

// R4: parks on the lock, then parks again on the channel.
task<void> lock_then_consume(AsyncMutex& m, channel_consumer<int>& c, Observed& first, Observed& second,
                             std::optional<int>& out) {
    {
        auto g       = co_await m.lock();
        first.ran_on = std::this_thread::get_id();
        first.done   = true;
    }
    out           = co_await c.next_async();
    second.ran_on = std::this_thread::get_id();
    second.done   = true;
}

// R5: a host coroutine type -- eager, not an rt::task -- awaiting an rt::task.
struct HostCoro {
    struct promise_type {
        HostCoro get_return_object() noexcept {
            return HostCoro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }
    };
    explicit HostCoro(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    HostCoro(HostCoro&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    HostCoro(HostCoro const&) = delete;
    ~HostCoro() {
        if (h_) h_.destroy();
    }
    [[nodiscard]] bool done() const noexcept { return h_.done(); }

private:
    std::coroutine_handle<promise_type> h_;
};

HostCoro host_body(AsyncMutex& m, Observed& inner, Observed& after) {
    co_await lock_and_record(m, inner);
    after.ran_on = std::this_thread::get_id();
    after.done   = true;
}

// R9 / R10
struct Exclusion {
    std::atomic<int> inside{0};
    std::atomic<int> most{0};
    void enter() {
        int const n = ++inside;
        int m       = most.load();
        while (n > m && !most.compare_exchange_weak(m, n)) {}
    }
    void leave() { --inside; }
};

task<void> counted(AsyncMutex& m, Exclusion& ex, long& count, std::thread::id expect, std::atomic<int>& wrong,
                   std::atomic<int>& done) {
    auto g = co_await m.lock();
    ex.enter();
    ++count;
    if (std::this_thread::get_id() != expect) ++wrong;
    ex.leave();
    ++done;
}

task<void> counted_any(AsyncMutex& m, long& count, std::atomic<int>& done) {
    auto g = co_await m.lock();
    ++count;
    ++done;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const only = argc > 1 ? argv[1] : "";
    auto const want = [&only](char const* id) { return only.empty() || only == id; };
    agentengine::test_support::fail_fast_on_windows();
    (void)agentengine::test_support::cap_process_memory(std::size_t{512} << 20, std::size_t{2048} << 20);
    std::thread([] {
        std::this_thread::sleep_for(240s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 240 s -- a deadlock regression\n");
        std::fflush(stdout);
        std::_Exit(3);
    }).detach();

    // R1 / R1c
    if (want("R1")) {
        auto       exec = std::make_shared<ThreadResumer>();
        AsyncMutex m;
        Observed   o;
        ExternalHolder holder(m);
        task<void> w = lock_and_record(m, o);
        {
            ScopedResumer const scope(exec);
            w.start();  // parks: `holder` owns m
        }
        check(!o.done.load(), "R1: the waiter parked (the mutex is held by another thread)");
        holder.release();
        holder.join();  // the Guard was dropped on the holder's thread and unlock() has returned there
        bool const ran = exec->wait_ran(1);
        check(ran && o.done.load() && w.done(), "R1: the waiter completed through the resumer");
        check(o.ran_on == exec->id() && o.ran_on != holder.id(),
              "R1: the critical section ran on the executor's thread, not the releasing thread");
        check(exec->posted() == 1, "R1: exactly one continuation was posted");
        check(o.held_inside, "R1: on the executor, the resumed waiter is the lock's holder");
        exec->stop();
    }
    if (want("R1c")) {
        AsyncMutex     m;
        Observed       o;
        ExternalHolder holder(m);
        task<void>     w = lock_and_record(m, o);
        w.start();
        holder.release();
        holder.join();
        check(o.done.load() && o.ran_on == holder.id(),
              "R1c (control): without a ScopedResumer the waiter runs on the releasing thread (issue #79)");
    }

    // R2 / R2c
    if (want("R2")) {
        auto               exec = std::make_shared<ThreadResumer>();
        auto [prod, cons]       = make_channel<int>(4);
        std::optional<int> out;
        Observed           o;
        task<void>         w = consume_record(cons, out, o);
        {
            ScopedResumer const scope(exec);
            w.start();
        }
        check(!o.done.load(), "R2: the consumer parked on an empty channel");
        std::thread::id producer_id;
        std::thread     producer([&] {
            producer_id = std::this_thread::get_id();
            (void)prod.push(42);
        });
        producer.join();
        bool const ran = exec->wait_ran(1);
        check(ran && o.done.load() && out == 42, "R2: the consumer received the pushed value");
        check(o.ran_on == exec->id() && o.ran_on != producer_id,
              "R2: the consumer resumed on the executor's thread, not the producer's");
        exec->stop();
    }
    if (want("R2c")) {
        auto [prod, cons] = make_channel<int>(4);
        std::optional<int> out;
        Observed           o;
        task<void>         w = consume_record(cons, out, o);
        w.start();
        std::thread::id producer_id;
        std::thread     producer([&] {
            producer_id = std::this_thread::get_id();
            (void)prod.push(7);
        });
        producer.join();
        check(o.done.load() && out == 7 && o.ran_on == producer_id,
              "R2c (control): without a ScopedResumer the consumer runs on the producer's thread");
    }

    // R3
    if (want("R3")) {
        auto               exec = std::make_shared<ThreadResumer>();
        auto [prod, cons]       = make_channel<int>(4);
        std::optional<int> out{-1};
        Observed           o;
        task<void>         w = consume_record(cons, out, o);
        {
            ScopedResumer const scope(exec);
            w.start();
        }
        std::thread closer([p = std::move(prod)]() mutable { p.close(); });
        closer.join();
        bool const ran = exec->wait_ran(1);
        check(ran && o.done.load() && !out.has_value() && o.ran_on == exec->id(),
              "R3: a close() wake-up is handed to the resumer too (nullopt, on the executor's thread)");
        exec->stop();
    }

    // R4
    if (want("R4")) {
        auto               exec = std::make_shared<ThreadResumer>();
        AsyncMutex         m;
        auto [prod, cons]       = make_channel<int>(4);
        std::optional<int> out;
        Observed           first;
        Observed           second;
        ExternalHolder     holder(m);
        task<void>         w = lock_then_consume(m, cons, first, second, out);
        {
            ScopedResumer const scope(exec);
            w.start();
        }
        holder.release();
        holder.join();
        bool const ran1 = exec->wait_ran(1);  // the lock continuation ran, then parked on the channel
        check(ran1 && first.done.load() && !second.done.load() && first.ran_on == exec->id(),
              "R4: the lock continuation ran on the executor and the coroutine parked again");
        std::thread producer([&] { (void)prod.push(5); });
        producer.join();
        bool const ran2 = exec->wait_ran(2);
        check(ran2 && second.done.load() && out == 5 && second.ran_on == exec->id(),
              "R4: the second park was handed back to the same resumer");
        check(exec->posted() == 2, "R4: two continuations posted");
        exec->stop();
    }

    // R5
    if (want("R5")) {
        auto       exec = std::make_shared<ThreadResumer>();
        AsyncMutex m;
        Observed   inner;
        Observed   after;
        exec->pause(true);
        ExternalHolder holder(m);
        std::optional<HostCoro> host;
        {
            ScopedResumer const scope(exec);
            host.emplace(host_body(m, inner, after));  // eager: runs into the rt::task, which parks
        }
        holder.release();
        holder.join();
        bool const queued = wait_until([&] { return exec->posted() == 1; });
        check(queued && !inner.done.load(), "R5: the continuation is queued on the paused executor");
        check(!m.is_held_by_current_thread(),
              "R5: while it is queued, the thread that opened the scope is not reported as the holder");
        exec->pause(false);
        bool const ran = exec->wait_ran(1);
        check(ran && inner.done.load() && inner.ran_on == exec->id() && inner.held_inside,
              "R5: the rt::task's critical section ran on the executor, as the lock's holder");
        check(after.done.load() && after.ran_on == exec->id() && host->done(),
              "R5: the host coroutine's code after co_await ran on the executor too");
        exec->stop();
    }

    // R6
    if (want("R6")) {
        auto           res = std::make_shared<DroppingResumer>();
        AsyncMutex     m;
        Observed       o;
        ExternalHolder holder(m);
        task<void>     w = lock_and_record(m, o);
        {
            ScopedResumer const scope(res);
            w.start();
        }
        holder.release();
        holder.join();
        check(res->dropped.load() == 1 && o.done.load() && w.done() && o.ran_on == holder.id(),
              "R6: a dropped continuation is resumed inline by the drop (on the releasing thread)");
        auto g = block_on(acquire(m));
        check(g.held(), "R6: the lock was released afterwards -- never held forever");
    }

    // R7
    if (want("R7")) {
        auto           res = std::make_shared<ThrowingResumer>();
        AsyncMutex     m;
        Observed       o;
        ExternalHolder holder(m);
        task<void>     w = lock_and_record(m, o);
        {
            ScopedResumer const scope(res);
            w.start();
        }
        holder.release();
        holder.join();  // the releasing thread survived post()'s exception
        check(res->attempts.load() == 1 && o.done.load() && w.done(),
              "R7: post() threw; the waker survived and the continuation still ran");
        auto g = block_on(acquire(m));
        check(g.held(), "R7: the lock was released afterwards");
    }

    // R8
    if (want("R8")) {
        auto           exec = std::make_shared<ThreadResumer>();
        AsyncMutex     m;
        Observed       o;
        ExternalHolder holder(m);
        std::thread    releaser([&] {
            std::this_thread::sleep_for(50ms);
            holder.release();
        });
        {
            ScopedResumer const scope(exec);
            block_on(lock_and_record(m, o));  // blocks this thread until granted
        }
        releaser.join();
        check(o.done.load() && o.ran_on == std::this_thread::get_id() && exec->posted() == 0,
              "R8: block_on() inside a ScopedResumer resumes its task on its own thread; nothing posted");
        exec->stop();
    }

    // R9
    if (want("R9")) {
        constexpr int N    = 2000;
        auto          exec = std::make_shared<ThreadResumer>();
        AsyncMutex    m;
        Exclusion     ex;
        long          count = 0;
        std::atomic<int> wrong{0};
        std::atomic<int> done{0};
        std::vector<task<void>> tasks;
        tasks.reserve(N);
        {
            ExternalHolder holder(m);
            {
                ScopedResumer const scope(exec);
                for (int i = 0; i < N; ++i) {
                    tasks.push_back(counted(m, ex, count, exec->id(), wrong, done));
                    tasks.back().start();
                }
            }
            holder.release();
        }
        bool const ran = exec->wait_ran(N);
        check(ran && done.load() == N && count == N && ex.most.load() == 1,
              "R9: 2,000 waiters in one scope: exact count, never two inside");
        check(wrong.load() == 0 && exec->posted() == N, "R9: every one ran on the executor's thread");
        exec->stop();
    }

    // R10
    if (want("R10")) {
        constexpr int N   = 5000;
        auto          res = std::make_shared<InlineResumer>();
        AsyncMutex    m;
        long          count = 0;
        std::atomic<int> done{0};
        std::vector<task<void>> tasks;
        tasks.reserve(N);
        {
            ExternalHolder holder(m);  // its thread has the platform's default stack
            {
                ScopedResumer const scope(res);
                for (int i = 0; i < N; ++i) {
                    tasks.push_back(counted_any(m, count, done));
                    tasks.back().start();
                }
            }
            holder.release();
        }
        check(done.load() == N && count == N && res->posted.load() == N,
              "R10: 5,000 continuations run inline by post() complete on one default stack");
        auto g = block_on(acquire(m));
        check(g.held(), "R10: the lock is free afterwards");
    }

    // R11
    if (want("R11")) {
        auto           exec = std::make_shared<ThreadResumer>();
        AsyncMutex     m;
        Observed       o;
        ExternalHolder holder(m);
        task<void>     w = lock_and_record(m, o);
        w.start();  // no scope -- even though a resumer exists in this process
        holder.release();
        holder.join();
        check(o.done.load() && o.ran_on == holder.id() && exec->posted() == 0,
              "R11: outside a scope nothing is posted; the releaser resumes the waiter as before");
        exec->stop();
    }

    // R12
    if (want("R12")) {
        auto       exec = std::make_shared<ThreadResumer>();
        AsyncMutex m;
        Observed   o;
        exec->pause(true);
        ExternalHolder holder(m);
        std::optional<task<void>> w;
        w.emplace(lock_and_record(m, o));
        {
            ScopedResumer const scope(exec);
            w->start();
        }
        holder.release();
        holder.join();
        bool const queued = wait_until([&] { return exec->posted() == 1; });
        w.reset();  // the host cancels: the parked coroutine is destroyed while its continuation is queued
        std::atomic<bool> got{false};
        std::thread       taker([&] {
            auto g = block_on(acquire(m));
            got    = g.held();
        });
        bool const freed = wait_until([&] { return got.load(); }, 5s);
        check(queued && freed,
              "R12: destroying a granted waiter whose continuation is queued releases the lock it was granted");
        exec->pause(false);
        bool const ran = exec->wait_ran(1);
        check(ran && !o.done.load(), "R12: the queued continuation then does nothing (the frame is gone)");
        if (freed) {
            taker.join();
        } else {
            taker.detach();  // the watchdog / exit ends it
        }
        exec->stop();
    }

    // R13
    if (want("R13")) {
        auto               exec = std::make_shared<ThreadResumer>();
        auto [prod, cons]       = make_channel<int>(4);
        std::optional<int> out;
        Observed           o;
        exec->pause(true);
        std::optional<task<void>> w;
        w.emplace(consume_record(cons, out, o));
        {
            ScopedResumer const scope(exec);
            w->start();
        }
        std::thread producer([&] { (void)prod.push(9); });
        producer.join();
        bool const queued = wait_until([&] { return exec->posted() == 1; });
        w.reset();
        exec->pause(false);
        bool const ran = exec->wait_ran(1);
        check(queued && ran && !o.done.load(),
              "R13: a consumer destroyed while its wake-up is queued is never resumed");
        check(cons.try_pop() == 9, "R13: the item it would have taken is still in the channel");
        exec->stop();
    }

    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
