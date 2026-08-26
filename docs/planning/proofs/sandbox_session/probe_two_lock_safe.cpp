// PROVE-PHASE POSITIVE PROBE for §15.3's two-lock MediatedFileSystem fix -- the actual antidote to
// probe_deadlock_demo.cpp's reproduced self-deadlock. Three claims tested for real: (1) a sync
// write() call completes promptly even while a slow async drain/commit is in flight on the OTHER
// lock; (2) no write is ever lost or duplicated across the copy-and-clear boundary; (3) concurrent
// writes from multiple real threads, interleaved with real drains, still reconcile exactly.

#include "media_fs.hpp"
#include "../common/block_on.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;
    using namespace std::chrono;

    // --- Test 1: write() stays fast even while a slow async drain holds commit_lock_ -------------
    {
        MediatedFileSystem fs;
        std::atomic<bool> drain_started{false};

        std::thread drain_thread([&]() {
            drain_started.store(true, std::memory_order_release);
            auto batch = block_on(fs.drain_staged_writes(/*simulated_commit_millis=*/300));
            CHECK(batch.has_value());
        });
        while (!drain_started.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(milliseconds(20));  // let the drain thread get well into its
                                                           // 300ms simulated commit, holding
                                                           // commit_lock_

        auto t0 = steady_clock::now();
        auto w = fs.write("some/path.txt", {std::byte{1}, std::byte{2}}, /*author_id=*/42);
        auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        CHECK(w.has_value());
        CHECK(elapsed_ms < 100);   // MUST be near-instant (well under the 300ms commit still in
                                    // flight) -- if write() shared commit_lock_, this would take
                                    // roughly the REMAINDER of the 300ms window instead

        drain_thread.join();
        std::printf("[1] sync write() while a slow (300ms) async drain holds the SEPARATE commit "
                    "lock: PASS (write() completed in %lldms, not ~300ms -- no lock sharing, no "
                    "deadlock)\n", (long long)elapsed_ms);
    }

    // --- Test 2: no write lost or duplicated across the copy-and-clear boundary -------------------
    {
        MediatedFileSystem fs;
        (void)fs.write("a", {}, 1);
        (void)fs.write("b", {}, 1);
        auto batch1 = block_on(fs.drain_staged_writes(0));
        CHECK(batch1.has_value());
        CHECK(batch1->size() == 2);

        // A write AFTER the drain must show up in the NEXT drain, not be lost, and not appear
        // twice.
        (void)fs.write("c", {}, 1);
        auto batch2 = block_on(fs.drain_staged_writes(0));
        CHECK(batch2.has_value());
        CHECK(batch2->size() == 1);
        CHECK((*batch2)[0].path == "c");

        // A drain with nothing staged returns an empty batch, not an error and not stale data.
        auto batch3 = block_on(fs.drain_staged_writes(0));
        CHECK(batch3.has_value());
        CHECK(batch3->empty());
        std::printf("[2] copy-and-clear boundary: PASS (no write lost, none duplicated, no stale "
                    "re-delivery)\n");
    }

    // --- Test 3: real concurrent writers + concurrent drains, total count reconciles exactly ------
    {
        MediatedFileSystem fs;
        constexpr int kWriters = 8;
        constexpr int kWritesPerThread = 500;
        std::atomic<int> total_written{0};
        std::atomic<bool> stop_draining{false};
        std::atomic<int> total_drained{0};

        std::thread drainer([&]() {
            while (!stop_draining.load(std::memory_order_acquire)) {
                auto batch = block_on(fs.drain_staged_writes(0));
                if (batch.has_value()) total_drained.fetch_add(static_cast<int>(batch->size()));
                std::this_thread::yield();
            }
            // Final drain to catch anything staged after the last writer finished but before this
            // loop's stop flag was observed.
            auto final_batch = block_on(fs.drain_staged_writes(0));
            if (final_batch.has_value()) total_drained.fetch_add(static_cast<int>(final_batch->size()));
        });

        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&, w]() {
                for (int i = 0; i < kWritesPerThread; ++i) {
                    auto r = fs.write("w" + std::to_string(w) + "/" + std::to_string(i), {},
                                       static_cast<std::uint64_t>(w));
                    if (r.has_value()) total_written.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : writers) th.join();
        stop_draining.store(true, std::memory_order_release);
        drainer.join();

        std::printf("[3] concurrent writers/drainer: total_written=%d, total_drained=%d\n",
                    total_written.load(), total_drained.load());
        CHECK(total_written.load() == kWriters * kWritesPerThread);
        CHECK(total_drained.load() == total_written.load());
        std::printf("[3] CONCURRENT sync write() from %d real threads racing a real async drain "
                    "loop: PASS (every write drained exactly once, none lost, none duplicated)\n",
                    kWriters);
    }

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
