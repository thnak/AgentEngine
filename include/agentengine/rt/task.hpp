#pragma once
// ADR-037 Phase 1: `agentengine::rt::task<T>`, the first piece of AgentEngine's own runtime
// substrate, replacing `quark::task<T>` (currently aliased verbatim in `core/task.hpp`). Lives under
// `agentengine::rt` — a NEW namespace, deliberately not yet wired into `core/task.hpp`'s existing
// `ae::task<T>` alias, so nothing in the live, Quark-based build is touched by this file existing.
// Phase 2 promotes this type into `core/task.hpp`'s place once `AgentSession`/`WorkflowSupervisor`
// stop deriving from `quark::Actor` (the two changes are coupled: Quark's own dispatch checks the
// EXACT type `quark::task<>` to recognize an async handler, so swapping the alias away today would
// break every `handle()` override before anything else in this migration is ready).
//
// Modeled closely on `quark::task<T>`'s own design (a well-understood, widely-implemented C++20
// pattern — lazy start, symmetric-transfer final suspend, exception containment at the coroutine
// boundary) with ONE deliberate simplification: Quark splits `task<void>`/`task<T>` into two
// specializations because its OWN dispatcher needs to detect "is this handler async" by checking the
// EXACT type `task<void>` (dispatch.hpp's `async_handler` concept) — a constraint specific to
// Quark's jump-table dispatch, not a general requirement. This type keeps the same two-specialization
// SHAPE (a `task<void>` full specialization + a `task<T>` primary template) because the storage need
// genuinely differs (no value to hold for void), but BOTH specializations here support the same two
// use modes uniformly: (a) `co_await`ed by a parent coroutine (symmetric transfer via a recorded
// `continuation_`), and (b) driven directly by an executor via `start()`/`resume()`/`done()` — Quark's
// `task<void>` only supports mode (b) and `task<T>` only supports mode (a); this type supports both
// from either specialization, since AgentEngine's own future executor (not yet built) needs handler-
// level tasks to be awaitable by OTHER handler-level tasks (e.g. a workflow executor awaiting a
// spawned child run) without forcing every intermediate layer to return a value.
//
// Exception containment: a throw anywhere in the coroutine body is caught by `unhandled_exception()`
// (never `std::terminate`) and either rethrown at the awaiting `co_await` (mode a) or left probable
// via `faulted()`/`fault_ptr()` for whatever drives it directly (mode b) — the same ADR-009-style
// handler-boundary guard `quark::task<>` already established, reproduced here rather than assumed.
//
// decisions/ADR-237 §4.2 (step 3) -- FINAL_SUSPEND ACROSS STRANDS. Symmetric transfer to the awaiter is
// correct only when the child and the awaiter run on the SAME strand: transferring across strands would run
// the awaiter on the child's lane slice while the awaiter's own strand may be running another continuation
// (I1). The promise therefore carries the awaiter's home (`detail::TaskLink`): the strand it was running on
// when it awaited (read from the lane worker's execution context, rt/resume_home.hpp). At final_suspend the
// child compares it with the strand it is finishing on: equal -- or no strand at all, every pre-lanes caller
// -- transfers as before; different -- posts the awaiter to its own strand and returns noop_coroutine() (one
// post per cross-strand join, so recursion stays bounded, round 2 G-a). A root started by `rt::Runtime`
// (`run`, `enter`) has no awaiting coroutine of this type; its completion is a callback set in the same
// link, run from the suspended final point as the last touch on the frame.

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

template <class T = void>
// ae-naming-lint: allow task — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class task;

