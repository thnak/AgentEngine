// Proof for decisions/ADR-175 -- who resumes a parked coroutine, and who owns a lock while its holder is
// parked (include/agentengine/rt/resume_home.hpp, async_mutex.hpp, channel.hpp, block_on.hpp,
// thread_pool.hpp). Each check below failed, crashed or hung against the code before ADR-175, unless it is
// marked as a guard against a regression a red-team round found in an intermediate design (those are
// proven by a planted mutant instead -- see the ADR's §7).
//
//   H1  -- issue #78's own program: 4 workers, jobs contending one AsyncMutex. Every job completes, none
//          faulted, and no two ever hold the mutex at once. Before: ASan heap-use-after-free/double-free.
//   H2  -- a pool job that parks and then throws is reported with its original exception.
//   H3  -- a pool job parked on channel<T>::next_async() completes with the pushed value, on its worker.
//   H4  -- a job's by-value parameters are destroyed before future::get() returns (the destructor sleeps,
//          so a driver that signals first is caught, not missed by timing).
//   H5  -- round 1 finding 2: a woken job that releases and re-acquires through block_on() completes.
//          Revision 1 livelocked the releasing thread inside unlock()'s trampoline.
//   H6  -- round 2 finding 1: after a hand-off to a successor that has not run yet, the RELEASING task is
//          not reported as holder, and no two tasks are ever inside the critical section (300 attempts:
//          the window closes when the successor's thread wakes).
//   H7  -- round 1 finding 3 / round 2 finding 2: a homeless coroutine resumed inline on a pool worker
//          while holding a lock -- and then another lock, taken under the holder id it recorded there --
//          does not make an unrelated job on that worker "the holder" of either.
//   H8  -- ADR-123's reentrant case survives: inside a round driven by block_on(), a nested synchronous
//          block_on() (a tool closure) is still the holder; a guard returned out of block_on() at top
//          level belongs to the calling thread.
//   H9  -- block_on() resumes a posted handle on the calling thread, never on the releasing thread.
//   H10 -- round 2 finding 3: one worker; job A holds L1 and parks on L2; job B blocks on L1. Completes.
//   H11 -- round 2 finding 6: a channel consumer destroyed while its cancel wake-up is posted -- the
//          resumed awaiter must not reach through the destroyed consumer (ASan guard).
//   H12 -- mixed load: block_on threads and pool jobs hammering one mutex; exclusion and count exact.
//   H13 -- round 3 finding 2: a task raw-resumed inside a pool job / a block_on() that then returns is not
//          stranded when the lock is granted later (it used to be posted to a queue nobody drained, and the
//          mutex stayed held forever).
//   H14 -- round 3 finding 2: a task that hops to a pool job by bare handle resume and parks after that job
//          finished still completes (the outer block_on() used to spin forever).
//   H15 -- round 3 finding 3: the thread a homeless round parked FROM is not reported as its holder once
//          the round holds the lock on another thread (HEAD said no; the first implementation said yes).
//   H16 -- round 4 findings 1-2: 5,000 bare-resumed waiters granted into closed homes complete on one
//          1 MiB stack (the first fix nested each inside the previous unlock() and overflowed at 3,000), and
//          such a waiter holds the lock under a fresh id, not the id of the block_on() it parked under.
//   H17 -- round 4 finding 3, pinned: a sub-task raw-resumed inside a round is not the round's holder.
//
// MEMORY/TIME: capped at 512 MiB (tests/support/memory_cap.hpp); a watchdog ends the process if any check
// hangs, since several of these are deadlock regressions. No daemon, network or credentials.

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/channel.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
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

// Counts how many tasks are inside a critical section at once.
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

task<AsyncMutex::Guard> acquire(AsyncMutex& m) { co_return co_await m.lock(); }

// Holds `m` on a dedicated thread until released.
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
    // After release(): returns once the holder thread has exited -- including anything its unlock() ran
    // inline (a homeless successor), so the caller may then read that successor's state race-free.
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

// H1
task<void> contended(AsyncMutex& m, Exclusion& ex, std::atomic<long>& entered) {
    auto g = co_await m.lock();
    ex.enter();
    entered.fetch_add(1);
    std::this_thread::yield();
    ex.leave();
}

// H2
task<void> park_then_throw(AsyncMutex& m) {
    auto g = co_await m.lock();
    throw std::runtime_error("thrown-after-parking");
}

