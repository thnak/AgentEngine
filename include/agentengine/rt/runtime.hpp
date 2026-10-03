#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §9 D7 ("an explicit, short host API"),
// §9 D5 (lane count), §4.1 (thread roles), §4.2 (foreign continuations run where the host said) and §4.5 rule 4
// (shutdown order) -- `rt::Runtime`, `rt::RuntimeConfig`, `rt::CompletionThread`. Step 3 of the ADR (§14).
//
//   ae::rt::Runtime runtime{{.lanes = ae::rt::half_of_hardware}};   // owns reactor + lanes + offload pools
//   auto r = runtime.run(make_task());                               // 1. synchronous host: block until done
//   auto r = co_await runtime.enter(make_task(), my_resumer);         // 2. host coroutine: back via its Resumer
//   ae::rt::CompletionThread completions{runtime};
//   auto r = co_await runtime.enter(make_task(), completions);        // 3. back on a named engine thread
//
// The engine body always runs on a lane, as a continuation of a FRESH strand in a fresh strand group (a new
// "session" for scheduling, rt/lanes.hpp) -- never on the host thread. The foreign continuation of `enter` runs
// where the host named: handed to its ADR-219 `Resumer`, or on the `CompletionThread` -- never on a lane (host
// code may block and would starve the lanes) and never on the reactor thread.
//
// `run` is `block_on` at the outer edge (§9 D7) and is refused with the same marker on lane and offload workers
// (§4.3): it would park a worker waiting for work that may need it.
//
// SHUTDOWN (§4.5 rule 4): stop accepting (`run`/`enter` throw `rt.runtime_closed`) -> wait for every root
// started by `run`/`enter` to finish (no task scopes exist yet, so there is nothing to cancel on the host's
// behalf; an optional deadline only REPORTS an overrun, the wait continues -- never a detach) -> offload pools
// (queued callers resume `canceled` on their strands, running bodies are joined) -> lanes (drain, join) ->
// reactor (pending completions delivered, its thread joined). No thread outlives the Runtime. Destroying a
// Runtime from one of its own lanes, or while a CompletionThread made from it is alive, is a checked violation
// (abort, every build, with a message).
//
// Interim, until the later steps of the ADR: the compile-time restriction on a bare `co_await` of an engine
// task from a foreign coroutine type (D7), and root adoption when a host drops an awaiter (§4.5 rule 3).
// Destroying a foreign coroutine while its `enter` root still runs is, for now, a checked violation (abort).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "agentengine/pal/cpu.hpp"
#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/lanes.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// CPUs this process may run on: affinity, and on Linux the cgroup quota (pal/cpu.hpp).
// ae-naming-lint: allow usable_cpus — ADR-237 §9 D5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
[[nodiscard]] inline unsigned usable_cpus() noexcept { return pal::usable_cpus(); }

// §9 D5: max(2, usable_cpus() / 2). The floor keeps one CPU-heavy tool from stalling every session on a 2-core box.
// ae-naming-lint: allow default_lane_count — ADR-237 §9 D5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
[[nodiscard]] inline std::size_t default_lane_count() noexcept {
    unsigned const half = usable_cpus() / 2;
    return half < 2 ? 2 : half;
}

// `RuntimeConfig::lanes` value meaning "the D5 default" (`default_lane_count()`).
// ae-naming-lint: allow half_of_hardware — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
inline constexpr std::size_t half_of_hardware = 0;

// ae-naming-lint: allow RuntimeConfig — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct RuntimeConfig {
    std::size_t lanes           = half_of_hardware;  // §9 D5
    std::size_t offload_workers = 2;                 // §4.6 general offload pool
    std::size_t dns_workers     = 2;                 // §4.6 separate DNS pool
    // Shutdown: report (never detach) when roots outlast this; unset waits without reporting.
    std::optional<std::chrono::milliseconds> shutdown_deadline{};
    // Called on the destroying thread with the number of roots still running. Must not throw.
    std::function<void(std::size_t roots_still_running)> on_shutdown_overrun{};
};

// ae-naming-lint: allow Runtime — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class Runtime;
// ae-naming-lint: allow CompletionThread — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class CompletionThread;

