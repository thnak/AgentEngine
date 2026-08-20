// Implements app_container_profile.hpp. See that header for the spec/ADR citations this satisfies.

#include "backends/native_jail/app_container_profile.hpp"

#include <userenv.h>
#include <aclapi.h>

#include <cstdio>
#include <string>

namespace agentengine::native_jail {

namespace {

// std::to_string on an HRESULT renders DECIMAL. Both call sites below prefix their value with "0x",
// so an 0x80070005 came out as "HRESULT 0x2147942405" -- a string that looks like a hex code, is not
// one, and cannot be looked up. These messages exist to be read off a CI log for a failure nobody
// can attach a debugger to, so the formatting is load-bearing, not cosmetic.
std::string hex32(unsigned long v) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "0x%08lX", v);
    return buf;
}

// Serializes CreateAppContainerProfile across PROCESSES, which is the scope the race actually has.
//
// An AppContainer profile is a machine-global, per-user resource: a directory under
// %LOCALAPPDATA%\Packages\<name> plus registry state. CreateAppContainerProfile is NOT safe to call
// concurrently on the same name from several processes -- it fails transiently while another
// process is mid-creation, and the failures are not ERROR_ALREADY_EXISTS, so they do not hit the
// idempotency path this class was built around.
//
// Measured, not argued -- tests/experiments/appcontainer_profile_race.cpp:
//
//   1 process  x  50 iterations : 0 unexpected failures
//   8 processes x 300 iterations: ~150 of 300 per process fail (~50%), worst 0x80070005
//                                 (ERROR_ACCESS_DENIED); CI has also produced 0x8007000A
//                                 (ERROR_BAD_ENVIRONMENT)
//
// That is exactly the shape of the intermittent native-jail failures: `ctest -j 4` runs six Windows
// jail test binaries concurrently, every one of them a separate process calling this with the same
// L"AgentEngine.NativeJail". DeriveAppContainerSidFromAppContainerName, by contrast, measured 0
// failures under the same contention and needs no protection.
//
// A plain (session-namespace) mutex name suffices: the contention is between sibling processes in
// one session, and `Global\` would add a privilege requirement for no benefit. Profile names are
// restricted to alphanumerics plus `.`/`-`/`_`, so embedding one in an object name is safe.
//
// Deliberately advisory: if the mutex cannot be created or waited on, creation proceeds unserialized
// rather than failing. The lock is an optimization against a known race, not a correctness
// precondition -- CreateAppContainerProfile's own result is still checked, and a caller that loses
// the race now retries on its next create() instead of latching (see native_jail_backend.cpp's
// shared_profile()).
class CrossProcessLock {
public:
    explicit CrossProcessLock(std::wstring const& name) {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_ == nullptr) return;
        // Profile creation is a sub-second operation; 30s is a generous bound that still cannot
        // wedge a test run. WAIT_ABANDONED means a holder died mid-creation -- we own it now, and
        // the state it left behind is exactly what ERROR_ALREADY_EXISTS handling is for.
        DWORD rc = WaitForSingleObject(handle_, 30'000);
        held_ = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
    }

    ~CrossProcessLock() {
        if (handle_ == nullptr) return;
        if (held_) ReleaseMutex(handle_);
        CloseHandle(handle_);
    }

    CrossProcessLock(CrossProcessLock const&) = delete;
    CrossProcessLock& operator=(CrossProcessLock const&) = delete;
    CrossProcessLock(CrossProcessLock&&) = delete;
    CrossProcessLock& operator=(CrossProcessLock&&) = delete;

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

}  // namespace

// A `win32_error()` helper reading GetLastError() used to sit here, uncalled (clang:
// "unused function 'win32_error'"). Deleted rather than annotated: it had no correct call site to be
// restored to. Every Win32 surface this file touches reports failure through its own return value --
// CreateAppContainerProfile/DeriveAppContainerSidFromAppContainerName return an HRESULT, and the
// Get/Set*SecurityInfoW/SetEntriesInAclW family returns a DWORD error code directly -- so none of
// them sets thread-last-error for a caller to read, and every error path below already carries the
// real code it was given.

result<AppContainerProfile> AppContainerProfile::ensure(std::wstring const& name,
                                                          std::wstring const& display_name,
                                                          std::wstring const& description) {
    // Held only across the create call -- see CrossProcessLock's comment for the measurement that
    // makes this necessary. Derive, below, is contention-safe and stays outside the lock.
    PSID created_sid = nullptr;
    HRESULT hr = S_OK;
    {
        CrossProcessLock creation_lock(L"AgentEngine.AppContainerProfileCreate." + name);
        hr = CreateAppContainerProfile(name.c_str(), display_name.c_str(), description.c_str(),
                                        nullptr, 0, &created_sid);
    }
    // ERROR_ALREADY_EXISTS is success (ADR-004 §3: one profile, reused across sessions) -- fall
    // through to DeriveAppContainerSidFromAppContainerName either way rather than trusting
    // `created_sid`, which CreateAppContainerProfile leaves unset on the already-exists path.
    if (FAILED(hr) && HRESULT_CODE(hr) != ERROR_ALREADY_EXISTS) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "CreateAppContainerProfile failed: HRESULT " +
                hex32(static_cast<unsigned long>(hr)),
            "app_container.create_profile_failed",
        });
    }
    if (created_sid != nullptr) FreeSid(created_sid);

    PSID derived_sid = nullptr;
    hr = DeriveAppContainerSidFromAppContainerName(name.c_str(), &derived_sid);
    if (FAILED(hr)) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "DeriveAppContainerSidFromAppContainerName failed: HRESULT " +
                hex32(static_cast<unsigned long>(hr)),
            "app_container.derive_sid_failed",
        });
    }
    return AppContainerProfile(derived_sid);
}

AppContainerProfile::~AppContainerProfile() { free_now(); }

void AppContainerProfile::free_now() {
    if (sid_ != nullptr) {
        FreeSid(sid_);
        sid_ = nullptr;
    }
}

AppContainerProfile::AppContainerProfile(AppContainerProfile&& other) noexcept : sid_(other.sid_) {
    other.sid_ = nullptr;
}

AppContainerProfile& AppContainerProfile::operator=(AppContainerProfile&& other) noexcept {
    if (this != &other) {
        free_now();
        sid_ = other.sid_;
        other.sid_ = nullptr;
    }
    return *this;
}

result<void> AppContainerProfile::grant_path(std::wstring const& path, bool read_write) const {
    if (sid_ == nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "grant_path called on a moved-from AppContainerProfile",
                                          "app_container.no_sid"});
    }

    DWORD mask = read_write ? (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE)
                             : (GENERIC_READ | GENERIC_EXECUTE);

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = mask;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;  // (OI)(CI) -- applies under the tree
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(sid_);

    PACL existing_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &existing_dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "GetNamedSecurityInfoW failed: Win32 error " + std::to_string(rc),
            "app_container.get_security_info_failed",
        });
    }

    PACL new_dacl = nullptr;
    rc = SetEntriesInAclW(1, &ea, existing_dacl, &new_dacl);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "SetEntriesInAclW failed: Win32 error " + std::to_string(rc),
            "app_container.set_entries_failed",
        });
    }

    rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
    LocalFree(new_dacl);
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "SetNamedSecurityInfoW failed: Win32 error " + std::to_string(rc),
            "app_container.set_security_info_failed",
        });
    }
    return {};
}

}  // namespace agentengine::native_jail
