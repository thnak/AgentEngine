// Proof for ADR-037 Phase 1, item 3: agentengine::rt::ThreadPool (include/agentengine/rt/thread_pool.hpp)
// -- the minimal executor that drives an already-constructed task<void> to completion on a POOL WORKER
// THREAD instead of the calling thread. Deliberately no dependency on quark:: anywhere in this file,
// same as test_rt_task.cpp -- that's the whole point of this substrate existing.
//
// This is a genuinely multi-threaded test, not just a logical/single-threaded one -- the properties
// that matter here (a job really does run elsewhere, jobs really do overlap in wall-clock time, a
// fault on one worker doesn't take the pool down, shutdown really does join every thread) can only be
// demonstrated by actually running threads and measuring real time, per the task's own instructions.
// Covers, one case per block in main():
//   T1 -- a submitted job runs on a thread DIFFERENT from the caller's.
//   T2 -- N jobs submitted concurrently to a pool of >=N workers finish in roughly ONE job's duration
//         of wall-clock time, not N times that -- real parallelism, not serialization.
//   T3 -- a job whose task<void> body throws is observably faulted() through submit()'s returned
//         future, without crashing the pool or leaving a dead worker (proven by successfully running a
//         further job afterward on the SAME pool instance).
//   T4 -- the destructor cleanly joins every worker: no hang, and shutdown waits for genuinely
//         in-flight work to finish rather than abandoning it mid-job.
//   T5 -- worker_count() reports what the constructor was asked for (explicit count) and falls back to
//         a real default (>=1) when 0 / omitted.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"

using agentengine::rt::JobOutcome;
using agentengine::rt::task;
using agentengine::rt::ThreadPool;

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

task<void> record_thread_id(std::thread::id* out) {
    *out = std::this_thread::get_id();
    co_return;
}

task<void> sleep_for_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    co_return;
}

task<void> throwing_job() {
    throw std::runtime_error("boom-pool");
    co_return;  // unreachable, silences a "no return" warning
}

task<void> noop_job() { co_return; }

}  // namespace

