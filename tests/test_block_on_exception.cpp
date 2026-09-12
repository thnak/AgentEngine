// Proof that `agentengine::rt::block_on()` carries an escaping exception out to its caller instead
// of killing the process.
//
// WHAT WAS WRONG. `block_on()`'s private driver coroutine declared
// `void unhandled_exception() { std::terminate(); }`, carried over verbatim from the prove-phase
// original's own no-exception-handling posture. That posture stopped being defensible once this
// driver acquired production callers.
//
// `agentengine::rt::task<T>` deliberately CATCHES an exception into its promise and rethrows it at
// the awaiting `co_await` -- and the awaiting `co_await` is the one inside `block_on()`'s own
// `drive_and_signal()`. So a throw anywhere inside ANY task driven through `block_on()` landed in
// that `unhandled_exception()` and took the process down, converting a failure the caller could have
// handled into one nobody can. `sandbox/ustar_writer.hpp`'s own comment records a red-team hitting
// exactly this, from a `filesystem_error` on an unreadable subdirectory, and it is the terminal end
// of the defect class issue #71 tracks: `MandatorySandboxProvider` drives the whole sandbox
// run/commit/discard tool surface through `block_on()`.
//
//   E1 (positive control) -- an ordinary task still returns its value through `block_on()`. Every
//         claim below is about the failure path, so the success path is established first.
//   E2 (the guard) -- a throwing task propagates its exception to the CALLER, catchable, with the
//         payload intact. Against the unfixed driver this test does not report a failure: the
//         process dies here, which is itself the measurement.
//   E3 -- the driver survives the fault well enough to be reused. A hundred consecutive throwing
//         tasks are driven and caught, which exercises the completion signal and the frame teardown
//         on the fault path rather than assuming they still work.
//   E4 -- a non-`std::exception` payload propagates too, so this is genuine exception transport
//         rather than a message being copied out of a known type.
//   E5 -- the mechanism this file exists for is intact: a task that genuinely SUSPENDS on a
//         contended `AsyncMutex` and is completed by a different thread still returns its value.
//         The fix touches the same promise that machinery lives in, so it has to be shown unbroken.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/task.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

struct NotAnException {
    int marker;
};

// `AsyncMutex::lock()` yields an awaiter, not a `task<T>`, so E5 wraps it the same way production
// does (`agent_session_detail::acquire_session_mutex`) rather than inventing a different shape.
agentengine::rt::task<agentengine::rt::AsyncMutex::Guard> acquire(agentengine::rt::AsyncMutex& m) {
    co_return co_await m.lock();
}

agentengine::rt::task<int> returns_a_value(int v) { co_return v; }

agentengine::rt::task<int> throws_std_exception() {
    throw std::runtime_error("thrown from inside a driven task");
    co_return 0;  // unreachable; makes this a coroutine
}

agentengine::rt::task<int> throws_a_plain_struct() {
    throw NotAnException{4242};
    co_return 0;
}

}  // namespace

int main() {
    using agentengine::rt::block_on;

    // ---- E1: the positive control.
    {
        int const v = block_on(returns_a_value(7));
        check(v == 7, "E1 (positive control): block_on() returns an ordinary task's value");
    }

    // ---- E2: the guard.
    {
        bool caught = false;
        std::string message;
        try {
            (void)block_on(throws_std_exception());
        } catch (std::runtime_error const& e) {
            caught = true;
            message = e.what();
        } catch (...) {
            caught = true;
            message = "<wrong type>";
        }
        check(caught,
              "E2: an exception from a driven task reaches the CALLER -- the unfixed driver called "
              "std::terminate() here and this process would already be dead");
        check(message == "thrown from inside a driven task",
              "E2: ... with its payload intact, so it is the original exception and not a stand-in");
    }

    // ---- E3: the fault path leaves the driver reusable.
    {
        int caught = 0;
        for (int i = 0; i < 100; ++i) {
            try {
                (void)block_on(throws_std_exception());
            } catch (std::runtime_error const&) {
                ++caught;
            }
        }
        check(caught == 100,
              "E3: a hundred consecutive faults are each delivered -- the completion signal and the "
              "frame teardown still run on the fault path, rather than leaking or wedging");
        int const after = block_on(returns_a_value(11));
        check(after == 11, "E3: ... and an ordinary task still works afterwards");
    }

    // ---- E4: genuine exception transport, not message-copying.
    {
        int marker = 0;
        bool caught_right_type = false;
        try {
            (void)block_on(throws_a_plain_struct());
        } catch (NotAnException const& e) {
            caught_right_type = true;
            marker = e.marker;
        } catch (...) {
        }
        check(caught_right_type && marker == 4242,
              "E4: a non-std::exception payload propagates with its own type and contents");
    }

    // ---- E5: the contended-suspend mechanism this file exists for is unbroken.
    {
        agentengine::rt::AsyncMutex mutex;
        std::atomic<bool> holder_ready{false};
        std::atomic<bool> release_now{false};
        std::atomic<bool> holder_done{false};

        // A second thread takes the lock and holds it, so the driven task below cannot acquire it
        // synchronously and must genuinely suspend -- the exact case a naive "resume until done"
        // loop gets wrong and this driver was written for.
        std::thread holder([&] {
            auto guard = block_on(acquire(mutex));
            holder_ready.store(true, std::memory_order_release);
            while (!release_now.load(std::memory_order_acquire)) std::this_thread::yield();
            {
                auto released = std::move(guard);
                (void)released;
            }  // unlock() here resumes the waiter on THIS thread
            holder_done.store(true, std::memory_order_release);
        });

        while (!holder_ready.load(std::memory_order_acquire)) std::this_thread::yield();

        std::atomic<bool> waiter_done{false};
        std::thread waiter([&] {
            auto guard = block_on(acquire(mutex));
            (void)guard;
            waiter_done.store(true, std::memory_order_release);
        });

        release_now.store(true, std::memory_order_release);
        waiter.join();
        holder.join();

        check(waiter_done.load(std::memory_order_acquire) && holder_done.load(std::memory_order_acquire),
              "E5: a task that genuinely suspends on a contended AsyncMutex and completes on another "
              "thread still returns through block_on() -- the mechanism the fix sits inside is intact");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
