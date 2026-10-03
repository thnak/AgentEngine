#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.6 ("Truly blocking work:
// `rt::offload`" and "Offload lifetime rules") with the threading rules of §4.1/§4.2/§4.3 and the lifetime
// rules of §4.5 -- `rt::OffloadPool` and `co_await rt::offload(pool, stop, fn, args...)`: run a blocking or
// CPU-heavy callable on a small, fixed pool of worker threads and resume the awaiting coroutine on its HOME
// with the result.
//
// What goes here (§4.6): `getaddrinfo` where no async resolver exists, file APIs with no async form, foreign
// C libraries that block, and CPU-heavy pure work that would otherwise starve a lane. Nothing that touches
// session state except through its own return value (§4.1).
//
// THE RULES, each mirroring rt/sleep.hpp (the reference awaitable) except where noted:
//   - Where it resumes: the waiter is posted to the home it parked from (ADR-175 `block_on` home, or an
//     ADR-219 host `Resumer`) through `detail::wake` -- never resumed on the offload worker, and never on the
//     thread that requested the stop. A waiter with no home (resumed raw, outside both) is REFUSED: never
//     resumed at all, and counted by `OffloadPool::homeless_refusals()` (§4.2).
//   - Inputs are taken BY VALUE (§4.6, red-team G3): the callable and every argument are decay-copied or
//     moved into the job, because the caller may resume `canceled` and unwind its frame while the body is
//     still running. `std::ref`/`std::cref` arguments are rejected at compile time. A lambda that captures
//     by reference cannot be detected by the language and is the CALLER'S BUG -- capture by value or move.
//     Returning a reference is rejected for the same reason.
//   - Cancellation (§4.6 residual, as MAF states it for `asyncio.to_thread`):
//       * stop requested before the body starts -> the body NEVER runs; the caller resumes `canceled`;
//       * stop requested while the body runs -> the CALLER resumes `canceled` promptly; the body is not
//         interrupted, runs to its end on the worker, and its result (or exception) is discarded and counted
//         (`discarded_results()`). Its side effect may still happen: a tool whose effect class is
//         `at_most_once` must not perform its effect inside an offload body (it uses reactor I/O instead).
//     Unlike a reactor operation there is no backend completion to wait for -- the caller's frame is never
//     referenced by the body (inputs by value, result in the shared job) -- so the cancel itself posts the
//     continuation. Still exactly once: one atomic state per job decides who posts (below).
//   - Exactly-once resume: the job's state moves arming -> queued -> running -> done, or to a canceled /
//     abandoned state, by compare-and-swap; only the transition that wins delivers the continuation. A
//     coroutine frame destroyed while it waits ABANDONS its job: a body not yet started never runs, a running
//     body's completion posts nothing, and a continuation already queued at a host Resumer is claimed so it
//     never runs (ADR-219 ticket, `detail::abandon_woken`).
//   - Ordering inside await_suspend: everything that can throw happens before anything refers to the handle;
//     the stop_callback is registered BEFORE the job is published to the pool; a stop that fires during that
//     registration only marks the job (it does not post -- the frame is still inside await_suspend) and
//     await_suspend then declines to suspend. Once the job is queued nothing in the frame is touched: a
//     cancel or a fast body may already have posted the continuation to a host thread.
//   - An exception thrown by the body never escapes on the worker thread: it is delivered to the caller as
//     `offload_status::faulted` with the exception_ptr.
//   - `block_on()` inside a body is refused (§4.3): every worker carries a `ScopedBlockOnRefusal` marker, so
//     `block_on` throws `block_on_refused` (`rt.block_on_on_offload_worker`), which reaches the caller as a
//     fault like any other exception.
//
// LIFETIME (§4.5 rules 1 and 4, §4.6 "Offload lifetime rules"):
//   - Workers are `std::jthread`s owned by the pool and JOINED in its destructor -- never detached. A
//     canceled-but-still-running body is not a detached task: the pool owns it, counts it (`running()`) and
//     joins it at shutdown.
//   - Shutdown (`shutdown()`, or the destructor): stop accepting work; every job still QUEUED resumes its
//     caller `canceled` and never runs; then wait for the RUNNING bodies. With `Options::shutdown_deadline`
//     set, a wait that outlasts the deadline is REPORTED -- `shutdown_overruns()` and `Options::on_overrun`
//     -- and the join then continues: the deadline bounds when the host learns of the overrun, never
//     whether a thread is left behind.
//   - The pool must outlive every `co_await rt::offload(pool, ...)` on it (the usual rule for an executor);
//     a caller resumed `canceled` may then destroy its frame freely.
//
// TWO POOLS, ONE TYPE (§4.6 "DNS gets its own bounded pool", §9 D5): the host/runtime owns
//     rt::OffloadPool general{{.workers = 2}};   // file I/O without async support, foreign calls, CPU work
//     rt::OffloadPool dns{{.workers = 2}};       // blocking getaddrinfo, and nothing else
// so a burst of slow lookups (glibc `getaddrinfo` can take ~30 s) cannot stall every session's file I/O.
// Both default to 2 workers and are deliberately small and fixed: they hold threads that block, and must not
// grow with the core count (CONVENTIONS: never `hardware_concurrency()` threads).
//
// No `failure_class` is involved yet: the result is a plain `offload_result<T>`. (`failure_class::canceled`,
// D6, arrives with the seam conversion.) Audit attribution of a discarded completion (I4, §4.6) belongs to
// the caller that bound the capability; the pool only counts it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow offload_status — ADR-237 §4.6: new runtime vocabulary, 027 §4 row added when the ADR is Judged
enum class offload_status : std::uint8_t {
    completed,  // the body returned; `value` holds its result (absent for void)
    canceled,   // the stop token was requested (or the pool shut down) before the body's result was delivered
    faulted,    // the body threw; `fault` holds the exception
};

