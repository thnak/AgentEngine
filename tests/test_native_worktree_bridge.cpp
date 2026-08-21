// Design -> red-team -> prove -> judge for decisions/ADR-071-native-unsandboxed-process-execution-
// providers.md's native_worktree_bridge.hpp: the argv path-shaped-argument validator that keeps
// worktree confinement mandatory for NativeShellProvider/NativeBashProvider/NativePythonProvider/
// NativeNodeProvider even though the OS sandbox jail is optional. Every negative-result claim below
// is paired with a positive control against a REAL directory tree (022 §5).

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "backends/native_process/native_worktree_bridge.hpp"

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

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

}  // namespace

int main() {
    std::filesystem::path const mount_root_path =
        std::filesystem::temp_directory_path() / "ae_native_worktree_bridge_test";
    std::error_code ec;
    std::filesystem::remove_all(mount_root_path, ec);
    std::filesystem::create_directories(mount_root_path / "subdir");
    std::ofstream(mount_root_path / "subdir" / "file.txt").put('x');
    std::wstring const mount_root = widen(mount_root_path.string());

    // ---- W1 (positive control): a bare filename with no separator is always accepted -------------
    {
        auto r = validate_argv_path(mount_root, "output.txt");
        AE_CHECK(r.has_value(), "W1: a bare filename (no path separator) is accepted unconditionally");
    }

    // ---- W2 (positive control): an existing nested path resolving inside mount_root is accepted --
    {
        auto r = validate_argv_path(mount_root, "subdir/file.txt");
        AE_CHECK(r.has_value(), "W2: an existing nested path under the mount is accepted");
        auto r_backslash = validate_argv_path(mount_root, "subdir\\file.txt");
        AE_CHECK(r_backslash.has_value(),
                  "W2: the identical path using Windows-style backslash separators is ALSO accepted "
                  "(normalization)");
    }

    // ---- R-W3: a not-yet-existing parent directory is rejected (named scope limit, not silently --
    // ---- allowed) -----------------------------------------------------------------------------
    {
        auto r = validate_argv_path(mount_root, "does_not_exist_yet/file.txt");
        AE_CHECK(!r.has_value(),
                  "R-W3: a nested path whose containing directory does not already exist is REJECTED, "
                  "not silently permitted (ADR-071's named scope limit)");
    }

    // ---- R-W4: absolute-path forms are rejected outright, even if lexically inside the mount ------
    {
        AE_CHECK(!validate_argv_path(mount_root, "C:\\Windows\\System32\\cmd.exe").has_value(),
                  "R-W4: a drive-letter absolute path is rejected");
        AE_CHECK(!validate_argv_path(mount_root, "\\\\server\\share\\file").has_value(),
                  "R-W4: a UNC path is rejected");
        AE_CHECK(!validate_argv_path(mount_root, "\\rooted\\path").has_value(),
                  "R-W4: a drive-relative-root path ('\\rooted') is rejected");
    }

    // ---- R-W5: '..' traversal is rejected by the underlying grammar check -------------------------
    {
        AE_CHECK(!validate_argv_path(mount_root, "subdir/../../../escape.txt").has_value(),
                  "R-W5: a '..'-bearing argument is rejected");
        AE_CHECK(!validate_argv_path(mount_root, "../escape.txt").has_value(),
                  "R-W5: a leading '..' is rejected");
    }

    // ---- R-W6: a leading-slash absolute-looking relative form is rejected -------------------------
    {
        AE_CHECK(!validate_argv_path(mount_root, "/etc/passwd").has_value(),
                  "R-W6: a leading '/' followed by a further separator (a real absolute-looking "
                  "path) is rejected by split_mount_path's own grammar");
    }

    // ---- W7 (positive control): a CLI flag is passed through unvalidated, never misread as a -----
    // ---- path escape attempt -- the exact bug found live wiring NativeShellProvider to cmd.exe ---
    {
        AE_CHECK(validate_argv_path(mount_root, "/c").has_value(),
                  "W7: a bare Windows-style flag ('/c') is accepted, not rejected as an absolute path");
        AE_CHECK(validate_argv_path(mount_root, "/v:on").has_value(),
                  "W7: a Windows-style flag with a value ('/v:on') is accepted");
        AE_CHECK(validate_argv_path(mount_root, "-n").has_value(),
                  "W7: a POSIX-style short flag ('-n') is accepted");
        AE_CHECK(validate_argv_path(mount_root, "--flag=value").has_value(),
                  "W7: a POSIX-style long flag ('--flag=value') is accepted");
        AE_CHECK(!validate_argv_path(mount_root, "/etc/shadow").has_value(),
                  "W7 (negative control, confirms the flag carve-out is narrow): a leading '/' WITH "
                  "a further separator is still rejected, not swept up by the flag exception");
    }

    std::filesystem::remove_all(mount_root_path, ec);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All native_worktree_bridge (ADR-071) checks passed.\n";
    return 0;
}
