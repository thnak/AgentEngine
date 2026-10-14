#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.5 (structured concurrency: rule 2 "no
// detached tasks", rule 3 "with_scope only ... when_all: on the first failure, request stop on every sibling, join
// all of them, then propagate") and §4.3 (parallel children are child strands of the session; the concurrency cap
// is a semaphore on the scope; results appended in emitted order, I5) -- step 5 of the ADR's implementation order
// (§14).
//
//   auto [a, b] = co_await rt::when_all(fetch_a(), fetch_b());              // fixed arity, tuple, emitted order
//   std::vector<R> rs = co_await rt::when_all(std::move(tasks), {.max_concurrency = 4});   // range form
//   co_await rt::with_scope([&](rt::task_scope& s) -> task<void> {           // spawn as you go
//       for (auto& c : calls) s.spawn(run_one(c));
//       co_return;
//   });                                                                       // every child joined here
//
// THE RULES, each with a test in tests/rt/test_rt_scope.cpp:
//   - A scope is opened only ON A STRAND (`rt.scope_off_strand` otherwise): its children run as CHILD STRANDS of the
//     owner's strand group -- the same "session" for scheduling (rt/lanes.hpp), so a 50-child batch gets the
//     session's share, not 50 shares. Each child is a fresh strand: children run in parallel on the lanes, each
//     one serial in itself.
//   - JOIN. The owner resumes on ITS OWN strand only after EVERY child finished: the last child to finish posts it
//     there (one cross-strand post per join, never a transfer onto the child's lane slice -- §4.2). A child is
//     "finished" after its task's frame is destroyed, so everything it held (a lock guard, a quota ticket) has been
//     released before the owner runs.
//   - NO CHILD OUTLIVES ITS SCOPE. A scope exists only inside `with_scope` / `when_all`, which join before
//     returning OR rethrowing (C++ has no async destructor, so a scope object that could be unwound over live
//     children is not offered). Destroying a scope with live children -- only possible by destroying a suspended
//     owner frame from outside -- is a checked violation (abort, every build).
//   - FAILURE. A child that throws records the first exception and requests stop on the scope's stop source; the
//     siblings observe it (`rt::scope_stop_token()`, or `task_scope::stop_token()`) and finish -- canceled, two-
//     phase: nothing is torn down under them -- and only after all of them finished is the first exception
//     rethrown to the owner. A body (`with_scope`) that throws does the same. A stop requested on the owner's own
//     scope token (or ScopeOptions::stop) is forwarded to the children.
//   - ORDER (I5). `when_all` returns results in the order the tasks were given, whatever order they finished in.
//   - CAP. `ScopeOptions::max_concurrency` (0 = unbounded) is a semaphore on the scope: at most that many children
//     run; the rest wait, unstarted, in spawn order.
//   - AWAIT CHAINS (§4.3, rt/await_chain.hpp). Each child is a fresh holder id forked from the owner's: an AWAITED
//     edge for `when_all` (the owner is suspended in the join from the start), and for `with_scope` while the owner
//     is suspended in its join. A scope opened with `fresh_chains` (a job runner's background scope) starts its
//     children on fresh, unlinked chains instead: they wait for a lock like any other caller.
//
// Exceptions here are the coroutine's own channel (a child's exception is what `when_all` propagates); values that
// encode failure (`result<T>`) are results like any other and cancel nothing.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/lanes.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow ScopeOptions — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct ScopeOptions {
    std::size_t     max_concurrency = 0;  // 0: unbounded
    std::stop_token stop{};               // an extra stop to forward to the children (besides the owner's scope)
    bool            fresh_chains = false; // children start fresh await chains (a job runner's background scope)
};

// ae-naming-lint: allow task_scope — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class task_scope;

