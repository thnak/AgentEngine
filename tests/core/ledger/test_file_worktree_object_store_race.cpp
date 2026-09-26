// Regression proof for GitHub issue #69: `FileWorktreeObjectStore::put_blob()` used the THROWING
// `std::filesystem::is_regular_file()` overload for its dedup check, on the one path the surrounding
// code documents as being concurrently renamed onto.
//
// THE CHAIN, every link of it measured rather than reasoned about, on a real Windows 11 host:
//
//   1. `is_regular_file()`'s throwing overload raises `filesystem_error{"status: Access is denied"}`
//      when a sibling thread renames onto the same path -- 15263 times in 2665982 queries in the
//      probe that found this. It is a narrow window, not a rare cosmic ray: roughly one query in 175.
//   2. Nothing in `put_blob()` caught it, so the exception escaped a function whose entire contract
//      is to fail closed into `result<T>` -- past twenty lines of comments establishing exactly that.
//   3. `tests/core/ledger/test_content_durability_concurrency.cpp`'s [1b] case calls `put_blob()` from sixteen
//      `std::thread` bodies with no handler anywhere.
//   4. An exception escaping a `std::thread` body on this toolchain exits the process with
//      0xC0000409 (measured directly, not inferred from the documentation).
//   5. 0xC0000409 is the exit code issue #69 recorded on CI, twice, a week apart.
//
// So this is not only a flaky test. Any host holding two `FileWorktreeObjectStore` instances over
// one root -- which this store's own comments call a real, reproduced configuration -- could have
// `put_blob()` kill the process instead of returning an error.
//
//   C1 (positive control) -- the throwing overload really does throw under this race, HERE, on the
//         machine running the test. Without this the fix's own test proves nothing: C2 would pass
//         identically on a platform where the window never opens. Reported as a skip, never a
//         failure, when the window does not open -- the point is to say honestly which of the two
//         happened rather than to fail a Linux leg for a Windows-shaped race.
//   C2 (the fix) -- the `error_code` overload reports that same denial through `ec` instead of
//         throwing, so the caller can decide. This is what `put_blob()` now uses.
//   C3 -- `put_blob()` under the barrier-synchronized shape `test_content_durability_concurrency`
//         uses: sixteen threads, one digest, separate store instances so nothing serializes them.
//         Every call is accounted for and none throws. Stated plainly because it matters: C3 does
//         NOT reproduce the crash -- measured against the unfixed code it saw zero escapes, because
//         every thread there queries before any rename is in flight. A check that passes equally
//         with and without the fix is not a regression guard and does not get to stand in for one.
//   C4 -- the bytes are right afterwards. A fix that quieted the race by dropping writes would
//         still pass C3.
//   C5 (the actual regression guard) -- `put_blob()` while a RIVAL writer renames the same digest
//         onto the same path, which is the configuration this store's own comments call real and
//         reproduced. Measured against the unfixed code: 80 exceptions escaped `put_blob()` in 3200
//         calls. In the durability test those escape a `std::thread` body, which is the process
//         kill. With the fix: zero.
//
// Bounded to a few seconds. Needs no daemon, no network and no privileges.

#include "agentengine/core/file_worktree_object_store.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_checks  = 0;
int g_failed  = 0;
int g_skipped = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

void skip(std::string const& what, std::string const& why) {
    ++g_skipped;
    std::printf("[skip] %s -- %s\n", what.c_str(), why.c_str());
}

// Runs the documented race -- N writers renaming small files onto one shared name -- while readers
// query that name, and reports how often each overload of the query failed. Small content on
// purpose: the window is a function of how often a rename is in flight, not of how many bytes it
// moves, so 4 KiB opens it far faster than the 3 MiB the durability test uses.
struct RaceCounts {
    int threw   = 0;   // the throwing overload raised filesystem_error
    int ec_set  = 0;   // the error_code overload reported that same DENIAL through `ec`
    long long queries = 0;
    std::string first_message;
};

[[nodiscard]] RaceCounts run_status_query_race(fs::path const& root, int iterations) {
    RaceCounts counts;
    fs::path const target = root / "blob";
    std::string const content(4 * 1024, 'x');

    constexpr int kWriters = 8;
    constexpr int kReaders = 4;

    std::atomic<int>  threw{0};
    std::atomic<int>  ec_set{0};
    std::atomic<long long> queries{0};
    std::atomic<bool> stop{false};
    std::string       first_message;
    std::atomic_flag  taken = ATOMIC_FLAG_INIT;

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                ++queries;
                // C2's subject: the overload put_blob() now uses. It must report, never raise.
                std::error_code ec;
                (void)fs::is_regular_file(target, ec);
                // Only the DENIAL, deliberately. The overwhelming majority of `ec` values here are
                // an ordinary "no such file" from the instants when no rename has landed yet, and
                // counting those would make C2 a check that cannot fail.
                if (ec == std::errc::permission_denied) ++ec_set;
                // C1's subject: the overload put_blob() used to use.
                try {
                    (void)fs::is_regular_file(target);
                } catch (fs::filesystem_error const& e) {
                    ++threw;
                    if (!taken.test_and_set()) first_message = e.what();
                }
            }
        });
    }

    for (int iter = 0; iter < iterations; ++iter) {
        std::atomic<bool>        go{false};
        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&, w] {
                fs::path const temp =
                        root / ("blob." + std::to_string(iter) + "." + std::to_string(w) + ".tmp");
                {
                    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                    out << content;
                }
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                std::error_code rename_ec;
                fs::rename(temp, target, rename_ec);
                std::error_code cleanup_ec;
                fs::remove(temp, cleanup_ec);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : writers) th.join();
        std::error_code rm_ec;
        fs::remove(target, rm_ec);
    }

    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    counts.threw         = threw.load();
    counts.ec_set        = ec_set.load();
    counts.queries       = queries.load();
    counts.first_message = first_message;
    return counts;
}

}  // namespace

