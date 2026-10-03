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
// Step 5 (§14):
//   - ROOT ADOPTION (§4.5 rule 3). A host that destroys the coroutine awaiting `enter` while the engine root still
//     runs (it dropped its awaiter) no longer aborts the process: the Runtime ADOPTS the root -- the engine chain
//     stays intact, so every referent is alive -- requests stop on it, and lets it finish normally, which releases
//     its locks, quota tickets and refunds through their ordinary destructors. Its result is discarded; the root
//     still counts toward shutdown (joined like any other) and is counted (`adopted_roots()`). The root runs inside
//     a small self-freeing frame that owns the engine task, so nothing waits on the dropped awaiter.
//   - A FOREIGN CONTINUATION WHOSE HOME IS GONE (§4.2): a host Resumer that drops an `enter` continuation (its
//     executor shut down) does not get it resumed inline on the dropping thread -- a lane, for an enter -- it is
//     never resumed, and counted (`dropped_continuations()`).
//   - ROOT STOP TOKENS: each `run`/`enter` root is a fresh await chain (rt/await_chain.hpp) carrying a stop source
//     of its own: `rt::scope_stop_token()` inside the root returns it; adoption and shutdown request it. Shutdown
//     is now: stop accepting -> request stop on every root -> wait for them -> pools -> lanes -> reactor.
//   - WATCHDOGS (§4.3): `RuntimeConfig::lane_stall_bound` / `stuck_run_bound` turn on rt/lanes.hpp's report-only
//     watchdogs; every root's strand group is watched for a stuck run while the root is in flight.
// Still interim: the compile-time restriction on a bare `co_await` of an engine task from a foreign coroutine type
// (D7).

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
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/pal/cpu.hpp"
#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/await_chain.hpp"
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
    // §4.3 watchdogs (step 5), report-only, off when unset. Callbacks run on the watchdog thread; must not throw.
    std::optional<std::chrono::milliseconds> lane_stall_bound{};  // a lane not back at its queue within this
    std::function<void(LaneStall const&)>    on_lane_stall{};
    std::optional<std::chrono::milliseconds> stuck_run_bound{};   // a root's strand group without a slice for this
    std::function<void(StuckRun const&)>     on_stuck_run{};
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

// Step 5 counters, shared so that an awaiter destroyed on a host thread, or a continuation dropped by a host after
// the Runtime is gone, never touches the Runtime itself.
struct SharedCounters {
    std::atomic<std::uint64_t> adopted{0};           // enter roots adopted (§4.5 rule 3)
    std::atomic<std::uint64_t> adopted_finished{0};  // ... that have since finished
};

// One `enter`: shared by the awaiter and the root frame; the root frame's reference keeps it alive after an
// adoption (§4.5 rule 3). `phase` decides, exactly once, who the end of the root belongs to.
template <class T>
struct EnterState {
    static constexpr int kRunning  = 0;
    static constexpr int kFinished = 1;  // the root finished first: the foreign continuation is handed over
    static constexpr int kAdopted  = 2;  // the awaiter was destroyed first: the Runtime owns the root

    Runtime*                               rt = nullptr;
    std::shared_ptr<SharedCounters>        counters;
    Strand                                 strand;
    std::stop_source                       stop;
    std::uint64_t                          root_holder = 0;
    std::coroutine_handle<>                foreign{};
    std::uint64_t                          foreign_holder = 0;
    std::shared_ptr<detail::ResumerTicket> ticket;  // copied into the continuation
    std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>> value;
    std::exception_ptr                     fault;
    std::atomic<int>                       phase{kRunning};
};

// The frame an `enter` root runs in: started on the root's strand, frees itself when its body ends (final_suspend
// never suspends), and owns the engine task -- so an adopted root needs no owner to be destroyed by.
struct RootFrame {
    struct promise_type {
        RootFrame get_return_object() noexcept {
            return RootFrame{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }  // the body catches everything
    };
    std::coroutine_handle<promise_type> h;
};

template <class T>
void finish_enter(EnterState<T>& st) noexcept;

template <class T>
RootFrame enter_root(task<T> t, std::shared_ptr<EnterState<T>> st) {
    {
        task<T> body = std::move(t);  // destroyed (with every guard its frame held) before the hand-off
        try {
            if constexpr (std::is_void_v<T>) {
                co_await body;
                st->value.emplace();
            } else {
                st->value.emplace(co_await body);
            }
        } catch (...) {
            st->fault = std::current_exception();
        }
    }
    finish_enter(*st);  // the last statement; `st` (this frame's parameter) keeps the state alive through it
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
        if (st_->fault) std::rethrow_exception(st_->fault);
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(*st_->value);
        }
    }