// What `co_await rt::offload(...)` yields. `value` is engaged iff `status == completed`; `fault` is set iff
// `status == faulted`.
template <class T>  // ae-naming-lint: allow offload_result — ADR-237 §4.6: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct offload_result {
    offload_status     status = offload_status::canceled;
    std::optional<T>   value;
    std::exception_ptr fault;

    [[nodiscard]] bool completed() const noexcept { return status == offload_status::completed; }
    // Rethrows the body's exception if it faulted; otherwise does nothing.
    void rethrow_if_faulted() const {
        if (fault) std::rethrow_exception(fault);
    }
};

// ae-naming-lint: allow offload_result — ADR-237 §4.6: new runtime vocabulary, 027 §4 row added when the ADR is Judged
template <>
struct offload_result<void> {
    offload_status     status = offload_status::canceled;
    std::exception_ptr fault;

    [[nodiscard]] bool completed() const noexcept { return status == offload_status::completed; }
    void rethrow_if_faulted() const {
        if (fault) std::rethrow_exception(fault);
    }
};

// ae-naming-lint: allow OffloadPool — ADR-237 §4.6 (declared below)
class OffloadPool;

namespace offload_detail {

inline constexpr char const* kRefusalCode = "rt.block_on_on_offload_worker";

// One job's state, shared by its awaiter and the pool (refcounted: whichever releases last frees it). Every
// transition is a compare-and-swap; the ones marked (*) deliver the continuation, and exactly one of them can
// ever win for a given job.
//   kArming          -> kQueued (await_suspend publishes)      | kCanceledArming (stop during registration)
//   kQueued          -> kRunning (a worker starts the body)    | kCanceledQueued (*) | kAbandonedQueued
//   kRunning         -> kDone (*) (the body finished)          | kCanceledRunning (*) | kAbandonedRunning
class JobBase {
public:
    static constexpr int kArming            = 0;
    static constexpr int kQueued            = 1;
    static constexpr int kRunning           = 2;
    static constexpr int kDone              = 3;
    static constexpr int kCanceledArming    = 4;
    static constexpr int kCanceledQueued    = 5;
    static constexpr int kCanceledRunning   = 6;
    static constexpr int kAbandonedQueued   = 7;
    static constexpr int kAbandonedRunning  = 8;