namespace scope_detail {

struct ScopeAccess;

// The wrapper every child runs in: lazily started, posted to its own strand, frees itself when its body ends.
struct ChildFrame {
    struct promise_type {
        ChildFrame get_return_object() noexcept {
            return ChildFrame{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }  // the body catches everything
    };
    std::coroutine_handle<promise_type> h;
};

// A child that has not been started (the cap is reached).
struct Pending {
    std::coroutine_handle<> handle{};
    Strand                  strand{};
    std::uint64_t           holder = 0;
};

inline ChildFrame run_child(task_scope* scope, std::uint64_t holder, task<void> t);

template <class T>
task<void> discard(task<T> t) {
    (void)co_await t;
}

}  // namespace scope_detail

// ae-naming-lint: allow task_scope — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class task_scope {
public:
    // Must be constructed on a strand (the owner's); see the file comment. Prefer `with_scope` / `when_all`.
    explicit task_scope(ScopeOptions opts = {})
        : owner_(Strand::current()), owner_holder_(current_holder_id()), opts_(std::move(opts)) {
        if (!owner_.valid()) {
            throw std::logic_error("rt.scope_off_strand: a task scope is opened only on a strand (ADR-237 §4.5)");
        }
        // Forward the owner's own scope stop and the caller's extra stop to this scope's children. Each body only
        // requests stop on this scope's source: a signal, whose own callbacks are reactor cancels and strand posts.
        std::stop_token const parent = chain_detail::Registry::instance().stop_of(owner_holder_);
        if (parent.stop_possible()) from_parent_.emplace(parent, Forward{source_});
        if (opts_.stop.stop_possible()) from_options_.emplace(opts_.stop, Forward{source_});
    }

    task_scope(task_scope const&)            = delete;
    task_scope& operator=(task_scope const&) = delete;

    ~task_scope() {
        bool live = false;
        {
            std::lock_guard lock(m_);
            live = live_ != 0;
        }
        if (live) {
            detail::checked_violation("rt::task_scope destroyed with live children (§4.5 rule 3: no child outlives "
                                      "its scope)");
        }
    }

    // Starts `t` as a child on a new child strand (or queues it behind the cap). Must be called on the owner's
    // strand, before the scope is joined for the last time. A non-void result is discarded (`when_all` keeps them).
    template <class T>
    void spawn(task<T> t) {
        if constexpr (std::is_void_v<T>) {
            spawn_void(std::move(t));
        } else {
            spawn_void(scope_detail::discard(std::move(t)));
        }
    }

    // The scope's stop token: stopped on the first child failure, a throwing body, `request_stop()`, or a forwarded
    // stop. Children also see it as `rt::scope_stop_token()`.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return source_.get_token(); }
    void request_stop() noexcept { source_.request_stop(); }

    // `co_await scope.join()`: resumes, on the owner's strand, once every child spawned so far finished.
    class JoinAwaiter {
    public:
        explicit JoinAwaiter(task_scope& s) noexcept : s_(&s) {}
        [[nodiscard]] bool await_ready() const {
            if (!s_->owner_.is_current()) {
                throw std::logic_error("rt.scope_join_off_owner: a task scope is joined on its owner's strand");
            }
            std::lock_guard lock(s_->m_);
            return s_->live_ == 0;
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard lock(s_->m_);
            if (s_->live_ == 0) return false;  // the last child finished meanwhile
            s_->waiter_        = h;
            s_->waiter_holder_ = current_holder_id();
            s_->owner_awaiting_.store(true, std::memory_order_release);  // its children's edges are awaited now
            return true;
        }
        void await_resume() const noexcept {
            if (!s_->always_awaited_) s_->owner_awaiting_.store(false, std::memory_order_release);
        }

    private:
        task_scope* s_;
    };
    [[nodiscard]] JoinAwaiter join() noexcept { return JoinAwaiter(*this); }

    // Children started or queued and not finished.
    [[nodiscard]] std::size_t live() const {
        std::lock_guard lock(m_);
        return live_;
    }
    // The first exception a child threw, if any.
    [[nodiscard]] std::exception_ptr first_fault() const {
        std::lock_guard lock(m_);
        return first_fault_;
    }
    // Diagnostics: the most children that ran at once, and how many joins resumed the owner by a post.
    [[nodiscard]] std::size_t peak_running() const {
        std::lock_guard lock(m_);
        return peak_running_;
    }
    [[nodiscard]] std::size_t join_posts() const {
        std::lock_guard lock(m_);
        return join_posts_;
    }

private:
    friend struct scope_detail::ScopeAccess;
    friend scope_detail::ChildFrame scope_detail::run_child(task_scope*, std::uint64_t, task<void>);

    struct Forward {
        mutable std::stop_source target;  // request_stop() is non-const in some standard libraries
        void operator()() const noexcept { target.request_stop(); }
    };

