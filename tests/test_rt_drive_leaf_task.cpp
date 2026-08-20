// Proof for decisions/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md §3 Design B --
// agentengine::rt::drive_leaf_task<T>() (include/agentengine/rt/drive_leaf_task.hpp), the narrow,
// opt-in "leaf task" driver VectorRagContextProvider::recall's invoke uses to drive a
// synchronous_leaf Embedder::embed_batch() synchronously from a synchronous ToolDescriptor::invoke
// closure. Covers, one case per block in main():
//   D1 -- a conforming leaf task (co_return only) completes in exactly one resume(); the outer
//         result<T> unwraps to the task's own genuine return value.
//   D2 -- the double-wrapped result<T> is preserved, not flattened: a leaf task that itself
//         co_returns std::unexpected(...) (the ORDINARY provider-error channel, e.g. a failed HTTP
//         call) drives cleanly (outer succeeds) with the inner result<T> carrying that error.
//   D3 -- a leaf task whose body throws a genuine C++ exception is mapped to an OUTER
//         "rt.leaf_task_faulted" error, distinct from D2's inner-error channel.
//   D4 -- a leaf task that internally co_awaits ANOTHER nested task<T> (real C++20 symmetric
//         transfer) still completes in exactly one resume(), regardless of nesting depth.
//   D5 -- a task that genuinely suspends on something other than a nested task<T>/task<void> (an
//         AsyncMutex::lock() under real contention) violates the synchronous_leaf contract:
//         drive_leaf_task() reports it as "rt.leaf_task_contract_violation" instead of looping
//         resume() on a parked coroutine handle (the exact hazard ADR-064 found in
//         rt::ThreadPool::run_job(), reproduced here directly against drive_leaf_task() itself, not
//         only through ThreadPool). Also proves the abandoned contender's destruction does not
//         corrupt the mutex for later legitimate use (AsyncMutex's own cancellation-safe self-
//         removal, ADR-017's "drop the handle = cancel").

#include <cstdio>
#include <stdexcept>

#include "agentengine/core/error.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/drive_leaf_task.hpp"
#include "agentengine/rt/task.hpp"

using agentengine::error;
using agentengine::failure_class;
using agentengine::result;
using agentengine::rt::AsyncMutex;
using agentengine::rt::drive_leaf_task;
using agentengine::rt::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

task<result<int>> ok_leaf() { co_return 42; }

task<result<int>> failing_leaf() {
    co_return std::unexpected(error{failure_class::transient, "scripted provider failure", "test.boom"});
}

task<result<int>> throwing_leaf() {
    throw std::runtime_error("leaf-body-exception");
    co_return 0;  // unreachable, silences a "no return" warning
}

task<result<int>> inner_ok() { co_return 5; }

// Real nested composition -- symmetric transfer, no external suspension -- still resolves in one
// resume() regardless of the extra layer.
task<result<int>> nested_leaf() {
    result<int> v = co_await inner_ok();
    co_return v.has_value() ? *v + 1 : -1;
}

// D5 helpers -- named free functions, not lambdas, per test_rt_async_mutex.cpp's own documented
// pitfall (a coroutine lambda's frame holds only a pointer to the closure, destroyed before a
// suspending body finishes running).
task<void> hold(AsyncMutex* mtx, AsyncMutex::Guard* out) { *out = co_await mtx->lock(); }

task<result<int>> contend_and_lock(AsyncMutex* mtx) {
    AsyncMutex::Guard guard = co_await mtx->lock();  // parks while `mtx` is already held elsewhere
    co_return 1;                                      // unreachable while genuinely contended
}

}  // namespace

int main() {
    // D1: a conforming leaf task drives cleanly; the outer result<T> unwraps to the real value.
    {
        auto driven = drive_leaf_task(ok_leaf());
        check(driven.has_value(), "D1: outer result<T> succeeds for a conforming leaf task");
        check(driven.has_value() && driven->has_value() && **driven == 42,
              "D1: both layers unwrap to the task's own genuine co_return value");
    }

    // D2: the double-wrap is preserved -- outer succeeds (the coroutine itself completed cleanly),
    // inner carries the ordinary provider-error channel.
    {
        auto driven = drive_leaf_task(failing_leaf());
        check(driven.has_value(),
              "D2: outer result<T> succeeds -- the leaf task's own coroutine body completed cleanly, "
              "it just co_returned an error value, which is not a task-level fault");
        check(driven.has_value() && !driven->has_value() && driven->error().code == "test.boom",
              "D2: the INNER result<T> carries the leaf's own ordinary error, unflattened");
    }

    // D3: a genuine C++ exception thrown inside the leaf task's body is a DIFFERENT failure channel
    // than D2's ordinary error -- mapped to a distinct, diagnosable OUTER error code.
    {
        auto driven = drive_leaf_task(throwing_leaf());
        check(!driven.has_value() && driven.error().code == "rt.leaf_task_faulted",
              "D3: a thrown exception inside the leaf task's body surfaces as an OUTER "
              "rt.leaf_task_faulted error, distinct from D2's inner ordinary-error channel");
    }

    // D4: nested task<T> composition (real symmetric transfer) still completes in exactly one
    // resume(), matching the "regardless of nesting depth" claim this function's own comment makes.
    {
        auto driven = drive_leaf_task(nested_leaf());
        check(driven.has_value() && driven->has_value() && **driven == 6,
              "D4: a leaf task composing another nested task<T> still drives cleanly in one resume()");
    }

    // D5: a task that genuinely suspends (AsyncMutex::lock() under real contention) violates the
    // synchronous_leaf contract -- reported, not looped into a cross-thread double-resume hazard.
    {
        AsyncMutex mtx;
        AsyncMutex::Guard holder_guard;
        task<void> holder = hold(&mtx, &holder_guard);
        if (!holder.done()) holder.resume();
        check(holder.done() && !holder.faulted() && holder_guard.held(),
              "D5 setup: the holder acquires the (uncontended) mutex directly and keeps it held");

        auto driven = drive_leaf_task(contend_and_lock(&mtx));
        check(!driven.has_value() && driven.error().code == "rt.leaf_task_contract_violation",
              "D5: a task parked on a genuinely contended AsyncMutex::lock() is reported as a "
              "violated synchronous_leaf contract, not resumed a second time");

        // Release the holder, then prove the mutex is still fully usable -- the abandoned contender's
        // destruction (which ran when `driven`'s local `task<result<int>>` parameter went out of
        // scope inside drive_leaf_task()) correctly self-removed from AsyncMutex's waiter queue.
        holder_guard = AsyncMutex::Guard{};
        check(!holder_guard.held(), "D5 setup: releasing the holder's guard frees the mutex");

        task<result<int>> later = contend_and_lock(&mtx);  // uncontended now
        if (!later.done()) later.resume();
        check(later.done() && !later.faulted() && later.take_value().has_value(),
              "D5: the mutex is still fully usable afterward -- the abandoned contender left no "
              "corruption behind");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_drive_leaf_task: ALL PASS\n");
    return 0;
}
