// ADR-179 stage 0 -- concurrent writers to ONE principal's memory ref.
//
// `write_memory_item()` (core/memory.hpp) is read-modify-write: it predicts `write_seq` from
// `last_seq()+1`, then `mount_write()` reads the ref's current tree, adds the blob, and appends a
// `RefMoved` for the new tree. `commit_ref` is a plain append with no compare-and-swap, so two writers
// that read the same base tree each produce a tree holding only THEIR item and the later append wins:
// the other item is silently lost, and both stamped a `write_seq` that the log no longer matches. The
// documented residual ("assumes no OTHER writer ... to the SAME ref") is not academic: memory is per
// PRINCIPAL and a principal can have many sessions, each with its own MemoryProvider::on_turn_end.
//
// Claims:
//   W1  N threads x M distinct items -> every one of the N*M items is present afterwards (none lost);
//   W2  every item's stamped `write_seq` is unique;
//   W3  the stamped `write_seq` values are exactly the ref log's own sequence numbers of those commits, i.e.
//       they are consecutive (max - min + 1 == count);
//   W4  writers to DIFFERENT refs are not serialized against each other (the lock is per ref, not global),
//       checked by structure (two distinct locks are handed out) rather than by timing.

#include <atomic>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/memory.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

constexpr int kThreads = 6;
constexpr int kPerThread = 40;

}  // namespace

int main() {
    ae::InMemoryWorktreeObjectStore object_store;
    ae::rt::InMemoryAppendLogStore ref_store;
    ae::Principal const principal{"p-concurrent", ""};
    AE_CHECK(ae::ensure_memory_worktree(object_store, ref_store, principal).has_value(),
             "setup: the memory worktree bootstraps");

    ae::Mount const mount = ae::memory_mount(principal);
    ae::cap::FsRead const read_cap{ae::memory_mount_id(principal), "", std::nullopt};
    ae::cap::FsWrite const write_cap{ae::memory_mount_id(principal), "", std::nullopt, std::nullopt};

    std::atomic<bool> go{false};
    std::atomic<int> write_errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) { /* spin: maximise overlap */ }
            for (int i = 0; i < kPerThread; ++i) {
                ae::MemoryItem item{};
                item.kind     = ae::memory_kind::episodic;
                item.content  = "thread " + std::to_string(t) + " item " + std::to_string(i);
                item.salience = 0.5f;
                item.origin   = ae::MemoryOrigin{ae::memory_source::tool_derived, "run", "0", principal};
                if (!ae::write_memory_item(object_store, ref_store, mount, write_cap, item)) {
                    write_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    auto listed = ae::list_memory_items(object_store, ref_store, mount, read_cap);
    AE_CHECK(listed.has_value(), "the final listing decodes");
    if (!listed) return 1;

    std::size_t const expected = static_cast<std::size_t>(kThreads) * kPerThread;
    AE_CHECK(write_errors.load() == 0, "no write returned an error");
    AE_CHECK(listed->size() == expected, "W1: every item written by every thread is present (none lost)");
    std::cout << "  .. items present: " << listed->size() << " of " << expected << "\n";

    std::set<std::uint64_t> seqs;
    std::uint64_t lo = ~0ull, hi = 0;
    for (auto const& it : *listed) {
        seqs.insert(it.write_seq);
        lo = std::min(lo, it.write_seq);
        hi = std::max(hi, it.write_seq);
    }
    AE_CHECK(seqs.size() == listed->size(), "W2: every stamped write_seq is unique");
    AE_CHECK(!listed->empty() && hi - lo + 1 == listed->size(),
             "W3: the stamped write_seq values are consecutive -- they match the ref log's own numbering");

    // W4: the lock is per ref. Two different ref names must be handed different mutexes.
    AE_CHECK(&ae::memory_detail::ref_write_mutex("principal:a") !=
                 &ae::memory_detail::ref_write_mutex("principal:b"),
             "W4: distinct refs get distinct locks (writers to different principals are not serialized)");
    AE_CHECK(&ae::memory_detail::ref_write_mutex("principal:a") ==
                 &ae::memory_detail::ref_write_mutex("principal:a"),
             "W4: the same ref always gets the same lock");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_memory_write_concurrency: all checks passed\n";
    return 0;
}
