// PROVE-PHASE PROBE: real concurrent contention on IdentityAuthority::adopt() -- §25.4/§26.4 named
// this as not stress-tested. adopt()'s critical section is a plain std::mutex (§15.1/§16's own
// choice: identity minting is rare/host-driven, a plain mutex is the right tool) -- this probe checks
// that choice actually holds under real multi-threaded load, not just single-threaded correctness.

#include "identity_authority.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
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
    IdentityAuthority& authority = IdentityAuthority::bootstrap();

    // === Test 1: many threads racing to adopt the SAME real id -- must all resolve to ONE internal id
    {
        constexpr int kThreads = 16;
        constexpr int kCallsPerThread = 500;
        std::vector<std::uint64_t> results(kThreads * kCallsPerThread);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kCallsPerThread; ++i) {
                    Principal p = authority.adopt("contended-real-id", "");
                    results[static_cast<std::size_t>(t) * kCallsPerThread + i] = p.id();
                }
            });
        }
        for (auto& th : threads) th.join();

        std::uint64_t const first = results[0];
        bool all_same = true;
        for (auto id : results) if (id != first) { all_same = false; break; }
        std::printf("[1] %d threads x %d adopt() calls on the SAME real id, concurrently: all "
                    "%zu results resolved to internal id=%llu -- all_same=%d\n",
                    kThreads, kCallsPerThread, results.size(), (unsigned long long)first,
                    (int)all_same);
        CHECK(all_same);
    }

    // === Test 2: many threads adopting DISTINCT real ids concurrently -- no id collisions, no lost
    // registrations (every distinct real id ends up with its own, distinct internal id) ============
    {
        constexpr int kThreads = 16;
        constexpr int kIdsPerThread = 200;
        std::vector<std::vector<std::uint64_t>> per_thread_results(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            per_thread_results[t].resize(kIdsPerThread);
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kIdsPerThread; ++i) {
                    std::string real_id = "distinct-" + std::to_string(t) + "-" + std::to_string(i);
                    Principal p = authority.adopt(real_id, "");
                    per_thread_results[t][i] = p.id();
                }
            });
        }
        for (auto& th : threads) th.join();

        std::set<std::uint64_t> seen;
        std::size_t total = 0;
        bool no_collisions = true;
        for (auto const& vec : per_thread_results) {
            for (auto id : vec) {
                ++total;
                if (!seen.insert(id).second) { no_collisions = false; }
            }
        }
        std::printf("[2] %d threads x %d DISTINCT real ids, concurrently: %zu total adoptions, "
                    "%zu unique internal ids, no_collisions=%d\n",
                    kThreads, kIdsPerThread, total, seen.size(), (int)no_collisions);
        CHECK(no_collisions);
        CHECK(seen.size() == total);   // every distinct real id got its own distinct internal id --
                                         // nothing silently merged or dropped under contention
    }

    // === Test 3: idempotency holds even when the FIRST adopt() of a given real id is itself racing
    // (not just re-adopting something already settled, as tests 1 covers with warm contention -- this
    // specifically races the INITIAL registration moment for several distinct ids at once) ==========
    {
        constexpr int kIds = 50;
        constexpr int kThreadsPerId = 8;
        bool all_consistent = true;
        for (int idx = 0; idx < kIds; ++idx) {
            std::string real_id = "race-first-" + std::to_string(idx);
            std::vector<std::uint64_t> results(kThreadsPerId);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreadsPerId; ++t) {
                threads.emplace_back([&, t]() { results[t] = authority.adopt(real_id, "").id(); });
            }
            for (auto& th : threads) th.join();
            std::uint64_t const first = results[0];
            for (auto id : results) if (id != first) all_consistent = false;
        }
        std::printf("[3] %d distinct real ids, each raced by %d threads at their FIRST-ever adopt() "
                    "call: all_consistent=%d (every id resolved to exactly one internal id, even "
                    "under first-registration race)\n", kIds, kThreadsPerId, (int)all_consistent);
        CHECK(all_consistent);
    }

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