namespace detail {

// ADR-237 §4.2: what a task's promise knows about where its awaiter continues.
struct TaskLink {
    // The strand the awaiter was running on when it co_awaited this task (null: not on a strand). A raw
    // pointer: the awaiter is a suspended continuation of that strand, and whoever arranged a cross-strand
    // await (`rt::on_strand`) holds a strong reference until the join.
    StrandHome*   awaiter_strand = nullptr;
    std::uint64_t awaiter_holder = 0;  // the holder id the awaiter resumes under when posted
    // A root's completion (rt::Runtime): called instead of resuming a continuation. Runs at the suspended
    // final point; must not touch the frame after handing the result off (the frame may be destroyed at once).
    void (*on_done)(void* ctx) noexcept = nullptr;
    void*  on_done_ctx                   = nullptr;
};

// The final_suspend decision, shared by both specializations (file comment).
[[nodiscard]] inline std::coroutine_handle<> complete_to(std::coroutine_handle<> continuation,
                                                       TaskLink const& link) noexcept {
    if (link.on_done != nullptr) {
        auto* const fn  = link.on_done;
        void* const ctx = link.on_done_ctx;
        fn(ctx);  // last touch: everything it needs was copied out of the frame first
        return std::noop_coroutine();
    }
    if (!continuation) return std::noop_coroutine();
    StrandHome* const here = current_execution().strand;
    if (link.awaiter_strand == nullptr || link.awaiter_strand == here) return continuation;  // same strand
    link.awaiter_strand->post(continuation, link.awaiter_holder, false);  // cross-strand: post, never transfer
    return std::noop_coroutine();
}

// Records the awaiter's home in a task's link (both specializations' await_suspend).
inline void link_awaiter(TaskLink& link) noexcept {
    link.awaiter_strand = current_execution().strand;
    link.awaiter_holder = current_holder_id();
}

// rt::Runtime and rt::on_strand start a task on a strand themselves: they need its frame and its link.
struct TaskAccess {
    template <class Task>
    [[nodiscard]] static auto handle(Task& t) noexcept {
        return t.h_;
    }
};

}  // namespace detail

template <>
class task<void> {
public:
    struct promise_type {
        [[nodiscard]] task<void> get_return_object() noexcept {
            return task<void>{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Lazy: the frame starts suspended so nothing runs until something explicitly starts it
        // (start()/resume(), mode b) or awaits it (await_suspend, mode a).
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        // Symmetric transfer to whoever is awaiting this task, or a no-op (never a bare .resume()
        // hop) if nobody is -- mode (b)'s driver observes done()/faulted() after its own resume()
        // call returns rather than being transferred to.
        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() noexcept { return false; }
            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                return detail::complete_to(h.promise().continuation_, h.promise().link_);  // ADR-237 §4.2
            }
            void await_resume() noexcept {}
        };
        [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { fault_ = std::current_exception(); }
        [[nodiscard]] bool faulted() const noexcept { return static_cast<bool>(fault_); }
        [[nodiscard]] std::exception_ptr fault_ptr() const noexcept { return fault_; }

        std::coroutine_handle<> continuation_{};
        std::exception_ptr fault_{};
        std::uint64_t raw_holder_ = 0;  // ADR-175: this task's holder id when driven by raw resume()
        detail::TaskLink link_{};       // ADR-237 §4.2: the awaiter's home
    };

    task() noexcept = default;
    explicit task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (h_) h_.destroy();
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }
    ~task() {
        if (h_) h_.destroy();
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(h_); }

    // Mode (b): an executor starts/resumes this frame directly, with no parent coroutine awaiting
    // it. Safe to call repeatedly until done() -- each resume() runs the body until its next
    // suspension point or completion. `start()` is `resume()` under a name that reads correctly at
    // the FIRST call site; both do the same thing.
    // decisions/ADR-175: a raw resume() drives this task outside any block_on(), so it runs with NO home
    // (a lock grant resumes it inline, as before ADR-175) and under ITS OWN holder id, minted on first
    // resume -- not the calling thread's context. Otherwise a task raw-resumed inside a pool job or tool
    // closure would park homed to a block_on() that may have returned by the time the lock is granted, and
    // a homeless task that parks on one thread and holds a lock on another would lend its identity to the
    // thread it parked from (ADR-175 round 3 findings 2 and 3).
    void resume() {
        auto& p = h_.promise();
        if (p.raw_holder_ == 0) p.raw_holder_ = mint_holder_id();
        // ADR-219: a host resumer, unlike a block_on() home, is kept: it never closes, and it is how a host
        // driving this task from its own executor gets the task's continuations back.
        // ADR-237 §4.3: and so is a strand -- a task raw-resumed inside a strand slice is part of that slice.
        ScopedExecution const raw(nullptr, p.raw_holder_, detail::current_execution().resumer,
                                  detail::current_execution().strand);
        h_.resume();
    }
    void start() { resume(); }
    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }
    [[nodiscard]] bool faulted() const noexcept { return h_ && h_.promise().faulted(); }
    [[nodiscard]] std::exception_ptr fault_ptr() const noexcept {
        return h_ ? h_.promise().fault_ptr() : nullptr;
    }

    // Mode (a): co_await protocol -- task<void> IS its own awaiter, symmetric transfer starts the
    // (lazy) frame in the same step the awaiter suspends, no scheduler hop.
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        h_.promise().continuation_ = awaiting;
        detail::link_awaiter(h_.promise().link_);  // ADR-237 §4.2
        return h_;
    }
    void await_resume() {
        if (h_.promise().fault_) std::rethrow_exception(h_.promise().fault_);
    }

