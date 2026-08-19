#pragma once
// ADR-037 Phase 1, item 3: `agentengine::rt::ThreadPool`, the "minimal executor for driving I/O
// continuations" the ADR calls for -- a small, fixed-size worker-thread pool that drives an already-
// constructed `agentengine::rt::task<void>` to completion on a POOL WORKER THREAD instead of the
// calling thread, so the caller isn't blocked for the round-trip.
//
// SCOPE, DELIBERATELY NARROW (see the ADR's own §5 red-team finding: "the minimal executor is new,
// unproven infrastructure exactly where AgentEngine currently gets a mature, independently-red-teamed
// scheduler for free... the single largest NEW risk this ADR introduces"): this type does NOT attempt
// general coroutine-parking-across-threads (a coroutine suspending mid-body on an external async event
// and being resumed later by a DIFFERENT thread than the one that started it). That is a genuinely
// separate, harder problem -- a naive version of it has already been found, in this same project, to
// have real thread-affinity/reentrancy hazards, and needs its own dedicated design -> red-team -> prove
// pass before anything depends on it. What THIS type does is much smaller and self-contained: take a
// task<void> whose body may internally `co_await` other NESTED task<T>s (those all resolve
// synchronously via symmetric transfer -- see task.hpp's own top comment -- no external suspension is
// ever involved) and run it to completion on one worker thread, uninterrupted, in exactly ONE resume()
// call. A worker never parks a partially-run task and hands it to another worker mid-flight; each job
// lives on exactly one thread for its entire lifetime.
//
// **THIS SCOPE IS NOW RUNTIME-ENFORCED, NOT MERELY DOCUMENTED (fixed 2026-08-19, found via red-team
// against decisions/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md's own design review):**
// `run_job()` originally drove a job with `while (!done()) resume();` -- a job that genuinely suspends
// on something other than a nested task<T> (e.g. `AsyncMutex::lock()`/`channel<T>`'s async surface
// under REAL contention -- `rt/async_mutex.hpp`, `rt/channel.hpp`) parks its coroutine handle for a
// LATER, possibly DIFFERENT thread's `unlock()`/`push()` call to resume directly. The naive loop would
// call `resume()` on that SAME handle again itself -- a genuine cross-thread double-resume race,
// undefined behavior per the C++20 coroutine spec, not merely a logic bug. This was never actually
// triggered in this codebase (every real usage today -- `tools/cli_chat.cpp`'s `AgentSession` driving,
// every job in `tests/test_rt_thread_pool.cpp` -- happens to complete in exactly one `resume()`), but
// nothing PREVENTED a future composition from hitting it silently -- `tests/test_rt_async_mutex.cpp`'s
// own top comment already names this exact hazard (discovered independently while drafting THAT file's
// cross-thread contention test, which deliberately does NOT use `ThreadPool` for that reason). Fixed:
// `run_job()` now resumes exactly once, then FAILS the job (`JobOutcome::faulted = true`, a diagnosable
// `std::runtime_error`) if it isn't `done()` yet, instead of looping. Abandoning (not further resuming)
// a job in that state is safe, not a new hazard: destroying its `task<void>` runs the coroutine frame's
// live locals' destructors, including whatever awaiter it is parked in -- `AsyncMutex::LockAwaiter`/
// `channel<T>`'s `next_awaiter` already implement safe self-removal from their primitive's own waiter
// queue on exactly this "destroyed while parked" path (ADR-017's "drop the handle = cancel" house
// idiom), so no later `unlock()`/`push()` can ever resume an already-destroyed frame. A caller that
// needs GENUINE cross-thread `AsyncMutex`/`channel<T>` contention should follow
// `tests/test_rt_async_mutex.cpp`'s own M3 pattern (real `std::thread`s, resume() once per job, then
// let the primitive's own internal hand-off carry it) instead of `ThreadPool`.
//
// WHY std::mutex + std::condition_variable_any, NOT a lock-free queue: this project's own ADR-037 §5
// explicitly wants conservative correctness here over throughput -- this is a correctness-first
// utility (the thing driving async model-call continuations), not a hot-path scheduler. A blocking
// queue with a handful of worker threads is easy to reason about and easy to get RIGHT; a lock-free
// MPMC queue is exactly the kind of cleverness the ADR's own red-team finding warns against investing
// in before the simple version has even been proven.
//
// WHY std::jthread + std::stop_token, NOT std::thread + a hand-rolled atomic<bool> flag: this mirrors
// the house convention already established at tools/cli_chat.cpp's own drain-thread (see that file's
// comment on its `std::jthread drain(...)`): "the destructor requests stop and joins automatically on
// every exit path (including an exception...), so the drain thread never outlives this scope." Same
// guarantee here, for N threads instead of one -- RAII-guaranteed request_stop()+join(), no detached
// threads, no hand-written join-in-destructor code that a future edit could accidentally skip on one
// exit path. `condition_variable_any::wait(lock, stop_token, predicate)` (C++20) is what lets a worker
// block efficiently AND wake promptly on either "new work arrived" (notify_one/notify_all) or "asked to
// stop" (request_stop), without polling.
//
// SHUTDOWN SEMANTICS: the destructor requests stop on every worker, then relies on each worker's own
// drain-to-empty behavior (see worker_loop()'s comment) to finish whatever is ALREADY QUEUED before
// exiting -- a job that was submitted but never picked up is still run, never silently dropped. A job
// already IN FLIGHT on a worker is never preempted (this type has no cancellation mechanism for a
// running job's body -- task<void> bodies here are plain synchronous code between suspension points,
// same as any coroutine, and are expected to run to their own completion); the destructor therefore
// blocks until every in-flight AND every already-queued job has finished. This is the same "conservative,
// no job silently abandoned" posture the ADR asks for -- a caller that wants bounded-time shutdown must
// arrange that itself (e.g. don't submit unboundedly long jobs it isn't prepared to wait out).
//
// FAULT SAFETY: a submitted task<void>'s body throwing is caught INSIDE task<void>'s own
// unhandled_exception() (task.hpp), which does not rethrow -- so resume() itself never throws because
// of a body exception; `faulted()`/`fault_ptr()` observe it afterward instead. Verified against
// task.hpp's actual promise_type (not assumed): `unhandled_exception() noexcept { fault_ =
// std::current_exception(); }`, called by the compiler-generated coroutine machinery, no rethrow inside
// it. The try/catch in run_job() below is still kept as defense-in-depth against a future change to
// task<T>'s contract (or misuse this type can't statically rule out) -- a worker thread must NEVER die
// from an unhandled exception; if the belt-and-suspenders catch ever actually fires, that is itself a
// signal task<void>'s contract changed underneath this file.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// hardware_concurrency() is allowed by the standard to return 0 (genuinely unknown) -- clamp to 1 so
// the default is always at least a usable single-worker pool, never a silently-inert zero-worker one.
[[nodiscard]] inline std::size_t default_worker_count() noexcept {
    unsigned const n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<std::size_t>(n);
}

