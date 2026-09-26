// Milestone 2 Phase C, task C4 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// 008-Sandbox-and-Isolation.md §9 G1 -- "one hostile test corpus runs against every available
// backend on every platform in the current target set and produces the same outcome
// classification for every case." This is the Linux half; tests/test_native_jail_parity_windows.cpp
// is the Windows half, both consuming the SAME table (tests/helpers/abuse_case_corpus.hpp) verbatim.
//
// Requires a delegated cgroup v2 root and CAP_SYS_ADMIN, same as C2/C3's Linux tests -- run via
// tests/helpers/cgroup_v2_test_setup.sh.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "backends/native_jail/linux_native_jail_backend.hpp"
#include "helpers/abuse_case_corpus.hpp"
#include "helpers/native_jail_linux_toolchain_mounts.hpp"

using namespace agentengine;
using agentengine::native_jail::LinuxNativeJailBackend;
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
    return std::string("\"") + AE_HOSTILE_CHILD_POSIX_EXE + "\" " + args;
}

// C2-Linux's own measured finding (tests/test_native_jail_backend_linux.cpp,
// docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md's C2 writeup): cgroups v2's
// reclaim-before-OOM-kill dance took ~2-7.9s wall-clock for this exact 512 MB-vs-32 MB shape in
// this harness's environment -- a real, documented, platform-specific latency this corpus's shared
// table deliberately does not encode (see that header's own comment). Every other case's default
// wall_ms is generous enough as-is.
std::uint64_t wall_ms_for(std::string_view case_name) {
    if (case_name == "oom") return 25000;
    return 5000;
}

}  // namespace

int main() {
    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_parity_test_linux";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");

    EffectContext ctx;

    for (std::size_t i = 0; i < kAbuseCorpusSize; ++i) {
        auto const& c = kAbuseCorpus[i];

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = wall_ms_for(c.name);
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
