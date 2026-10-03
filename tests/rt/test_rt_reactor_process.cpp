// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 "Processes" (and "Handle hygiene
// becomes mandatory", round-1 G4), §4.4 "Process cancellation", §2 H-e and §8.2 gates 1d and 4: child processes
// on the reactor (pal/reactor_process.hpp, src/backends/reactor_asio/reactor_process_asio.cpp) and the
// awaitables over them (rt/process.hpp). Children are tests/rt/process_child_helper.cpp, whose every mode is
// time-bounded (machine safety); every wait below is bounded too, and a watchdog ends the run.
//
//   P1  run_process captures exit code, stdout and stderr, and resumes on the block_on() thread (its home),
//       never the reactor thread (§4.2). Mutant A: JoinState wakes inline -> the observed thread is the reactor's.
//   P1b Under an ADR-219 ScopedResumer the whole run resumes on the host Resumer's thread.
//   P2  1 MiB through stdin -> stdout (echo) completes: the stdin write and the stdout read run concurrently.
//   P3  Gate 4 / H-e: a child writing 16 MiB before it exits completes normally, nothing lost. Mutant B: drain
//       only after the exit -> the child blocks on a full pipe and the run ends `timed_out`.
//   P4  The output cap keeps the first N bytes, reads (and drops) the rest so the child is never blocked.
//       Mutant C: stop reading at the cap -> the child blocks and the run times out.
//   P5  Gate 1d: 64 children spawned concurrently, interleaving 4 s sleepers with echo children that exit at
//       stdin EOF, each reach EOF at their own exit (the echoes finish long before the sleepers). Driven by ONE
//       host-resumer thread: 64 parked runs, no thread each. Mutant D (Linux): pipe() without O_CLOEXEC and no
//       closefrom -> a sleeper inherits an echo's stdin write end and the echo waits for the sleeper.
//   P6  G4 against the HOST: an inheritable handle another component of this process left open while we spawn
//       is not inherited by the child -- closing our copy gives EOF at once. Mutant E: drop HANDLE_LIST
//       (Windows) / drop closefrom (Linux) -> EOF only when the child exits.
//   P7  A stop mid-run kills the child: the run ends `canceled` promptly, the child is gone.
//   P8  ... and the kill reaches the whole job / process group: a grandchild is gone too. Mutant F: kill only
//       the leader -> the grandchild survives.
//   P9  The wall deadline (a reactor timer) kills the child: `timed_out`, promptly.
//   P10 A grandchild holding stdout after the leader exited does not hang the run: after the drain grace the
//       group is killed and the run ends `exited` with the leader's status.
//   P11 A frame destroyed while a read is pending: nothing is posted afterwards, no crash, the child is killed.
//       Mutant G: the completion ignores the abandon -> a continuation is posted to a destroyed frame.
//   P12 A homeless waiter (raw resume) is REFUSED and counted, never resumed on the reactor thread. Mutant H.
//   P13 Reactor shutdown completes a pending exit wait `canceled` promptly and kills the child.
//   P14 Sticky cancel (round-2 M2): an exit wait cancelled before its start completes `canceled` at once.
//       Mutant I: ignore the sticky flag -> it waits for the (sleeping) child.
//   P15 The single-op awaitables: chunked reads to EOF, read_all, wait_exit (twice: the status is kept), and a
//       write to a child that exited reports `broken_pipe` as a value -- on Linux without SIGPIPE killing this
//       process. Mutant J (Linux): no SIGPIPE block on the reactor thread -> this test process dies.
//   P16 Spawn failures are values: a missing program (os_error), an empty program (invalid_spec).
//   P17 The environment is explicit: the child sees what the spec passes and nothing of the host's.
//   P18 argv round-trips (spaces, quotes, backslashes); P19 cwd is honoured.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fstream>
#include <unistd.h>
#endif

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_process.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/process.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"
#include "../support/crt_fail_fast.hpp"

