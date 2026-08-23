// decisions/ADR-004-appcontainer-native-jail-windows-backend.md SS11 item 4 -- "Decide LPAC vs.
// regular AppContainer by running SS5's tier-3 corpus under both, not by reasoning from static ACL
// inspection alone." SS6 finding 4 (LPAC not tested) and SS3's own rejection of LPAC ("expected to
// not close the headline finding... stated as an expectation from ACL inspection, not as tested
// behavior") are exactly what this spike replaces with real, executed evidence.
//
// Deliberately a standalone spike, not a change to NativeJailBackend's own production
// CreateProcessW call sites (native_jail_backend.cpp) -- this is a one-time DECISION experiment
// (does LPAC change the win.ini read-leak outcome?), matching ADR-004's own original
// ac_setup.cpp/ac_run.cpp spike methodology (never committed, per that ADR's text) rather than a
// permanent regression test. If the decision this produces is "adopt LPAC," a follow-up would wire
// PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY into native_jail_backend.cpp for real --
// not done here.
//
// Reuses AppContainerProfile (the real, shipped class) for profile creation/SID derivation/ACL
// grants, and tests/helpers/hostile_child.cpp's `escape <path>` mode (the exact probe
// test_native_jail_abuse_corpus_windows.cpp's own Case 4 uses) as the launched child -- so this
// spike's evidence is comparable against that file's own already-measured non-LPAC result (ALLOWED
// for win.ini) rather than a differently-shaped probe.
//
// The ONE thing this spike changes relative to native_jail_backend.cpp's exec() (mirrored
// byte-for-byte otherwise): a 3rd attribute, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY
// = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT -- the literal definition of an LPAC launch
// (opts the process's token OUT of the ALL_APPLICATION_PACKAGES SID; only
// ALL_RESTRICTED_APPLICATION_PACKAGES plus explicitly granted capability SIDs remain).

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "backends/native_jail/app_container_profile.hpp"

using agentengine::native_jail::AppContainerProfile;

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

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { close_now(); }
    void close_now() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    }
};

