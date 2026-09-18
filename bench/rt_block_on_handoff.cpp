// decisions/ADR-175 -- hand-off costs of the resume-home model: block_on(), AsyncMutex under contention,
// channel<T> with a block_on() consumer, ThreadPool throughput, AsyncMutex::is_held_by_current_thread().
// Prints numbers; asserts nothing. Written by ADR-175's third red-team round to measure the change against
// HEAD, and kept so the next change to these primitives is measured the same way.
//
// There is no bench build yet (bench/README.md, RFC 023). Build and run by hand, Release, no sanitizer:
//   MSVC:   cl /nologo /std:c++latest /EHsc /O2 /I include /I tests bench\rt_block_on_handoff.cpp
//   g++-14: g++-14 -std=c++23 -O2 -pthread -I include -I tests bench/rt_block_on_handoff.cpp
// To compare against another revision, put that revision's include/agentengine/rt headers in a separate
// directory passed with /I (or -I) BEFORE include -- never next to this file, which MSVC searches first.
//
// Reference numbers (ADR-175 §7, MSVC 14.51 /O2, 3rd repetition; HEAD = before ADR-175):
//   block_on(trivial)                       HEAD 137 ns    ADR-175 148 ns
//   block_on(uncontended lock)              HEAD 193 ns    ADR-175 195 ns
//   co_await lock/unlock inside a block_on  HEAD  39 ns    ADR-175  27 ns
//   is_held_by_current_thread()             HEAD 2.5 ns    ADR-175 2.0 ns
//   4 threads x block_on(contended lock)    HEAD 564 ns    ADR-175 ~850 ns   (7,280 ns before the spin)
//   channel cap 1, producer thread          HEAD  67 ns    ADR-175 ~750 ns   (HEAD ran the consumer inline)
//   ThreadPool(4) trivial jobs              unchanged
// Thread count is fixed at 4 (CLAUDE.md machine safety); memory is capped.

#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/channel.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "support/memory_cap.hpp"

using namespace agentengine::rt;
using clk = std::chrono::steady_clock;

namespace {

double ns_per(clk::time_point a, clk::time_point b, double n) {
    return std::chrono::duration<double, std::nano>(b - a).count() / n;
}

task<int> trivial() { co_return 1; }
task<int> lock_once(AsyncMutex& m) {
    auto g = co_await m.lock();
    co_return 1;
}
task<long> lock_loop(AsyncMutex& m, int n) {
    long s = 0;
    for (int i = 0; i < n; ++i) {
        auto g = co_await m.lock();
        ++s;
    }
    co_return s;
}
task<int> bump(AsyncMutex& m, long& c) {
    auto g = co_await m.lock();
    ++c;
    co_return 0;
}
task<void> empty_job() { co_return; }
task<long> consume_all(channel_consumer<int>& c) {
    long s = 0;
    while (auto v = co_await c.next_async()) s += *v;
    co_return s;
}

}  // namespace

int main() {
    (void)agentengine::test_support::cap_process_memory(std::size_t{512} << 20, std::size_t{2048} << 20);
    for (int rep = 0; rep < 3; ++rep) {
        constexpr int N = 2000000;
        AsyncMutex m;
        long s = 0;
        auto const a = clk::now();
        for (int i = 0; i < N; ++i) s += block_on(trivial());
        auto const b = clk::now();
        for (int i = 0; i < N; ++i) s += block_on(lock_once(m));
        auto const c = clk::now();
        s += block_on(lock_loop(m, N));
        auto const d = clk::now();
        long held = 0;
        for (int i = 0; i < 10 * N; ++i) held += m.is_held_by_current_thread() ? 1 : 0;
        auto const e = clk::now();
        std::printf("[rep %d] block_on(trivial) %.1f ns | block_on(lock) %.1f ns | co_await lock/unlock in one "
                    "block_on %.1f ns | is_held %.2f ns (%ld %ld)\n",
                    rep, ns_per(a, b, N), ns_per(b, c, N), ns_per(c, d, N), ns_per(d, e, 10.0 * N), s, held);

        {
            AsyncMutex cm;
            long counter = 0;
            constexpr int per = 50000;
            auto const t0 = clk::now();
            std::vector<std::thread> ts;
            for (int t = 0; t < 4; ++t) {
                ts.emplace_back([&] {
                    for (int i = 0; i < per; ++i) (void)block_on(bump(cm, counter));
                });
            }
            for (auto& t : ts) t.join();
            auto const t1 = clk::now();
            std::printf("[rep %d] contended 4 threads x %d block_on(lock): %.1f ns/acq (count %ld)\n", rep, per,
                        ns_per(t0, t1, 4.0 * per), counter);
        }
        {
            ThreadPool pool(4);
            constexpr int J = 200000;
            auto const t0 = clk::now();
            std::vector<std::future<JobOutcome>> fs;
            fs.reserve(J);
            for (int i = 0; i < J; ++i) fs.push_back(pool.submit(empty_job()));
            for (auto& f : fs) (void)f.get();
            auto const t1 = clk::now();
            std::printf("[rep %d] ThreadPool(4) %d trivial jobs: %.1f ns/job\n", rep, J, ns_per(t0, t1, J));
        }
        {
            auto pair = make_channel<int>(1);
            constexpr int K = 200000;
            auto const t0 = clk::now();
            std::thread prod([&] {
                for (int i = 0; i < K; ++i) (void)pair.producer.push(1);
                pair.producer.close();
            });
            long const got = block_on(consume_all(pair.consumer));
            prod.join();
            auto const t1 = clk::now();
            std::printf("[rep %d] channel cap 1, %d items, block_on consumer: %.1f ns/item (got %ld)\n", rep, K,
                        ns_per(t0, t1, K), got);
        }
    }
    return 0;
}