    virtual ~JobBase() = default;
    JobBase(JobBase const&)            = delete;
    JobBase& operator=(JobBase const&) = delete;

    // Awaiter, in await_suspend, before anything refers to the handle.
    void set_parked(detail::ParkedResumer parked, OffloadPool* pool) noexcept {
        ticket_ = parked.ticket;
        parked_ = std::move(parked);
        pool_   = pool;
    }

    // Awaiter, after registering the stop_callback: kArming -> kQueued. False if a stop fired meanwhile.
    [[nodiscard]] bool try_arm() noexcept {
        int expected = kArming;
        return state_.compare_exchange_strong(expected, kQueued, std::memory_order_acq_rel);
    }

    // The stop_callback (any thread), or pool shutdown for a queued job. Posts the continuation if it wins.
    void cancel() noexcept {
        int s = state_.load(std::memory_order_acquire);
        for (;;) {
            int to = 0;
            switch (s) {
                case kArming: to = kCanceledArming; break;
                case kQueued: to = kCanceledQueued; break;
                case kRunning: to = kCanceledRunning; break;
                default: return;  // done, already canceled, or abandoned
            }
            if (state_.compare_exchange_weak(s, to, std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (to != kCanceledArming) deliver();  // kCanceledArming: still inside await_suspend, which sees it
                return;
            }
        }
    }

    // Awaiter, when the pool refused the job (shut down): kQueued -> kCanceledQueued WITHOUT posting. True if
    // this call won (the awaiter resumes at once); false if a racing cancel() already posted it.
    [[nodiscard]] bool cancel_unsubmitted() noexcept {
        int expected = kQueued;
        return state_.compare_exchange_strong(expected, kCanceledQueued, std::memory_order_acq_rel);
    }

    // Awaiter destructor while its frame is destroyed mid-wait. The body (if queued) never runs; a running
    // body's completion posts nothing; a continuation already handed to a host Resumer is claimed.
    void abandon() noexcept {
        int s = state_.load(std::memory_order_acquire);
        for (;;) {
            int to = 0;
            if (s == kQueued) {
                to = kAbandonedQueued;
            } else if (s == kRunning) {
                to = kAbandonedRunning;
            } else {
                (void)detail::abandon_woken(ticket_);  // already delivered (or never armed)
                return;
            }
            if (state_.compare_exchange_weak(s, to, std::memory_order_acq_rel, std::memory_order_acquire)) return;
        }
    }

    // Worker thread. Runs the body unless the job was canceled or abandoned first; never throws.
    void run() noexcept;

    [[nodiscard]] int state() const noexcept { return state_.load(std::memory_order_acquire); }

protected:
    JobBase() noexcept = default;
    // Calls the body and stores its result or exception. Worker thread only, at most once.
    virtual void invoke_body() noexcept = 0;

private:
    friend class rt::OffloadPool;

    void deliver() noexcept;  // defined after OffloadPool

    std::atomic<int>                       state_{kArming};
    detail::ParkedResumer                  parked_;
    std::shared_ptr<detail::ResumerTicket> ticket_;  // kept for abandon(); parked_ is moved out on delivery
    OffloadPool*                           pool_ = nullptr;

    // Intrusive queue link (pool mutex only): no allocation when a job is submitted, so submitting cannot
    // throw after the job is published. `self_` is the queue's own reference, moved out when popped.
    JobBase*                 next_ = nullptr;
    std::shared_ptr<JobBase> self_;
};

template <class R>
class ResultJob : public JobBase {
public:
    using stored_type = std::conditional_t<std::is_void_v<R>, std::monostate, R>;

