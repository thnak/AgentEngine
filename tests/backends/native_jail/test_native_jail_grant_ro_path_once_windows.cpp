// Proves NativeJailBackend::grant_ro_path_once() (native_jail_backend.hpp) -- the new public entry
// point docs/planning/pdf-text-extraction-design-draft.md's round-5/round-6 red-team found was
// missing: `grant_ro_deduped()`/`shared_profile()` are anonymous-namespace internals of
// native_jail_backend.cpp, unreachable from any other translation unit, so a caller outside this file
// (a future first-party Tool<> granting one fixed scratch directory) had no way to reach the SAME
// dedup guarantee `create_python_worker()`'s own fixed-path grants (python_home, extra_sys_path, the
// worker binary directory) already get internally.
//
// Two real claims proven, using the SAME dacl_entry_count() technique
// test_native_jail_teardown_cycles_windows.cpp already established and validated as sensitive (its
// own positive control there shows a differing-value repeat grant genuinely accumulates ACEs, so a
// flat count here is not merely an insensitive metric):
//   1. grant_ro_path_once() actually grants real access -- the target path's DACL gains a real entry.
//   2. Calling it again with the SAME path is a real no-op, not merely "doesn't error" -- the DACL
//      entry count does not grow, proving the dedup path (AppContainerProfile::grant_path()'s own
//      documented "additive, not idempotent" behavior would otherwise accumulate a redundant ACE on
//      every call, exactly the unbounded-DACL-growth defect this design's own history already found
//      and fixed once for python_home et al.).

#include <windows.h>

#include <aclapi.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "backends/native_jail/native_jail_backend.hpp"
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

// Same technique as test_native_jail_teardown_cycles_windows.cpp's own dacl_entry_count() -- total
// DACL entry count on `path`, not filtered to a specific trustee; the point is whether the count
// changes across repeated grant_ro_path_once() calls, not which SID owns which entry.
DWORD dacl_entry_count(std::wstring const& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || dacl == nullptr) return 0;
    ACL_SIZE_INFORMATION size_info{};
    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation);
    if (sd != nullptr) LocalFree(sd);
    return size_info.AceCount;
}

}  // namespace

int main() {
    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_grant_ro_path_once_test";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    AE_CHECK(!ec, "setup: scratch directory exists");

    NativeJailBackend backend;
    std::wstring const scratch_w = scratch.wstring();

    DWORD const before = dacl_entry_count(scratch_w);

    auto first = backend.grant_ro_path_once(scratch_w);
    if (!first.has_value()) {
        std::cerr << "grant_ro_path_once() error: " << agentengine::test_support::describe(first.error())
                   << "\n";
    }
    AE_CHECK(first.has_value(), "grant_ro_path_once() succeeds on a real, existing directory");

    DWORD const after_first = dacl_entry_count(scratch_w);
    AE_CHECK(after_first > before,
             "the first call actually grants real access -- the DACL gains a real entry, not a no-op");

    auto second = backend.grant_ro_path_once(scratch_w);
    AE_CHECK(second.has_value(), "a repeat call on the SAME path still succeeds");

    DWORD const after_second = dacl_entry_count(scratch_w);
    AE_CHECK(after_second == after_first,
             "a repeat call on the SAME path does not accumulate a second ACE -- real dedup, not "
             "merely 'doesn't error' (the exact unbounded-DACL-growth defect this method exists to "
             "avoid on a caller-facing surface)");

    // A third, larger repeat count -- the same non-vacuous "would have grown if undeduped" argument
    // test_native_jail_teardown_cycles_windows.cpp's own kTeardownCycles loop makes, scaled down
    // since this test needs no real process spawn per iteration (grant_ro_path_once() never spawns
    // anything -- it is a pure ACL-grant call).
    for (int i = 0; i < 10; ++i) {
        auto repeat = backend.grant_ro_path_once(scratch_w);
        AE_CHECK(repeat.has_value(), "grant_ro_path_once() remains successful across repeat calls");
    }
    DWORD const after_many = dacl_entry_count(scratch_w);
    AE_CHECK(after_many == after_first,
             "10 further repeat calls still do not accumulate ACEs -- dedup holds under real repeated "
             "use, not just a single second call");

    std::filesystem::remove_all(scratch, ec);

    if (g_failures == 0) {
        std::cout << "test_native_jail_grant_ro_path_once_windows: all checks passed\n";
        return 0;
    }
    std::cerr << "test_native_jail_grant_ro_path_once_windows: " << g_failures << " check(s) failed\n";
    return 1;
}