namespace runtime_detail {

[[noreturn]] inline void violation(char const* what) noexcept {
    std::fprintf(stderr, "agentengine: checked violation (ADR-237): %s\n", what);
    std::fflush(stderr);
    std::abort();
}

// What `Runtime::run` blocks on. Signalled from the root's suspended final point; the lane thread's last touch is
// the unlock, after which the host may destroy it (POSIX: a mutex may be destroyed once unlocked).
class RunSignal {
public:
    static void done(void* ctx) noexcept {
        auto* const self = static_cast<RunSignal*>(ctx);
        std::lock_guard lock(self->m_);
        self->done_ = true;
        self->cv_.notify_one();
    }
    void wait() {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return done_; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    done_ = false;
};

template <class T>
[[nodiscard]] T take_result(task<T>& t) {
    auto h = detail::TaskAccess::handle(t);
    if (h.promise().fault_) std::rethrow_exception(h.promise().fault_);
    if constexpr (std::is_void_v<T>) {
        return;
    } else {
        return std::move(*h.promise().value_ptr());
    }
}

}  // namespace runtime_detail

// A named engine thread that runs foreign continuations for a host that has neither its own executor nor a
// thread to block (§9 D7, case 3). Its destructor joins it. Destroying it while an `enter` awaiting it is still
// outstanding is a checked violation (abort, every build): the continuation would have nowhere to run (§4.2,
// round 3). It must be destroyed before its Runtime. Coroutines it resumes that park on engine primitives come
// back to it (it is their ADR-219 `Resumer`).
// ae-naming-lint: allow CompletionThread — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class CompletionThread {
public:
    explicit CompletionThread(Runtime& runtime);
    ~CompletionThread();
    CompletionThread(CompletionThread const&)            = delete;
    CompletionThread& operator=(CompletionThread const&) = delete;

    [[nodiscard]] std::thread::id id() const noexcept { return thread_.get_id(); }
    [[nodiscard]] bool is_current() const noexcept { return std::this_thread::get_id() == thread_.get_id(); }
    // `enter` calls whose continuation has not yet run on this thread.
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_.load(std::memory_order_acquire); }
    [[nodiscard]] std::shared_ptr<Resumer> resumer() const noexcept { return queue_; }

private:
    template <class T>
    friend class EnterAwaiter;

    class Queue final : public Resumer {
    public:
        void post(ParkedContinuation c) override {
            std::unique_lock lock(m_);
            if (closed_) {
                // Only a generic park can arrive after close (an outstanding `enter` aborts the destructor
                // first): ADR-219's fallback -- `c` resumes on the posting thread when it is dropped here.
                lock.unlock();
                return;
            }
            q_.push_back(std::move(c));
            cv_.notify_one();
        }
        void run() {
            std::unique_lock lock(m_);
            for (;;) {
                cv_.wait(lock, [this] { return closed_ || !q_.empty(); });
                if (q_.empty()) return;
                ParkedContinuation c = std::move(q_.front());
                q_.pop_front();
                lock.unlock();
                c.resume();  // with this queue as its resumer: what parks again comes back here
                lock.lock();
            }
        }
        void close() {
            std::lock_guard lock(m_);
            closed_ = true;
            cv_.notify_all();
        }

    private:
        std::mutex                     m_;
        std::condition_variable        cv_;
        std::deque<ParkedContinuation> q_;
        bool                           closed_ = false;
    };

    Runtime*                 runtime_;
    std::shared_ptr<Queue>   queue_;
    std::atomic<std::size_t> outstanding_{0};
    std::jthread             thread_;
};

// `co_await runtime.enter(task, resumer)` / `(task, completion_thread)`.
template <class T>
// ae-naming-lint: allow EnterAwaiter — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class EnterAwaiter {
public:
    EnterAwaiter(Runtime& rt, task<T> t, std::shared_ptr<Resumer> resumer, CompletionThread* ct) noexcept
        : rt_(&rt), task_(std::move(t)), resumer_(std::move(resumer)), ct_(ct) {}
    EnterAwaiter(EnterAwaiter const&)            = delete;
    EnterAwaiter& operator=(EnterAwaiter const&) = delete;
    ~EnterAwaiter();

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void               await_suspend(std::coroutine_handle<> foreign);
    T                  await_resume() {
        resumed_ = true;
        if (ct_ != nullptr) ct_->outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return runtime_detail::take_result(task_);
    }

private:
    static void root_done(void* ctx) noexcept;

    Runtime*                               rt_;
    task<T>                                task_;
    std::shared_ptr<Resumer>               resumer_;
    CompletionThread*                      ct_;
    std::coroutine_handle<>                foreign_{};
    std::uint64_t                          holder_ = 0;
    std::shared_ptr<detail::ResumerTicket> ticket_;       // handed to the continuation
    std::shared_ptr<detail::ResumerTicket> ticket_keep_;  // kept to claim a queued continuation (destructor)
    std::atomic<bool>                      root_finished_{false};
    bool                                   started_ = false;
    bool                                   resumed_ = false;
};

