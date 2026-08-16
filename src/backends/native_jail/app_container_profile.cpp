// Implements app_container_profile.hpp. See that header for the spec/ADR citations this satisfies.

#include "backends/native_jail/app_container_profile.hpp"

#include <userenv.h>
#include <aclapi.h>

namespace agentengine::native_jail {

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
    PSID created_sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(name.c_str(), display_name.c_str(), description.c_str(),
                                            nullptr, 0, &created_sid);
    // ERROR_ALREADY_EXISTS is success (ADR-004 §3: one profile, reused across sessions) -- fall
    // through to DeriveAppContainerSidFromAppContainerName either way rather than trusting
    // `created_sid`, which CreateAppContainerProfile leaves unset on the already-exists path.
    if (FAILED(hr) && HRESULT_CODE(hr) != ERROR_ALREADY_EXISTS) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "CreateAppContainerProfile failed: HRESULT 0x" +
                std::to_string(static_cast<unsigned long>(hr)),
            "app_container.create_profile_failed",
        });
    }
    if (created_sid != nullptr) FreeSid(created_sid);

    PSID derived_sid = nullptr;
    hr = DeriveAppContainerSidFromAppContainerName(name.c_str(), &derived_sid);
    if (FAILED(hr)) {
        return std::unexpected(ae::error{
            failure_class::fatal,
            "DeriveAppContainerSidFromAppContainerName failed: HRESULT 0x" +
                std::to_string(static_cast<unsigned long>(hr)),
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