// H3
task<void> consume_one(channel_consumer<int>& c, std::optional<int>& out, std::thread::id& before,
                       std::thread::id& after) {
    before = std::this_thread::get_id();
    out    = co_await c.next_async();
    after  = std::this_thread::get_id();
}

// H4
struct SlowToDestroy {
    std::atomic<bool>* destroyed;
    explicit SlowToDestroy(std::atomic<bool>* d) noexcept : destroyed(d) {}
    SlowToDestroy(SlowToDestroy&& o) noexcept : destroyed(std::exchange(o.destroyed, nullptr)) {}
    SlowToDestroy(SlowToDestroy const&) = delete;
    ~SlowToDestroy() {
        if (!destroyed) return;
        std::this_thread::sleep_for(50ms);
        destroyed->store(true);
    }
};
task<void> park_with_param(AsyncMutex& m, SlowToDestroy param) {
    auto g = co_await m.lock();
    (void)param;
}

// H5
task<void> release_then_relock(AsyncMutex& m, std::atomic<int>& stage) {
    {
        auto g = co_await m.lock();
        stage = 1;
    }
    stage   = 2;
    auto g2 = block_on(acquire(m));
    stage   = 3;
}

// H6
task<int> round_holding(AsyncMutex& s, std::promise<void>& acquired, std::shared_future<void> release) {
    auto g = co_await s.lock();
    acquired.set_value();
    release.wait();
    co_return 0;
}
task<void> job_round_then_check(AsyncMutex& s, Exclusion& ex, std::promise<void>& acquired,
                                std::shared_future<void> release, std::atomic<int>& claimed_after) {
    (void)block_on(round_holding(s, acquired, release));
    if (s.is_held_by_current_thread()) {  // what fork_from() asks before skipping the lock
        ++claimed_after;
        ex.enter();
        std::this_thread::sleep_for(2ms);
        ex.leave();
    }
    co_return;
}
task<void> job_enter(AsyncMutex& s, Exclusion& ex, std::atomic<bool>& entered) {
    auto g = co_await s.lock();
    ex.enter();
    entered = true;
    std::this_thread::sleep_for(2ms);
    ex.leave();
}

// H7
task<void> job_hold_until(AsyncMutex& s, std::shared_future<void> go) {
    auto g = co_await s.lock();
    go.wait();
}
task<void> homeless_s_q_r(AsyncMutex& s, AsyncMutex& q, AsyncMutex& r, std::atomic<int>& stage) {
    auto gs = co_await s.lock();
    stage   = 1;
    auto gq = co_await q.lock();
    stage   = 2;
    auto gr = co_await r.lock();
    stage   = 3;
}
task<void> job_ask_is_held(AsyncMutex& s, std::atomic<int>& answer) {
    answer = s.is_held_by_current_thread() ? 1 : 0;
    co_return;
}

// H8
task<bool> nested_check(AsyncMutex& s) { co_return s.is_held_by_current_thread(); }
task<bool> round_with_tool(AsyncMutex& s) {
    auto g = co_await s.lock();
    co_return block_on(nested_check(s));  // a tool closure's synchronous reentrant check
}

// H9
task<std::thread::id> lock_and_report_thread(AsyncMutex& m) {
    auto g = co_await m.lock();
    co_return std::this_thread::get_id();
}

// H10
task<void> hold_l1_park_on_l2(AsyncMutex& l1, AsyncMutex& l2, std::atomic<int>& stage) {
    auto g1 = co_await l1.lock();
    stage   = 1;
    auto g2 = co_await l2.lock();
    stage   = 2;
}
task<void> block_on_l1(AsyncMutex& l1, std::atomic<int>& stage) {
    auto g = block_on(acquire(l1));
    stage  = stage + 10;
    co_return;
}

// H11
task<int> await_through_consumer(channel_consumer<int>* consumer) {
    std::optional<int> v = co_await consumer->next_async();  // `consumer` is not touched after this
    co_return v.has_value() ? 1 : 0;
}

// H12
task<void> bump(AsyncMutex& m, Exclusion& ex, long& counter) {
    auto g = co_await m.lock();
    ex.enter();
    ++counter;  // unsynchronized on purpose: only exclusion keeps it exact
    ex.leave();
}

// H13
task<void> helper_takes_lock(AsyncMutex& m, std::atomic<int>& stage) {
    auto g = co_await m.lock();
    stage = 1;
}
task<void> job_raw_resumes(task<void>* helper) {
    helper->resume();  // raw resume once; the lock's hand-off is expected to finish it
    co_return;
}
task<int> tool_raw_resumes(task<void>* helper) {
    helper->resume();
    co_return 0;
}