// ae-naming-lint: allow Runtime — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {})
        : config_(std::move(config)),
          reactor_(pal::make_default_reactor()),
          lanes_(config_.lanes == half_of_hardware ? default_lane_count() : config_.lanes),
          offload_(OffloadPool::Options{config_.offload_workers, {}, {}}),
          dns_(OffloadPool::Options{config_.dns_workers, {}, {}}) {}

    ~Runtime() {
        if (detail::block_on_refusal_code() == lanes_detail::kRefusalCode) {
            runtime_detail::violation("rt::Runtime destroyed on one of its own lanes (it would join itself)");
        }
        std::unique_lock lock(m_);
        accepting_ = false;
        if (completion_threads_ != 0) {
            runtime_detail::violation("rt::Runtime destroyed while a CompletionThread made from it is alive "
                                      "(destroy the CompletionThread first, §4.5 rule 4)");
        }
        if (config_.shutdown_deadline) {
            if (!idle_cv_.wait_for(lock, *config_.shutdown_deadline, [this] { return roots_ == 0; })) {
                std::size_t const still = roots_;
                lock.unlock();
                if (config_.on_shutdown_overrun) {
                    try {
                        config_.on_shutdown_overrun(still);
                    } catch (...) {  // NOLINT(bugprone-empty-catch): a reporter must not stop the join
                    }
                }
                lock.lock();
            }
        }
        idle_cv_.wait(lock, [this] { return roots_ == 0; });
        lock.unlock();
        dns_.shutdown();      // queued callers resume `canceled` on their strands; running bodies joined
        offload_.shutdown();
        lanes_.stop();        // drain, join
        reactor_.reset();     // pending completions delivered (posts to the stopped lanes are dropped), joined
    }

    Runtime(Runtime const&)            = delete;
    Runtime& operator=(Runtime const&) = delete;

    [[nodiscard]] pal::Reactor& reactor() noexcept { return *reactor_; }
    [[nodiscard]] OffloadPool&  offload_pool() noexcept { return offload_; }
    [[nodiscard]] OffloadPool&  dns_pool() noexcept { return dns_; }
    [[nodiscard]] LanePool&     lane_pool() noexcept { return lanes_; }
    [[nodiscard]] std::size_t   lanes() const noexcept { return lanes_.lanes(); }

    // A new "session" for scheduling (§4.3); make its strands with `new_strand()`.
    [[nodiscard]] StrandGroup new_strand_group() const { return lanes_.new_group(); }

    // Roots started by run/enter that have not finished.
    [[nodiscard]] std::size_t roots_in_flight() const {
        std::lock_guard lock(m_);
        return roots_;
    }

    // Case 1 (§9 D7): runs `t` on a fresh strand and blocks this thread until it finishes; returns its value or
    // rethrows its exception. Refused (block_on_refused) on a lane or offload worker.
    template <class T>
    [[nodiscard]] T run(task<T> t) {
        return run(new_strand_group().new_strand(), std::move(t));
    }

    // As above, on a strand the caller chose (a session's).
    template <class T>
    [[nodiscard]] T run(Strand const& strand, task<T> t) {
        if (char const* refused = detail::block_on_refusal_code()) throw block_on_refused(refused);  // §4.3
        if (!t.valid() || !strand.valid()) throw std::invalid_argument("rt::Runtime::run: empty task or strand");
        admit();
        runtime_detail::RunSignal signal;
        auto                      h = detail::TaskAccess::handle(t);
        h.promise().link_.on_done     = &runtime_detail::RunSignal::done;
        h.promise().link_.on_done_ctx = &signal;
        strand.post(h, mint_holder_id());
        signal.wait();
        release_root();
        return runtime_detail::take_result(t);
    }

    // Case 2 (§9 D7): the engine body runs on a lane; the awaiting (foreign) coroutine is handed to `resumer`
    // when it finishes. `resumer` must outlive the continuation (the shared_ptr overload owns it instead).
    template <class T>
    [[nodiscard]] EnterAwaiter<T> enter(task<T> t, Resumer& resumer) {
        return EnterAwaiter<T>(*this, std::move(t), std::shared_ptr<Resumer>(std::shared_ptr<void>{}, &resumer),
                               nullptr);
    }
    template <class T>
    [[nodiscard]] EnterAwaiter<T> enter(task<T> t, std::shared_ptr<Resumer> resumer) {
        return EnterAwaiter<T>(*this, std::move(t), std::move(resumer), nullptr);
    }
    // Case 3 (§9 D7): back on a CompletionThread the host created by name.
    template <class T>
    [[nodiscard]] EnterAwaiter<T> enter(task<T> t, CompletionThread& thread) {
        return EnterAwaiter<T>(*this, std::move(t), thread.resumer(), &thread);
    }

