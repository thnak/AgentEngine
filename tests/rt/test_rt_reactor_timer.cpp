// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md, implementation step 1: the I/O reactor
// seam (pal/reactor.hpp), its Asio backend (src/backends/reactor_asio/) and the first engine awaitable on it,
// rt::sleep_until (rt/sleep.hpp). Each claim names the ADR rule it proves and how it would fail.
//
//   R1  An expired sleep resumes on the thread that block_on()s it -- its home -- never on the reactor thread
//       (§4.2). Mutant: wake inline in on_complete -> the observed thread is the reactor's.
//   R2  A stop requested from another thread mid-sleep resumes the waiter promptly with `canceled` (§4.4).
//   R3  A stop already requested before the co_await completes synchronously, without a reactor round trip.
//   R4  Sticky cancel (round-2 M2): stops requested at racing moments around the start are never lost --
//       every one of many sleeps of 60 s ends `canceled`, quickly. That race rarely hits the narrow window, so
//   R4b proves the backend rule deterministically: an op cancelled BEFORE its start completes `canceled`
//       at once. Mutant: ignore the sticky flag at start -> R4b waits out its 60 s timer.
//   R5  Under an ADR-219 ScopedResumer the waiter is handed to the host Resumer and resumed on ITS thread.
//   R6  A homeless waiter (raw resume(), no block_on, no resumer) is REFUSED, not resumed on the reactor
//       thread (§4.2); the refusal is counted. Mutant: drop the refusal -> R6 sees it run on the reactor.
//   R7  A frame destroyed while waiting abandons its operation: the late completion posts nothing.
//   R8  Destroying the reactor cancels pending sleeps and delivers their completions before joining (§4.5
//       rule 4): a waiter blocked for 60 s returns `canceled` promptly.
//   R9  The completion reports what happened (round-1 M1): a stop requested after expiry still reads
//       `expired`.
//   R10 Concurrency: 8 threads x 100 sleeps, half of them cancelled, all finish with the right status.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/task.hpp"
#include "../support/crt_fail_fast.hpp"

using namespace agentengine;
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

struct Observed {
    sleep_status    status = sleep_status::canceled;
    std::thread::id resumed_on{};
};

task<Observed> nap(pal::Reactor& r, std::chrono::milliseconds d, std::stop_token stop = {}) {
    Observed o;
    o.status     = co_await sleep_for(r, d, std::move(stop));
    o.resumed_on = std::this_thread::get_id();
    co_return o;
}

// Coroutines are free functions taking their state by pointer: an immediately-invoked lambda coroutine would
// read its captures through a closure object destroyed at the end of the full expression, before the lazily
// started body runs (a real use-after-free on gcc/Linux, found by this test's first version).
task<void> nap_into(pal::Reactor* r, std::chrono::milliseconds d, Observed* out, std::atomic<bool>* finished) {
    *out = co_await nap(*r, d);
    finished->store(true, std::memory_order_release);
}

task<void> nap_then_flag(pal::Reactor* r, std::chrono::milliseconds d, std::atomic<bool>* ran) {
    (void)co_await sleep_for(*r, d);
    ran->store(true, std::memory_order_release);
}

task<sleep_status> nap_status(pal::Reactor* r, std::chrono::milliseconds d, std::stop_token stop) {
    co_return co_await sleep_for(*r, d, std::move(stop));
}

// A host Resumer with its own worker thread (as in test_rt_host_resumer.cpp).
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
    void stop() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
    [[nodiscard]] std::thread::id id() const { return worker_.get_id(); }
    [[nodiscard]] int posted() {
        std::lock_guard lock(m_);
        return posted_;
    }

private:
    void loop() {
        for (;;) {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
        }
    }

    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    int                            posted_ = 0;
    bool                           stop_   = false;
    std::thread                    worker_;
};

// The reactor thread's id, learned from inside a completion (no API exposes it; on_reactor_thread() is the
// predicate the engine uses).
struct ProbeOp final : pal::ReactorOp {
    std::atomic<bool>            done{false};
    std::thread::id              where{};
    pal::op_status               status = pal::op_status::completed;
    void on_complete(pal::op_status st) noexcept override {
        where  = std::this_thread::get_id();
        status = st;
        done.store(true, std::memory_order_release);
    }
};