    // Awaiter, after resuming. kDone is read with acquire, which orders the body's writes before these reads.
    [[nodiscard]] offload_result<R> take_result() {
        offload_result<R> out;
        if (state() != kDone) {
            out.status = offload_status::canceled;
            return out;
        }
        if (fault_) {
            out.status = offload_status::faulted;
            out.fault  = std::move(fault_);
            return out;
        }
        out.status = offload_status::completed;
        if constexpr (!std::is_void_v<R>) out.value.emplace(std::move(*value_));
        return out;
    }

protected:
    std::optional<stored_type> value_;
    std::exception_ptr         fault_;
};

template <class R, class F, class... Args>
class Job final : public ResultJob<R> {
public:
    template <class F2, class... A2>
    explicit Job(F2&& f, A2&&... args) : fn_(std::forward<F2>(f)), args_(std::forward<A2>(args)...) {}

private:
    void invoke_body() noexcept override {
        try {
            if constexpr (std::is_void_v<R>) {
                std::apply(std::move(fn_), std::move(args_));
                this->value_.emplace();
            } else {
                this->value_.emplace(std::apply(std::move(fn_), std::move(args_)));
            }
        } catch (...) {
            this->fault_ = std::current_exception();
        }
    }

    F                   fn_;
    std::tuple<Args...> args_;
};

template <class T>
inline constexpr bool is_reference_wrapper_v = false;
template <class T>
inline constexpr bool is_reference_wrapper_v<std::reference_wrapper<T>> = true;

}  // namespace offload_detail

// A small, fixed pool of worker threads for `rt::offload` (§4.1 "Offload worker", §4.6). See the file comment
// for the rules; the host/runtime owns the instances (a general pool and a separate DNS pool).
// ae-naming-lint: allow OffloadPool — ADR-237 §4.6: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class OffloadPool {
public:
    struct Options {
        std::size_t                              workers = 2;  // §9 D5: small, fixed, configurable
        std::optional<std::chrono::milliseconds> shutdown_deadline{};  // unset: wait without reporting
        // Called (on the shutting-down thread) when running bodies outlast `shutdown_deadline`, with how many
        // are still running. Must not throw. The join continues after it returns.
        std::function<void(std::size_t still_running)> on_overrun{};
    };

