// Milestone 2 Phase C, task C4 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// 008-Sandbox-and-Isolation.md §9 G1 -- "one hostile test corpus runs against every available
// backend on every platform in the current target set and produces the same outcome
// classification for every case." This is the Windows half; tests/test_native_jail_parity_linux.cpp
// is the Linux half, both consuming the SAME table (tests/helpers/abuse_case_corpus.hpp) verbatim.
//
// Real child processes under real AppContainer + Job Object isolation -- bounded by this test's
// own generous-but-finite wall_ms (CLAUDE.md Machine Safety).

#include <windows.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/native_jail_backend.hpp"
#include "helpers/abuse_case_corpus.hpp"
#include "support/crt_fail_fast.hpp"

using namespace agentengine;
using agentengine::native_jail::NativeJailBackend;
using agentengine::native_jail::test::kAbuseCorpus;
using agentengine::native_jail::test::kAbuseCorpusSize;
using agentengine::native_jail::test::to_string;

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
        std::filesystem::temp_directory_path() / "ae_native_jail_parity_test";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");

    EffectContext ctx;

    for (std::size_t i = 0; i < kAbuseCorpusSize; ++i) {
        auto const& c = kAbuseCorpus[i];

        NativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        spec.limits.wall_ms = 5000;  // generous: classification parity, not tightest-bound timing
        spec.limits.memory_bytes = c.memory_bytes;
        spec.limits.pids = c.pids;
        spec.limits.output_bytes = c.output_bytes;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), std::string("G1 parity [") + c.name + "]: create() succeeds");
        if (!handle.has_value()) continue;

        ExecRequest req{.language = "native", .source = hostile_child_cmd(c.probe_args)};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value(), std::string("G1 parity [") + c.name + "]: exec() returns a result");
        if (outcome.has_value()) {
            std::cout << "  measured: [" << c.name << "] got " << to_string(outcome->klass)
                      << ", expected " << to_string(c.expected) << "\n";
            AE_CHECK(outcome->klass == c.expected,
                      std::string("G1 parity [") + c.name + "]: outcome classification is " +
                          to_string(c.expected));
        }
        backend.destroy(*handle);
    }

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
