// Regression test for a real, confirmed data race: `NativeJailBackend::instances_`
// (src/backends/native_jail/native_jail_backend.{hpp,cpp}) was an unsynchronized
// `std::unordered_map`, mutated by `create()`/`exec()`/`destroy()`/`create_python_worker()` with no
// lock at all. Surfaced as a side-finding during Revision 4 of
// docs/planning/office-document-extraction-design-draft.md (round-3 finding-11 investigation): a
// SINGLE `NativeJailBackend` is genuinely shared across threads in this codebase today --
// `extract_pdf_text.hpp`'s `invoke_worker()` and `tools/cli_chat.cpp`'s `shared_python_runner()`
// both reach one process-wide `static NativeJailBackend`. Two concurrently-running sessions in one
// host process already race on this map with NO opt-in tag required; ADR-160's real (if not yet
// Judged) parallel-batch tool-call scheduler adds a second, same-session path once any tool declares
// `Parallelizable`/`ExclusivityGroup<Name>`. The fix: `instances_mutex_` now guards `emplace`/
// `find`/`erase` (via `find_instance_locked`/`insert_instance_locked`/`erase_instance_locked`),
// never the long blocked body of `exec()` itself.
//
// This is a stress/regression test, not a guaranteed race detector: this project has no
// ThreadSanitizer build configuration (TSan has no meaningful MSVC support; the one sanitizer
// config in CMakeLists.txt is `-fsanitize=address,undefined` scoped to the libFuzzer target, and
// ASan does not reliably catch a pure happens-before data race the way TSan does). Before the fix,
// running this test's thread/iteration count against the unguarded map reproducibly crashed or hung
// this binary on this machine (observed directly while developing this fix, via a temporary local
// revert); after the fix, it passes cleanly, repeatably. That empirical before/after difference is
// this test's real evidence, not a formal guarantee.

#include <windows.h>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/native_jail_backend.hpp"
#include "support/crt_fail_fast.hpp"
#include "support/error_detail.hpp"

using namespace agentengine;
using agentengine::native_jail::NativeJailBackend;

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

std::string hostile_child_cmd(std::string const& args) {
    return std::string("\"") + AE_HOSTILE_CHILD_EXE + "\" " + args;
}

}  // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_backend_concurrency_test";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch mount directory exists");

    SandboxSpec spec;
    spec.mounts.push_back(MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
    spec.limits.wall_ms = 5000;
    spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB
    spec.limits.pids = 4;
    spec.limits.output_bytes = 1024 * 1024;

    // ONE shared backend, deliberately -- reproducing the exact real-world shape (a single
    // process-wide `static NativeJailBackend`) that `extract_pdf_text.hpp`/`cli_chat.cpp` both use.
    NativeJailBackend shared_backend;

    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 40;  // 320 create/exec/destroy cycles total, real overlap
    std::atomic<int> ok_count{0};
    std::atomic<int> unexpected_error_count{0};

    // Each thread's own EffectContext -- exec() itself never mutates it in a way create()/exec()'s
    // own call sites don't already isolate per-caller; the SHARED state under test is purely
    // `shared_backend`'s own `instances_` map.
    auto worker = [&](int thread_index) {
        for (int i = 0; i < kIterationsPerThread; ++i) {
            EffectContext ctx;
            auto handle = shared_backend.create(spec, ctx);
            if (!handle.has_value()) {
                ++unexpected_error_count;
                continue;
            }
            // A short, real sleep (varying by thread so create()/exec()/destroy() calls from
            // different threads genuinely interleave rather than lock-stepping) -- long enough that
            // OTHER threads' own create()/destroy() calls land WHILE this exec() is still blocked in
            // wait_or_kill(), the exact "map mutated while a sibling call holds a live Instance&"
            // shape the fix addresses.
            int const sleep_ms = 5 + (thread_index % 4) * 3 + (i % 5);
            ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep " + std::to_string(sleep_ms))};
            auto outcome = shared_backend.exec(*handle, req, ctx);
            shared_backend.destroy(*handle);

            if (outcome.has_value() && outcome->klass == exec_outcome_class::ok) {
                ++ok_count;
            } else {
                ++unexpected_error_count;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // Reaching this line at all (rather than crashing/aborting/hanging mid-run) is itself part of
    // this test's evidence -- `fail_fast_on_windows()` above ensures any CRT heap corruption from a
    // racing map mutation dies loudly (stderr + abort) instead of hanging or silently corrupting
    // further, so ctest observing a clean process exit here is meaningful, not just the assertions
    // below.
    int const expected_total = kThreads * kIterationsPerThread;
    AE_CHECK(ok_count.load() == expected_total,
              "concurrency: every create/exec/destroy cycle across all threads completed with "
              "exec_outcome_class::ok, none lost or corrupted by a racing sibling call");
    AE_CHECK(unexpected_error_count.load() == 0,
              "concurrency: no create()/exec() call observed a spurious failure from a racing "
              "sibling call's map mutation");

    // A second pass, this time deliberately overlapping create_python_worker()'s own emplace path
    // is out of scope here (it needs a real worker exe + PythonWorkerSessionConfig, exercised by
    // test_native_jail_python_worker_slice1.cpp instead) -- this test's job is proving the shared
    // `instances_` map itself, via the create()/exec()/destroy() surface every real caller
    // (extract_pdf_text.hpp) actually uses concurrently today.

    if (g_failures == 0) {
        std::cout << "test_native_jail_backend_concurrency_windows: ALL PASS ("
                  << ok_count.load() << "/" << expected_total << " cycles ok)\n";
        return 0;
    }
    std::cerr << "test_native_jail_backend_concurrency_windows: " << g_failures << " FAILURE(S)\n";
    return 1;
}
