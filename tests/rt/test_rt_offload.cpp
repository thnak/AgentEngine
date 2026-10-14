// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §4.6 ("Truly blocking work:
// `rt::offload`", "Offload lifetime rules") and the §4.3 refusal of block_on on offload workers --
// rt::OffloadPool / rt::offload (rt/offload.hpp). Each claim names the rule it proves and how it would fail.
//
//   O1  The body runs on a worker; the caller resumes on its HOME (the block_on() thread), never on the worker
//       (§4.2). Mutant: resume inline from the worker -> resumed_on is the worker.
//   O2  Under an ADR-219 ScopedResumer the caller is handed to the host Resumer and resumes on ITS thread.
//   O3  A homeless waiter (raw resume(), no block_on, no resumer) is REFUSED -- never resumed on the worker or
//       anywhere -- and counted (§4.2). Mutant: drop the refusal -> it runs on the worker.
//   O4  Cancel before start: a stop already requested, or requested while the job waits in the queue, resumes
//       the caller `canceled` and the body NEVER runs (§4.6). Mutant: the worker ignores the job state. A
//       stop already requested goes through the ARMING path (the stop_callback fires inline while it is
//       registered; await_suspend declines to suspend). Mutant: post the continuation from that path too ->
//       a double resume.
//   O5  Cancel while running: the caller resumes `canceled` promptly while the body is still running; the
//       body is not interrupted, its result is discarded and counted (§4.6 residual). Mutant: a cancel of a
//       running job posts nothing (the caller waits for the body).
//   O6  A throwing body delivers its exception to the caller as `faulted`; nothing escapes on the worker.
//   O7  A frame destroyed while waiting abandons its job: a running body's completion posts nothing, a queued
//       body never runs. Mutant: abandon() does nothing -> a continuation is posted for a dead frame.
//   O8  block_on() inside a body is refused with `rt.block_on_on_offload_worker` (§4.3), delivered to the
//       caller as a fault; block_on() elsewhere is unaffected. Mutant: no marker on workers.
//   O9  Destroying the pool waits for a running body -- even one whose caller already resumed `canceled` --
//       and joins it (never detaches, §4.5 rules 1 and 4); queued jobs resume `canceled` and never run. With
//       a shutdown deadline, the overrun is reported and the join still completes. Mutants: detach instead of
//       join; skip the overrun report.
//   O10 Submitting to a shut-down pool resumes the caller `canceled` at once; the body never runs.
//   O11 Concurrency: 8 threads x 200 jobs on a 2-worker pool, a third pre-canceled and a third canceled at
//       racing moments; every caller resumes exactly once, on its own home, with a consistent outcome, and
//       bodies finished == completed + discarded (exactly-once accounting).
//
// Coroutines are free functions taking their state by value or by pointer, never immediately-invoked lambda
// coroutines with captures (ADR-237 §14: a closure destroyed before the lazily started body ran).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/resume_home.hpp"
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

// A body that holds its worker until released. Shared by value (shared_ptr) -- the body never refers to the
// caller's frame (§4.6 by-value rule).
struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<int>  runs{0};
};