private:
    Runtime*                                         rt_;
    task<T>                                          task_;
    std::shared_ptr<Resumer>                         resumer_;
    CompletionThread*                                ct_;
    std::shared_ptr<runtime_detail::EnterState<T>>   st_;
    std::shared_ptr<detail::ResumerTicket>           ticket_keep_;  // kept to claim a queued continuation
    bool                                             started_ = false;
    bool                                             resumed_ = false;
};

// ae-naming-lint: allow Runtime — ADR-237 §9 D7: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {})
        : config_(std::move(config)),
          reactor_(pal::make_default_reactor()),
          lanes_(config_.lanes == half_of_hardware ? default_lane_count() : config_.lanes,
                 WatchdogOptions{config_.lane_stall_bound, config_.on_lane_stall, config_.stuck_run_bound,
                                 config_.on_stuck_run}),
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
        // §4.5 rule 4 (step 5): cancel every root before joining it. Requested outside `m_`: a stop runs its
        // callbacks inline, and those only signal and post (rt/strand_stop_callback.hpp, reactor cancels).
        std::vector<std::stop_source> stops;
        stops.reserve(live_roots_.size());
        for (auto const& [holder, source] : live_roots_) stops.push_back(source);
        lock.unlock();
        for (std::stop_source& s : stops) s.request_stop();
        stops.clear();
        lock.lock();
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
        std::uint64_t const holder = mint_holder_id();  // a fresh await chain (step 5)
        std::stop_source    stop;
        admit(holder, stop);
        try {
            chain_detail::Registry::instance().add(holder, chain_detail::Link{0, nullptr, stop.get_token(), 0});
            lanes_.watch_root(strand);
        } catch (...) {
            (void)chain_detail::Registry::instance().remove(holder);
            release_root(holder);
            throw;
        }
        runtime_detail::RunSignal signal;
        auto                      h = detail::TaskAccess::handle(t);
        h.promise().link_.on_done     = &runtime_detail::RunSignal::done;
        h.promise().link_.on_done_ctx = &signal;
        strand.post(h, holder);
        signal.wait();
        lanes_.unwatch_root(strand);
        (void)chain_detail::Registry::instance().remove(holder);
        release_root(holder);
        return runtime_detail::take_result(t);
    }

    // §4.5 rule 3 (step 5): `enter` roots whose awaiter was destroyed and that the Runtime adopted, and how many of
    // those have finished since.
    [[nodiscard]] std::uint64_t adopted_roots() const noexcept {
        return counters_->adopted.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t adopted_roots_finished() const noexcept {
        return counters_->adopted_finished.load(std::memory_order_acquire);
    }
    // §4.2 (step 5): `enter` continuations a host Resumer dropped -- never resumed.
    [[nodiscard]] std::uint64_t dropped_continuations() const noexcept {
        return dropped_->load(std::memory_order_acquire);
    }
    // §4.3 watchdog reports so far (step 5).
    [[nodiscard]] std::uint64_t lane_stalls() const noexcept { return lanes_.lane_stalls(); }
    [[nodiscard]] std::uint64_t stuck_runs() const noexcept { return lanes_.stuck_runs(); }

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
    template <class T>
    friend void runtime_detail::finish_enter(runtime_detail::EnterState<T>& st) noexcept;

    void admit(std::uint64_t holder, std::stop_source const& stop) {
        std::lock_guard lock(m_);
        if (!accepting_) throw std::logic_error("rt.runtime_closed: the Runtime is shutting down (ADR-237 §4.5)");
        live_roots_.emplace(holder, stop);
        ++roots_;
    }
    // Last touch on the Runtime from a finishing root: the destructor may proceed once it unlocks.
    void release_root(std::uint64_t holder) noexcept {
        std::lock_guard lock(m_);
        live_roots_.erase(holder);
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

    std::shared_ptr<runtime_detail::SharedCounters> counters_ = std::make_shared<runtime_detail::SharedCounters>();
    std::shared_ptr<std::atomic<std::uint64_t>>     dropped_  = std::make_shared<std::atomic<std::uint64_t>>(0);

    mutable std::mutex      m_;
    std::condition_variable idle_cv_;
    std::size_t             roots_              = 0;
    std::size_t             completion_threads_ = 0;
    bool                    accepting_          = true;
    std::unordered_map<std::uint64_t, std::stop_source> live_roots_;  // holder -> its stop (shutdown cancels)
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
    auto st         = std::make_shared<runtime_detail::EnterState<T>>();
    st->rt          = rt_;
    st->counters    = rt_->counters_;
    st->strand      = rt_->new_strand_group().new_strand();
    st->root_holder = mint_holder_id();  // a fresh await chain (step 5)
    st->ticket      = std::make_shared<detail::ResumerTicket>(resumer_);
    st->ticket->dropped_counter = rt_->dropped_;  // §4.2: a dropped enter continuation is never resumed
    ticket_keep_                = st->ticket;
    runtime_detail::RootFrame frame = runtime_detail::enter_root(std::move(task_), st);
    try {
        rt_->admit(st->root_holder, st->stop);
    } catch (...) {
        frame.h.destroy();
        throw;
    }
    try {
        chain_detail::Registry::instance().add(st->root_holder,
                                               chain_detail::Link{0, nullptr, st->stop.get_token(), 0});
        rt_->lanes_.watch_root(st->strand);
    } catch (...) {
        frame.h.destroy();
        (void)chain_detail::Registry::instance().remove(st->root_holder);
        rt_->release_root(st->root_holder);
        throw;
    }
    st->foreign        = foreign;
    st->foreign_holder = current_holder_id();
    if (ct_ != nullptr) ct_->outstanding_.fetch_add(1, std::memory_order_acq_rel);
    st_      = st;
    started_ = true;
    Strand const strand = st->strand;
    strand.post(frame.h, st->root_holder);
    // Nothing in this awaiter may be touched from here on: the continuation may already be running.
}

namespace runtime_detail {

// The end of an `enter` root, on its lane (the last statement of `enter_root`). Exactly one of: hand the foreign
// continuation to the host's Resumer (the awaiter is still there), or -- the awaiter was destroyed and the root
// adopted -- just finish. Then release the root: the last touch on the Runtime.
template <class T>
void finish_enter(EnterState<T>& st) noexcept {
    Runtime* const      rt     = st.rt;
    std::uint64_t const holder = st.root_holder;
    (void)chain_detail::Registry::instance().remove(holder);
    rt->lanes_.unwatch_root(st.strand);
    detail::ParkedResumer r{st.foreign, {}, st.foreign_holder, st.ticket, {}};
    int expected = EnterState<T>::kRunning;
    if (st.phase.compare_exchange_strong(expected, EnterState<T>::kFinished, std::memory_order_acq_rel)) {
        // The awaiter may be destroyed from here on (the host resumes the foreign coroutine on its own thread);
        // `st` stays alive through this frame's own reference.
        detail::hand_to_resumer(std::move(r));
    } else {
        st.counters->adopted_finished.fetch_add(1, std::memory_order_acq_rel);  // §4.5 rule 3: adopted, done
    }
    rt->release_root(holder);
}

}  // namespace runtime_detail

// The host destroyed the coroutine awaiting this `enter` (it dropped its awaiter). Touches only the shared state and
// its own fields -- never the Runtime, which may already be gone if the root finished.
template <class T>
EnterAwaiter<T>::~EnterAwaiter() {
    if (!started_ || resumed_) return;
    int expected = runtime_detail::EnterState<T>::kRunning;
    if (st_->phase.compare_exchange_strong(expected, runtime_detail::EnterState<T>::kAdopted,
                                           std::memory_order_acq_rel)) {
        // §4.5 rule 3: the engine chain is still running. It is adopted -- it keeps running, intact, in its own
        // frame -- and asked to stop; it finishes normally, releasing what it holds, and the Runtime joins it at
        // shutdown like any other root. Nothing will continue the foreign coroutine.
        st_->counters->adopted.fetch_add(1, std::memory_order_acq_rel);
        st_->stop.request_stop();  // callbacks only signal and post (rt/strand_stop_callback.hpp)
        if (ct_ != nullptr) ct_->outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    // Finished, its continuation queued at the host and never run: claim it so it never touches this frame.
    (void)detail::abandon_woken(ticket_keep_);
    if (ct_ != nullptr) ct_->outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace agentengine::rt