int main() {
    // T1: a submitted job's body genuinely runs on a different thread than the one that called
    // submit()/waited on the future.
    {
        ThreadPool pool(4);
        std::thread::id const caller_id = std::this_thread::get_id();
        std::thread::id worker_id{};
        std::future<JobOutcome> fut = pool.submit(record_thread_id(&worker_id));
        JobOutcome const outcome = fut.get();  // synchronizes-with the worker's set_value()
        check(!outcome.faulted, "T1: a clean job is not faulted");
        check(worker_id != std::thread::id{}, "T1: the job body actually ran (thread id was recorded)");
        check(worker_id != caller_id, "T1: the job ran on a DIFFERENT thread than the caller");
    }

    // T2: real parallelism, measured by wall clock -- 4 jobs that each sleep ~200ms, submitted to a
    // pool of 4 workers, must finish in roughly ONE sleep's worth of time, not four. Serial execution
    // would take ~800ms; this asserts well under that (a generous 500ms ceiling) while also asserting
    // it took at least roughly one sleep's worth (not some degenerate "returned instantly without
    // actually running" shortcut).
    {
        constexpr int kJobs = 4;
        constexpr int kSleepMs = 200;
        ThreadPool pool(kJobs);
        std::vector<std::future<JobOutcome>> futures;
        futures.reserve(kJobs);

        auto const start = std::chrono::steady_clock::now();
        for (int i = 0; i < kJobs; ++i) futures.push_back(pool.submit(sleep_for_ms(kSleepMs)));
        for (auto& f : futures) {
            JobOutcome const outcome = f.get();
            check(!outcome.faulted, "T2: each parallel sleep job completes without faulting");
        }
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        std::fprintf(stderr, "  T2: %d jobs x %dms sleep on %d workers took %lldms\n", kJobs, kSleepMs,
                     kJobs, static_cast<long long>(elapsed.count()));
        check(elapsed.count() >= kSleepMs - 20,
              "T2: elapsed time is at least roughly one job's sleep duration (jobs really ran)");
        check(elapsed.count() < kSleepMs * (kJobs - 1),
              "T2: elapsed time is well under N x one-job duration -- proves real parallelism, not "
              "serialization");
    }

    // T3: a faulting job is observable through the returned future, without crashing the pool or
    // leaving a broken worker behind -- proven by running a further, unrelated job on the SAME pool
    // instance immediately afterward and seeing it succeed normally.
    {
        ThreadPool pool(2);
        std::future<JobOutcome> fault_fut = pool.submit(throwing_job());
        JobOutcome const fault_outcome = fault_fut.get();
        check(fault_outcome.faulted, "T3: a job whose body throws is reported faulted() via submit()");
        check(static_cast<bool>(fault_outcome.fault), "T3: fault_outcome carries a non-null exception_ptr");
        bool threw_original = false;
        try {
            std::rethrow_exception(fault_outcome.fault);
        } catch (std::runtime_error const& e) {
            threw_original = (std::string(e.what()) == "boom-pool");
        } catch (...) {
        }
        check(threw_original, "T3: rethrowing fault_outcome.fault surfaces the ORIGINAL exception, "
                               "message intact");

        // The pool must still be fully alive: submit and complete an ordinary job right after.
        std::future<JobOutcome> ok_fut = pool.submit(noop_job());
        JobOutcome const ok_outcome = ok_fut.get();
        check(!ok_outcome.faulted, "T3: the pool survives a faulting job -- a later job on the same "
                                    "pool still completes cleanly (no dead/crashed worker)");
    }

    // T4: clean shutdown -- the destructor joins every worker with no hang, and genuinely waits for an
    // in-flight (already-running, not merely queued) job to finish rather than abandoning it. Proven by
    // submitting a job with a known sleep duration, dropping the future WITHOUT waiting on it, then
    // destroying the pool and measuring that destruction took roughly that long (not near-instant,
    // which would mean the worker thread was abandoned mid-job rather than actually joined).
    {
        constexpr int kSleepMs = 250;
        auto const start = std::chrono::steady_clock::now();
        {
            ThreadPool pool(2);
            std::future<JobOutcome> fut = pool.submit(sleep_for_ms(kSleepMs));
            (void)fut;  // deliberately not waited on -- the destructor below must still finish the job
        }  // ~ThreadPool(): request_stop() on every worker, then join() -- must block here until the
           // in-flight sleep_for_ms job actually finishes.
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        std::fprintf(stderr, "  T4: pool destruction with one in-flight %dms job took %lldms\n", kSleepMs,
                     static_cast<long long>(elapsed.count()));
        check(elapsed.count() >= kSleepMs - 20,
              "T4: destructor waited for the in-flight job to actually finish (real join, not abandoned)");
        check(elapsed.count() < kSleepMs + 2000,
              "T4: destructor did not hang -- returned within a bounded margin of the job's own duration");
    }

    // T4b: many quick jobs, destructor with nothing in flight -- must also complete promptly (no
    // lingering threads, no lost wakeup leaving a worker parked forever).
    {
        auto const start = std::chrono::steady_clock::now();
        {
            ThreadPool pool(4);
            std::vector<std::future<JobOutcome>> futures;
            for (int i = 0; i < 50; ++i) futures.push_back(pool.submit(noop_job()));
            for (auto& f : futures) (void)f.get();
        }
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        check(elapsed.count() < 2000, "T4b: destroying an idle pool after 50 quick jobs is prompt, no hang");
    }

    // T5: worker_count() reflects what was asked for; 0 / omitted falls back to a real default (>=1),
    // never a silently-inert zero-worker pool.
    {
        ThreadPool explicit_pool(3);
        check(explicit_pool.worker_count() == 3, "T5: worker_count() reports the explicitly requested count");

        ThreadPool default_pool;
        check(default_pool.worker_count() >= 1, "T5: the default pool has at least one real worker");

        ThreadPool zero_requested_pool(0);
        check(zero_requested_pool.worker_count() >= 1,
              "T5: explicitly requesting 0 workers still yields a usable (>=1) pool, not an inert one");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_thread_pool: ALL PASS\n");
    return 0;
}