#ifndef AE_PROCESS_CHILD_HELPER
#error "AE_PROCESS_CHILD_HELPER must name the helper executable (tests/rt/CMakeLists.txt)"
#endif

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

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
bool wait_until(Pred p, std::chrono::milliseconds limit = 10s) {
    auto const end = clock_type::now() + limit;
    while (!p()) {
        if (clock_type::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

long long ms_since(clock_type::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
}

pal::ProcessSpec helper(std::vector<std::string> args) {
    pal::ProcessSpec s;
    s.program = AE_PROCESS_CHILD_HELPER;
    s.argv.push_back(s.program);
    for (auto& a : args) s.argv.push_back(std::move(a));
#ifdef _WIN32
    // The explicit environment: only what the child needs to start (no ambient host variables).
    char        root[MAX_PATH];
    DWORD const n = GetEnvironmentVariableA("SystemRoot", root, MAX_PATH);
    if (n > 0 && n < MAX_PATH) s.env.push_back(std::string("SystemRoot=") + root);
#endif
    return s;
}

// True once `pid` no longer runs (gone, or a zombie) -- polled up to `limit`.
bool process_gone(std::int64_t pid, std::chrono::milliseconds limit = 5s) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return true;
    DWORD const w = WaitForSingleObject(h, static_cast<DWORD>(limit.count()));
    CloseHandle(h);
    return w == WAIT_OBJECT_0;
#else
    return wait_until(
        [pid] {
            std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
            if (!stat) return true;
            std::string line;
            std::getline(stat, line);
            auto const close = line.rfind(')');
            return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
        },
        limit);
#endif
}

std::int64_t parse_pid(std::string const& text) {
    auto const at = text.find("pid=");
    if (at == std::string::npos) return 0;
    return std::strtoll(text.c_str() + at + 4, nullptr, 10);
}

// A host Resumer with its own worker thread (as in test_rt_reactor_timer.cpp).
class ThreadResumer final : public Resumer {
public:
    ThreadResumer() : worker_([this] { loop(); }) {}
    ~ThreadResumer() override { stop(); }
    ThreadResumer(ThreadResumer const&)            = delete;
    ThreadResumer& operator=(ThreadResumer const&) = delete;

    void post(ParkedContinuation c) override {
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(c));
            ++posted_;
        }
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
    [[nodiscard]] std::thread::id id() const { return worker_.get_id(); }
    [[nodiscard]] int posted() {
        std::lock_guard lock(m_);
        return posted_;
    }

private:
    void loop() {
        for (;;) {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
        }
    }

    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    int                            posted_ = 0;
    bool                           stop_   = false;
    std::thread                    worker_;
};

struct ProbeOp final : pal::ReactorOp {
    std::atomic<bool> done{false};
    std::thread::id   where{};
    void on_complete(pal::op_status) noexcept override {
        where = std::this_thread::get_id();
        done.store(true, std::memory_order_release);
    }
};

std::thread::id reactor_thread_of(pal::Reactor& r) {
    auto probe = std::make_shared<ProbeOp>();
    r.start_timer(probe, pal::Reactor::clock::now());
    (void)wait_until([&] { return probe->done.load(std::memory_order_acquire); });
    return probe->where;
}

struct ExitProbe final : pal::ExitWaitOp {
    std::atomic<bool> done{false};
    pal::op_status    status = pal::op_status::completed;
    void on_complete(pal::op_status s) noexcept override {
        status = s;
        done.store(true, std::memory_order_release);
    }
};

// ---- coroutines: free functions taking their state by pointer or value (never lambda coroutines with
// captures -- ADR-237 §14 step 1's use-after-free) -------------------------------------------------------

struct Observed {
    ProcessRunResult r;
    std::thread::id  resumed_on{};
    long long        elapsed_ms = 0;
};

task<Observed> run_observed(pal::Reactor* reactor, pal::ProcessSpec spec, std::stop_token stop,
                            ProcessRunLimits limits, std::optional<std::string> in) {
    Observed   o;
    auto const t0 = clock_type::now();
    o.r           = co_await run_process(*reactor, std::move(spec), std::move(stop), limits, std::move(in));
    o.resumed_on  = std::this_thread::get_id();
    o.elapsed_ms  = ms_since(t0);
    co_return o;
}

Observed run(pal::Reactor& reactor, pal::ProcessSpec spec, ProcessRunLimits limits = {},
             std::optional<std::string> in = std::nullopt, std::stop_token stop = {}) {
    return block_on(run_observed(&reactor, std::move(spec), std::move(stop), limits, std::move(in)));
}

task<void> run_into(pal::Reactor* reactor, pal::ProcessSpec spec, std::optional<std::string> in, Observed* out,
                    std::atomic<int>* done) {
    *out = co_await run_observed(reactor, std::move(spec), {}, ProcessRunLimits{}, std::move(in));
    done->fetch_add(1, std::memory_order_acq_rel);
}

task<pal::SpawnResult> spawn_only(pal::Process* p, pal::ProcessSpec spec) {
    auto s = co_await spawn_process(*p, std::move(spec));
    co_return s.result;
}

task<ProcessOpOutcome<pal::ExitWaitResult>> exit_of(pal::Process* p) { co_return co_await wait_exit(*p); }

task<void> wait_then_flag(pal::Process* p, std::atomic<bool>* ran) {
    (void)co_await wait_exit(*p);
    ran->store(true, std::memory_order_release);
}

task<void> spawn_then_read(pal::Reactor* r, std::atomic<std::int64_t>* pid_out, std::atomic<bool>* ran_after) {
    std::shared_ptr<pal::Process> const proc = pal::make_process(*r);  // owned by this frame
    auto                                s    = co_await spawn_process(*proc, helper({"sleep"}));
    pid_out->store(s.result.pid, std::memory_order_release);
    (void)co_await read_output(*proc, pal::process_stream::out);  // a sleeper writes nothing: stays pending
    ran_after->store(true, std::memory_order_release);
}

struct SingleOps {
    std::string             out_chunks;
    int                     chunks = 0;
    bool                    out_eof = false;
    std::string             err_all;
    bool                    err_eof = false;
    pal::ExitWaitResult     first;
    pal::ExitWaitResult     second;
    bool                    canceled_any = false;
};

task<SingleOps> single_ops(pal::Reactor* r) {
    SingleOps                           o;
    std::shared_ptr<pal::Process> const proc = pal::make_process(*r);
    auto                                s    = co_await spawn_process(*proc, helper({"exit", "3"}));
    if (!s.result.ok()) co_return o;
    for (int i = 0; i < 100; ++i) {
        auto c = co_await read_output(*proc, pal::process_stream::out, 2);  // tiny chunks: several reads
        o.canceled_any = o.canceled_any || c.canceled;
        if (c.result.eof || c.canceled || c.result.error != pal::process_error::none) {
            o.out_eof = c.result.eof;
            break;
        }
        ++o.chunks;
        o.out_chunks += c.result.data;
    }
    auto e    = co_await read_all_output(*proc, pal::process_stream::err, 1024);
    o.err_all = e.result.data;
    o.err_eof = e.result.eof;
    auto w1   = co_await wait_exit(*proc);
    auto w2   = co_await wait_exit(*proc);
    o.first   = w1.result;
    o.second  = w2.result;
    co_return o;
}

struct BrokenPipe {
    pal::PipeWriteResult w;
    bool                 canceled = true;
    std::int64_t         code     = -1;
};

task<BrokenPipe> write_after_exit(pal::Reactor* r) {
    BrokenPipe                          o;
    std::shared_ptr<pal::Process> const proc = pal::make_process(*r);
    pal::ProcessSpec                    spec = helper({"exit", "0"});
    spec.stdin_pipe                          = true;
    auto s                                   = co_await spawn_process(*proc, std::move(spec));
    if (!s.result.ok()) co_return o;
    auto x     = co_await wait_exit(*proc);
    o.code     = x.result.exit.code;
    auto w     = co_await write_stdin(*proc, std::string(1024 * 1024, 'z'));
    o.w        = w.result;
    o.canceled = w.canceled;
    co_return o;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(240s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 240 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main

    auto                  reactor   = pal::make_default_reactor();
    std::thread::id const reactor_t = reactor_thread_of(*reactor);
    check(reactor_t != std::thread::id{} && reactor_t != std::this_thread::get_id(),
          "setup: the reactor runs its own thread");

    // P1 ------------------------------------------------------------------------------------------------
    {
        Observed o = run(*reactor, helper({"exit", "7"}));
        check(o.r.end == run_end::exited, "P1: the run ends `exited`");
        check(o.r.exit.known && o.r.exit.code == 7, "P1: exit code 7 captured (" + std::to_string(o.r.exit.code) + ")");
        check(o.r.stdout_text == "out-7" && o.r.stderr_text == "err-7", "P1: stdout and stderr captured separately");
        check(!o.r.output_incomplete && !o.r.exit.killed, "P1: both pipes reached EOF; nothing was killed");
        check(o.resumed_on == std::this_thread::get_id(), "P1: resumed on the block_on() thread (its home)");
        check(o.resumed_on != reactor_t, "P1: not on the reactor thread");
    }

    // P1b -----------------------------------------------------------------------------------------------
    {
        auto             resumer = std::make_shared<ThreadResumer>();
        std::atomic<int> done{0};
        Observed         o;
        auto             t = run_into(reactor.get(), helper({"exit", "1"}), std::nullopt, &o, &done);
        {
            ScopedResumer scope(resumer);
            t.resume();
        }
        check(wait_until([&] { return done.load() == 1; }), "P1b: the run finished under a host Resumer");
        check(o.r.end == run_end::exited && o.r.exit.code == 1, "P1b: exit code 1");
        check(o.resumed_on == resumer->id(), "P1b: resumed on the host Resumer's thread");
        check(o.resumed_on != reactor_t, "P1b: not on the reactor thread");
        resumer->stop();
    }

    // P2 ------------------------------------------------------------------------------------------------
    {
        std::string input(1024 * 1024, '\0');
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<char>('a' + (i * 7) % 26);
        ProcessRunLimits lim;
        lim.wall   = 60s;
        Observed o = run(*reactor, helper({"echo"}), lim, input);
        check(o.r.end == run_end::exited && o.r.exit.code == 0, "P2: echo of 1 MiB exits 0");
        check(o.r.stdout_text == input, "P2: stdout equals the 1 MiB written to stdin (" +
                                            std::to_string(o.r.stdout_text.size()) + " bytes)");
    }

    // P3 ------------------------------------------------------------------------------------------------
    {
        ProcessRunLimits lim;
        lim.wall   = 30s;
        Observed o = run(*reactor, helper({"flood", "16"}), lim);
        check(o.r.end == run_end::exited && o.r.exit.code == 0,
              "P3 (gate 4, H-e): a child writing 16 MiB before exiting completes normally (" +
                  std::to_string(o.elapsed_ms) + " ms)");
        check(o.r.stdout_text.size() == 16u * 1024 * 1024 && !o.r.stdout_truncated,
              "P3: all 16 MiB captured (" + std::to_string(o.r.stdout_text.size()) + ")");
        check(o.r.stdout_text.find_first_not_of('a') == std::string::npos && o.r.stderr_text == "done",
              "P3: the bytes are intact and stderr was read concurrently");
    }

    // P4 ------------------------------------------------------------------------------------------------
    {
        ProcessRunLimits lim;
        lim.wall             = 30s;
        lim.output_cap_bytes = 1024 * 1024;
        Observed o           = run(*reactor, helper({"flood", "4"}), lim);
        check(o.r.end == run_end::exited, "P4: a child writing 4 MiB past a 1 MiB cap still exits normally");
        check(o.r.stdout_text.size() == 1024u * 1024 && o.r.stdout_truncated,
              "P4: exactly the first 1 MiB is kept, `truncated` set (" + std::to_string(o.r.stdout_text.size()) + ")");
        check(!o.r.output_incomplete, "P4: the rest was read to EOF, not abandoned in the pipe");
    }

    // P5 ------------------------------------------------------------------------------------------------
    {
        constexpr int         kN = 64;
        auto                  resumer = std::make_shared<ThreadResumer>();
        std::atomic<int>      done{0};
        std::vector<Observed> obs(kN);
        std::vector<task<void>> tasks;
        tasks.reserve(kN);
        auto const t0 = clock_type::now();
        for (int i = 0; i < kN; ++i) {
            bool const sleeper = i % 2 == 0;
            tasks.push_back(run_into(reactor.get(), sleeper ? helper({"sleepms", "4000"}) : helper({"echo"}),
                                     sleeper ? std::nullopt : std::optional<std::string>("x"), &obs[i], &done));
            ScopedResumer scope(resumer);
            tasks.back().resume();
        }
        bool const all = wait_until([&] { return done.load() == kN; }, 60s);
        check(all, "P5: 64 concurrent runs all finished (" + std::to_string(done.load()) + "/64)");
        int       ok = 0, echoes_fast = 0;
        long long slowest_echo = 0;
        for (int i = 0; i < kN && all; ++i) {
            bool const       sleeper = i % 2 == 0;
            Observed const&  o       = obs[i];
            bool const       right   = o.r.end == run_end::exited && o.r.exit.code == 0 && !o.r.output_incomplete &&
                               o.r.stdout_text == (sleeper ? "slept" : "x");
            if (right) ++ok;
            if (!sleeper) {
                slowest_echo = std::max(slowest_echo, o.elapsed_ms);
                if (o.elapsed_ms < 3000) ++echoes_fast;
            }
        }
        check(ok == kN, "P5 (gate 1d): every child exited with its own output and reached EOF -- " +
                            std::to_string(ok) + "/64");
        check(echoes_fast == kN / 2, "P5 (gate 1d): every echo child reached EOF at its own exit, not a "
                                     "sleeping sibling's (" + std::to_string(echoes_fast) + "/32 under 3 s; "
                                     "slowest " + std::to_string(slowest_echo) + " ms)");
        check(ms_since(t0) < 30000, "P5: the whole batch took one sleeper's time, not a sum");
        check(resumer->posted() >= kN, "P5: 64 parked runs were driven by ONE host thread");
        resumer->stop();
    }

    // P6 ------------------------------------------------------------------------------------------------
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};  // a careless component: inheritable both ends
        HANDLE              rd = nullptr, wr = nullptr;
        bool const          made = CreatePipe(&rd, &wr, &sa, 0) != 0;
#else
        int        fds[2] = {-1, -1};
        bool const made   = ::pipe(fds) == 0;  // no O_CLOEXEC: what another component of the host might leak
#endif
        check(made, "P6: setup: a foreign inheritable pipe exists in this process");
        Observed    o;
        std::thread runner([&] { o = run(*reactor, helper({"sleepms", "3000"})); });
        std::this_thread::sleep_for(500ms);  // the child is running now
        auto const t0 = clock_type::now();
#ifdef _WIN32
        CloseHandle(wr);
        char  b;
        DWORD n = 0;
        while (ReadFile(rd, &b, 1, &n, nullptr) != 0 && n > 0) {
        }
        CloseHandle(rd);
#else
        ::close(fds[1]);
        char b;
        while (::read(fds[0], &b, 1) > 0) {
        }
        ::close(fds[0]);
#endif
        long long const eof_ms = ms_since(t0);
        runner.join();
        check(eof_ms < 1500, "P6 (G4): the child did not inherit a foreign inheritable handle -- EOF " +
                                 std::to_string(eof_ms) + " ms after our close, not at the child's exit");
        check(o.r.end == run_end::exited, "P6: the child itself ran normally");
    }

    // P7 ------------------------------------------------------------------------------------------------
    {
        std::stop_source src;
        std::thread      stopper([&] {
            std::this_thread::sleep_for(300ms);
            src.request_stop();
        });
        Observed o = run(*reactor, helper({"sleep"}), {}, std::nullopt, src.get_token());
        stopper.join();
        check(o.r.end == run_end::canceled, "P7: a stop mid-run ends it `canceled`");
        check(o.elapsed_ms < 5000, "P7: promptly (" + std::to_string(o.elapsed_ms) + " ms)");
        check(o.r.exit.killed, "P7: the exit is reported as a kill");
        check(process_gone(o.r.spawn.pid), "P7: the child is gone");
        check(o.resumed_on == std::this_thread::get_id(), "P7: still resumed on its home thread");
    }

    // P8 ------------------------------------------------------------------------------------------------
    {
        std::stop_source src;
        std::thread      stopper([&] {
            std::this_thread::sleep_for(1000ms);
            src.request_stop();
        });
        Observed o = run(*reactor, helper({"grandchild"}), {}, std::nullopt, src.get_token());
        stopper.join();
        std::int64_t const grandchild = parse_pid(o.r.stdout_text);
        check(o.r.end == run_end::canceled && o.elapsed_ms < 5000, "P8: a stopped run with a grandchild ends "
                                                                   "`canceled` promptly");
        check(grandchild > 0, "P8: the grandchild's pid was read from the partial output (" +
                                  std::to_string(grandchild) + ")");
        check(process_gone(o.r.spawn.pid), "P8: the child is gone");
        check(process_gone(grandchild), "P8: the GRANDCHILD is gone too (job / process-group kill)");
    }

    // P9 ------------------------------------------------------------------------------------------------
    {
        ProcessRunLimits lim;
        lim.wall   = 500ms;
        Observed o = run(*reactor, helper({"sleep"}), lim);
        check(o.r.end == run_end::timed_out, "P9: the wall deadline ends the run `timed_out`");
        check(o.elapsed_ms >= 450 && o.elapsed_ms < 5000, "P9: at the deadline, promptly (" +
                                                              std::to_string(o.elapsed_ms) + " ms)");
        check(o.r.exit.killed && process_gone(o.r.spawn.pid), "P9: the child was killed and is gone");
    }

    // P10 -----------------------------------------------------------------------------------------------
    {
        ProcessRunLimits lim;
        lim.wall             = 30s;
        lim.exit_drain_grace = 500ms;
        Observed           o          = run(*reactor, helper({"orphan"}), lim);
        std::int64_t const grandchild = parse_pid(o.r.stdout_text);
        check(o.r.end == run_end::exited && o.r.exit.code == 0,
              "P10: the leader's own exit is reported although a grandchild held stdout");
        check(o.elapsed_ms < 5000, "P10: the drain grace ended the wait (" + std::to_string(o.elapsed_ms) + " ms)");
        check(grandchild > 0 && process_gone(grandchild), "P10: the lingering grandchild was killed");
    }

    // P11 -----------------------------------------------------------------------------------------------
    {
        auto                      resumer = std::make_shared<ThreadResumer>();
        std::atomic<std::int64_t> pid{0};
        std::atomic<bool>         ran_after{false};
        int                       posted_before = 0;
        {
            auto t = spawn_then_read(reactor.get(), &pid, &ran_after);
            {
                ScopedResumer scope(resumer);
                t.resume();
            }
            check(wait_until([&] { return pid.load() > 0; }), "P11: setup: the child was spawned");
            std::this_thread::sleep_for(200ms);  // parked on the read by now
            posted_before = resumer->posted();
        }  // frame destroyed while the read is pending
        std::this_thread::sleep_for(300ms);
        check(resumer->posted() == posted_before && !ran_after.load(),
              "P11: a frame destroyed during a pending read gets nothing posted afterwards");
        check(process_gone(pid.load()), "P11: the child (owned by the dead frame) was killed");
        resumer->stop();
    }

    // P12 -----------------------------------------------------------------------------------------------
    {
        auto                proc   = pal::make_process(*reactor);
        pal::SpawnResult    s      = block_on(spawn_only(proc.get(), helper({"exit", "0"})));
        std::uint64_t const before = reactor->homeless_refusals();
        std::atomic<bool>   ran{false};
        auto                t = wait_then_flag(proc.get(), &ran);
        t.resume();  // homeless: no block_on, no ScopedResumer
        check(s.ok() && wait_until([&] { return reactor->homeless_refusals() == before + 1; }, 5s),
              "P12: the homeless wake-up was refused and counted");
        std::this_thread::sleep_for(50ms);
        check(!ran.load(), "P12: the homeless waiter was never resumed (not on the reactor thread, not anywhere)");
    }

    // P13 -----------------------------------------------------------------------------------------------
    {
        auto                                   doomed = pal::make_default_reactor();
        auto                                   proc   = pal::make_process(*doomed);
        pal::SpawnResult const                 s      = block_on(spawn_only(proc.get(), helper({"sleep"})));
        ProcessOpOutcome<pal::ExitWaitResult>  w;
        std::atomic<bool>                      done{false};
        pal::Process*                          raw = proc.get();
        std::thread                            waiter([&w, &done, raw] {
            w = block_on(exit_of(raw));
            done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(200ms);
        auto const t0 = clock_type::now();
        doomed.reset();  // shutdown with a pending exit wait
        check(ms_since(t0) < 5000, "P13: reactor shutdown returned promptly");
        waiter.join();
        check(s.ok() && done.load() && w.canceled, "P13: the pending exit wait completed `canceled`, not dropped");
        check(process_gone(s.pid), "P13: shutdown killed the child");
        proc.reset();  // after its reactor: torn down already, posts nothing
        check(true, "P13: a Process destroyed after its reactor is harmless");
    }

    // P14 -----------------------------------------------------------------------------------------------
    {
        auto                   proc  = pal::make_process(*reactor);
        pal::SpawnResult const s     = block_on(spawn_only(proc.get(), helper({"sleep"})));
        auto                   probe = std::make_shared<ExitProbe>();
        reactor->cancel(probe);  // before the start: only the sticky flag can carry it
        proc->start_wait_exit(probe);
        check(s.ok() && wait_until([&] { return probe->done.load(std::memory_order_acquire); }, 5s),
              "P14: an exit wait cancelled before its start completes at once (sticky cancel)");
        check(probe->status == pal::op_status::canceled, "P14: ... as `canceled`");
        proc.reset();
        check(process_gone(s.pid), "P14: dropping the Process killed the sleeper");
    }

    // P15 -----------------------------------------------------------------------------------------------
    {
        SingleOps o = block_on(single_ops(reactor.get()));
        check(o.out_chunks == "out-3" && o.out_eof && o.chunks >= 3,
              "P15: chunked reads deliver stdout then EOF (" + std::to_string(o.chunks) + " chunks)");
        check(o.err_all == "err-3" && o.err_eof, "P15: read_all_output reads stderr to EOF");
        check(o.first.exit.known && o.first.exit.code == 3 && o.second.exit.code == 3,
              "P15: wait_exit reports 3, and a second wait returns the kept status");
        BrokenPipe b = block_on(write_after_exit(reactor.get()));
        check(b.code == 0 && !b.canceled && b.w.broken_pipe,
              "P15: a write to an exited child reports `broken_pipe` as a value (this process is alive)");
    }

    // P16 -----------------------------------------------------------------------------------------------
    {
        pal::ProcessSpec missing = helper({});
        missing.program          = (std::filesystem::temp_directory_path() / "ae-no-such-program-xyz").string();
        Observed o               = run(*reactor, missing);
        check(o.r.end == run_end::spawn_failed && o.r.spawn.error == pal::process_error::os_error &&
                  o.r.spawn.os_error != 0,
              "P16: a missing program is a spawn failure value (" + o.r.spawn.message + ")");
        pal::ProcessSpec empty;
        Observed         e = run(*reactor, empty);
        check(e.r.end == run_end::spawn_failed && e.r.spawn.error == pal::process_error::invalid_spec,
              "P16: an empty program is `invalid_spec`");
    }

    // P17 -----------------------------------------------------------------------------------------------
    {
#ifdef _WIN32
        _putenv_s("AE_PROCESS_HOST_ONLY", "leak");
#else
        ::setenv("AE_PROCESS_HOST_ONLY", "leak", 1);
#endif
        pal::ProcessSpec with = helper({"env", "AE_PROCESS_TEST_VAR"});
        with.env.push_back("AE_PROCESS_TEST_VAR=hello world");
        Observed a = run(*reactor, with);
        check(a.r.stdout_text == "hello world", "P17: the child sees the variable the spec passes (" +
                                                    a.r.stdout_text + ")");
        Observed b = run(*reactor, helper({"env", "AE_PROCESS_HOST_ONLY"}));
        check(b.r.stdout_text == "<unset>", "P17: and not the host's own (" + b.r.stdout_text + ")");
        pal::ProcessSpec inherit = helper({"env", "AE_PROCESS_HOST_ONLY"});
        inherit.inherit_environment = true;
        Observed c                  = run(*reactor, inherit);
        check(c.r.stdout_text == "leak", "P17: unless inherit_environment asks for it");
    }

    // P18 / P19 -----------------------------------------------------------------------------------------
    {
        std::vector<std::string> const args{"two words", "quo\"te", "back\\slash\\", "", "tail"};
        std::vector<std::string>       argv{"argv"};
        argv.insert(argv.end(), args.begin(), args.end());
        Observed    o = run(*reactor, helper(argv));
        std::string expect;
        for (auto const& a : args) expect += a + "\n";
        std::string const& got = o.r.stdout_text;  // the helper's stdout is binary: "\n" on every OS
        check(got == expect, "P18: argv round-trips (spaces, quotes, backslashes, empty)");

        auto const       dir = std::filesystem::temp_directory_path();
        pal::ProcessSpec cwd = helper({"cwd"});
        cwd.cwd              = dir.string();
        Observed d           = run(*reactor, cwd);
        std::error_code  ec;
        check(std::filesystem::equivalent(std::filesystem::path(d.r.stdout_text), dir, ec),
              "P19: cwd is honoured (" + d.r.stdout_text + ")");
    }

    reactor.reset();
    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