private:
    friend struct detail::TaskAccess;
    std::coroutine_handle<promise_type> h_{};
};

// --- task<T>, T != void: adds inline value storage to the void specialization's shape above. -----
template <class T>
// ae-naming-lint: allow task — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class task {
public:
    struct promise_type {
        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() noexcept { return false; }
            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                return detail::complete_to(h.promise().continuation_, h.promise().link_);  // ADR-237 §4.2
            }
            void await_resume() noexcept {}
        };
        [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }

        // Inline storage for the co_returned T -- zero extra heap allocation beyond the compiler-
        // managed coroutine frame itself, gated by has_value_ (matching quark::task<T>'s identical
        // manually-managed-storage idiom, itself matching quark::detail::ReplyCell<R>'s).
        template <class U>
            requires std::constructible_from<T, U&&>
        void return_value(U&& v) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            std::construct_at(value_ptr(), std::forward<U>(v));
            has_value_ = true;
        }
        void unhandled_exception() noexcept { fault_ = std::current_exception(); }
        [[nodiscard]] bool faulted() const noexcept { return static_cast<bool>(fault_); }
        [[nodiscard]] std::exception_ptr fault_ptr() const noexcept { return fault_; }

        [[nodiscard]] T* value_ptr() noexcept {
            return std::launder(static_cast<T*>(static_cast<void*>(store_)));
        }

        promise_type() noexcept = default;
        ~promise_type() {
            if (has_value_) value_ptr()->~T();
        }

        std::coroutine_handle<> continuation_{};
        std::exception_ptr fault_{};
        std::uint64_t raw_holder_ = 0;  // ADR-175: this task's holder id when driven by raw resume()
        detail::TaskLink link_{};       // ADR-237 §4.2: the awaiter's home
        alignas(T) unsigned char store_[sizeof(T)];
        bool has_value_ = false;
    };

    task() noexcept = default;
    explicit task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

    task(task const&) = delete;
    task& operator=(task const&) = delete;
    task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (h_) h_.destroy();
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }
    ~task() {
        if (h_) h_.destroy();
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(h_); }

    // Mode (b): direct driving. `await_resume()`-equivalent for a non-awaited caller: `take_value()`
    // moves the result out (or rethrows a fault) -- named distinctly from await_resume() since a
    // mode-(b) driver calls resume() in a loop until done(), THEN takes the value once, whereas
    // await_resume() is only ever called by the compiler-generated co_await machinery.
    // decisions/ADR-175: a raw resume() drives this task outside any block_on(), so it runs with NO home
    // (a lock grant resumes it inline, as before ADR-175) and under ITS OWN holder id, minted on first
    // resume -- not the calling thread's context. Otherwise a task raw-resumed inside a pool job or tool
    // closure would park homed to a block_on() that may have returned by the time the lock is granted, and
    // a homeless task that parks on one thread and holds a lock on another would lend its identity to the
    // thread it parked from (ADR-175 round 3 findings 2 and 3).
    void resume() {
        auto& p = h_.promise();
        if (p.raw_holder_ == 0) p.raw_holder_ = mint_holder_id();
        // ADR-219: a host resumer, unlike a block_on() home, is kept: it never closes, and it is how a host
        // driving this task from its own executor gets the task's continuations back.
        // ADR-237 §4.3: and so is a strand -- a task raw-resumed inside a strand slice is part of that slice.
        ScopedExecution const raw(nullptr, p.raw_holder_, detail::current_execution().resumer,
                                  detail::current_execution().strand);
        h_.resume();
    }
    void start() { resume(); }
    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }
    [[nodiscard]] bool faulted() const noexcept { return h_ && h_.promise().faulted(); }
    [[nodiscard]] std::exception_ptr fault_ptr() const noexcept {
        return h_ ? h_.promise().fault_ptr() : nullptr;
    }
    [[nodiscard]] T take_value() {
        promise_type& p = h_.promise();
        if (p.fault_) std::rethrow_exception(p.fault_);
        return std::move(*p.value_ptr());
    }

    // Mode (a): co_await protocol, symmetric transfer.
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        h_.promise().continuation_ = awaiting;
        detail::link_awaiter(h_.promise().link_);  // ADR-237 §4.2
        return h_;
    }
    [[nodiscard]] T await_resume() {
        promise_type& p = h_.promise();
        if (p.fault_) std::rethrow_exception(p.fault_);
        return std::move(*p.value_ptr());
    }

private:
    friend struct detail::TaskAccess;
    std::coroutine_handle<promise_type> h_{};
};

}  // namespace agentengine::rt