std::thread::id reactor_thread_of(pal::Reactor& r) {
    auto probe = std::make_shared<ProbeOp>();
    r.start_timer(probe, pal::Reactor::clock::now());
    (void)wait_until([&] { return probe->done.load(std::memory_order_acquire); });
    return probe->where;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(120s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 120 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main

    auto                  reactor   = pal::make_default_reactor();
    std::thread::id const reactor_t = reactor_thread_of(*reactor);
    check(reactor_t != std::thread::id{} && reactor_t != std::this_thread::get_id(),
          "setup: the reactor runs its own thread");

    // R1 ------------------------------------------------------------------------------------------------
    {
        auto const t0 = std::chrono::steady_clock::now();
        Observed   o  = block_on(nap(*reactor, 30ms));
        auto const dt = std::chrono::steady_clock::now() - t0;
        check(o.status == sleep_status::expired, "R1: the sleep expires");
        check(dt >= 30ms, "R1: it lasted at least its duration");
        check(o.resumed_on == std::this_thread::get_id(), "R1: it resumed on the block_on() thread (its home)");
        check(o.resumed_on != reactor_t, "R1: it did not resume on the reactor thread");
    }

    // R2 ------------------------------------------------------------------------------------------------
    {
        std::stop_source src;
        std::thread      stopper([&] {
            std::this_thread::sleep_for(50ms);
            src.request_stop();
        });
        auto const t0 = std::chrono::steady_clock::now();
        Observed   o  = block_on(nap(*reactor, 60s, src.get_token()));
        auto const dt = std::chrono::steady_clock::now() - t0;
        stopper.join();
        check(o.status == sleep_status::canceled, "R2: a stop mid-sleep ends it `canceled`");
        check(dt < 5s, "R2: promptly (well before the 60 s deadline)");
        check(o.resumed_on == std::this_thread::get_id(), "R2: still resumed on its home thread");
    }

    // R3 ------------------------------------------------------------------------------------------------
    {
        std::stop_source src;
        src.request_stop();
        Observed o = block_on(nap(*reactor, 60s, src.get_token()));
        check(o.status == sleep_status::canceled, "R3: an already-requested stop completes `canceled`");
    }

    // R4 ------------------------------------------------------------------------------------------------
    {
        int        canceled = 0;
        auto const t0       = std::chrono::steady_clock::now();
        for (int i = 0; i < 300; ++i) {
            std::stop_source src;
            std::thread      stopper([&src, i] {
                if (i % 3 == 1) std::this_thread::yield();
                if (i % 3 == 2) std::this_thread::sleep_for(std::chrono::microseconds(50));
                src.request_stop();
            });
            Observed o = block_on(nap(*reactor, 60s, src.get_token()));
            stopper.join();
            if (o.status == sleep_status::canceled) ++canceled;
        }
        check(canceled == 300, "R4: every one of 300 racing stops was honoured (none lost between "
                               "registration and start) -- " + std::to_string(canceled) + "/300");
        check(std::chrono::steady_clock::now() - t0 < 30s, "R4: and none waited out its 60 s timer");
    }

    // R4b -----------------------------------------------------------------------------------------------
    {
        auto probe = std::make_shared<ProbeOp>();
        reactor->cancel(probe);  // before the start: only the sticky flag can carry it
        reactor->start_timer(probe, pal::Reactor::clock::now() + 60s);
        check(wait_until([&] { return probe->done.load(std::memory_order_acquire); }, 5s),
              "R4b: an op cancelled before its start completes at once (sticky cancel)");
        check(probe->done.load(std::memory_order_acquire) && probe->status == pal::op_status::canceled,
              "R4b: ... as `canceled`");
        check(probe->where == reactor_t, "R4b: the completion ran on the reactor thread");
    }

    // R5 ------------------------------------------------------------------------------------------------
    {
        auto             resumer = std::make_shared<ThreadResumer>();
        std::atomic<bool> finished{false};
        Observed          o;
        auto              t = nap_into(reactor.get(), 20ms, &o, &finished);
        {
            ScopedResumer scope(resumer);
            t.resume();  // raw drive under a host resumer: parks on the reactor
        }
        check(wait_until([&] { return finished.load(std::memory_order_acquire); }),
              "R5: the waiter finished");
        check(o.status == sleep_status::expired, "R5: expired");
        check(o.resumed_on == resumer->id(), "R5: resumed on the host Resumer's thread");
        check(o.resumed_on != reactor_t, "R5: not on the reactor thread");
        check(resumer->posted() >= 1, "R5: the continuation went through Resumer::post()");
        resumer->stop();
    }

    // R6 ------------------------------------------------------------------------------------------------
    {
        std::uint64_t const before = reactor->homeless_refusals();
        std::atomic<bool>   ran{false};
        auto                t = nap_then_flag(reactor.get(), 10ms, &ran);
        t.resume();  // homeless: no block_on, no ScopedResumer
        check(wait_until([&] { return reactor->homeless_refusals() == before + 1; }, 5s),
              "R6: the homeless wake-up was refused and counted");
        std::this_thread::sleep_for(50ms);
        check(!ran.load(std::memory_order_acquire), "R6: the homeless waiter was never resumed (not on the "
                                                    "reactor thread, not anywhere)");
        // `t` is destroyed here while parked: its awaiter sees the operation already woken and does nothing.
    }

    // R7 ------------------------------------------------------------------------------------------------
    {
        auto resumer = std::make_shared<ThreadResumer>();
        {
            std::atomic<bool> ran{false};
            auto              t = nap_then_flag(reactor.get(), 30ms, &ran);
            ScopedResumer scope(resumer);
            t.resume();
        }  // frame destroyed while the timer is pending
        std::this_thread::sleep_for(150ms);
        check(resumer->posted() == 0, "R7: a frame destroyed while waiting gets no continuation posted later");
        resumer->stop();
    }

    // R8 ------------------------------------------------------------------------------------------------
    {
        auto               doomed = pal::make_default_reactor();
        std::atomic<bool>  done{false};
        Observed           o;
        pal::Reactor*      raw = doomed.get();  // the waiter must not read the unique_ptr main resets (TSan)
        std::thread        waiter([&o, &done, raw] {
            o = block_on(nap(*raw, 60s));
            done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(50ms);
        auto const t0 = std::chrono::steady_clock::now();
        doomed.reset();  // shutdown with a pending sleep
        check(std::chrono::steady_clock::now() - t0 < 5s, "R8: reactor shutdown returned promptly");
        waiter.join();
        check(done.load() && o.status == sleep_status::canceled,
              "R8: the pending sleep was completed `canceled` by shutdown, not dropped");
    }

    // R9 ------------------------------------------------------------------------------------------------
    {
        std::stop_source src;
        sleep_status s = block_on(nap_status(reactor.get(), 1ms, src.get_token()));
        src.request_stop();  // after expiry: must not rewrite history
        check(s == sleep_status::expired, "R9: a stop after expiry leaves the outcome `expired`");
    }

    // R10 -----------------------------------------------------------------------------------------------
    {
        std::atomic<int> right{0};
        std::vector<std::thread> threads;
        for (int k = 0; k < 8; ++k) {
            threads.emplace_back([&, k] {
                for (int i = 0; i < 100; ++i) {
                    bool const       cancel = (i + k) % 2 == 0;
                    std::stop_source src;
                    if (cancel) src.request_stop();
                    Observed o = block_on(nap(*reactor, cancel ? 60s : 1ms, src.get_token()));
                    if (o.status == (cancel ? sleep_status::canceled : sleep_status::expired) &&
                        o.resumed_on == std::this_thread::get_id()) {
                        right.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        check(right.load() == 800, "R10: 800 concurrent sleeps, each with the right status on its own home "
                                   "thread -- " + std::to_string(right.load()) + "/800");
    }

    reactor.reset();
    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