// What submit() hands back: whether the job's task<void> body completed WITHOUT faulting, and, if it
// did fault, the original exception_ptr (rethrow it yourself via std::rethrow_exception to see the
// real exception -- same idiom task<void>::fault_ptr() already uses). Deliberately NOT the task<void>
// itself -- ownership of the task moved into the pool when it was submitted, so a caller has no raw
// handle to race against a worker thread still resuming it; JobOutcome is a plain, thread-safely-
// delivered value (via std::promise/std::future, which itself guarantees the happens-before edge
// between the worker writing it and the caller reading it after wait()/get()).
// ae-naming-lint: allow JobOutcome — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct JobOutcome {
    bool faulted = false;
    std::exception_ptr fault{};
};

// A fixed-size pool of N worker threads draining one shared FIFO queue of task<void> jobs. Each
// submit() call moves ownership of a fully-constructed (but not yet started -- task<void> is lazy,
// per task.hpp) task<void> into the pool and returns a std::future<JobOutcome> the caller can wait on
// without blocking the calling thread for the job's own duration.
//
// CONCURRENCY POSTURE: many threads may call submit() concurrently (only the internal mutex is shared
// mutable state on that path). The pool itself is NOT copyable or movable -- worker threads capture
// `this` for their entire lifetime, so relocating the pool object while workers are running would
// invalidate every one of those captures; a pool is meant to be constructed once and held by reference
// or through a stable owner (e.g. a unique_ptr), matching how a fixed thread pool is used everywhere
// else in C++ (std::jthread itself is the same non-movable-while-running shape, one level down).
// ae-naming-lint: allow ThreadPool — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ThreadPool {
public:
    // `worker_count == 0` (including the default) means "use default_worker_count()" -- a caller who
    // explicitly asks for zero workers would get a pool that queues forever and never runs anything
    // (every submit()'d future would simply never become ready), which is never a useful configuration
    // to actually construct, so it is treated the same as "didn't specify" rather than honored literally.
    explicit ThreadPool(std::size_t worker_count = 0) {
        std::size_t const n = worker_count == 0 ? default_worker_count() : worker_count;
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this](std::stop_token stop) { worker_loop(std::move(stop)); });
        }
    }

    ThreadPool(ThreadPool const&) = delete;
    ThreadPool& operator=(ThreadPool const&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Signals every worker to stop accepting NEW empty-queue waits (see worker_loop()'s drain-to-empty
    // behavior), wakes anyone currently parked in cv_.wait(), then lets `workers_`'s own destructor
    // join every std::jthread in turn. Calling request_stop() on every worker BEFORE any of them are
    // joined (rather than relying solely on each jthread's own destructor, which would only request-
    // stop worker i once workers 0..i-1 have already been fully joined) means every worker observes
    // the stop request as close to simultaneously as possible, instead of being woken one at a time in
    // whatever order the vector happens to destroy its elements -- pure latency, not correctness (join()
    // is unconditionally safe regardless of ordering), but it keeps shutdown from being needlessly
    // serialized across N workers.
    ~ThreadPool() {
        for (std::jthread& w : workers_) w.request_stop();
        {
            // notify_all() must happen while holding (or at least after acquiring-then-releasing) the
            // same mutex the waiters check their predicate under, so a worker that is between checking
            // the queue and entering the wait can't miss this wakeup (the standard lost-wakeup hazard).
            std::lock_guard<std::mutex> lock(mutex_);
        }
        cv_.notify_all();
        // workers_'s destructor runs here: each std::jthread joins (request_stop() again is a no-op,
        // already requested above). No detached threads, no leaks, matches the RAII-guaranteed
        // request_stop()+join() convention this codebase already uses at tools/cli_chat.cpp's drain
        // thread.
    }

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    // Moves `job` into the pool's queue and returns a future the caller waits on for the outcome.
    // `job` must be a valid, not-yet-started task<void> (the normal state right after calling a
    // task<void>-returning coroutine function, per task.hpp's lazy-start contract) -- ownership
    // transfers here; the caller must not touch its own (now moved-from) `job` afterward.
    [[nodiscard]] std::future<JobOutcome> submit(task<void> job) {
        std::promise<JobOutcome> promise;
        std::future<JobOutcome> future = promise.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(QueuedJob{std::move(job), std::move(promise)});
        }
        cv_.notify_one();  // wake exactly one idle worker -- there is exactly one new item
        return future;
    }

private:
    struct QueuedJob {
        task<void> job;
        std::promise<JobOutcome> promise;
    };

    // One worker's whole life: pull the next queued job (blocking, cooperatively wakeable by either a
    // new submission or a stop request) and drive it to completion, forever, until told to stop AND
    // the queue is empty.
    //
    // DRAIN-TO-EMPTY ON STOP: `condition_variable_any::wait(lock, stop_token, predicate)`'s documented
    // contract evaluates `predicate()` one more time even after stop_requested() becomes true, and only
    // returns false (giving up) if the predicate is STILL false at that point. So a worker that wakes
    // up because request_stop() was called, but the queue is genuinely non-empty at that instant, still
    // picks up and runs that last item rather than abandoning it -- a job that made it into the queue
    // is guaranteed to run, never silently dropped by a race with shutdown. Only once wait() returns
    // false (stop requested AND the queue was empty at that check) does this worker actually exit its
    // loop and return, letting std::jthread join it.
    void worker_loop(std::stop_token stop) {
        for (;;) {
            QueuedJob item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                bool const got_work = cv_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (!got_work) return;  // stop requested AND nothing left queued -- this worker exits
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            // Deliberately OUTSIDE the lock: running a job must never hold mutex_, or every other
            // worker (and every submit() caller) would serialize behind whichever worker happens to be
            // mid-job -- the entire point of having N workers.
            run_job(std::move(item));
        }
    }

    static void run_job(QueuedJob item) {
        JobOutcome outcome;
        try {
            // Exactly ONE resume() first, not a `while (!done())` loop -- see this file's top comment
            // ("SCOPE, DELIBERATELY NARROW") for why: a job whose body only ever nests further
            // task<T>/task<void> awaits (real C++20 symmetric transfer, no external suspension) always
            // reaches done() in exactly one resume() call, regardless of nesting depth -- there is no
            // legitimate scenario where a SECOND resume() from THIS loop would ever be correct.
            if (!item.job.done()) item.job.resume();
            if (!item.job.done()) {
                // The job suspended on something other than a nested task<T>/task<void> -- e.g.
                // AsyncMutex::lock() or channel<T>'s async surface genuinely contended (async_mutex.hpp,
                // channel.hpp), which resumes the SAME coroutine handle later from WHATEVER thread calls
                // unlock()/push(). Looping resume() here would race that thread for the same handle --
                // undefined behavior, not merely wrong output (found via red-team against ADR-064,
                // 2026-08-19; tests/test_rt_async_mutex.cpp's own top comment already names this exact
                // hazard, discovered independently while drafting that file's M3). FAIL LOUD instead:
                // this job is outside ThreadPool's documented scope, full stop. Destroying `item.job`
                // below (via its own destructor once this function returns) is SAFE, not a new hazard --
                // it exercises the SAME "abandon a genuinely-parked awaiter" path AsyncMutex's
                // `LockAwaiter`/channel<T>'s `next_awaiter` already designed and tested for cancellation
                // (ADR-017's "drop the handle = cancel" house idiom): `coroutine_handle::destroy()` runs
                // the frame's live locals' destructors, including the parked awaiter, which removes its
                // own registration from the primitive's internal waiter queue under that primitive's own
                // lock -- so no later unlock()/push() can ever resume this now-destroyed frame.
                outcome.faulted = true;
                outcome.fault = std::make_exception_ptr(std::runtime_error(
                    "ThreadPool::run_job(): job did not reach done() within one resume() call -- it "
                    "suspended on something other than a nested task<T>/task<void> (e.g. a genuinely "
                    "contended AsyncMutex::lock() or channel<T> await), which is outside this type's "
                    "documented scope (see this file's own top comment). The job has been abandoned "
                    "(not driven further) rather than risking a cross-thread double-resume."));
            } else {
                outcome.faulted = item.job.faulted();
                outcome.fault = item.job.fault_ptr();
            }
        } catch (...) {
            // Defense-in-depth only -- see this file's top comment on why resume() is not expected to
            // ever throw here given task<void>'s actual promise_type. If this branch is ever reached,
            // a worker thread still must not die from it (an uncaught exception on a std::jthread's
            // invoked function calls std::terminate, taking the WHOLE POOL down, not just one job).
            outcome.faulted = true;
            outcome.fault = std::current_exception();
        }
        // set_value() on a promise synchronizes-with the corresponding future's successful get()/wait()
        // return (standard library guarantee) -- so anything this worker wrote (including everything
        // the job's own body did) is visible to whoever reads the future afterward, no extra
        // synchronization needed on the caller's side.
        item.promise.set_value(std::move(outcome));
    }

    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<QueuedJob> queue_;
    std::vector<std::jthread> workers_;  // declared LAST: workers must be joined (destroyed) before
                                          // mutex_/cv_/queue_ are torn down, since worker_loop()
                                          // references them until it actually returns. Members are
                                          // destroyed in REVERSE declaration order, so workers_ being
                                          // last means it is destroyed FIRST.
};

}  // namespace agentengine::rt