    void spawn_void(task<void> t) {
        if (!owner_.is_current()) {
            throw std::logic_error("rt.scope_spawn_off_owner: children are spawned on the scope owner's strand");
        }
        if (!t.valid()) throw std::invalid_argument("rt::task_scope::spawn: empty task");
        std::uint64_t const child = mint_holder_id();
        auto&               chains = chain_detail::Registry::instance();
        chains.add(child, opts_.fresh_chains
                              ? chain_detail::Link{0, nullptr, source_.get_token(), 0}
                              : chain_detail::Link{owner_holder_, &owner_awaiting_, source_.get_token(), 0});
        scope_detail::ChildFrame frame{};
        Strand                   strand;
        try {
            frame  = scope_detail::run_child(this, child, std::move(t));
            strand = owner_.sibling();
        } catch (...) {
            if (frame.h) frame.h.destroy();
            (void)chains.remove(child);
            throw;
        }
        bool start = false;
        {
            std::lock_guard lock(m_);
            ++live_;
            if (opts_.max_concurrency == 0 || running_ < opts_.max_concurrency) {
                ++running_;
                if (running_ > peak_running_) peak_running_ = running_;
                start = true;
            } else {
                pending_.push_back(scope_detail::Pending{frame.h, std::move(strand), child});
            }
        }
        if (start) strand.post(frame.h, child);
    }

    // A child threw: remember the first exception and stop the siblings (two-phase: they finish on their own).
    void note_fault(std::exception_ptr e) noexcept {
        {
            std::lock_guard lock(m_);
            if (!first_fault_) first_fault_ = std::move(e);
        }
        source_.request_stop();
    }

    // The last touch of a finishing child on this scope: its frame's task was already destroyed. Once `m_` is
    // released the owner may run and destroy the scope; only locals are used after that.
    void child_done(std::uint64_t holder) noexcept {
        if (chain_detail::Registry::instance().remove(holder) != 0) {
            detail::checked_violation("an AsyncMutex loan was still outstanding when its borrower (a task scope "
                                      "child) finished (§4.3)");
        }
        scope_detail::Pending           next{};
        std::coroutine_handle<>         owner{};
        std::uint64_t                   owner_holder = 0;
        std::shared_ptr<detail::StrandHome> owner_home;
        {
            std::lock_guard lock(m_);
            --running_;
            --live_;
            if (!pending_.empty()) {
                next = std::move(pending_.front());
                pending_.pop_front();
                ++running_;
                if (running_ > peak_running_) peak_running_ = running_;
            }
            if (live_ == 0 && waiter_) {
                owner        = std::exchange(waiter_, {});
                owner_holder = waiter_holder_;
                owner_home   = owner_.home();
                ++join_posts_;
            }
        }
        if (next.handle) next.strand.post(next.handle, next.holder);
        if (owner) owner_home->post(owner, owner_holder, false);  // §4.2: one cross-strand post per join
    }

    Strand                     owner_;
    std::uint64_t              owner_holder_;
    ScopeOptions               opts_;
    std::stop_source           source_;
    std::atomic<bool>          owner_awaiting_{false};
    bool                       always_awaited_ = false;  // when_all: the owner is in the join from the start

    mutable std::mutex                 m_;
    std::size_t                        live_         = 0;
    std::size_t                        running_      = 0;
    std::size_t                        peak_running_ = 0;
    std::size_t                        join_posts_   = 0;
    std::deque<scope_detail::Pending>  pending_;
    std::coroutine_handle<>            waiter_{};
    std::uint64_t                      waiter_holder_ = 0;
    std::exception_ptr                 first_fault_{};

    // Declared last: destroyed first, so no forwarded stop can reach a half-destroyed scope.
    std::optional<std::stop_callback<Forward>> from_parent_;
    std::optional<std::stop_callback<Forward>> from_options_;
};

