// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.2 (§9 D1) -- the production
// `pal::Reactor`: standalone Asio 1.38.2, one io_context run by one reactor thread. The only translation unit
// that includes Asio; nothing Asio-typed leaves it.
//
// Every operation's state is created, completed and cancelled on the reactor thread (the io_context is run by
// exactly one thread, so it is its own strand): callers only post. That is the "one serial context per I/O
// object" rule of §4.4 in its simplest form.

#include "agentengine/pal/reactor.hpp"

#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

namespace agentengine::pal {
namespace {

// Backend state of one timer operation; lives in ReactorOp::backend_state, touched on the reactor thread only.
struct TimerState {
    explicit TimerState(asio::io_context& io) : timer(io) {}
    asio::steady_timer timer;
};

class AsioReactor final : public Reactor {
public:
    AsioReactor() : work_(asio::make_work_guard(io_)) {
        thread_ = std::thread([this] {
            thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
            io_.run();
        });
    }

    // Shutdown (§4.5 rule 4): cancel every pending operation on the reactor thread, let their completions run,
    // then let run() return and join. No completion is dropped and no thread outlives the reactor.
    ~AsioReactor() override {
        asio::post(io_, [this] {
            shutting_down_ = true;
            for (auto const& op : pending_) cancel_on_reactor(op);
        });
        work_.reset();
        if (thread_.joinable()) thread_.join();
    }

    void start_timer(std::shared_ptr<ReactorOp> op, clock::time_point deadline) override {
        asio::post(io_, [this, op = std::move(op), deadline]() mutable {
            // Sticky cancel (ADR-237 round-2 M2): a cancel that arrived before this start never lets the
            // operation begin. Shutdown counts as a cancel.
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
                // The completion reports what actually happened (round-1 M1): an expiry that won the race
                // against a cancel is still `completed`.
                op->on_complete(ec ? op_status::canceled : op_status::completed);
            });
        });
    }

    [[nodiscard]] bool on_reactor_thread() const noexcept override {
        return std::this_thread::get_id() == thread_id_.load(std::memory_order_acquire);
    }

protected:
    void post_cancel(std::shared_ptr<ReactorOp> const& op) override {
        asio::post(io_, [this, op] { cancel_on_reactor(op); });
    }

private:
    // Reactor thread only. Not started yet: the sticky flag stops it at start. Already completed: nothing.
    void cancel_on_reactor(std::shared_ptr<ReactorOp> const& op) {
        if (!pending_.contains(op)) return;
        auto* state = static_cast<TimerState*>(op->backend_state.get());
        if (state != nullptr) state->timer.cancel();
    }

    asio::io_context                                         io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::unordered_set<std::shared_ptr<ReactorOp>>           pending_;  // reactor thread only
    bool                                                     shutting_down_ = false;  // reactor thread only
    std::atomic<std::thread::id>                             thread_id_{};
    std::thread                                              thread_;  // joined in the destructor, never detached
};

}  // namespace

std::unique_ptr<Reactor> make_default_reactor() { return std::make_unique<AsioReactor>(); }

}  // namespace agentengine::pal
