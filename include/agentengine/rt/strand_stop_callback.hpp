#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.4 ("per-operation stop sources", red-team
// M5: a deadline's stop "is posted to the owning strand, never run on the reactor thread") and §4.2 ("timer-driven
// stops are posted to the owning strand ... every existing stop_callback body is reduced to signal / enqueue") --
// step 5 of the ADR's implementation order (§14).
//
// `std::stop_source::request_stop()` runs every registered `std::stop_callback` INLINE, on the requesting thread --
// a host thread, the reactor thread (a deadline timer), another session's lane. An engine callback that does real
// work there runs engine code off its strand (I1) or on the reactor (§4.2). `rt::StrandStopCallback` is the
// replacement: registered like a `std::stop_callback`, but its body is POSTED to the home that was current when it
// was registered -- the owning strand for engine work (or the block_on home / host Resumer, by the precedence of
// rt/resume_home.hpp) -- and runs there, as a continuation of that strand, under the registering task's holder id.
// The only thing that runs on the requesting thread is the post.
//
//   rt::StrandStopCallback cb(token, [&] { session.settle_canceled(); });   // on the session's strand
//
// Semantics, matching `std::stop_callback` where they can:
//   - registered when constructed; a token already stopped posts at once (never runs inline);
//   - at most once;
//   - after the destructor returns the body never STARTS: a post already queued finds it disarmed and does
//     nothing (the strand is serial, so a destructor running on the owning strand and the posted body never
//     overlap). A destructor running on another thread while the body runs does not wait for it -- unlike
//     `std::stop_callback` -- so the body must not touch state the destructor's caller frees without the strand's
//     ordering; engine code destroys its callbacks on its own strand.
//   - homeless registration (no strand, no block_on, no Resumer) falls back to running the body inline on the
//     requesting thread: there is nowhere to post it. Engine work always has a home (§4.2).
// Residual: a post made after the lane pool stopped is dropped (rt/lanes.hpp) and the body never runs; its small
// frame is not freed.
//
// `detail::HomedCall` is the mechanism, also used for the per-attempt connect deadline of rt/dns.hpp (§14 step 4's
// recorded deviation, moved onto it in step 5): a one-shot call armed on the current home, fired from any thread.

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

namespace detail {

class HomedCall;

// The posted frame: lazily started, frees itself when its body ends (final_suspend never suspends, so no code
// touches the frame after it is freed). Never awaited.
struct HomedCallFrame {
    struct promise_type {
        HomedCallFrame get_return_object() noexcept {
            return HomedCallFrame{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }  // the body catches everything
    };
    std::coroutine_handle<promise_type> h;
};

// A one-shot call to `fn`, armed on the home current at `arm()` and posted there by `fire()`.
class HomedCall : public std::enable_shared_from_this<HomedCall> {
public:
    // Captures the current home (may allocate a block_on home or a Resumer ticket, and throw -- at arm time only).
    [[nodiscard]] static std::shared_ptr<HomedCall> arm(std::move_only_function<void()> fn) {
        auto call   = std::shared_ptr<HomedCall>(new HomedCall(std::move(fn)));  // NOLINT: private constructor
        call->frame_ = run(call.get()).h;
        call->parked_ = capture_parked(call->frame_);
        return call;
    }

    HomedCall(HomedCall const&)            = delete;
    HomedCall& operator=(HomedCall const&) = delete;
    ~HomedCall() {
        // Never fired: the frame never ran; free it here. Fired: the frame owns itself (and kept us alive until its
        // body ended, so this destructor runs from that body or after it).
        if (frame_ && state_.load(std::memory_order_acquire) == kArmed) frame_.destroy();
    }

    // Any thread, any number of times; posts the body to its home at most once. Must be called through a live
    // shared_ptr (the caller's keeps this object alive until the post has been made).
    void fire() noexcept {
        int expected = kArmed;
        if (!state_.compare_exchange_strong(expected, kFired, std::memory_order_acq_rel)) return;
        self_ = shared_from_this();  // the posted frame keeps the call alive until its body ran
        detail::ParkedResumer r = std::move(parked_);
        detail::wake(std::move(r));  // posts to the strand / block_on home / Resumer; homeless: runs inline
    }

    // The owner: the body never starts after this returns (a post already queued does nothing).
    void disarm() noexcept { live_.store(false, std::memory_order_release); }

    [[nodiscard]] bool fired() const noexcept { return state_.load(std::memory_order_acquire) == kFired; }

private:
    static constexpr int kArmed = 0;
    static constexpr int kFired = 1;

    explicit HomedCall(std::move_only_function<void()> fn) noexcept : fn_(std::move(fn)) {}

    static HomedCallFrame run(HomedCall* call) {
        std::shared_ptr<HomedCall> const keep = std::move(call->self_);  // set by fire() before the post
        if (keep->live_.load(std::memory_order_acquire)) {
            try {
                keep->fn_();
            } catch (...) {  // NOLINT(bugprone-empty-catch): a stop callback must not throw (std::stop_callback)
                std::terminate();
            }
        }
        co_return;
    }

    std::move_only_function<void()> fn_;
    std::coroutine_handle<>         frame_{};
    detail::ParkedResumer           parked_{};
    std::shared_ptr<HomedCall>      self_{};
    std::atomic<int>                state_{kArmed};
    std::atomic<bool>               live_{true};
};

}  // namespace detail

// A `std::stop_callback` whose body runs on the registering task's home, never on the requesting thread (file
// comment). `F` is invoked with no arguments.
template <class F>
// ae-naming-lint: allow StrandStopCallback — ADR-237 §4.4: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class StrandStopCallback {
public:
    StrandStopCallback(std::stop_token const& token, F fn)
        : call_(detail::HomedCall::arm(std::move(fn))) {
        cb_.emplace(token, Fire{call_});  // a token already stopped fires here: a post, never the body
    }
    ~StrandStopCallback() {
        cb_.reset();       // no further fire() (std::stop_callback waits for a running one; it only posts)
        call_->disarm();   // a post already queued finds it disarmed
    }
    StrandStopCallback(StrandStopCallback const&)            = delete;
    StrandStopCallback& operator=(StrandStopCallback const&) = delete;

    // True once the stop was requested and the body posted (not necessarily run yet).
    [[nodiscard]] bool fired() const noexcept { return call_->fired(); }

private:
    struct Fire {
        std::shared_ptr<detail::HomedCall> call;
        void operator()() const noexcept { call->fire(); }
    };
    std::shared_ptr<detail::HomedCall>       call_;
    std::optional<std::stop_callback<Fire>> cb_;
};

template <class F>
StrandStopCallback(std::stop_token const&, F) -> StrandStopCallback<F>;

}  // namespace agentengine::rt
