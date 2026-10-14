// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §4.5 rule 2 (step 33):
// `rt::Runtime::spawn` -- an OWNED root the caller does not await, on a fresh strand of a group, tracked
// exactly like a `run`/`enter` root (shutdown requests stop on it and joins it), plus `rt::RootHandle`.
// rt/runtime.hpp.
//
//   S1  spawn() runs the body on a lane (a strand is current; block_on is refused there), never on the host
//       thread; RootHandle::join() returns once it is done.
//   S2  spawn() does NOT block: it returns before the body finished, done() is false meanwhile, and the root
//       parks on the reactor (rt::sleep_for) and resumes on a lane (join() then completes).
//   S3  RootHandle::request_stop() cancels a parked root promptly (a 10 s park ends in well under it).
//   S4  a throwing root records the exception on the handle (fault()); the process is not terminated.
//   S5  SHUTDOWN joins an outstanding spawn root: destroying the Runtime while the root parks 10 s on its
//       scope stop token returns promptly (shutdown requests stop) and joins it.
//   S6  RootHandle::join() on a lane is refused with rt.block_on_on_lane (§4.3).
//   S7  spawn() DURING shutdown (from a root that wakes because shutdown stopped it) throws rt.runtime_closed and
//       leaves nothing counted: the Runtime's destructor still returns (a leaked root count would hang it).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/task.hpp"

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;

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

std::thread::id g_host_thread;

task<void> noop_void() { co_return; }

struct SimpleState {
    std::atomic<bool> on_lane{false};
    std::atomic<bool> on_host{false};
    std::atomic<bool> refused{false};
};

task<void> body_simple(SimpleState* s) {
    s->on_lane.store(Strand::current().valid(), std::memory_order_relaxed);
    if (std::this_thread::get_id() == g_host_thread) s->on_host.store(true, std::memory_order_relaxed);
    try {
        block_on(noop_void());  // refused on a lane before it drives anything
    } catch (block_on_refused const&) {
        s->refused.store(true, std::memory_order_relaxed);
    }
    co_return;
}

struct SleepState {
    std::atomic<int>  woke{0};
    std::atomic<bool> saw_lane{false};
};

