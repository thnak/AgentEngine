#pragma once
// Implements decisions/ADR-004-appcontainer-native-jail-windows-backend.md §3 step 2 (design) as
// real code -- Milestone 2 Phase C task C2. One AppContainer profile per deployment, not per
// session ("One profile, reused across sessions -- the SID is stable identity, not per-session
// state," ADR-004 §3): `ensure()` is idempotent (ERROR_ALREADY_EXISTS from
// CreateAppContainerProfile is treated as success, then the SID is derived from the name either
// way), so calling it from every `NativeJailBackend::create()` is correct, not merely tolerated.
//
// Grants (RX)/(M) ACEs on host paths per 008 §2's mount contract -- this is 008 §4's "bind/junction
// mounts + path canonicalization + FS restrictions" row for `native-jail`'s ACL layer specifically.
// It is explicitly NOT the primary filesystem boundary (decisions/ADR-004-appcontainer-native-jail-
// windows-backend.md §6 finding 1 / §8.1: a curated set of OS files carry `ALL (RESTRICTED)
// APPLICATION PACKAGES` read ACEs by Windows' own default, independent of what this profile grants
// or withholds) -- it is the §1b layer-3 backstop, real and load-bearing for paths AgentEngine
// controls (confirmed: ADR-004 §5.3's `C:\Users` / interpreter-tree denials), but interpreter-level
// `open()` mediation (010, not yet built -- M3) is what 008 §1b names as primary for reads. Callers
// of this class must not treat a `grant_path` as the whole filesystem boundary.

#include <string>

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

    // Grants this profile's SID (OI)(CI)(RX) -- or, when `read_write` is true, (OI)(CI)(M) -- on
    // `path`, which must already exist. Additive: does not remove or replace any existing ACE, so
    // calling this once per mount at session-create time is the expected, safe usage; it is not
    // idempotent against repeated calls with different `read_write` values for the same path (that
    // would accumulate ACEs, not replace one) -- callers grant each mount exactly once.
    [[nodiscard]] result<void> grant_path(std::wstring const& path, bool read_write) const;

private:
    explicit AppContainerProfile(PSID sid) : sid_(sid) {}
    void free_now();

    PSID sid_ = nullptr;  // owned via FreeSid, allocated by DeriveAppContainerSidFromAppContainerName
};

}  // namespace agentengine::native_jail