// Launches `cmdline` inside `profile`'s AppContainer, `lpac` toggling the ONE extra attribute
// described above. Mirrors native_jail_backend.cpp's exec() (CREATE_SUSPENDED ->
// PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED ->
// ResumeThread) except: no Job Object (irrelevant to this decision), stdout captured via one pipe
// rather than split stdout/stderr (irrelevant to this decision), and the LPAC attribute itself.
std::string run_in_appcontainer(AppContainerProfile const& profile, std::wstring cmdline, bool lpac) {
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    SECURITY_CAPABILITIES sec_cap{};
    sec_cap.AppContainerSid = profile.sid();
    sec_cap.Capabilities = nullptr;
    sec_cap.CapabilityCount = 0;

    SECURITY_ATTRIBUTES pipe_sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleGuard out_read, out_write;
    if (!CreatePipe(&out_read.h, &out_write.h, &pipe_sa, 64 * 1024)) {
        return "SPIKE_ERROR CreatePipe";
    }
    SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0);
    // Same hardening this spike exists to compare against, not a lesser standard for test code:
    // native_jail_backend.cpp's exec() had NO handle list and inherited the host's real stdin before
    // ADR-004 SS12's fix -- this spike must not reproduce, in test code, the exact BLOCKING pattern
    // that fix closed (an independent code-review pass over that fix flagged this file's first draft
    // for exactly that, before this spike was ever committed).
    HandleGuard nul_input;
    nul_input.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &pipe_sa, OPEN_EXISTING, 0, nullptr);
    if (nul_input.h == INVALID_HANDLE_VALUE) {
        return "SPIKE_ERROR CreateFileW(NUL)";
    }

    DWORD const attr_count = lpac ? 4 : 3;
    SIZE_T attr_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_list_size);
    std::vector<std::byte> attr_buf(attr_list_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, attr_count, 0, &attr_list_size)) {
        return "SPIKE_ERROR InitializeProcThreadAttributeList";
    }
    struct AttrListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrListGuard() { DeleteProcThreadAttributeList(list); }
    } attr_guard{attr_list};

    DWORD child_process_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    DWORD all_app_packages_policy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;  // the LPAC bit
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST rejects a duplicate handle VALUE in the array (confirmed
    // directly: listing out_write.h twice for its dual stdout/stderr role failed CreateProcessW with
    // ERROR_INVALID_PARAMETER) -- list each unique handle once; the same value still gets assigned
    // to both hStdOutput and hStdError below.
    HANDLE inherit_list[2] = {out_write.h, nul_input.h};

    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sec_cap,
                                    sizeof(sec_cap), nullptr, nullptr)) {
        return "SPIKE_ERROR UpdateProcThreadAttribute(SECURITY_CAPABILITIES)";
    }
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                    &child_process_policy, sizeof(child_process_policy), nullptr,
                                    nullptr)) {
        return "SPIKE_ERROR UpdateProcThreadAttribute(CHILD_PROCESS_POLICY)";
    }
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list,
                                    sizeof(inherit_list), nullptr, nullptr)) {
        return "SPIKE_ERROR UpdateProcThreadAttribute(HANDLE_LIST)";
    }
    if (lpac && !UpdateProcThreadAttribute(attr_list, 0,
                                            PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                                            &all_app_packages_policy,
                                            sizeof(all_app_packages_policy), nullptr, nullptr)) {
        return "SPIKE_ERROR UpdateProcThreadAttribute(ALL_APPLICATION_PACKAGES_POLICY)";
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = out_write.h;
    si.StartupInfo.hStdError = out_write.h;
    si.StartupInfo.hStdInput = nul_input.h;
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi{};
    // Mirrors native_jail_backend.cpp's build_minimal_environment_block() exactly -- discovered by
    // this spike's own first run: an env block with ONLY PATH set (this file's original attempt)
    // made EVERY CreateProcessW call into the AppContainer fail with gle=203
    // (ERROR_ENVVAR_NOT_FOUND), regular-AppContainer arm included -- an AppContainer launch
    // apparently requires SystemRoot (and, empirically, LOCALAPPDATA/USERPROFILE too) to be present
    // in its own environment block, not merely PATH. A real, previously-undocumented-in-this-file
    // constraint on any AppContainer launch, not specific to the LPAC question this spike exists to
    // answer -- recorded here since production's own env block already (accidentally or not)
    // satisfies it and this spike's first draft did not.
    wchar_t system_root_buf[MAX_PATH]{};
    DWORD sr_len = GetEnvironmentVariableW(L"SystemRoot", system_root_buf, MAX_PATH);
    std::wstring root = (sr_len > 0 && sr_len < MAX_PATH) ? std::wstring(system_root_buf, sr_len)
                                                            : std::wstring(L"C:\\Windows");
    wchar_t lad_buf[MAX_PATH]{};
    DWORD lad_len = GetEnvironmentVariableW(L"LOCALAPPDATA", lad_buf, MAX_PATH);
    wchar_t up_buf[MAX_PATH]{};
    DWORD up_len = GetEnvironmentVariableW(L"USERPROFILE", up_buf, MAX_PATH);

    std::wstring block;
    block += L"SystemRoot=" + root;
    block.push_back(L'\0');
    block += L"Path=" + root + L"\\System32;" + root;
    block.push_back(L'\0');
    if (lad_len > 0 && lad_len < MAX_PATH) {
        block += L"LOCALAPPDATA=" + std::wstring(lad_buf, lad_len);
        block.push_back(L'\0');
    }
    if (up_len > 0 && up_len < MAX_PATH) {
        block += L"USERPROFILE=" + std::wstring(up_buf, up_len);
        block.push_back(L'\0');
    }
    std::vector<wchar_t> env_block(block.begin(), block.end());
    env_block.push_back(L'\0');  // double-null terminator required by CREATE_UNICODE_ENVIRONMENT
    BOOL created = CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                       CREATE_UNICODE_ENVIRONMENT,
                                   env_block.data(), nullptr,
                                   reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
    out_write.close_now();
    nul_input.close_now();
    if (!created) {
        DWORD err = GetLastError();
        return "SPIKE_ERROR CreateProcessW gle=" + std::to_string(err);
    }
    HandleGuard process_guard{pi.hProcess};
    HandleGuard thread_guard{pi.hThread};
    ResumeThread(pi.hThread);

    WaitForSingleObject(pi.hProcess, 5000);
    TerminateProcess(pi.hProcess, 0);  // no-op if already exited; bounds this spike (Machine Safety)

    std::string out;
    char buf[4096];
    DWORD avail = 0;
    while (PeekNamedPipe(out_read.h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD n = 0;
        if (!ReadFile(out_read.h, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        out.append(buf, n);
        if (!PeekNamedPipe(out_read.h, nullptr, 0, nullptr, &avail, nullptr)) break;
    }
    return out;
}

}  // namespace

int main() {
    char const* windir = std::getenv("WINDIR");
    if (windir == nullptr) windir = "C:\\Windows";
    std::string win_ini = std::string(windir) + "\\win.ini";
    std::string hosts = std::string(windir) + "\\System32\\drivers\\etc\\hosts";
    std::string secret_path =
        (std::filesystem::temp_directory_path() / "ae_adr004_lpac_spike_secret.txt").string();
    {
        std::ofstream f(secret_path);
        f << "not_curated_by_windows";
    }

    std::string cmd = std::string("\"") + AE_HOSTILE_CHILD_EXE + "\" escape ";

    // ---- Regular AppContainer (ADR-004's already-measured baseline; re-measured here for a true
    // apples-to-apples comparison against the LPAC arm below, same profile-creation code path). ----
    auto profile_regular = AppContainerProfile::ensure(L"AgentEngine.Spike.LPACDecision.Regular",
                                                          L"AE LPAC decision spike (regular)",
                                                          L"ADR-004 SS11 item 4");
    AE_CHECK(profile_regular.has_value(), "setup: regular AppContainer profile created");
    if (profile_regular.has_value()) {
        std::string out_win_ini =
            run_in_appcontainer(*profile_regular, widen(cmd + "\"" + win_ini + "\""), /*lpac=*/false);
        std::cout << "  measured: regular AppContainer, win.ini -> " << out_win_ini;
        AE_CHECK(out_win_ini.find("ESCAPE_OK") != std::string::npos,
                  "regular AppContainer: win.ini reads ESCAPE_OK (reproduces ADR-004 SS5.3's "
                  "already-measured finding, confirms this spike's harness is faithful)");

        std::string out_secret =
            run_in_appcontainer(*profile_regular, widen(cmd + "\"" + secret_path + "\""), /*lpac=*/false);
        std::cout << "  measured: regular AppContainer, ungranted secret -> " << out_secret;
        AE_CHECK(out_secret.find("ESCAPE_DENIED") != std::string::npos,
                  "regular AppContainer positive control: an ungranted, non-curated path is denied "
                  "(the win.ini leak is a curated-file-set exception, not general containment "
                  "failure)");
    }

    // ---- LPAC ----
    auto profile_lpac = AppContainerProfile::ensure(L"AgentEngine.Spike.LPACDecision.LPAC",
                                                       L"AE LPAC decision spike (LPAC)",
                                                       L"ADR-004 SS11 item 4");
    AE_CHECK(profile_lpac.has_value(), "setup: LPAC-target AppContainer profile created");
    if (profile_lpac.has_value()) {
        std::string out_win_ini =
            run_in_appcontainer(*profile_lpac, widen(cmd + "\"" + win_ini + "\""), /*lpac=*/true);
        std::cout << "  measured: LPAC, win.ini -> " << out_win_ini;
        bool lpac_still_leaks = out_win_ini.find("ESCAPE_OK") != std::string::npos;
        bool lpac_denies = out_win_ini.find("ESCAPE_DENIED") != std::string::npos;
        std::cout << "  RESULT: LPAC " << (lpac_still_leaks ? "STILL LEAKS win.ini"
                                            : lpac_denies    ? "DENIES win.ini (closes ADR-004 SS6.1)"
                                                              : "produced neither outcome (see "
                                                                "SPIKE_ERROR above -- launch itself "
                                                                "may have failed under LPAC)")
                  << "\n";

        std::string out_hosts =
            run_in_appcontainer(*profile_lpac, widen(cmd + "\"" + hosts + "\""), /*lpac=*/true);
        std::cout << "  measured: LPAC, hosts -> " << out_hosts;

        std::string out_secret =
            run_in_appcontainer(*profile_lpac, widen(cmd + "\"" + secret_path + "\""), /*lpac=*/true);
        std::cout << "  measured: LPAC, ungranted secret -> " << out_secret;
    }

    std::error_code ec;
    std::filesystem::remove(secret_path, ec);

    // This spike's own pass/fail is about the HARNESS being faithful (the regular-AppContainer arm
    // reproducing ADR-004's known result) -- the LPAC arm's outcome is the DECISION this produces,
    // reported above, not asserted against an assumed answer either way.
    if (g_failures == 0) {
        std::cout << "ALL PASS (harness fidelity confirmed; see RESULT line above for the LPAC "
                     "decision itself)\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
