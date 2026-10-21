#pragma once
// Implements decisions/archive/ADR-004-appcontainer-native-jail-windows-backend.md §3 step 2 (design) as
// real code -- Milestone 2 Phase C task C2. One AppContainer profile per deployment, not per
// session ("One profile, reused across sessions -- the SID is stable identity, not per-session
// state," ADR-004 §3): `ensure()` is idempotent (ERROR_ALREADY_EXISTS from
// CreateAppContainerProfile is treated as success, then the SID is derived from the name either
// way), so calling it from every `NativeJailBackend::create()` is correct, not merely tolerated.
//
// Grants (RX)/(M) ACEs on host paths per 008 §2's mount contract -- this is 008 §4's "bind/junction
// mounts + path canonicalization + FS restrictions" row for `native-jail`'s ACL layer specifically.
// It is explicitly NOT the primary filesystem boundary (decisions/archive/ADR-004-appcontainer-native-jail-
// windows-backend.md §6 finding 1 / §8.1: a curated set of OS files carry `ALL (RESTRICTED)
// APPLICATION PACKAGES` read ACEs by Windows' own default, independent of what this profile grants
// or withholds) -- it is the §1b layer-3 backstop, real and load-bearing for paths AgentEngine
// controls (confirmed: ADR-004 §5.3's `C:\Users` / interpreter-tree denials), but interpreter-level
// `open()` mediation (010, not yet built -- M3) is what 008 §1b names as primary for reads. Callers
// of this class must not treat a `grant_path` as the whole filesystem boundary.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "agentengine/core/error.hpp"

namespace agentengine::native_jail {

class AppContainerProfile {
public:
    // `name` is the AppContainer profile's stable identity (package family name-like; alphanumeric
    // plus `.`/`-`/`_`, no spaces -- Win32's own CreateAppContainerProfile restriction).
    [[nodiscard]] static result<AppContainerProfile> ensure(std::wstring const& name,
                                                              std::wstring const& display_name,
                                                              std::wstring const& description);

    ~AppContainerProfile();
    AppContainerProfile(AppContainerProfile const&) = delete;
    AppContainerProfile& operator=(AppContainerProfile const&) = delete;
    AppContainerProfile(AppContainerProfile&& other) noexcept;
    AppContainerProfile& operator=(AppContainerProfile&& other) noexcept;

    [[nodiscard]] PSID sid() const noexcept { return sid_; }

    // Grants this profile's SID (OI)(CI)(RX) -- or, when `read_write` is true, (OI)(CI)(M) -- on `path`, which
    // must already exist. Idempotent: an explicit ACE already carrying the mask is left as it is and nothing is
    // written (Windows also merges identical ACEs; measured 2026-10-21, ADR-244 §2.4). The backend no longer makes
    // read-write grants on this SID: they go to a per-sandbox capability SID (`grant_sid_path`), because an ACE
    // on the shared SID outlives every process and reaches every later sandbox.
    [[nodiscard]] result<void> grant_path(std::wstring const& path, bool read_write) const;

    // Strips this profile's SID's own explicit ACE from `path`, if one exists -- the sibling
    // `grant_path` never got, because nothing needed it as the PRIMARY retirement mechanism (deleting
    // a granted path already destroys its ACE as a structural NTFS side effect -- see
    // docs/planning/office-document-extraction-design-draft.md, "Fix for finding 14"). This exists
    // ONLY as a failure-path safety net for the one case deletion cannot reach: a granted path that
    // could not be deleted (finding 31's cleanup-retry-exhaustion path). Same three-Win32-call shape
    // as `grant_path`, `SetEntriesInAclW`'s `ACCESS_MODE` flipped to `REVOKE_ACCESS`. `path` not
    // existing is success, not failure (nothing to strip an ACE from) -- callers rely on this to make
    // a fixed enumeration of possibly-uncreated children (finding 33's fix) safe to call unconditionally.
    [[nodiscard]] result<void> revoke_path(std::wstring const& path) const;

private:
    explicit AppContainerProfile(PSID sid) : sid_(sid) {}
    void free_now();

    PSID sid_ = nullptr;  // owned via FreeSid, allocated by DeriveAppContainerSidFromAppContainerName
};

// An owned copy of a SID. ADR-244 §2.4: each sandbox with read-write mounts gets its own capability SID, derived
// from a random name; its process token holds it and the read-write ACEs name it. An ACE left behind by a crash
// names a SID no later token holds, so it grants nothing.
class OwnedSid {
public:
    [[nodiscard]] static result<OwnedSid> derive_capability(std::wstring const& name);
    [[nodiscard]] PSID get() const noexcept {
        return bytes_.empty() ? nullptr : static_cast<PSID>(const_cast<std::byte*>(bytes_.data()));
    }

private:
    std::vector<std::byte> bytes_;
};

// (OI)(CI) allow for `sid` on `path`: RX, or M when `read_write`. Idempotent as `grant_path` is.
[[nodiscard]] result<void> grant_sid_path(std::wstring const& path, PSID sid, bool read_write);
// Removes `sid`'s explicit ACEs from `path`; a missing path is success.
[[nodiscard]] result<void> revoke_sid_path(std::wstring const& path, PSID sid);
// The SID string of an allow ACE on `path` (explicit or inherited, effective or inherit-only) giving write to
// `package_sid` or to any AppContainer capability SID; nullopt if there is none.
[[nodiscard]] result<std::optional<std::wstring>> appcontainer_writer(std::wstring const& path, PSID package_sid);
// True if any allow ACE on `path` (explicit or inherited) names `sid`.
[[nodiscard]] result<bool> dacl_names(std::wstring const& path, PSID sid);
// DACL writes this process has made (test seam: proves a skipped grant wrote nothing).
[[nodiscard]] std::uint64_t acl_write_count() noexcept;

}  // namespace agentengine::native_jail
