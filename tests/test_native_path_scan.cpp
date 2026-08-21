// Design -> red-team -> prove -> judge for decisions/ADR-071-native-unsandboxed-process-execution-
// providers.md's native_path_scan.hpp: PATH enumeration filtered against an already-granted
// allowlist. The load-bearing claim (ADR-070 property 3 / trust/capability.hpp's own
// native_exec_pattern_covers): scanning can never surface a program the caller did not already
// name a grant for, even when that program genuinely exists on PATH -- proved here against REAL
// files in a REAL, controlled PATH directory, not a synthetic string comparison.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "agentengine/pal/env.hpp"
#include "backends/native_process/native_path_scan.hpp"

using namespace agentengine::native_process;

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

void touch(std::filesystem::path const& p) { std::ofstream(p).put('x'); }

bool contains_name(std::vector<DiscoveredExecutable> const& found, std::string const& name) {
    for (auto const& e : found) {
        if (e.short_name == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    // native_path_scan.cpp reads PATH via agentengine::pal::env_var(), which on MSVC is backed by
    // the CRT's OWN environment table (_dupenv_s) -- a table Win32's SetEnvironmentVariableA does
    // NOT keep in sync with (a real, easy-to-hit divergence: the CRT and Win32 process-environment
    // views can disagree). _putenv_s is the CRT-side write that actually reaches _dupenv_s, so it
    // is the only correct way to control what this test's own scan_path() calls observe.
    std::string const saved_path = agentengine::pal::env_var("PATH").value_or("");

    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_native_path_scan_test";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch);
    touch(scratch / "node.exe");
    touch(scratch / "python3.11.exe");
    touch(scratch / "rm.exe");            // present on PATH, but never granted below
    touch(scratch / "readme.txt");        // not a recognized executable extension at all

    _putenv_s("PATH", scratch.string().c_str());

    // ---- P1: an exact-name grant finds exactly that program, real file on a real PATH -----------
    {
        auto found = scan_path({"node"});
        AE_CHECK(contains_name(found, "node"), "P1 (positive control): a granted exact name is found");
        AE_CHECK(found.size() == 1, "P1: nothing else is reported for a single exact-name grant");
    }

    // ---- P2: a prefix grant finds the matching concrete file --------------------------------------
    {
        auto found = scan_path({"python*"});
        AE_CHECK(contains_name(found, "python3.11"),
                  "P2 (positive control): a granted prefix pattern finds the real matching file");
    }

    // ---- R-P3: a REAL executable on PATH that matches no granted pattern is NEVER reported --------
    // The load-bearing claim: "rm.exe" genuinely exists in the scanned directory (proven by P1/P2
    // finding its siblings there), yet is absent from every result below because nothing granted it.
    {
        auto found_node_only = scan_path({"node"});
        AE_CHECK(!contains_name(found_node_only, "rm"),
                  "R-P3: an ungranted-but-real executable ('rm') is never surfaced when only 'node' "
                  "is granted");
        auto found_both = scan_path({"node", "python*"});
        AE_CHECK(!contains_name(found_both, "rm"),
                  "R-P3: still absent even when scanning with multiple OTHER grants active");
        AE_CHECK(contains_name(found_both, "node") && contains_name(found_both, "python3.11"),
                  "R-P3 (positive control): the two ACTUALLY granted programs are still both found "
                  "in the same call -- the denial above is real filtering, not scan_path failing "
                  "outright");
    }

    // ---- P4: no patterns granted at all -> nothing is EVER discovered, regardless of what exists --
    {
        auto found = scan_path({});
        AE_CHECK(found.empty(),
                  "P4: an empty grant list discovers nothing, even though real executables exist on "
                  "PATH");
    }

    // ---- P5: a non-executable file (wrong extension) is never reported, even if a grant would match
    {
        auto found = scan_path({"readme*"});
        AE_CHECK(found.empty(),
                  "P5: 'readme.txt' is not a recognized executable extension, so no grant can ever "
                  "surface it");
    }

    _putenv_s("PATH", saved_path.c_str());
    std::filesystem::remove_all(scratch, ec);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All native_path_scan (ADR-071) checks passed.\n";
    return 0;
}
