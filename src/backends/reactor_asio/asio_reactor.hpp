#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.2 -- the standalone-Asio
// `pal::Reactor`, as an internal header of src/backends/reactor_asio/ so each I/O family (timers here;
// sockets, pipes and processes in their own translation units) can reach the one io_context without any Asio
// type leaving this directory. Never included from include/.
//
// Every operation's state is created, completed and cancelled on the reactor thread (the io_context is run
// by exactly one thread, so it is its own strand): callers only post. That is the "one serial context per
// I/O object" rule of §4.4 in its simplest form. A new I/O family follows start_timer()'s shape:
//   1. post the start to io();
//   2. on the reactor thread, if `op->cancel_requested() || shutting_down()`, complete `canceled` and stop
//      (sticky cancel, round-2 M2);
//   3. store a `BackendOpState` subclass in `op->backend_state` whose cancel() cancels the Asio object, and
//      `add_pending(op)`;
//   4. in the Asio completion handler: `remove_pending(op)`, reset `op->backend_state`, report the BACKEND's
//      outcome (round-1 M1: a finished operation reports finished even if a cancel raced it).

#include "agentengine/pal/reactor.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

namespace agentengine::pal::asio_backend {

// What every I/O family stores in ReactorOp::backend_state. Touched on the reactor thread only.
struct BackendOpState {
    virtual ~BackendOpState() = default;
    virtual void cancel() noexcept = 0;
};

struct TimerState final : BackendOpState {
    explicit TimerState(asio::io_context& io) : timer(io) {}
    void cancel() noexcept override { timer.cancel(); }
    asio::steady_timer timer;
};

class AsioReactor final : public Reactor {
public:
    AsioReactor() : work_(asio::make_work_guard(io_)) {
        {
            std::lock_guard const lock(live_mutex());
            live().insert(this);
        }
        thread_ = std::thread([this] {
            thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
            io_.run();
        });
    }

    // Shutdown (§4.5 rule 4): cancel every pending operation on the reactor thread, let their completions run,
    // then let run() return and join. No completion is dropped and no thread outlives the reactor.
    ~AsioReactor() override {
        {
            std::lock_guard const lock(live_mutex());
            live().erase(this);
        }
        asio::post(io_, [this] {
            shutting_down_ = true;
            for (auto const& op : pending_) cancel_on_reactor(op);
        });
        work_.reset();
        if (thread_.joinable()) thread_.join();
    }

    AsioReactor(AsioReactor const&)            = delete;
    AsioReactor& operator=(AsioReactor const&) = delete;

    void start_timer(std::shared_ptr<ReactorOp> op, clock::time_point deadline) override {
        asio::post(io_, [this, op = std::move(op), deadline]() mutable {
            if (op->cancel_requested() || shutting_down_) {
                op->on_complete(op_status::canceled);
                return;
            }
            auto state = std::make_shared<TimerState>(io_);
            state->timer.expires_at(deadline);
            op->backend_state = state;
            pending_.insert(op);
            state->timer.async_wait([this, op](std::error_code const& ec) {
                pending_.erase(op);
                op->backend_state.reset();
                op->on_complete(ec ? op_status::canceled : op_status::completed);
            });
        });
    }

    [[nodiscard]] bool on_reactor_thread() const noexcept override {
        return std::this_thread::get_id() == thread_id_.load(std::memory_order_acquire);
    }

    // This backend's reactor behind a `pal::Reactor&`, or null for any other implementation (a test's manual
    // reactor). Without RTTI (CONVENTIONS): a registry of live instances, touched at construction, destruction
    // and by this lookup only. Used by the process family (reactor_process_asio.cpp, ADR-237 §6.3).
    [[nodiscard]] static AsioReactor* from(Reactor& r) noexcept {
        std::lock_guard const lock(live_mutex());
        return live().contains(&r) ? static_cast<AsioReactor*>(&r) : nullptr;
    }

    // ---- shared by the other I/O families in this directory (reactor thread only, except io()) ----------
    [[nodiscard]] asio::io_context& io() noexcept { return io_; }
    [[nodiscard]] bool shutting_down() const noexcept { return shutting_down_; }
    void add_pending(std::shared_ptr<ReactorOp> const& op) { pending_.insert(op); }
    void remove_pending(std::shared_ptr<ReactorOp> const& op) { pending_.erase(op); }

protected:
    void post_cancel(std::shared_ptr<ReactorOp> const& op) override {
        asio::post(io_, [this, op] { cancel_on_reactor(op); });
    }

private:
    static std::mutex& live_mutex() noexcept {
        static std::mutex m;
        return m;
    }
    static std::unordered_set<Reactor const*>& live() noexcept {
        static std::unordered_set<Reactor const*> s;
        return s;
    }

    // Reactor thread only. Not started yet: the sticky flag stops it at start. Already completed: nothing.
    void cancel_on_reactor(std::shared_ptr<ReactorOp> const& op) {
        if (!pending_.contains(op)) return;
        auto* state = static_cast<BackendOpState*>(op->backend_state.get());
        if (state != nullptr) state->cancel();
    }

    asio::io_context                                           io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::unordered_set<std::shared_ptr<ReactorOp>>             pending_;               // reactor thread only
    bool                                                       shutting_down_ = false;  // reactor thread only
    std::atomic<std::thread::id>                               thread_id_{};
    std::thread                                                thread_;  // joined in the destructor, never detached
};

}  // namespace agentengine::pal::asio_backend
