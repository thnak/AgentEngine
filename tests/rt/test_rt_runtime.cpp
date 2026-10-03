// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §9 D7 (rt::Runtime: run / enter /
// CompletionThread), §9 D5 (usable_cpus, default lane count), §4.2 (engine work resumes on its strand; foreign
// continuations run where the host said) and §4.5 rule 4 (shutdown order; no thread outlives the Runtime) --
// rt/runtime.hpp and pal/cpu.hpp. Each claim names how it would fail.
//
//   R1  runtime.run(task) runs the body on a lane (a strand is current, block_on is refused there), never on the
//       host thread, and returns its value; an exception is rethrown on the host.
//   R2  A reactor wake (rt::sleep_for) of a task under runtime.run resumes on a lane worker under the SAME strand --
//       not on the reactor thread, not on the host thread. Mutant: a strand wake resumes inline -> reactor thread.
//   R3  An offload completion under runtime.run resumes on the strand too (offload uses the same parked record).
//   R4  runtime.run and block_on on a lane are refused with `rt.block_on_on_lane`.
//   R5  co_await runtime.enter(task, Resumer&) from a foreign coroutine type: the body runs on a lane, the foreign
//       continuation is handed to the Resumer and runs on the Resumer's thread -- never on a lane. Mutant: the root
//       completion resumes the foreign coroutine inline -> it runs on the lane.
//   R6  co_await runtime.enter(task, CompletionThread&): the continuation runs on that thread; `outstanding()` is 1
//       while the body runs and 0 after. Destroying a CompletionThread with an outstanding enter is a checked
//       violation: a child process doing it aborts. Mutant: drop the check -> the child exits 0.
//   R7  Config (D5): `lanes = half_of_hardware` gives max(2, usable_cpus()/2); an explicit count is honoured.
//   R8  usable_cpus honours affinity: restricted to 1 CPU it reports 1 (and the default lane count is the floor, 2);
//       to 2 CPUs, at most 2. Linux: sched_setaffinity on the calling thread; Windows: SetProcessAffinityMask.
//       The cgroup v1/v2 and /proc/self/cgroup parsers are checked against fixed inputs on every OS. Mutant: ignore
//       the affinity mask -> reports every core.
//   R9  Shutdown drains (§4.5 rule 4): a Runtime destroyed while an enter() root sleeps waits for it, the foreign
//       continuation is still delivered, and then everything is joined. Mutant: skip the wait for roots -> the
//       root is never resumed.
//   R10 No thread outlives the Runtime: the process's thread count after destruction equals the count before
//       (Linux /proc/self/task, exact; Windows Toolhelp, at most as many as before -- the OS loader's own pool
//       threads may exit meanwhile). Mutant: lanes detached instead of joined.
//   Step 5 (§4.5 rule 3, §4.2, §4.3, §4.5 rule 4):
//   R11 ROOT ADOPTION: a host destroys the coroutine awaiting enter() while the root holds an AsyncMutex and
//       sleeps -- no abort: the root is adopted, asked to stop (its scope stop token), finishes promptly, releases the
//       lock (a later run acquires it), is counted; the foreign coroutine is never continued. Mutant: adoption
//       disabled (the step-3 abort) -> the process dies.
//   R12 A host Resumer that DROPS an enter continuation: it is never resumed (not inline on the lane), and counted.
//       Mutant: ADR-219's inline-resume fallback for enter continuations -> it runs on the lane.
//   R13 WATCHDOGS (report-only): a run making progress in short slices is never reported; a body blocking its lane
//       300 ms is one lane-stall report naming its strand; a run parked 600 ms is one stuck-run report naming its
//       strand group. Mutants: no progress stamp -> the progressing run is reported stuck; report every scan -> the
//       stall is reported several times.
//   R14 SHUTDOWN cancels before it joins: a Runtime destroyed while a root sleeps 10 s on its scope stop token
//       returns promptly, the sleep canceled, and the continuation is still delivered. Mutant: no stop at shutdown
//       -> 10 s.
//
// Coroutines are free functions taking their state by pointer, never immediately-invoked lambda coroutines with
// captures (ADR-237 §14 step 1). Result containers are declared BEFORE the runtime that writes them (step 2).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/cpu.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/task.hpp"
#include "../support/crt_fail_fast.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sched.h>
#endif

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 20s) {
    auto const end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

RuntimeConfig config(std::size_t lanes, std::size_t offload_workers = 2, std::size_t dns_workers = 2) {
    RuntimeConfig c;
    c.lanes           = lanes;
    c.offload_workers = offload_workers;
    c.dns_workers     = dns_workers;
    return c;
}

bool on_lane() { return detail::block_on_refusal_code() != nullptr && current_strand_id() != 0; }

// ---- R1 / R4 ------------------------------------------------------------------------------------------------
struct Where {
    std::thread::id thread;
    std::uint64_t   strand = 0;
    bool            lane   = false;
};

task<Where> where_am_i() { co_return Where{std::this_thread::get_id(), current_strand_id(), on_lane()}; }

task<int> throws_body() {
    throw std::runtime_error("boom");
    co_return 0;
}

task<int> one() { co_return 1; }

struct NestedOut {
    std::string run_code;
    std::string block_on_code;
};

task<NestedOut> nested_on_lane(Runtime* rt) {
    NestedOut out;
    try {
        (void)rt->run(one());
    } catch (block_on_refused const& e) {
        out.run_code = std::string(e.code());
    }
    try {
        (void)block_on(one());
    } catch (block_on_refused const& e) {
        out.block_on_code = std::string(e.code());
    }
    co_return out;
}

void r1_r4_run(Runtime& rt) {
    Where const w = rt.run(where_am_i());
    check(w.thread != std::this_thread::get_id(), "R1 run: the body did not run on the host thread");
    check(w.lane && w.strand != 0, "R1 run: the body ran on a lane worker, under a strand");
    bool threw = false;
    try {
        (void)rt.run(throws_body());
    } catch (std::runtime_error const& e) {
        threw = std::string(e.what()) == "boom";
    }
    check(threw, "R1 run: the body's exception is rethrown on the host");
    NestedOut const n = rt.run(nested_on_lane(&rt));
    check(n.run_code == "rt.block_on_on_lane", "R4 runtime.run on a lane is refused with rt.block_on_on_lane");
    check(n.block_on_code == "rt.block_on_on_lane", "R4 block_on on a lane is refused with rt.block_on_on_lane");
}

// ---- R2 / R3 ------------------------------------------------------------------------------------------------
struct Wake {
    Where           before;
    Where           after;
    bool            after_on_reactor = true;
    std::thread::id body_thread;
    offload_status  status = offload_status::canceled;
};

task<Wake> sleep_under_run(Runtime* rt) {
    Wake w;
    w.before = co_await where_am_i();
    (void)co_await sleep_for(rt->reactor(), 20ms);
    w.after            = co_await where_am_i();
    w.after_on_reactor = rt->reactor().on_reactor_thread();
    co_return w;
}

task<Wake> offload_under_run(Runtime* rt) {
    Wake w;
    w.before = co_await where_am_i();
    auto r   = co_await offload(rt->offload_pool(), {}, [] { return std::this_thread::get_id(); });
    w.status = r.status;
    if (r.status == offload_status::completed) w.body_thread = *r.value;
    w.after = co_await where_am_i();
    co_return w;
}

void r2_r3_wakes(Runtime& rt) {
    Wake const w = rt.run(sleep_under_run(&rt));
    check(w.after.lane && w.after.strand == w.before.strand,
          "R2 a reactor wake resumed the task on a lane worker under its own strand");
    check(!w.after_on_reactor, "R2 ... not on the reactor thread");
    check(w.after.thread != std::this_thread::get_id(), "R2 ... not on the host thread");
    check(rt.reactor().homeless_refusals() == 0, "R2 no homeless refusal");

    Wake const o = rt.run(offload_under_run(&rt));
    check(o.status == offload_status::completed && o.body_thread != o.after.thread,
          "R3 the offload body ran on an offload worker");
    check(o.after.lane && o.after.strand == o.before.strand,
          "R3 the offload completion resumed the task on a lane under its own strand");
}

// ---- R5 / R6: a foreign coroutine type ------------------------------------------------------------------------
struct Foreign {
    struct promise_type {
        Foreign get_return_object() noexcept {
            return Foreign{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Started explicitly with start(), never from inside the ramp: with a suspend_never initial suspend, gcc's
        // ramp function writes to the frame after the body's first suspension returns to it -- a race once the body
        // has already been resumed on another thread (TSan).
        std::suspend_always initial_suspend() noexcept { return {}; }
        // Frees itself: the test never destroys a frame another thread may still be finishing (TSan found the
        // first version doing exactly that right after its `done` flag).
        std::suspend_never final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
    void start() const { h.resume(); }  // runs the body until its first co_await
};

class LoopResumer final : public Resumer {
public:
    LoopResumer() : thread_([this](std::stop_token st) { loop(st); }) {}
    ~LoopResumer() override {
        thread_.request_stop();
        cv_.notify_all();
    }
    void post(ParkedContinuation c) override {
        std::lock_guard lock(m_);
        q_.push_back(std::move(c));
        cv_.notify_one();
    }
    [[nodiscard]] std::thread::id id() const noexcept { return thread_.get_id(); }

private:
    void loop(std::stop_token const& st) {
        std::unique_lock lock(m_);
        for (;;) {
            cv_.wait(lock, [&] { return st.stop_requested() || !q_.empty(); });
            if (q_.empty()) return;
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
            lock.lock();
        }
    }
    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    std::jthread                   thread_;  // last: joined first
};

struct EnterOut {
    Where             body;
    std::thread::id   after;
    bool              after_on_lane = true;
    int               value         = 0;
    std::size_t       outstanding_during = 0;
    std::atomic<bool> done{false};
};

task<Where> body_where(CompletionThread* ct, EnterOut* out) {
    if (ct != nullptr) out->outstanding_during = ct->outstanding();
    co_return co_await where_am_i();
}

task<int> body_sleep_value(Runtime* rt, int v, std::chrono::milliseconds d) {
    (void)co_await sleep_for(rt->reactor(), d);
    co_return v;
}

Foreign enter_with_resumer(Runtime* rt, LoopResumer* r, EnterOut* out) {
    out->body          = co_await rt->enter(body_where(nullptr, out), *r);
    out->after         = std::this_thread::get_id();
    out->after_on_lane = detail::block_on_refusal_code() != nullptr;
    out->done.store(true);
}

Foreign enter_with_ct(Runtime* rt, CompletionThread* ct, EnterOut* out) {
    out->body          = co_await rt->enter(body_where(ct, out), *ct);
    out->after         = std::this_thread::get_id();
    out->after_on_lane = detail::block_on_refusal_code() != nullptr;
    out->done.store(true);
}

void r5_enter_resumer(Runtime& rt) {
    EnterOut    out;
    LoopResumer loop;
    enter_with_resumer(&rt, &loop, &out).start();  // frees itself
    check(wait_until([&] { return out.done.load(); }), "R5 enter(task, Resumer&): the foreign coroutine finished");
    check(out.body.lane && out.body.thread != std::this_thread::get_id(), "R5 the engine body ran on a lane");
    check(out.after == loop.id(), "R5 the foreign continuation ran on the Resumer's thread");
    check(!out.after_on_lane, "R5 ... never on a lane");
}

void r6_enter_completion_thread(Runtime& rt) {
    EnterOut out;
    {
        CompletionThread ct(rt);
        enter_with_ct(&rt, &ct, &out).start();  // frees itself
        check(wait_until([&] { return out.done.load(); }), "R6 enter(task, CompletionThread&): finished");
        check(out.after == ct.id() && !out.after_on_lane, "R6 the continuation ran on the CompletionThread");
        check(out.body.lane, "R6 the engine body ran on a lane");
        check(out.outstanding_during == 1 && ct.outstanding() == 0,
              "R6 outstanding() is 1 while the body runs and 0 after the continuation ran");
    }
}

// The child half of R6's violation test: destroys a CompletionThread while an enter() is outstanding.
Foreign enter_long(Runtime* rt, CompletionThread* ct, int* out) {
    *out = co_await rt->enter(body_sleep_value(rt, 5, 300ms), *ct);
}

int ct_violation_child() {
    int     v = 0;
    Runtime rt{config(2)};
    std::optional<CompletionThread> ct;
    ct.emplace(rt);
    enter_long(&rt, &*ct, &v).start();
    ct.reset();  // checked violation: must abort here
    std::this_thread::sleep_for(500ms);
    return 0;
}

void r6_violation(char const* self) {
    std::string cmd = std::string("\"") + self + "\" --ct-violation";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one outer pair of quotes
#endif
    int const rc = std::system(cmd.c_str());
    check(rc != 0, "R6 destroying a CompletionThread with an outstanding enter() aborts (child exit " +
                       std::to_string(rc) + ")");
}

// ---- R7 / R8 ------------------------------------------------------------------------------------------------
void r7_config() {
    std::size_t const want = std::max<std::size_t>(2, usable_cpus() / 2);
    check(default_lane_count() == want, "R7 default_lane_count() == max(2, usable_cpus()/2) = " + std::to_string(want));
    {
        Runtime rt;
        check(rt.lanes() == want, "R7 Runtime{} uses the D5 default lane count");
        check(rt.offload_pool().workers() == 2 && rt.dns_pool().workers() == 2, "R7 offload and DNS pools default to 2");
    }
    {
        Runtime rt{config(3, 1, 3)};
        check(rt.lanes() == 3 && rt.offload_pool().workers() == 1 && rt.dns_pool().workers() == 3,
              "R7 explicit lane/offload/DNS counts are honoured");
    }
}

void r8_parsers() {
    using namespace pal::cpu_detail;
    check(!parse_cgroup2_cpu_max("max 100000\n"), "R8 cgroup2 'max 100000' is no limit");
    check(parse_cgroup2_cpu_max("50000 100000\n") == 1U, "R8 cgroup2 0.5 CPU rounds up to 1");
    check(parse_cgroup2_cpu_max("150000 100000") == 2U, "R8 cgroup2 1.5 CPUs rounds up to 2");
    check(parse_cgroup2_cpu_max("400000 100000") == 4U, "R8 cgroup2 4 CPUs");
    check(!parse_cgroup2_cpu_max("garbage"), "R8 cgroup2 garbage is no limit");
    check(!parse_cgroup1_quota("-1\n", "100000\n"), "R8 cgroup1 quota -1 is no limit");
    check(parse_cgroup1_quota("250000\n", "100000\n") == 3U, "R8 cgroup1 2.5 CPUs rounds up to 3");
    CgroupPaths const v2 = parse_proc_self_cgroup("0::/user.slice/app.scope\n");
    check(v2.v2 == std::string("/user.slice/app.scope") && !v2.v1_cpu, "R8 /proc/self/cgroup v2 path");
    CgroupPaths const v1 = parse_proc_self_cgroup("12:memory:/x\n4:cpu,cpuacct:/docker/abc\n1:name=systemd:/y\n");
    check(v1.v1_cpu == std::string("/docker/abc") && !v1.v2, "R8 /proc/self/cgroup v1 cpu controller path");
    check(combine(8, 2U) == 2 && combine(2, 8U) == 2 && combine(3, std::nullopt) == 3 && combine(0, std::nullopt) == 1,
          "R8 usable = min(affinity, quota), at least 1");
}

void r8_affinity() {
#if defined(_WIN32)
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask  = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == 0) {
        std::printf("[skip] R8 affinity: GetProcessAffinityMask failed\n");
        return;
    }
    DWORD_PTR const lowest = process_mask & (~process_mask + 1);
    if (SetProcessAffinityMask(GetCurrentProcess(), lowest) == 0) {
        std::printf("[skip] R8 affinity: SetProcessAffinityMask failed\n");
        return;
    }
    unsigned const one = usable_cpus();
    std::size_t const lanes_one = default_lane_count();
    std::optional<unsigned> two;
    DWORD_PTR const rest = process_mask & ~lowest;
    if (rest != 0) {
        DWORD_PTR const second = rest & (~rest + 1);
        if (SetProcessAffinityMask(GetCurrentProcess(), lowest | second) != 0) two = usable_cpus();
    }
    SetProcessAffinityMask(GetCurrentProcess(), process_mask);
#else
    cpu_set_t original;
    CPU_ZERO(&original);
    if (sched_getaffinity(0, sizeof(original), &original) != 0) {
        std::printf("[skip] R8 affinity: sched_getaffinity failed\n");
        return;
    }
    int first = -1;
    int second = -1;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &original)) continue;
        if (first < 0) {
            first = c;
        } else {
            second = c;
            break;
        }
    }
    cpu_set_t only;
    CPU_ZERO(&only);
    CPU_SET(first, &only);
    sched_setaffinity(0, sizeof(only), &only);
    unsigned const one = usable_cpus();
    std::size_t const lanes_one = default_lane_count();
    std::optional<unsigned> two;
    if (second >= 0) {
        CPU_SET(second, &only);
        sched_setaffinity(0, sizeof(only), &only);
        two = usable_cpus();
    }
    sched_setaffinity(0, sizeof(original), &original);
#endif
    check(one == 1, "R8 restricted to 1 CPU, usable_cpus() == 1 (got " + std::to_string(one) + ")");
    check(lanes_one == 2, "R8 ... and the default lane count is the floor, 2");
    if (two) {
        check(*two >= 1 && *two <= 2, "R8 restricted to 2 CPUs, usable_cpus() <= 2 (got " + std::to_string(*two) + ")");
    } else {
        std::printf("[skip] R8 two-CPU mask: this machine exposes one CPU\n");
    }
    check(usable_cpus() >= one, "R8 the original mask is restored");
}

