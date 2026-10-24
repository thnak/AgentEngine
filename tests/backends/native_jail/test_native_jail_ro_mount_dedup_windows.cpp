// Proves decisions/ADR-244-required-choices-step-1b.md §2.4: a read-only `SandboxSpec.mounts` entry on
// `NativeJailBackend::create()` is granted to the shared AppContainer profile once, however many sandboxes
// mount it. This replaced `grant_ro_path_once()`, which granted outside any capability check; the mount now has
// to be covered by a `cap::SandboxMount` grant like any other.
//
// The DACL entry count alone cannot show this: Windows merges an identical ACE, so even an undeduplicated grant
// leaves the count flat (measured, ADR-244 §6). The proof counts DACL *writes* instead
// (`native_jail::acl_write_count`), which an unconditional grant would raise on every create.
//   1. The first create writes the grant (positive control: the counter moves) and the DACL gains an entry.
//   2. Ten more creates mounting the same path write nothing.
//   3. A create whose grants do not cover the mount is refused (`sandbox.mount_not_authorized`).
#include <windows.h>

#include <aclapi.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "agentengine/rt/block_on.hpp"
#include "backends/native_jail/app_container_profile.hpp"
#include "backends/native_jail/native_jail_backend.hpp"
#include "../../support/error_detail.hpp"

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
// changes across repeated creates, not which SID owns which entry.
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

namespace {

[[nodiscard]] SandboxSpec ro_spec(std::filesystem::path const& dir, bool granted) {
    SandboxSpec spec;
    spec.mounts.push_back(MountSpec{.source = dir.string(), .guest_path = "/ro", .read_write = false, .quota_bytes = cap::Limit::unbounded()});
    if (granted) {
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxMount{dir.string(), "/ro", false}}});
    }
    return spec;
}

[[nodiscard]] bool create_and_destroy(NativeJailBackend& backend, SandboxSpec const& spec) {
    EffectContext ctx{.deadline = agentengine::Deadline::none()};
    auto h = rt::block_on(backend.create(spec, ctx));
    if (!h.has_value()) {
        std::cerr << "create() error: " << agentengine::test_support::describe(h.error()) << "\n";
        return false;
    }
    rt::block_on(backend.destroy(*h));
    return true;
}

}  // namespace

int main() {
    std::filesystem::path const scratch = std::filesystem::temp_directory_path() / "ae_ro_mount_dedup_test";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    AE_CHECK(!ec, "setup: scratch directory exists");

    NativeJailBackend backend;
    std::wstring const scratch_w = scratch.wstring();

    DWORD const before = dacl_entry_count(scratch_w);
    std::uint64_t const writes_before = native_jail::acl_write_count();
    AE_CHECK(create_and_destroy(backend, ro_spec(scratch, true)), "a create with a granted read-only mount succeeds");
    std::uint64_t const writes_after_first = native_jail::acl_write_count();
    DWORD const after_first = dacl_entry_count(scratch_w);
    AE_CHECK(writes_after_first > writes_before && after_first > before,
             "the first create writes the grant (positive control) -- the DACL gains an entry");

    for (int i = 0; i < 10; ++i) {
        AE_CHECK(create_and_destroy(backend, ro_spec(scratch, true)), "a repeat create with the same mount succeeds");
    }
    AE_CHECK(native_jail::acl_write_count() == writes_after_first,
             "10 further creates mounting the same read-only path write nothing to the DACL");
    AE_CHECK(dacl_entry_count(scratch_w) == after_first, "and the DACL entry count is unchanged");

    {
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        auto refused = rt::block_on(backend.create(ro_spec(scratch, false), ctx));
        AE_CHECK(!refused.has_value() && refused.error().code == "sandbox.mount_not_authorized",
                 "a read-only mount no grant covers is refused (sandbox.mount_not_authorized)");
        if (refused.has_value()) rt::block_on(backend.destroy(*refused));
    }

    std::filesystem::remove_all(scratch, ec);

    if (g_failures == 0) {
        std::cout << "test_native_jail_ro_mount_dedup_windows: all checks passed\n";
        return 0;
    }
    std::cerr << "test_native_jail_ro_mount_dedup_windows: " << g_failures << " check(s) failed\n";
    return 1;
}