private:
    friend class CompletionThread;
    template <class T>
    friend class EnterAwaiter;

    void admit() {
        std::lock_guard lock(m_);
        if (!accepting_) throw std::logic_error("rt.runtime_closed: the Runtime is shutting down (ADR-237 §4.5)");
        ++roots_;
    }
    // Last touch on the Runtime from a finishing root: the destructor may proceed once it unlocks.
    void release_root() noexcept {
        std::lock_guard lock(m_);
        --roots_;
        if (roots_ == 0) idle_cv_.notify_all();
    }
    void add_completion_thread() {
        std::lock_guard lock(m_);
        if (!accepting_) throw std::logic_error("rt.runtime_closed: the Runtime is shutting down (ADR-237 §4.5)");
        ++completion_threads_;
    }
    void remove_completion_thread() noexcept {
        std::lock_guard lock(m_);
        --completion_threads_;
    }

    RuntimeConfig                 config_;
    std::unique_ptr<pal::Reactor> reactor_;  // declared first among the threads: destroyed last
    LanePool                      lanes_;
    OffloadPool                   offload_;
    OffloadPool                   dns_;

    mutable std::mutex      m_;
    std::condition_variable idle_cv_;
    std::size_t             roots_              = 0;
    std::size_t             completion_threads_ = 0;
    bool                    accepting_          = true;
};

// --- CompletionThread ------------------------------------------------------------------------------------------

inline CompletionThread::CompletionThread(Runtime& runtime)
    : runtime_(&runtime), queue_(std::make_shared<Queue>()) {
    runtime_->add_completion_thread();
    try {
        thread_ = std::jthread([q = queue_] { q->run(); });
    } catch (...) {
        runtime_->remove_completion_thread();
        throw;
    }
}

inline CompletionThread::~CompletionThread() {
    if (outstanding_.load(std::memory_order_acquire) != 0) {
        runtime_detail::violation("rt::CompletionThread destroyed with an enter() continuation still outstanding "
                                  "(ADR-237 §4.2: it would have nowhere to run)");
    }
    queue_->close();
    if (thread_.joinable()) thread_.join();  // never detached (§4.5 rule 1)
    runtime_->remove_completion_thread();
}

// --- EnterAwaiter ----------------------------------------------------------------------------------------------

template <class T>
void EnterAwaiter<T>::await_suspend(std::coroutine_handle<> foreign) {
    if (!task_.valid() || !resumer_) throw std::invalid_argument("rt::Runtime::enter: empty task or resumer");
    // Everything that can throw happens before the root is posted, while nothing refers to `foreign` yet.
    Strand const strand = rt_->new_strand_group().new_strand();
    ticket_             = std::make_shared<detail::ResumerTicket>(resumer_);
    ticket_keep_        = ticket_;
    rt_->admit();
    foreign_ = foreign;
    holder_  = current_holder_id();
    if (ct_ != nullptr) ct_->outstanding_.fetch_add(1, std::memory_order_acq_rel);
    auto h                        = detail::TaskAccess::handle(task_);
    h.promise().link_.on_done     = &EnterAwaiter::root_done;
    h.promise().link_.on_done_ctx = this;
    started_                      = true;
    strand.post(h, mint_holder_id());
    // Nothing in this awaiter may be touched from here on: the continuation may already be running.
}

template <class T>
void EnterAwaiter<T>::root_done(void* ctx) noexcept {
    auto* const self = static_cast<EnterAwaiter*>(ctx);
    Runtime* const        rt = self->rt_;
    detail::ParkedResumer r{self->foreign_, {}, self->holder_, std::move(self->ticket_), {}};
    self->root_finished_.store(true, std::memory_order_release);
    // `self` may be destroyed from here on (the host resumes the foreign coroutine on its own thread).
    detail::hand_to_resumer(std::move(r));
    rt->release_root();
}

template <class T>
EnterAwaiter<T>::~EnterAwaiter() {
    if (!started_ || resumed_) return;
    if (!root_finished_.load(std::memory_order_acquire)) {
        // §4.5 rule 3 (root adoption) is a later step; until then this would destroy a running engine chain.
        runtime_detail::violation("a coroutine awaiting rt::Runtime::enter was destroyed while its engine task "
                                  "was still running");
    }
    // Finished, its continuation queued at the host and never run: claim it so it never touches this frame.
    (void)detail::abandon_woken(ticket_keep_);
    if (ct_ != nullptr) ct_->outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace agentengine::rt
