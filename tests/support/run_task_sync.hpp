#pragma once
// Test-only synchronous driver for an `ae::task<T>` (`agentengine::rt::task<T>`, `core/task.hpp`)
// whose body is known to never genuinely park -- e.g. a ChatClient test conformer exercised directly
// from a plain `main()`, with no `rt::ThreadPool`/session runtime anywhere in the picture.
// `rt::task<T>` has no synchronous "drive to completion and give me the value" API by design: it is
// meant to be `co_await`ed only from inside another coroutine (`rt/task.hpp`'s own banner comment;
// `task_value_return_test.cpp` never drives one from a plain function either). Milestone 5 Phase B4
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md) needs exactly this seam: several
// pre-existing tests call a `ChatClient` conformer's `chat()` directly, outside any session, and the
// conformers themselves are trivial (no real I/O, no cross-call `co_await` inside) -- so a minimal
// driver coroutine, manually `resume()`d once, is enough; nothing here supplies a wakeup carrier, so
// if the awaited task ever DID genuinely park, this would simply hang (never do this outside a test).
// Real, session-hosted call sites (`AgentSession::start_run`) run inline on the caller's own coroutine
// instead (historical: this comment used to describe `quark::task<T>` and a real `quark::Activation`
// before ADR-037 removed Quark as a dependency; `agentengine::rt::task<T>` has no actor/activation
// concept at all).

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace agentengine::test_support {

namespace run_task_sync_detail {

template <class T>
struct Driver;  // forward-declared: DriverPromise::get_return_object() constructs one below

template <class T>
struct DriverPromise {
    std::exception_ptr fault{};

    Driver<T> get_return_object() noexcept {
        return Driver<T>{std::coroutine_handle<DriverPromise>::from_promise(*this)};
    }
    // Lazy, matching rt::task<T>'s own idiom: the caller resumes it explicitly, once.
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { fault = std::current_exception(); }
};

template <class T>
struct Driver {
    using promise_type = DriverPromise<T>;
    std::coroutine_handle<promise_type> handle;
};

}  // namespace run_task_sync_detail

// `awaitable` is any `co_await`-able expression (an `ae::task<T>` rvalue) whose own body never
// genuinely suspends. Returns the awaited `T` or rethrows whatever the awaited coroutine threw.
template <class T, class Awaitable>
T run_task_sync(Awaitable&& awaitable) {
    using run_task_sync_detail::Driver;

    std::optional<T> out;
    auto driver = [&]() -> Driver<T> {
        out = co_await std::forward<Awaitable>(awaitable);
    }();

    driver.handle.resume();  // one external resume -- the whole (non-parking) chain runs inline here

    std::exception_ptr fault = driver.handle.promise().fault;
    driver.handle.destroy();
    if (fault) std::rethrow_exception(fault);
    return std::move(*out);
}

}  // namespace agentengine::test_support