    OffloadPool() : OffloadPool(Options{}) {}
    explicit OffloadPool(Options options) : options_(std::move(options)) {
        if (options_.workers == 0) throw std::invalid_argument("rt::OffloadPool: workers must be at least 1");
        workers_.reserve(options_.workers);
        try {
            for (std::size_t i = 0; i < options_.workers; ++i) workers_.emplace_back([this] { worker_loop(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~OffloadPool() { shutdown(); }

    OffloadPool(OffloadPool const&)            = delete;
    OffloadPool& operator=(OffloadPool const&) = delete;

    // Stops accepting work, resumes every queued job's caller `canceled` (its body never runs), waits for the
    // running bodies (reporting an overrun of `shutdown_deadline`), and joins every worker. Idempotent; must
    // not be called from a worker (a body cannot shut down its own pool -- it would join itself).
    void shutdown() noexcept {
        JobBase* drained = nullptr;
        {
            std::lock_guard lock(m_);
            if (shut_) return;
            shut_    = true;
            drained  = std::exchange(head_, nullptr);
            tail_    = nullptr;
            queued_  = 0;
        }
        work_cv_.notify_all();
        // Queued callers resume `canceled`, outside the pool lock (wake-ups only enqueue at their homes).
        while (drained != nullptr) {
            std::shared_ptr<JobBase> job = std::move(drained->self_);
            drained                      = std::exchange(drained->next_, nullptr);
            job->cancel();
        }
        if (options_.shutdown_deadline) {
            std::unique_lock lock(m_);
            if (!idle_cv_.wait_for(lock, *options_.shutdown_deadline, [this] { return running_ == 0; })) {
                std::size_t const still = running_;
                lock.unlock();
                shutdown_overruns_.fetch_add(1, std::memory_order_relaxed);
                if (options_.on_overrun) {
                    try {
                        options_.on_overrun(still);
                    } catch (...) {  // NOLINT(bugprone-empty-catch): a reporter must not stop the join
                    }
                }
            }
        }
        for (auto& w : workers_) {
            if (w.joinable()) w.join();  // never detached (§4.5 rule 1)
        }
    }

    // Diagnostics; monotonic counters except running()/queued().
    [[nodiscard]] std::size_t workers() const noexcept { return options_.workers; }
    [[nodiscard]] std::size_t running() const {
        std::lock_guard lock(m_);
        return running_;
    }
    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lock(m_);
        return queued_;
    }
    // Wake-ups refused because the waiter had no home (§4.2): it is never resumed, on the worker or anywhere.
    [[nodiscard]] std::uint64_t homeless_refusals() const noexcept {
        return homeless_refusals_.load(std::memory_order_relaxed);
    }
    // Bodies that finished after their caller had already resumed `canceled` (§4.6 residual).
    [[nodiscard]] std::uint64_t discarded_results() const noexcept {
        return discarded_results_.load(std::memory_order_relaxed);
    }
    // Shutdowns whose running bodies outlasted `shutdown_deadline`.
    [[nodiscard]] std::uint64_t shutdown_overruns() const noexcept {
        return shutdown_overruns_.load(std::memory_order_relaxed);
    }

    // Publishes a job. Never throws and never allocates (intrusive queue); false if the pool is shut down.
    [[nodiscard]] bool submit(std::shared_ptr<offload_detail::JobBase> job) noexcept {
        offload_detail::JobBase* raw = job.get();
        {
            std::lock_guard lock(m_);
            if (shut_) return false;
            raw->self_ = std::move(job);
            raw->next_ = nullptr;
            if (tail_ != nullptr) {
                tail_->next_ = raw;
            } else {
                head_ = raw;
            }
            tail_ = raw;
            ++queued_;
        }
        work_cv_.notify_one();
        return true;
    }

private:
    using JobBase = offload_detail::JobBase;
    friend class offload_detail::JobBase;

    void note_homeless_refusal() noexcept { homeless_refusals_.fetch_add(1, std::memory_order_relaxed); }
    void note_discarded() noexcept { discarded_results_.fetch_add(1, std::memory_order_relaxed); }

    void worker_loop() {
        ScopedBlockOnRefusal const no_block_on(offload_detail::kRefusalCode);  // §4.3
        std::unique_lock           lock(m_);
        for (;;) {
            work_cv_.wait(lock, [this] { return shut_ || head_ != nullptr; });
            if (head_ == nullptr) return;  // shut down and drained
            JobBase* raw = head_;
            head_        = std::exchange(raw->next_, nullptr);
            if (head_ == nullptr) tail_ = nullptr;
            --queued_;
            std::shared_ptr<JobBase> job = std::move(raw->self_);
            ++running_;
            lock.unlock();
            job->run();
            job.reset();  // the body's inputs and an undelivered result are released here, not under the lock
            lock.lock();
            --running_;
            if (running_ == 0) idle_cv_.notify_all();
        }
    }

    Options                    options_;
    mutable std::mutex         m_;
    std::condition_variable    work_cv_;
    std::condition_variable    idle_cv_;
    JobBase*                   head_    = nullptr;  // intrusive FIFO, guarded by m_
    JobBase*                   tail_    = nullptr;
    std::size_t                queued_  = 0;
    std::size_t                running_ = 0;
    bool                       shut_    = false;
    std::atomic<std::uint64_t> homeless_refusals_{0};
    std::atomic<std::uint64_t> discarded_results_{0};
    std::atomic<std::uint64_t> shutdown_overruns_{0};
    std::vector<std::jthread>  workers_;  // declared last: started after every member above exists
};

namespace offload_detail {

inline void JobBase::run() noexcept {
    int expected = kQueued;
    if (!state_.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) return;  // never runs
    invoke_body();
    expected = kRunning;
    if (state_.compare_exchange_strong(expected, kDone, std::memory_order_acq_rel)) {
        deliver();
        return;
    }
    if (expected == kCanceledRunning) pool_->note_discarded();  // the caller already resumed `canceled`
}

inline void JobBase::deliver() noexcept {
    if (!parked_.home && !parked_.ticket) {
        // Homeless: resuming here would run the coroutine on a worker (or the stopping thread). Refused.
        pool_->note_homeless_refusal();
        return;
    }
    detail::wake(std::move(parked_));
}

struct Canceler {
    std::shared_ptr<JobBase> job;
    void operator()() const noexcept { job->cancel(); }
};

template <class R>
class OffloadAwaiter {
public:
    OffloadAwaiter(OffloadPool& pool, std::stop_token stop, std::shared_ptr<ResultJob<R>> job) noexcept
        : pool_(&pool), stop_(std::move(stop)), job_(std::move(job)) {}

    OffloadAwaiter(OffloadAwaiter const&)            = delete;
    OffloadAwaiter& operator=(OffloadAwaiter const&) = delete;

    ~OffloadAwaiter() {
        if (!armed_) return;
        cb_.reset();       // after this no Canceler runs concurrently
        job_->abandon();   // frame destroyed while waiting
    }

    // Always suspends, deliberately: a stop already requested is handled by the arming path below (the
    // stop_callback fires inline during registration, try_arm() fails, await_suspend declines to suspend), so
    // that path -- otherwise reachable only through a narrow race -- runs on every pre-canceled offload and is
    // tested deterministically. The cost is one capture_parked() on an already-canceled call.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        // Everything that can throw happens here, while nothing refers to `h` yet.
        job_->set_parked(detail::capture_parked(h), pool_);
        std::shared_ptr<JobBase> const job  = job_;  // locals: usable after the frame may be running elsewhere
        OffloadPool* const             pool = pool_;
        armed_                              = true;
        if (stop_.stop_possible()) cb_.emplace(stop_, Canceler{job});
        if (!job->try_arm()) return false;  // a stop fired during registration: resume now, `canceled`
        // From here nothing in this frame may be touched: a cancel or a finished body may already have posted
        // the continuation, and a host Resumer may be running it on another thread.
        if (pool->submit(job)) return true;
        return !job->cancel_unsubmitted();  // pool shut down: resume now unless a racing cancel already posted
    }

    [[nodiscard]] offload_result<R> await_resume() {
        cb_.reset();
        armed_                                   = false;
        std::shared_ptr<ResultJob<R>> const job = std::move(job_);
        return job->take_result();
    }

private:
    OffloadPool*                                pool_;
    std::stop_token                             stop_;
    std::shared_ptr<ResultJob<R>>               job_;
    std::optional<std::stop_callback<Canceler>> cb_;
    bool                                        armed_ = false;
};

}  // namespace offload_detail

// Runs `fn(args...)` on a worker of `pool` and resumes the awaiting coroutine on its home with an
// `offload_result<R>`. `fn` and `args` are decay-copied/moved into the job (BY VALUE -- see the file
// comment; a by-reference lambda capture is the caller's bug). `stop` cancels as described above.
template <class F, class... Args>
[[nodiscard]] auto offload(OffloadPool& pool, std::stop_token stop, F&& fn, Args&&... args) {
    using Fn = std::decay_t<F>;
    using R  = std::invoke_result_t<Fn&&, std::decay_t<Args>&&...>;
    static_assert(std::is_move_constructible_v<Fn> && (std::is_move_constructible_v<std::decay_t<Args>> && ...),
                  "rt::offload: the callable and every argument are moved into the job (ADR-237 §4.6)");
    static_assert(!offload_detail::is_reference_wrapper_v<Fn> &&
                      !(offload_detail::is_reference_wrapper_v<std::decay_t<Args>> || ...),
                  "rt::offload: inputs are taken by value -- std::ref/std::cref would let the body outlive what "
                  "it refers to when the caller resumes `canceled` (ADR-237 §4.6, red-team G3)");
    static_assert(!std::is_reference_v<R>,
                  "rt::offload: the body must return a value, not a reference (ADR-237 §4.6)");
    auto job = std::make_shared<offload_detail::Job<R, Fn, std::decay_t<Args>...>>(std::forward<F>(fn),
                                                                                   std::forward<Args>(args)...);
    return offload_detail::OffloadAwaiter<R>(pool, std::move(stop), std::move(job));
}

}  // namespace agentengine::rt