// ---- R9 -----------------------------------------------------------------------------------------------------
struct DrainOut {
    int               value = 0;
    std::atomic<bool> done{false};
};

Foreign enter_sleeping(Runtime* rt, LoopResumer* r, DrainOut* out) {
    out->value = co_await rt->enter(body_sleep_value(rt, 42, 150ms), *r);
    out->done.store(true);
}

void r9_shutdown_drains() {
    DrainOut    out;
    LoopResumer loop;  // outlives the runtime: the continuation is delivered during its shutdown
    std::size_t in_flight = 0;
    auto const  start     = std::chrono::steady_clock::now();
    {
        Runtime rt{config(2)};
        enter_sleeping(&rt, &loop, &out).start();  // frees itself
        in_flight = rt.roots_in_flight();
    }  // destroyed while the root sleeps
    auto const elapsed = std::chrono::steady_clock::now() - start;
    check(in_flight == 1, "R9 one root in flight when the Runtime was destroyed");
    check(elapsed >= 100ms, "R9 the destructor waited for the root");
    check(wait_until([&] { return out.done.load(); }, 5s) && out.value == 42,
          "R9 the foreign continuation was still delivered, with the value");
}

// ---- R11 / R12 / R14 (step 5) --------------------------------------------------------------------------------------
// A foreign coroutine type the host destroys explicitly (it "drops its awaiter").
struct Droppable {
    struct promise_type {
        Droppable get_return_object() noexcept {
            return Droppable{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
};

struct AdoptState {
    std::atomic<bool> locked{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> sleep_canceled{false};
    std::atomic<bool> foreign_continued{false};
};

task<int> adopt_body(Runtime* rt, AsyncMutex* m, AdoptState* st) {
    AsyncMutex::Guard g = co_await m->lock();
    st->locked.store(true);
    sleep_status const s = co_await sleep_for(rt->reactor(), 10s, scope_stop_token());
    st->sleep_canceled.store(s == sleep_status::canceled);
    st->finished.store(true);
    co_return 1;
}

Droppable adopt_foreign(Runtime* rt, LoopResumer* r, AsyncMutex* m, AdoptState* st) {
    (void)co_await rt->enter(adopt_body(rt, m, st), *r);
    st->foreign_continued.store(true);
}

task<bool> lock_and_release(AsyncMutex* m) {
    AsyncMutex::Guard g = co_await m->lock();
    co_return g.held();
}

void r11_adoption() {
    AsyncMutex  m;
    AdoptState  st;
    LoopResumer loop;
    Runtime     rt{config(2)};
    Droppable   f = adopt_foreign(&rt, &loop, &m, &st);
    f.h.resume();  // runs to the co_await of enter
    check(wait_until([&] { return st.locked.load(); }, 5s), "R11 the entered root holds the session lock");
    auto const t0 = std::chrono::steady_clock::now();
    f.h.destroy();  // the host drops its awaiter while the engine chain runs (and holds the lock)
    check(rt.adopted_roots() == 1, "R11 destroying the awaiter of a running enter() adopted the root (no abort)");
    check(wait_until([&] { return st.finished.load(); }, 5s) && st.sleep_canceled.load(),
          "R11 the adopted chain was asked to stop and finished normally (its sleep canceled)");
    check(std::chrono::steady_clock::now() - t0 < 2s, "R11 ... promptly, not after its 10 s sleep");
    check(wait_until([&] { return rt.adopted_roots_finished() == 1 && rt.roots_in_flight() == 0; }, 5s),
          "R11 the adopted root is counted finished and no root is left in flight");
    check(rt.run(lock_and_release(&m)), "R11 a later entry acquires the lock the adopted chain released");
    check(!st.foreign_continued.load(), "R11 the dropped foreign coroutine was never continued");
}

class DroppingResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override {
        posts.fetch_add(1);
        (void)c;  // dropped: the host's executor is gone
    }
    std::atomic<int> posts{0};
};

struct DropState {
    std::atomic<bool> continued{false};
};

Droppable drop_foreign(Runtime* rt, DroppingResumer* r, DropState* st) {
    (void)co_await rt->enter(one(), *r);
    st->continued.store(true);
}

void r12_dropped_continuation() {
    DropState       st;
    DroppingResumer dropper;
    Runtime         rt{config(2)};
    Droppable       f = drop_foreign(&rt, &dropper, &st);
    f.h.resume();
    check(wait_until([&] { return dropper.posts.load() == 1 && rt.roots_in_flight() == 0; }, 5s),
          "R12 the root finished and handed its continuation to the Resumer, which dropped it");
    std::this_thread::sleep_for(50ms);
    check(!st.continued.load() && rt.dropped_continuations() == 1,
          "R12 a dropped enter continuation is never resumed (not inline on the lane) and is counted");
    f.h.destroy();  // the host's own frame, never resumed
}

struct ShutdownState {
    std::atomic<bool> started{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> delivered{false};
};

task<int> shutdown_body(Runtime* rt, ShutdownState* st) {
    st->started.store(true);
    sleep_status const s = co_await sleep_for(rt->reactor(), 10s, scope_stop_token());
    st->canceled.store(s == sleep_status::canceled);
    co_return 7;
}

Foreign shutdown_foreign(Runtime* rt, LoopResumer* r, ShutdownState* st) {
    (void)co_await rt->enter(shutdown_body(rt, st), *r);
    st->delivered.store(true);
}

void r14_shutdown_cancels_roots() {
    ShutdownState st;
    LoopResumer   loop;  // outlives the runtime
    auto          t0 = std::chrono::steady_clock::now();
    {
        Runtime rt{config(2)};
        shutdown_foreign(&rt, &loop, &st).start();  // frees itself
        (void)wait_until([&] { return st.started.load(); }, 5s);
        t0 = std::chrono::steady_clock::now();
    }  // §4.5 rule 4: stop every root, then join it
    auto const elapsed = std::chrono::steady_clock::now() - t0;
    check(st.canceled.load() && elapsed < 2s, "R14 shutdown requested stop on the running root (its scope token) "
                                              "and joined it promptly");
    check(wait_until([&] { return st.delivered.load(); }, 5s), "R14 ... and its continuation was still delivered");
}

// ---- R13 (step 5): watchdogs --------------------------------------------------------------------------------------
task<int> progress_body(Runtime* rt, std::chrono::milliseconds total) {
    auto const end = std::chrono::steady_clock::now() + total;
    int        n   = 0;
    while (std::chrono::steady_clock::now() < end) {
        (void)co_await sleep_for(rt->reactor(), 5ms);
        ++n;
    }
    co_return n;
}

task<std::uint64_t> stall_body() {
    std::this_thread::sleep_for(300ms);  // blocks its lane: what the lane-stall watchdog exists to see
    co_return current_strand_id();
}

task<std::uint64_t> parked_body(Runtime* rt) {
    (void)co_await sleep_for(rt->reactor(), 600ms);  // parked, no progress, no blocked thread
    co_return Strand::current().group_id();
}

void r13_watchdogs() {
    std::mutex             mx;
    std::vector<LaneStall> stalls;
    std::vector<StuckRun>  stuck;
    RuntimeConfig          c = config(2);
    c.lane_stall_bound       = 60ms;
    c.on_lane_stall          = [&](LaneStall const& s) {
        std::lock_guard lock(mx);
        stalls.push_back(s);
    };
    c.stuck_run_bound = 150ms;
    c.on_stuck_run    = [&](StuckRun const& s) {
        std::lock_guard lock(mx);
        stuck.push_back(s);
    };
    Runtime rt{c};

    int const slices = rt.run(progress_body(&rt, 400ms));
    {
        std::lock_guard lock(mx);
        check(slices > 10 && stalls.empty() && stuck.empty() && rt.lane_stalls() == 0 && rt.stuck_runs() == 0,
              "R13 a run that keeps making progress (" + std::to_string(slices) + " short slices) is never reported");
    }

    std::uint64_t const strand = rt.run(stall_body());
    {
        std::lock_guard lock(mx);
        check(rt.lane_stalls() == 1 && stalls.size() == 1,
              "R13 a body blocking its lane for 300 ms is reported as a lane stall, once (" +
                  std::to_string(stalls.size()) + ")");
        check(!stalls.empty() && stalls[0].strand == strand && stalls[0].running_for >= 60ms,
              "R13 ... naming its strand and how long it had run");
    }

    std::size_t const   stuck_before = rt.stuck_runs();
    std::uint64_t const group        = rt.run(parked_body(&rt));
    {
        std::lock_guard lock(mx);
        std::size_t matching = 0;
        for (StuckRun const& s : stuck) {
            if (s.group == group) ++matching;
        }
        check(rt.stuck_runs() == stuck_before + 1 && matching == 1,
              "R13 a run parked 600 ms with no progress is reported as stuck, once, naming its strand group");
        check(rt.lane_stalls() == 1, "R13 ... and a parked run is not a lane stall");
    }
}

// ---- R10 ----------------------------------------------------------------------------------------------------
std::optional<std::size_t> thread_count() {
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
    THREADENTRY32 te{};
    te.dwSize          = sizeof(te);
    std::size_t n      = 0;
    DWORD const pid    = GetCurrentProcessId();
    if (Thread32First(snap, &te) != 0) {
        do {
            if (te.th32OwnerProcessID == pid) ++n;
        } while (Thread32Next(snap, &te) != 0);
    }
    CloseHandle(snap);
    return n;
#else
    std::error_code ec;
    std::size_t     n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    if (ec) return std::nullopt;
    return n;
#endif
}

void r10_no_thread_outlives() {
    std::thread([] {}).join();  // a sanitizer's own helper thread, started on the first thread creation, is not ours
    auto const before = thread_count();
    std::optional<std::size_t> during;
    {
        Runtime rt{config(3, 2, 2)};
        (void)rt.run(one());  // let everything start
        during = thread_count();
    }
    auto const after = thread_count();
    if (!before || !after || !during) {
        std::printf("[skip] R10 thread count unavailable\n");
        return;
    }
    std::printf("       R10 threads: before %zu, during %zu, after %zu\n", *before, *during, *after);
    check(*during >= *before + 3 + 2 + 2 + 1, "R10 the Runtime started its lanes, pools and reactor thread");
#if defined(_WIN32)
    // The OS loader's own pool threads may exit meanwhile, never ours: at most as many as before.
    check(*after <= *before, "R10 no thread outlives the Runtime (Windows: after <= before)");
#else
    check(*after == *before, "R10 no thread outlives the Runtime");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    agentengine::test_support::fail_fast_on_windows();
    if (argc > 1 && std::string(argv[1]) == "--ct-violation") return ct_violation_child();

    r10_no_thread_outlives();  // first: before this process has other long-lived threads
    {
        Runtime rt{config(3)};
        r1_r4_run(rt);
        r2_r3_wakes(rt);
        r5_enter_resumer(rt);
        r6_enter_completion_thread(rt);
    }
    r6_violation(argv[0]);
    r7_config();
    r8_parsers();
    r8_affinity();
    r9_shutdown_drains();
    r11_adoption();
    r12_dropped_continuation();
    r13_watchdogs();
    r14_shutdown_cancels_roots();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