// H14
struct OffloadToPool {
    ThreadPool* pool;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        struct Job {
            static task<void> run(std::coroutine_handle<> resumed) {
                resumed.resume();  // a bare handle resume, from inside the job's own block_on()
                co_return;
            }
        };
        (void)pool->submit(Job::run(h));
    }
    void await_resume() const noexcept {}
};
task<int> offloaded_then_contended(ThreadPool& pool, AsyncMutex& m, std::atomic<int>& stage) {
    co_await OffloadToPool{&pool};
    stage   = 1;
    auto g  = co_await m.lock();
    stage   = 2;
    co_return 7;
}

// H16 -- a coroutine type no rt:: driver knows, resumed by bare handle (a foreign awaitable's shape).
struct BareCoroutine {
    struct promise_type {
        BareCoroutine get_return_object() noexcept {
            return BareCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle{};
};
BareCoroutine bare_waiter(AsyncMutex& m, std::atomic<int>& completed) {
    {
        AsyncMutex::Guard g = co_await m.lock();
        completed.fetch_add(1);
    }
}
task<void> start_bare_inside(AsyncMutex& m, std::atomic<int>& completed, std::vector<BareCoroutine>& out) {
    out.push_back(bare_waiter(m, completed));
    out.back().handle.resume();  // parks on `m`, homed to THIS block_on(), which then returns
    co_return;
}
BareCoroutine bare_holder_parks(AsyncMutex& m, channel_consumer<int>& c, std::atomic<bool>& holds) {
    AsyncMutex::Guard g = co_await m.lock();
    holds = true;
    (void)co_await c.next_async();  // park while HOLDING m
}
task<void> start_bare_holder_inside(AsyncMutex& m, channel_consumer<int>& c, std::atomic<bool>& holds,
                                    BareCoroutine& out) {
    out = bare_holder_parks(m, c, holds);
    out.handle.resume();
    co_return;
}

// H17
task<bool> raw_driven_check(AsyncMutex& s) { co_return s.is_held_by_current_thread(); }
task<bool> round_raw_resumes_subtask(AsyncMutex& s) {
    auto g   = co_await s.lock();
    auto sub = raw_driven_check(s);
    while (!sub.done()) sub.resume();  // the examples' hand-written drive idiom, inside a round
    co_return sub.take_value();
}

// H15
task<void> homeless_round(AsyncMutex& s, std::promise<void>& in_critical_section, std::shared_future<void> leave) {
    auto g = co_await s.lock();
    in_critical_section.set_value();
    leave.wait();
}

}  // namespace

int main(int argc, char** argv) {
    // Optional argument: run only the named check (e.g. H6) -- used by ADR-175's planted-mutant runs.
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

    // H1
    if (want("H1")) {
        AsyncMutex m;
        Exclusion ex;
        std::atomic<long> entered{0};
        long faulted = 0;
        long total   = 0;
        {
            ThreadPool pool(4);
            for (int round = 0; round < 600; ++round) {
                std::vector<std::future<JobOutcome>> fs;
                for (int j = 0; j < 32; ++j) fs.push_back(pool.submit(contended(m, ex, entered)));
                for (auto& f : fs) {
                    faulted += f.get().faulted ? 1 : 0;
                    ++total;
                }
            }
        }
        check(faulted == 0 && entered.load() == total && ex.most.load() == 1,
              "H1: issue #78 -- " + std::to_string(total) + " contending pool jobs, faulted=" +
                  std::to_string(faulted) + ", entered=" + std::to_string(entered.load()) +
                  ", most inside at once=" + std::to_string(ex.most.load()));
    }

    // H2
    if (want("H2")) {
        AsyncMutex m;
        std::future<JobOutcome> f;
        {
            ExternalHolder holder(m);
            ThreadPool pool(1);
            f = pool.submit(park_then_throw(m));
            std::this_thread::sleep_for(50ms);
            holder.release();
            JobOutcome const o = f.get();
            bool original = false;
            try {
                if (o.fault) std::rethrow_exception(o.fault);
            } catch (std::runtime_error const& e) {
                original = std::string(e.what()) == "thrown-after-parking";
            } catch (...) {
            }
            check(o.faulted && original, "H2: a job that throws after parking reports its original exception");
        }
    }

    // H3
    if (want("H3")) {
        auto pair = make_channel<int>(4);
        std::optional<int> got;
        std::thread::id before{};
        std::thread::id after{};
        ThreadPool pool(2);
        auto f = pool.submit(consume_one(pair.consumer, got, before, after));
        std::this_thread::sleep_for(50ms);
        std::thread producer([&] { (void)pair.producer.push(7); });
        JobOutcome const o = f.get();
        producer.join();
        check(!o.faulted && got.has_value() && *got == 7 && after == before,
              "H3: a pool job parked on next_async() completes with the pushed value, on the worker it parked on");
    }

    // H4
    if (want("H4")) {
        AsyncMutex m;
        std::atomic<bool> destroyed{false};
        ExternalHolder holder(m);
        ThreadPool pool(1);
        auto f = pool.submit(park_with_param(m, SlowToDestroy(&destroyed)));
        std::this_thread::sleep_for(30ms);
        holder.release();
        (void)f.get();
        check(destroyed.load(), "H4: the job's by-value parameters were destroyed before future::get() returned");
    }

    // H5
    if (want("H5")) {
        AsyncMutex m;
        std::atomic<int> stage{0};
        std::future<JobOutcome> f;
        ThreadPool pool(2);
        {
            ExternalHolder holder(m);
            f = pool.submit(release_then_relock(m, stage));
            std::this_thread::sleep_for(50ms);
            holder.release();
        }
        bool const done = f.wait_for(20s) == std::future_status::ready;
        check(done && stage.load() == 3 && !f.get().faulted,
              "H5: a woken job that releases and re-acquires through block_on() completes (stage=" +
                  std::to_string(stage.load()) + ")");
    }

    // H6
    if (want("H6")) {
        // Repeated: the window is from the hand-off until the successor's thread wakes, so one attempt can
        // miss it (a planted "record the owner at resume" mutant passed a single attempt).
        constexpr int kAttempts = 300;
        AsyncMutex s;
        Exclusion ex;
        std::atomic<int> claimed_after{0};
        int b_entered = 0;
        {
            ThreadPool pa(1);
            ThreadPool pb(1);
            for (int i = 0; i < kAttempts; ++i) {
                std::promise<void> a_acquired;
                std::promise<void> a_release;
                std::shared_future<void> const release = a_release.get_future().share();
                std::atomic<bool> entered{false};
                auto fa = pa.submit(job_round_then_check(s, ex, a_acquired, release, claimed_after));
                a_acquired.get_future().wait();
                auto fb = pb.submit(job_enter(s, ex, entered));
                std::this_thread::sleep_for(2ms);  // B parks on S
                a_release.set_value();
                (void)fa.get();
                (void)fb.get();
                b_entered += entered.load() ? 1 : 0;
            }
        }
        check(claimed_after.load() == 0 && b_entered == kAttempts && ex.most.load() == 1,
              "H6: after handing S to a parked successor, the releasing task is never reported as holder (" +
                  std::to_string(claimed_after.load()) + " of " + std::to_string(kAttempts) +
                  " attempts claimed it, most inside at once=" + std::to_string(ex.most.load()) + ")");
    }

    // H7
    if (want("H7")) {
        AsyncMutex s;
        AsyncMutex q;
        AsyncMutex r;
        std::atomic<int> stage{0};
        std::atomic<int> answer_s{-1};
        std::atomic<int> answer_q{-1};
        std::promise<void> a_go;
        std::shared_future<void> const go = a_go.get_future().share();
        ExternalHolder q_holder(q);
        ExternalHolder r_holder(r);
        task<void> homeless = homeless_s_q_r(s, q, r, stage);
        {
            ThreadPool pool(1);
            auto fa = pool.submit(job_hold_until(s, go));
            std::this_thread::sleep_for(50ms);
            homeless.resume();  // raw resume: parks on S, held by the job
            a_go.set_value();
            (void)fa.get();  // the job released S: the homeless coroutine ran inline on the worker, took S, parked on Q
            for (int i = 0; i < 400 && stage.load() < 1; ++i) std::this_thread::sleep_for(5ms);
            (void)pool.submit(job_ask_is_held(s, answer_s)).get();
            // Q's release resumes it inline on Q's holder thread: it takes Q -- granted under the holder id it
            // recorded while running inline on the worker -- and parks on R.
            q_holder.release();
            for (int i = 0; i < 400 && stage.load() < 2; ++i) std::this_thread::sleep_for(5ms);
            (void)pool.submit(job_ask_is_held(q, answer_q)).get();
            r_holder.release();
            // R's release resumes the homeless coroutine inline on R's holder thread, which runs it to
            // completion before that thread exits: joining is the happens-before edge for reading it here
            // (polling done() instead was a data race, caught by TSan on g++-14).
            r_holder.join();
            q_holder.join();
        }
        check(stage.load() == 3 && answer_s.load() == 0 && answer_q.load() == 0 && homeless.done(),
              "H7: while a homeless coroutine holds S, then Q, an unrelated job on the worker that once resumed "
              "it is the holder of neither (S answer=" + std::to_string(answer_s.load()) + ", Q answer=" +
                  std::to_string(answer_q.load()) + ")");
    }

    // H8
    if (want("H8")) {
        AsyncMutex s;
        check(block_on(round_with_tool(s)),
              "H8: inside a round driven by block_on(), a nested synchronous block_on() is still the holder");
        auto g = block_on(acquire(s));
        check(s.is_held_by_current_thread(), "H8: a guard returned out of a top-level block_on() belongs to the caller");
    }

    // H9
    if (want("H9")) {
        AsyncMutex m;
        std::thread::id released_on{};
        std::thread::id resumed_on{};
        {
            ExternalHolder holder(m);
            released_on = holder.id();
            std::thread releaser([&] {
                std::this_thread::sleep_for(50ms);
                holder.release();
            });
            resumed_on = block_on(lock_and_report_thread(m));
            releaser.join();
        }
        check(resumed_on == std::this_thread::get_id() && resumed_on != released_on,
              "H9: block_on() resumed its parked task on the calling thread, not the releasing thread");
    }

    // H10
    if (want("H10")) {
        AsyncMutex l1;
        AsyncMutex l2;
        std::atomic<int> stage{0};
        ThreadPool pool(1);
        std::future<JobOutcome> fa;
        std::future<JobOutcome> fb;
        {
            ExternalHolder l2_holder(l2);
            fa = pool.submit(hold_l1_park_on_l2(l1, l2, stage));
            std::this_thread::sleep_for(50ms);
            fb = pool.submit(block_on_l1(l1, stage));
            std::this_thread::sleep_for(50ms);
            l2_holder.release();
        }
        bool const done = fb.wait_for(20s) == std::future_status::ready;
        check(done && !fa.get().faulted && !fb.get().faulted && stage.load() == 12,
              "H10: one worker -- a job parked while holding L1, and a job blocking on L1, both complete (stage=" +
                  std::to_string(stage.load()) + ")");
    }

    // H11
    if (want("H11")) {
        auto pair     = make_channel<int>(4);
        auto consumer = std::make_unique<channel_consumer<int>>(std::move(pair.consumer));
        channel_consumer<int>* const raw = consumer.get();
        std::thread destroyer([&] {
            std::this_thread::sleep_for(50ms);
            consumer.reset();  // cancel(): the parked awaiter's wake-up is posted to the block_on below
        });
        int const got = block_on(await_through_consumer(raw));
        destroyer.join();
        check(got == 0, "H11: an awaiter woken by its consumer's destruction resumes without reaching through it");
    }

    // H12
    if (want("H12")) {
        AsyncMutex m;
        Exclusion ex;
        long counter = 0;
        constexpr int kPerThread = 3000;
        constexpr int kJobs      = 6000;
        std::vector<std::thread> threads;
        {
            ThreadPool pool(3);
            std::vector<std::future<JobOutcome>> fs;
            for (int t = 0; t < 3; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kPerThread; ++i) block_on(bump(m, ex, counter));
                });
            }
            for (int i = 0; i < kJobs; ++i) fs.push_back(pool.submit(bump(m, ex, counter)));
            for (auto& f : fs) (void)f.get();
            for (auto& t : threads) t.join();
        }
        check(counter == 3 * kPerThread + kJobs && ex.most.load() == 1,
              "H12: block_on threads and pool jobs on one mutex -- count " + std::to_string(counter) +
                  ", most inside at once " + std::to_string(ex.most.load()));
    }

    // H13
    if (want("H13")) {
        for (bool const via_pool : {true, false}) {
            AsyncMutex m;
            std::atomic<int> stage{0};
            task<void> helper = helper_takes_lock(m, stage);
            {
                ExternalHolder holder(m);
                if (via_pool) {
                    ThreadPool pool(1);
                    (void)pool.submit(job_raw_resumes(&helper)).get();
                } else {
                    (void)block_on(tool_raw_resumes(&helper));
                }
                holder.release();
                holder.join();  // its unlock() ran the helper to completion before the thread exited
            }
            bool acquired = false;
            if (helper.done()) {
                auto g   = block_on(acquire(m));
                acquired = g.held();
            }
            check(stage.load() == 1 && helper.done() && acquired,
                  std::string("H13: a task raw-resumed inside a ") + (via_pool ? "pool job" : "block_on()") +
                      " whose driver has returned still completes when the lock is granted, and the lock is free "
                      "afterwards");
        }
    }

    // H14
    if (want("H14")) {
        ThreadPool pool(2);
        AsyncMutex m;
        std::atomic<int> stage{0};
        int result = 0;
        {
            ExternalHolder holder(m);
            std::thread releaser([&] {
                while (stage.load() < 1) std::this_thread::yield();
                std::this_thread::sleep_for(100ms);
                holder.release();
            });
            result = block_on(offloaded_then_contended(pool, m, stage));
            releaser.join();
        }
        check(result == 7 && stage.load() == 2,
              "H14: a task that hops to a pool job by bare handle resume, then parks on a contended lock after "
              "that job's block_on() returned, still completes");
    }

    // H15
    if (want("H15")) {
        AsyncMutex s;
        std::promise<void> in_cs;
        std::promise<void> leave_p;
        std::shared_future<void> const leave = leave_p.get_future().share();
        bool top_level = true;
        bool in_block_on = true;
        task<void> round = homeless_round(s, in_cs, leave);
        {
            ExternalHolder holder(s);
            round.resume();   // this thread, no driver: parks on S
            holder.release();
            in_cs.get_future().wait();  // the round now holds S, running on the holder's thread
            top_level   = s.is_held_by_current_thread();
            in_block_on = block_on(nested_check(s));
            leave_p.set_value();
            holder.join();
        }
        check(!top_level && !in_block_on && round.done(),
              "H15: the thread a homeless round parked FROM is not its holder once it holds the lock elsewhere "
              "(top level " + std::to_string(top_level) + ", inside block_on " + std::to_string(in_block_on) + ")");
    }

    // H16
    if (want("H16")) {
        constexpr int kWaiters = 5000;
        AsyncMutex m;
        std::atomic<int> completed{0};
        std::atomic<bool> main_says_held{true};
        std::atomic<bool> holder_got_lock{false};
        // On its own thread with the default 1 MiB stack, so a nesting regression overflows here, not in
        // whatever thread the test harness happens to have.
        std::thread runner([&] {
            std::vector<BareCoroutine> frames;
            frames.reserve(kWaiters);
            {
                AsyncMutex::Guard g0 = block_on(acquire(m));
                for (int i = 0; i < kWaiters; ++i) block_on(start_bare_inside(m, completed, frames));
            }  // g0 released: every waiter's block_on() has returned, so each is granted into a closed home
            for (auto& f : frames) f.handle.destroy();

            auto pair = make_channel<int>(1);
            BareCoroutine holder;
            {
                AsyncMutex::Guard g1 = block_on(acquire(m));
                block_on(start_bare_holder_inside(m, pair.consumer, holder_got_lock, holder));
                std::thread releaser([&] { g1 = AsyncMutex::Guard{}; });  // granted after that block_on returned
                releaser.join();
            }
            main_says_held = m.is_held_by_current_thread();
            (void)pair.producer.push(1);  // let the holder finish and release
            holder.handle.destroy();
        });
        runner.join();
        check(completed.load() == kWaiters,
              "H16: " + std::to_string(kWaiters) + " bare-resumed waiters granted into closed homes complete without "
              "nesting stack frames (" + std::to_string(completed.load()) + " completed)");
        check(holder_got_lock.load() && !main_says_held.load(),
              "H16: a waiter granted into a closed home runs under a fresh holder id -- the thread whose block_on() "
              "it parked under is not its holder");
    }

    // H17
    if (want("H17")) {
        AsyncMutex s;
        check(!block_on(round_raw_resumes_subtask(s)),
              "H17 (pinned choice, ADR-175 §4): a sub-task raw-resumed inside a round runs under its own holder id, "
              "so it is NOT the round's lock holder -- inheriting would reopen round 3 finding 3's I1 false positive");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