task<void> body_sleep(SleepState* s, pal::Reactor* reactor, std::chrono::milliseconds d) {
    (void)co_await sleep_for(*reactor, d, scope_stop_token());
    s->saw_lane.store(Strand::current().valid(), std::memory_order_relaxed);
    s->woke.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void> body_throw() {
    throw std::runtime_error("deliberate spawn-root failure");
    co_return;
}

struct JoinOnLane {
    RootHandle*       target = nullptr;
    std::atomic<bool> refused{false};
};

task<void> body_join_on_lane(JoinOnLane* s) {
    try {
        s->target->join();  // on a lane: refused
    } catch (block_on_refused const&) {
        s->refused.store(true, std::memory_order_relaxed);
    }
    co_return;
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

struct SpawnLate {
    std::atomic<int> threw_closed{0};
    std::atomic<int> other{0};
};

task<void> body_spawn_after_stop(SpawnLate* s, Runtime* rt, pal::Reactor* reactor) {
    (void)co_await sleep_for(*reactor, 10s, scope_stop_token());  // ends when shutdown requests stop
    try {
        (void)rt->spawn(noop_void());
        s->other.fetch_add(1);
    } catch (std::logic_error const& e) {
        if (std::string_view(e.what()).find("rt.runtime_closed") != std::string_view::npos) s->threw_closed.fetch_add(1);
        else s->other.fetch_add(1);
    }
    co_return;
}

int main() {
    g_host_thread = std::this_thread::get_id();

    // ---- S1: runs on a lane, not the host -----------------------------------------------
    {
        SimpleState s;
        Runtime     rt{RuntimeConfig{.lanes = 2}};
        RootHandle  h = rt.spawn(body_simple(&s));
        check(h.valid(), "S1: spawn() returns a valid handle");
        check(h.join(), "S1: join() returns once the root finished");
        check(h.done(), "S1: the handle reports done()");
        check(s.on_lane.load(), "S1: the body ran with a strand current (a lane)");
        check(!s.on_host.load(), "S1: the body did NOT run on the host thread");
        check(s.refused.load(), "S1: block_on() inside the root was refused (rt.block_on_on_lane)");
    }

    // ---- S2: non-blocking obtain; parks on the reactor; resumes on a lane ---------------
    {
        SleepState s;
        Runtime    rt{RuntimeConfig{.lanes = 2}};
        RootHandle h = rt.spawn(body_sleep(&s, &rt.reactor(), 200ms));
        check(!h.done(), "S2: spawn() returned BEFORE the body finished (it is non-blocking)");
        check(s.woke.load() == 0, "S2: the body has not completed yet");
        check(h.join(), "S2: join() waits for the parked root");
        check(s.woke.load() == 1, "S2: the root woke exactly once");
        check(s.saw_lane.load(), "S2: the reactor wake resumed the root on a lane (not the reactor thread)");
    }

    // ---- S3: request_stop() cancels a parked root promptly ------------------------------
    {
        SleepState s;
        Runtime    rt{RuntimeConfig{.lanes = 2}};
        RootHandle h = rt.spawn(body_sleep(&s, &rt.reactor(), 10s));
        std::this_thread::sleep_for(50ms);
        auto const t0 = std::chrono::steady_clock::now();
        h.request_stop();
        h.join();
        check(elapsed_ms(t0) < 2000, "S3: request_stop() canceled a 10 s park promptly");
        check(s.woke.load() == 1, "S3: the canceled root still ran its tail once");
    }

    // ---- S4: a throwing root records the fault, no terminate ----------------------------
    {
        Runtime    rt{RuntimeConfig{.lanes = 2}};
        RootHandle h = rt.spawn(body_throw());
        h.join();
        check(h.done(), "S4: a throwing root is still done");
        check(static_cast<bool>(h.fault()), "S4: the exception is recorded on the handle");
    }

    // ---- S5: shutdown stops + joins an outstanding spawn root promptly ------------------
    {
        SleepState s;
        auto const t0 = std::chrono::steady_clock::now();
        {
            Runtime    rt{RuntimeConfig{.lanes = 2}};
            RootHandle h = rt.spawn(body_sleep(&s, &rt.reactor(), 10s));
            check(!h.done(), "S5: the root is parked before destruction");
            // `rt` destroyed here: shutdown must request stop on the root and join it.
        }
        check(elapsed_ms(t0) < 3000, "S5: destroying the Runtime stopped + joined the parked root promptly");
    }

    // ---- S6: join() on a lane is refused ------------------------------------------------
    {
        JoinOnLane s;
        SleepState sleep_state;
        Runtime    rt{RuntimeConfig{.lanes = 2}};
        RootHandle h = rt.spawn(body_sleep(&sleep_state, &rt.reactor(), 10s));
        s.target      = &h;
        (void)rt.run(body_join_on_lane(&s));
        check(s.refused.load(), "S6: RootHandle::join() on a lane is refused with rt.block_on_on_lane");
        h.request_stop();
        h.join();
    }

    // ---- S7: spawn during shutdown throws rt.runtime_closed and does not wedge the destructor ----------------
    {
        SpawnLate s;
        auto const t0 = std::chrono::steady_clock::now();
        {
            Runtime rt{RuntimeConfig{.lanes = 2}};
            (void)rt.spawn(body_spawn_after_stop(&s, &rt, &rt.reactor()));
            std::this_thread::sleep_for(50ms);
        }
        check(s.threw_closed.load() == 1 && s.other.load() == 0,
              "S7: spawn() during shutdown threw rt.runtime_closed");
        check(elapsed_ms(t0) < 5000, "S7: and the Runtime's destructor still returned promptly");
    }

    if (g_failures == 0) {
        std::printf("test_rt_spawn: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_spawn: %d failure(s)\n", g_failures);
    return 1;
}