namespace scope_detail {

struct ScopeAccess {
    // when_all: the owner is suspended in the join for the scope's whole life, so its children's edges are awaited
    // from the first moment (a child may ask for a lock before the owner reaches the join).
    static void mark_always_awaited(task_scope& s) noexcept {
        s.always_awaited_ = true;
        s.owner_awaiting_.store(true, std::memory_order_release);
    }
};

// The child's body. `t` is destroyed (its frame and every guard it held released) BEFORE the scope learns the child
// finished; `child_done` is the last statement, after which this frame only frees itself.
inline ChildFrame run_child(task_scope* scope, std::uint64_t holder, task<void> t) {
    {
        task<void> body = std::move(t);
        try {
            co_await body;
        } catch (...) {
            scope->note_fault(std::current_exception());
        }
    }
    scope->child_done(holder);
}

template <class T>
using value_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template <class T>
task<void> store_into(task<T> t, std::optional<value_t<T>>* slot) {
    if constexpr (std::is_void_v<T>) {
        co_await t;
        slot->emplace();
    } else {
        slot->emplace(co_await t);
    }
}

template <class Slots, class... Ts, std::size_t... I>
void spawn_all(task_scope& scope, Slots& slots, std::index_sequence<I...> /*unused*/, task<Ts>&... ts) {
    (scope.spawn(store_into(std::move(ts), &std::get<I>(slots))), ...);
}

template <class>
struct task_value;
template <class T>
struct task_value<task<T>> {
    using type = T;
};

template <class R, class F>
task<R> with_scope_impl(F fn, ScopeOptions opts) {
    task_scope                         scope(std::move(opts));
    std::exception_ptr                 body_fault;
    std::optional<value_t<R>>          out;
    try {
        if constexpr (std::is_void_v<R>) {
            co_await fn(scope);
        } else {
            out.emplace(co_await fn(scope));
        }
    } catch (...) {
        body_fault = std::current_exception();
        scope.request_stop();
    }
    co_await scope.join();  // every child, before returning or rethrowing
    if (body_fault) std::rethrow_exception(body_fault);
    if (std::exception_ptr const f = scope.first_fault()) std::rethrow_exception(f);
    if constexpr (!std::is_void_v<R>) co_return std::move(*out);
}

}  // namespace scope_detail

// Runs `fn(scope)` (a callable returning `task<R>`) with a fresh task scope, joins every child the body spawned, and
// returns the body's value -- or rethrows the body's exception, else the first child's (file comment). `fn` is
// kept in this coroutine's frame for the whole call, so a lambda capturing by reference is safe here.
// ae-naming-lint: allow with_scope — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
template <class F>
[[nodiscard]] auto with_scope(F fn, ScopeOptions opts = {})
    -> task<typename scope_detail::task_value<std::invoke_result_t<F&, task_scope&>>::type> {
    using R = typename scope_detail::task_value<std::invoke_result_t<F&, task_scope&>>::type;
    return scope_detail::with_scope_impl<R>(std::move(fn), std::move(opts));
}

// Runs every task as a child (file comment) and returns their values in the order given (void -> monostate). On
// the first exception the siblings are stopped, ALL are joined, then it is rethrown.
// ae-naming-lint: allow when_all — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
template <class... Ts>
[[nodiscard]] task<std::tuple<scope_detail::value_t<Ts>...>> when_all(task<Ts>... ts) {
    std::tuple<std::optional<scope_detail::value_t<Ts>>...> slots;
    task_scope                                              scope;
    scope_detail::ScopeAccess::mark_always_awaited(scope);
    std::exception_ptr spawn_fault;
    try {
        scope_detail::spawn_all(scope, slots, std::index_sequence_for<Ts...>{}, ts...);
    } catch (...) {
        spawn_fault = std::current_exception();
        scope.request_stop();
    }
    co_await scope.join();
    if (spawn_fault) std::rethrow_exception(spawn_fault);
    if (std::exception_ptr const f = scope.first_fault()) std::rethrow_exception(f);
    co_return std::apply(
        [](auto&... s) { return std::tuple<scope_detail::value_t<Ts>...>(std::move(*s)...); }, slots);
}

// The range form: `tasks` run as children (at most `opts.max_concurrency` at once); results in the order given.
template <class T>
[[nodiscard]] task<std::vector<scope_detail::value_t<T>>> when_all(std::vector<task<T>> tasks, ScopeOptions opts = {}) {
    std::vector<std::optional<scope_detail::value_t<T>>> slots(tasks.size());
    task_scope                                           scope(std::move(opts));
    scope_detail::ScopeAccess::mark_always_awaited(scope);
    std::exception_ptr spawn_fault;
    try {
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            scope.spawn(scope_detail::store_into(std::move(tasks[i]), &slots[i]));
        }
    } catch (...) {
        spawn_fault = std::current_exception();
        scope.request_stop();
    }
    co_await scope.join();
    if (spawn_fault) std::rethrow_exception(spawn_fault);
    if (std::exception_ptr const f = scope.first_fault()) std::rethrow_exception(f);
    std::vector<scope_detail::value_t<T>> out;
    out.reserve(slots.size());
    for (auto& s : slots) out.push_back(std::move(*s));
    co_return out;
}

}  // namespace agentengine::rt
