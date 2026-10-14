#pragma once
// ADR-237 §4.2/§4.3, ADR-175, ADR-219: the per-thread EXECUTION CONTEXT -- which home a coroutine parking on this
// thread records (block_on slot, host Resumer, strand) and which holder it runs for. The design is in
// rt/resume_home.hpp's file comment; this is the part of it that rt/task.hpp needs, split out so the ~370 TUs
// that only use `task<T>` do not also parse (and rebuild for edits to) the parking and wake machinery.

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <utility>

namespace agentengine::rt {

[[nodiscard]] inline std::uint64_t mint_holder_id() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

class Resumer;

namespace detail {

class CallerHome;

// ADR-237 §4.3: a strand seen as a home -- where a parked engine coroutine is posted when it wakes. The one
// implementation is rt/lanes.hpp's strand core; this base exists so the wake path here needs no include of
// the scheduler (the same type-erasure level as `Resumer`: one indirect call per wake-up). `post` only
// enqueues; it never runs the coroutine on the caller's thread. Shared ownership: a parked record holds a
// strong reference, so a strand outlives every continuation queued for it.
class StrandHome : public std::enable_shared_from_this<StrandHome> {
public:
    virtual ~StrandHome() = default;
    // `priority`: a cancel/approval resume -- picked before other strands, never reordered within this one.
    virtual void post(std::coroutine_handle<> h, std::uint64_t holder, bool priority) noexcept = 0;
    // A process-unique id (diagnostic; 0 is never used).
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    StrandHome(StrandHome const&)            = delete;
    StrandHome& operator=(StrandHome const&) = delete;

protected:
    StrandHome() noexcept : id_(mint_strand_id()) {}

private:
    [[nodiscard]] static std::uint64_t mint_strand_id() noexcept {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t id_;
};

// Owned by one `block_on()` call, on its stack; `home` is created on the first park, by the thread that
// owns the call. Read from another thread only by a completion signal that came through a foreign
// (non-homing) awaitable -- after that thread's own write, which the hand-off orders before it.
struct HomeSlot {
    std::shared_ptr<CallerHome> home;
};

struct ExecutionContext {
    HomeSlot*     slot   = nullptr;  // the block_on() currently driving this thread's coroutine, if any
    std::uint64_t holder = 0;        // 0: use the thread's ambient id
    // ADR-219: the host resumer driving this thread's coroutine, if any. Points at a shared_ptr owned by
    // the enclosing ScopedResumer or ParkedContinuation::resume() frame, which outlives every use. Ignored
    // while `slot` is set: a block_on() inside a host drive owns what parks under it.
    std::shared_ptr<Resumer> const* resumer = nullptr;
    // ADR-237 §4.3: the strand whose continuation this thread is running -- set only by a lane worker, and
    // kept by scopes nested inside its slice. Highest precedence (file comment). The lane worker holds a
    // strong reference for the duration of the slice.
    StrandHome* strand = nullptr;
};

// Not inlinable under MSVC: inlined into a coroutine body, MSVC's optimizer computes the thread_local's address
// once and reuses it after a co_await that resumed on another thread (a lane hop), so the coroutine reads the
// PREVIOUS thread's context (test_rt_scope S1, MSVC Release). An opaque call recomputes it on every access.
#if defined(_MSC_VER) && !defined(__clang__)
#define AGENTENGINE_TLS_ACCESSOR __declspec(noinline)
#else
#define AGENTENGINE_TLS_ACCESSOR
#endif

[[nodiscard]] AGENTENGINE_TLS_ACCESSOR inline ExecutionContext& current_execution() noexcept {
    thread_local ExecutionContext ctx;
    return ctx;
}

[[nodiscard]] AGENTENGINE_TLS_ACCESSOR inline std::uint64_t ambient_holder_id() noexcept {
    thread_local std::uint64_t const id = mint_holder_id();
    return id;
}

}  // namespace detail

// The logical task the current thread is running: set by the driver that resumed it, else the thread's own.
[[nodiscard]] inline std::uint64_t current_holder_id() noexcept {
    std::uint64_t const h = detail::current_execution().holder;
    return h != 0 ? h : detail::ambient_holder_id();
}

// RAII: coroutines resumed on this thread until destruction belong to `holder` and park back to `slot`
// (nullptr: homeless, or to `resumer` if one is given -- ADR-219; to `strand`, above both -- ADR-237 §4.3).
// The previous context is restored on destruction.
// ae-naming-lint: allow ScopedExecution — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ScopedExecution {
public:
    ScopedExecution(detail::HomeSlot* slot, std::uint64_t holder,
                    std::shared_ptr<Resumer> const* resumer = nullptr,
                    detail::StrandHome* strand = nullptr) noexcept
        : saved_(std::exchange(detail::current_execution(),
                               detail::ExecutionContext{slot, holder, resumer, strand})) {}
    ~ScopedExecution() { detail::current_execution() = saved_; }
    ScopedExecution(ScopedExecution const&) = delete;
    ScopedExecution& operator=(ScopedExecution const&) = delete;

private:
    detail::ExecutionContext saved_;
};

}  // namespace agentengine::rt