int gated_body(std::shared_ptr<Gate> g, int v) {
    g->runs.fetch_add(1);
    g->entered.store(true, std::memory_order_release);
    while (!g->release.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    g->finished.store(true, std::memory_order_release);
    return v;
}

struct Observed {
    offload_status  status = offload_status::canceled;
    int             value  = -1;
    std::thread::id body_on{};
    std::thread::id resumed_on{};
};

struct IdAndValue {
    std::thread::id id;
    int             value;
};

task<Observed> run_id_body(OffloadPool* pool, std::stop_token stop, int v) {
    auto r = co_await offload(*pool, std::move(stop), [](int x) { return IdAndValue{std::this_thread::get_id(), x}; },
                              v);
    Observed o;
    o.status     = r.status;
    o.resumed_on = std::this_thread::get_id();
    if (r.value) {
        o.body_on = r.value->id;
        o.value   = r.value->value;
    }
    co_return o;
}

task<Observed> run_gated(OffloadPool* pool, std::stop_token stop, std::shared_ptr<Gate> g, int v) {
    auto r = co_await offload(*pool, std::move(stop), gated_body, std::move(g), v);
    Observed o;
    o.status     = r.status;
    o.resumed_on = std::this_thread::get_id();
    if (r.value) o.value = *r.value;
    co_return o;
}

task<void> run_gated_into(OffloadPool* pool, std::stop_token stop, std::shared_ptr<Gate> g, Observed* out,
                          std::atomic<bool>* done) {
    *out = co_await run_gated(pool, std::move(stop), std::move(g), 7);
    done->store(true, std::memory_order_release);
}

task<offload_result<int>> run_throwing(OffloadPool* pool) {
    co_return co_await offload(*pool, std::stop_token{}, []() -> int { throw std::runtime_error("body failed"); });
}

task<offload_result<int>> run_doubling(OffloadPool* pool, std::stop_token stop, int v, std::atomic<int>* finished) {
    co_return co_await offload(
        *pool, std::move(stop),
        [](int x, std::atomic<int>* fin) {
            // A short, varying run time, so racing cancels land before, during and after the body.
            auto const until = std::chrono::steady_clock::now() + std::chrono::microseconds((x % 4) * 30);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
            fin->fetch_add(1);
            return x * 2;
        },
        v, finished);
}

task<int> trivially_42() { co_return 42; }

task<offload_result<int>> run_block_on_inside(OffloadPool* pool) {
    co_return co_await offload(*pool, std::stop_token{}, [] { return block_on(trivially_42()); });
}

task<void> trivially_void() { co_return; }

task<offload_result<void>> run_block_on_void_inside(OffloadPool* pool) {
    co_return co_await offload(*pool, std::stop_token{}, [] { block_on(trivially_void()); });
}

task<offload_result<void>> run_void(OffloadPool* pool, std::shared_ptr<std::atomic<int>> counter) {
    co_return co_await offload(*pool, std::stop_token{}, [](std::shared_ptr<std::atomic<int>> c) { c->fetch_add(1); },
                               std::move(counter));
}

// A host Resumer with its own worker thread (as in test_rt_reactor_timer.cpp).
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

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(150s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 150 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main

    OffloadPool pool;  // default: 2 workers
    check(pool.workers() == 2, "setup: the default pool has 2 workers (§9 D5)");

    // O1 ------------------------------------------------------------------------------------------------
    {
        Observed o = block_on(run_id_body(&pool, {}, 21));
        check(o.status == offload_status::completed && o.value == 21, "O1: the body's result is delivered");
        check(o.body_on != std::thread::id{} && o.body_on != std::this_thread::get_id(),
              "O1: the body ran on an offload worker, not the caller's thread");
        check(o.resumed_on == std::this_thread::get_id(), "O1: the caller resumed on its block_on() thread (home)");
        check(o.resumed_on != o.body_on, "O1: ... not on the worker");
        auto counter = std::make_shared<std::atomic<int>>(0);
        auto rv      = block_on(run_void(&pool, counter));
        check(rv.completed() && counter->load() == 1, "O1: a void body completes too");
    }

    // O2 ------------------------------------------------------------------------------------------------
    {
        auto              resumer = std::make_shared<ThreadResumer>();
        auto              gate    = std::make_shared<Gate>();
        gate->release.store(true);
        std::atomic<bool> done{false};
        Observed          o;
        auto              t = run_gated_into(&pool, {}, gate, &o, &done);
        {
            ScopedResumer scope(resumer);
            t.resume();
        }
        check(wait_until([&] { return done.load(std::memory_order_acquire); }), "O2: the caller finished");
        check(o.status == offload_status::completed && o.value == 7, "O2: completed with the body's value");
        check(o.resumed_on == resumer->id(), "O2: resumed on the host Resumer's thread");
        check(resumer->posted() >= 1, "O2: the continuation went through Resumer::post()");
        resumer->stop();
    }

    // O3 ------------------------------------------------------------------------------------------------
    {
        std::uint64_t const before = pool.homeless_refusals();
        auto                gate   = std::make_shared<Gate>();
        gate->release.store(true);
        std::atomic<bool> done{false};
        Observed          o;
        auto              t = run_gated_into(&pool, {}, gate, &o, &done);
        t.resume();  // homeless: no block_on, no ScopedResumer
        check(wait_until([&] { return pool.homeless_refusals() == before + 1; }, 5s),
              "O3: the homeless wake-up was refused and counted");
        std::this_thread::sleep_for(50ms);
        check(gate->finished.load() && !done.load(std::memory_order_acquire),
              "O3: the body ran, but the homeless caller was never resumed (not on the worker, not anywhere)");
        // `t` is destroyed here while parked: its job is already done, so nothing happens.
    }

    // O4 ------------------------------------------------------------------------------------------------
    {
        auto             gate = std::make_shared<Gate>();
        std::stop_source src;
        src.request_stop();
        Observed o = block_on(run_gated(&pool, src.get_token(), gate, 1));
        check(o.status == offload_status::canceled, "O4: a stop requested before the co_await ends it `canceled`");
        check(gate->runs.load() == 0, "O4: ... and the body never ran");

        // A job still waiting in the queue: a one-worker pool kept busy by a blocker.
        OffloadPool       one({.workers = 1});
        auto              blocker = std::make_shared<Gate>();
        std::atomic<bool> blocker_done{false};
        Observed          blocker_o;
        auto              resumer = std::make_shared<ThreadResumer>();
        auto              bt      = run_gated_into(&one, {}, blocker, &blocker_o, &blocker_done);
        {
            ScopedResumer scope(resumer);
            bt.resume();
        }
        check(wait_until([&] { return blocker->entered.load(); }), "O4: the blocker occupies the only worker");
        auto             queued_gate = std::make_shared<Gate>();
        queued_gate->release.store(true);
        std::stop_source qsrc;
        std::thread      stopper([&] {
            (void)wait_until([&] { return one.queued() == 1; });
            qsrc.request_stop();
        });
        Observed q = block_on(run_gated(&one, qsrc.get_token(), queued_gate, 2));
        stopper.join();
        check(q.status == offload_status::canceled, "O4: a stop while queued resumes the caller `canceled`");
        check(q.resumed_on == std::this_thread::get_id(), "O4: ... on its home");
        blocker->release.store(true);
        check(wait_until([&] { return blocker_done.load(std::memory_order_acquire); }), "O4: the blocker finishes");
        check(wait_until([&] { return one.running() == 0 && one.queued() == 0; }), "O4: the pool drains");
        check(queued_gate->runs.load() == 0, "O4: the canceled queued body never ran");
        resumer->stop();
    }

    // O5 ------------------------------------------------------------------------------------------------
    {
        std::uint64_t const discarded_before = pool.discarded_results();
        auto                gate             = std::make_shared<Gate>();
        std::stop_source    src;
        std::thread         stopper([&] {
            (void)wait_until([&] { return gate->entered.load(); });
            src.request_stop();
        });
        auto const t0 = std::chrono::steady_clock::now();
        Observed   o  = block_on(run_gated(&pool, src.get_token(), gate, 3));
        auto const dt = std::chrono::steady_clock::now() - t0;
        stopper.join();
        check(o.status == offload_status::canceled, "O5: a stop while the body runs resumes the caller `canceled`");
        check(dt < 5s && !gate->finished.load(), "O5: promptly -- the body is still running (not interrupted)");
        check(o.resumed_on == std::this_thread::get_id(), "O5: ... on its home");
        check(pool.running() == 1, "O5: the running body stays owned and counted by the pool");
        gate->release.store(true);
        check(wait_until([&] { return pool.discarded_results() == discarded_before + 1; }),
              "O5: when the body finishes, its result is discarded and counted");
        check(gate->finished.load() && gate->runs.load() == 1, "O5: the body ran to its end exactly once");
    }

    // O6 ------------------------------------------------------------------------------------------------
    {
        auto r = block_on(run_throwing(&pool));
        check(r.status == offload_status::faulted && r.fault != nullptr && !r.value,
              "O6: a throwing body is delivered as `faulted` with its exception");
        std::string what;
        try {
            r.rethrow_if_faulted();
        } catch (std::runtime_error const& e) {
            what = e.what();
        }
        check(what == "body failed", "O6: the caller can rethrow the body's own exception");
        Observed after = block_on(run_id_body(&pool, {}, 5));
        check(after.status == offload_status::completed, "O6: the worker survived the throw");
    }

    // O7 ------------------------------------------------------------------------------------------------
    {
        auto resumer = std::make_shared<ThreadResumer>();
        auto gate    = std::make_shared<Gate>();
        {
            std::atomic<bool> done{false};
            Observed          o;
            auto              t = run_gated_into(&pool, {}, gate, &o, &done);
            {
                ScopedResumer scope(resumer);
                t.resume();
            }
            (void)wait_until([&] { return gate->entered.load(); });
        }  // frame destroyed while the body runs
        gate->release.store(true);
        check(wait_until([&] { return gate->finished.load() && pool.running() == 0; }), "O7: the body finished");
        std::this_thread::sleep_for(50ms);
        check(resumer->posted() == 0, "O7: a frame destroyed while its body ran gets no continuation posted");

        // A frame destroyed while its job is still queued: the body never runs.
        OffloadPool one({.workers = 1});
        auto        blocker = std::make_shared<Gate>();
        auto        queued  = std::make_shared<Gate>();
        queued->release.store(true);
        std::atomic<bool> bdone{false};
        Observed          bo;
        auto              bt = run_gated_into(&one, {}, blocker, &bo, &bdone);
        {
            ScopedResumer scope(resumer);
            bt.resume();
        }
        (void)wait_until([&] { return blocker->entered.load(); });
        {
            std::atomic<bool> done{false};
            Observed          o;
            auto              t = run_gated_into(&one, {}, queued, &o, &done);
            ScopedResumer     scope(resumer);
            t.resume();
        }  // destroyed while queued
        blocker->release.store(true);
        check(wait_until([&] { return bdone.load(std::memory_order_acquire); }), "O7: the blocker finished");
        check(wait_until([&] { return one.running() == 0 && one.queued() == 0; }), "O7: the pool drained");
        check(queued->runs.load() == 0, "O7: an abandoned queued job's body never ran");
        check(resumer->posted() == 1, "O7: only the live caller was posted");
        resumer->stop();
    }

    // O8 ------------------------------------------------------------------------------------------------
    {
        auto r = block_on(run_block_on_inside(&pool));
        check(r.status == offload_status::faulted, "O8: block_on() inside an offload body fails the body");
        std::string code;
        try {
            r.rethrow_if_faulted();
        } catch (block_on_refused const& e) {
            code = std::string(e.code());
        } catch (...) {  // NOLINT(bugprone-empty-catch): a different exception leaves `code` empty -> FAIL
        }
        check(code == "rt.block_on_on_offload_worker", "O8: ... with block_on_refused `rt.block_on_on_offload_worker`");
        auto rv = block_on(run_block_on_void_inside(&pool));
        std::string vcode;
        try {
            rv.rethrow_if_faulted();
        } catch (block_on_refused const& e) {
            vcode = std::string(e.code());
        } catch (...) {  // NOLINT(bugprone-empty-catch): a different exception leaves `vcode` empty -> FAIL
        }
        check(rv.status == offload_status::faulted && vcode == "rt.block_on_on_offload_worker",
              "O8: the block_on(task<void>) overload is refused the same way");
        check(block_on(trivially_42()) == 42, "O8: block_on() on an ordinary thread is unaffected");
    }

    // O9 ------------------------------------------------------------------------------------------------
    {
        auto gate = std::make_shared<Gate>();
        auto local = std::make_unique<OffloadPool>(OffloadPool::Options{.workers = 1});
        std::stop_source src;
        std::thread      stopper([&] {
            (void)wait_until([&] { return gate->entered.load(); });
            src.request_stop();
        });
        Observed o = block_on(run_gated(local.get(), src.get_token(), gate, 4));
        stopper.join();
        check(o.status == offload_status::canceled && !gate->finished.load(),
              "O9: the caller resumed `canceled` while its body still runs");

        // Queue one more job behind it; shutdown must resume that caller `canceled` without running it.
        auto              queued_gate = std::make_shared<Gate>();
        queued_gate->release.store(true);
        auto              resumer = std::make_shared<ThreadResumer>();
        std::atomic<bool> qdone{false};
        Observed          qo;
        auto              qt = run_gated_into(local.get(), {}, queued_gate, &qo, &qdone);
        {
            ScopedResumer scope(resumer);
            qt.resume();
        }
        std::thread releaser([gate] {
            std::this_thread::sleep_for(300ms);
            gate->release.store(true);
        });
        auto const t0 = std::chrono::steady_clock::now();
        local.reset();  // must wait for the running body
        auto const dt = std::chrono::steady_clock::now() - t0;
        releaser.join();
        check(gate->finished.load(), "O9: destroying the pool waited for the running body (joined, not detached)");
        check(dt >= 200ms, "O9: ... the destructor blocked until it finished");
        check(wait_until([&] { return qdone.load(std::memory_order_acquire); }) &&
                  qo.status == offload_status::canceled && queued_gate->runs.load() == 0,
              "O9: a queued job at shutdown resumed its caller `canceled` and never ran");
        resumer->stop();

        // With a deadline: the overrun is reported, and the join still completes.
        auto        slow     = std::make_shared<Gate>();
        std::size_t reported = 0;
        auto        bounded  = std::make_unique<OffloadPool>(OffloadPool::Options{
                    .workers = 1, .shutdown_deadline = 50ms, .on_overrun = [&](std::size_t n) { reported = n; }});
        auto        r2       = std::make_shared<ThreadResumer>();
        std::atomic<bool> sdone{false};
        Observed          so;
        auto              st = run_gated_into(bounded.get(), {}, slow, &so, &sdone);
        {
            ScopedResumer scope(r2);
            st.resume();
        }
        (void)wait_until([&] { return slow->entered.load(); });
        std::thread slow_releaser([slow] {
            std::this_thread::sleep_for(300ms);
            slow->release.store(true);
        });
        bounded->shutdown();
        std::uint64_t const overruns = bounded->shutdown_overruns();
        bounded.reset();
        slow_releaser.join();
        check(overruns == 1 && reported == 1, "O9: a body outlasting the shutdown deadline is reported as an overrun");
        check(slow->finished.load(), "O9: ... and the join still completed (no thread left behind)");
        check(wait_until([&] { return sdone.load(std::memory_order_acquire); }) &&
                  so.status == offload_status::completed,
              "O9: a body that finishes during shutdown still delivers its result");
        r2->stop();
    }

    // O10 -----------------------------------------------------------------------------------------------
    {
        OffloadPool closed({.workers = 1});
        closed.shutdown();
        auto     gate = std::make_shared<Gate>();
        Observed o    = block_on(run_gated(&closed, {}, gate, 9));
        check(o.status == offload_status::canceled && gate->runs.load() == 0,
              "O10: a job offered to a shut-down pool resumes `canceled` and never runs");
        bool threw = false;
        try {
            OffloadPool zero({.workers = 0});
        } catch (std::invalid_argument const&) {
            threw = true;
        }
        check(threw, "O10: a pool with zero workers is refused");
    }

    // O11 -----------------------------------------------------------------------------------------------
    {
        OffloadPool              stress;
        std::atomic<int>         bodies_finished{0};
        std::atomic<int>         completed{0};
        std::atomic<int>         canceled{0};
        std::atomic<int>         wrong{0};
        std::vector<std::thread> threads;
        for (int k = 0; k < 8; ++k) {
            threads.emplace_back([&, k] {
                for (int i = 0; i < 200; ++i) {
                    int const        mode = (i + k) % 3;  // 0: plain, 1: pre-canceled, 2: racing cancel
                    std::stop_source src;
                    if (mode == 1) src.request_stop();
                    std::thread racer;
                    if (mode == 2) {
                        racer = std::thread([&src, i] {
                            if (i % 2 == 0) std::this_thread::yield();
                            src.request_stop();
                        });
                    }
                    int const  v = k * 1000 + i;
                    auto const r = block_on(run_doubling(&stress, src.get_token(), v, &bodies_finished));
                    if (racer.joinable()) racer.join();
                    if (r.status == offload_status::completed) {
                        completed.fetch_add(1);
                        if (!r.value || *r.value != v * 2) wrong.fetch_add(1);
                    } else if (r.status == offload_status::canceled) {
                        canceled.fetch_add(1);
                        if (mode == 0) wrong.fetch_add(1);
                    } else {
                        wrong.fetch_add(1);
                    }
                    if (mode == 1 && r.status != offload_status::canceled) wrong.fetch_add(1);
                }
            });
        }
        for (auto& th : threads) th.join();
        stress.shutdown();
        int const total = completed.load() + canceled.load();
        check(total == 1600 && wrong.load() == 0,
              "O11: 1600 concurrent offloads, each resumed once with a consistent outcome -- " +
                  std::to_string(total) + " resumed, " + std::to_string(wrong.load()) + " wrong");
        check(static_cast<std::uint64_t>(bodies_finished.load()) ==
                  static_cast<std::uint64_t>(completed.load()) + stress.discarded_results(),
              "O11: bodies finished == completed + discarded (" + std::to_string(bodies_finished.load()) + " == " +
                  std::to_string(completed.load()) + " + " + std::to_string(stress.discarded_results()) + ")");
        check(stress.homeless_refusals() == 0, "O11: no homeless wake-ups under block_on");
    }

    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