int main() {
    using agentengine::FileWorktreeObjectStore;

    fs::path const root = fs::temp_directory_path() / "ae_test_object_store_race";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // ---- C1/C2: does the window open here, and does the error_code overload report it?
    {
        fs::path const race_root = root / "status_race";
        fs::create_directories(race_root);
        RaceCounts const counts = run_status_query_race(race_root, 400);
        std::printf("       status-query race: %lld queries, throwing overload raised %d, "
                    "error_code overload reported %d denial(s)\n",
                    counts.queries, counts.threw, counts.ec_set);
        if (counts.threw == 0) {
            skip("C1/C2",
                 "the concurrent-rename status-query window did not open on this platform, so there "
                 "is nothing here for the error_code overload to report either");
        } else {
            check(counts.threw > 0,
                  "C1 (positive control): the THROWING is_regular_file() overload really does raise "
                  "filesystem_error under concurrent rename onto the same path");
            check(!counts.first_message.empty() &&
                          counts.first_message.find("denied") != std::string::npos,
                  "C1 (positive control): ... and it is the access denial the fix's comment names, "
                  "not some unrelated error");
            check(counts.ec_set > 0,
                  "C2 (the fix): the error_code overload reports that same denial through ec instead "
                  "of throwing, so put_blob() can decide what it means");
        }
        fs::remove_all(race_root, ec);
    }

    // ---- C3/C4: put_blob() itself, under the shape that killed the process.
    {
        fs::path const objects_dir = root / "put_blob_race";
        std::string const content(64 * 1024, 'q');
        std::vector<std::byte> bytes(content.size());
        for (std::size_t i = 0; i < content.size(); ++i) {
            bytes[i] = static_cast<std::byte>(content[i]);
        }

        constexpr int kThreads    = 16;
        constexpr int kIterations = 60;

        std::atomic<int> escaped{0};   // an exception left put_blob() -- before the fix, a process kill
        std::atomic<int> returned{0};
        std::atomic<int> failed{0};
        std::atomic<int> unverified{0};  // issue #70: the bounded retry window expired, nothing concluded

        for (int iter = 0; iter < kIterations; ++iter) {
            fs::remove_all(objects_dir, ec);
            fs::create_directories(objects_dir);

            std::atomic<int>  ready{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    // Each thread gets its OWN store over the SAME root, so put_blob()'s per-instance
                    // mutex serializes nothing across them -- the configuration this store's own
                    // comments call real and reproduced.
                    FileWorktreeObjectStore store(objects_dir);
                    ++ready;
                    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                    // The catch-all is the whole point. Before the fix this did not record a
                    // failure: the exception escaped the thread and took the process with it, so
                    // there was no "failed" branch to reach.
                    try {
                        auto put_r = store.put_blob(bytes);
                        if (put_r.has_value()) {
                            ++returned;
                        } else {
                            ++failed;
                            // The two failure codes mean different things and a future regression
                            // should not have to guess which one fired. `blob_write_failed` says
                            // the destination was read back and was wrong or absent;
                            // `blob_commit_unverified` (issue #70) says it stayed unreadable for
                            // the whole bounded retry window and nothing was concluded.
                            if (put_r.error().code == "worktree.blob_commit_unverified") {
                                ++unverified;
                            }
                        }
                    } catch (...) {
                        ++escaped;
                    }
                });
            }
            while (ready.load() < kThreads) std::this_thread::yield();
            go.store(true, std::memory_order_release);
            for (auto& th : threads) th.join();
        }

        std::printf("       put_blob race: %d returned a value, %d returned an error (%d of them "
                    "unverified-window), %d let an exception escape\n",
                    returned.load(), failed.load(), unverified.load(), escaped.load());
        check(escaped.load() == 0,
              "C3: put_blob() never lets an exception escape under concurrent same-digest writes "
              "through separate store instances -- the shape that exited 0xC0000409");
        check(returned.load() + failed.load() == kThreads * kIterations,
              "C3: ... and every call is accounted for -- each one either returned a digest or "
              "reported an error through result<T>, none vanished");
        // This WAS a printed measurement rather than a check, because it was a separate pre-existing
        // defect (issue #70) that #69's fix did not get to quietly absorb: the rename loses to a
        // sibling, and the readback-and-recompute meant to rescue that case failed too, because the
        // destination was momentarily inaccessible for the same reason the rename was. Content that
        // was on disk and byte-correct was reported as a durability failure. Measured in one
        // session, same binary otherwise, fix stashed and restored:
        //
        //     unfixed   182, 179, 201 failures of 960
        //     fixed       0,   0,   0 failures of 960
        //
        // The bound below is 1%, not zero, deliberately. Zero is what three runs measured, but the
        // rescue closes the window by waiting a BOUNDED time, and a sufficiently loaded host could
        // still exhaust it. A knife-edge assertion would turn that into a flake and teach the next
        // reader to distrust the check. One percent is still two orders of magnitude below the
        // defect this guards against.
        check(failed.load() * 100 < kThreads * kIterations,
              "C3: ... and under 1% of calls report a failure for content that is in fact durable "
              "-- this measured ~19% before issue #70's fix");

        // C4: a fix that simply stopped writing would satisfy C3.
        FileWorktreeObjectStore probe(objects_dir);
        auto digest_r = agentengine::compute_digest(bytes);
        check(digest_r.has_value(), "C4 (precondition): the content has a digest to look up");
        if (digest_r.has_value()) {
            auto get_r = probe.get_blob(*digest_r);
            check(get_r.has_value(), "C4: the blob is genuinely on disk afterwards");
            check(get_r.has_value() && get_r->size() == bytes.size() && *get_r == bytes,
                  "C4: ... and byte-for-byte correct, so C3 was not bought by dropping the write");
        }
        fs::remove_all(objects_dir, ec);
    }

    // ---- C5: the case that actually opens the window inside put_blob().
    //
    // C3 above runs the barrier-synchronized shape, and it is honest to record that C3 does NOT
    // reproduce the crash: every thread there queries, then writes, then renames, so the queries all
    // happen before any rename is in flight. Measured against the unfixed code, C3 saw zero escapes.
    // A check that passes equally with and without the fix is not a regression guard, so it does not
    // get to stand in for one.
    //
    // This is the shape that does open it, and it is the configuration this store's own comments
    // already call real and reproduced: a SECOND writer committing the SAME digest through a
    // different path, so a rename onto the destination is in flight while put_blob() is querying it.
    // Modelled with a background renamer of byte-identical content, which is what a second store
    // instance's rename is.
    {
        fs::path const objects_dir = root / "put_blob_vs_renamer";
        fs::create_directories(objects_dir / "blobs");

        std::string const content(256 * 1024, 'z');
        std::vector<std::byte> bytes(content.size());
        for (std::size_t i = 0; i < content.size(); ++i) {
            bytes[i] = static_cast<std::byte>(content[i]);
        }
        auto digest_r = agentengine::compute_digest(bytes);
        check(digest_r.has_value(), "C5 (precondition): the content has a digest");
        if (digest_r.has_value()) {
            fs::path const target = objects_dir / "blobs" / *digest_r;

            std::atomic<bool> stop{false};
            std::atomic<int>  renames{0};
            std::thread renamer([&] {
                int n = 0;
                while (!stop.load(std::memory_order_acquire)) {
                    fs::path const temp =
                            objects_dir / "blobs" / ("rival." + std::to_string(n++) + ".tmp");
                    {
                        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                        out << content;
                    }
                    std::error_code rename_ec;
                    fs::rename(temp, target, rename_ec);
                    if (!rename_ec) ++renames;
                    std::error_code cleanup_ec;
                    fs::remove(temp, cleanup_ec);
                    fs::remove(target, cleanup_ec);
                }
            });

            std::atomic<int> escaped{0};
            std::atomic<int> calls{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t) {
                threads.emplace_back([&] {
                    FileWorktreeObjectStore store(objects_dir);
                    for (int i = 0; i < 400; ++i) {
                        try {
                            auto put_r = store.put_blob(bytes);
                            (void)put_r;   // success or a reported error are both fine here
                        } catch (...) {
                            ++escaped;
                        }
                        ++calls;
                    }
                });
            }
            for (auto& th : threads) th.join();
            stop.store(true, std::memory_order_release);
            renamer.join();

            std::printf("       put_blob vs. a rival renamer: %d calls against %d rival renames, "
                        "%d exception(s) escaped\n",
                        calls.load(), renames.load(), escaped.load());
            check(escaped.load() == 0,
                  "C5 (the regression guard): put_blob() lets NO exception escape while a rival "
                  "writer renames the same digest onto the same path -- unfixed, this is the "
                  "0xC0000409 process kill issue #69 recorded");
        }
        fs::remove_all(objects_dir, ec);
    }

    fs::remove_all(root, ec);

    if (g_checks == 0) {
        ++g_failed;
        std::printf("[FAIL] no check ran at all\n");
    }
    std::printf("\n%d checks, %d failed, %d skipped\n", g_checks, g_failed, g_skipped);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
