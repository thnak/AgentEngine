// Proof for Milestone 3 Phase G2 (026 §5's `agent.files` "listing" claim) --
// core/worktree_mount_fs.hpp's `list_within_mount_root`, the new primitive `_ae_internal.listdir`
// (mediated_python_runner.cpp) is built on. Two concerns, matching ADR-014's own precedent for
// `open_within_mount_root`: ordinary correctness (root/subdir listing, entry name/is_dir/size), and
// one containment negative control (a junction escaping the mount root must not be walkable), paired
// with a positive control proving the mechanism still allows a legitimate junction pointing back
// INSIDE the root (022 §5) -- so the escape rejection is shown to discriminate, not just refuse
// everything indiscriminately.
//
// Setup is real Win32 I/O against a scratch directory under the system temp path, matching
// test_worktree_mount_fs_escape_corpus.cpp's own established fixture shape (not duplicated wholesale
// -- this file only needs directories/files/one junction, not the full escape corpus).

#include <windows.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/worktree_mount_fs.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

[[noreturn]] void fatal_setup_failure(char const* what, DWORD code) {
    std::cerr << "SETUP FAILURE: " << what << " GetLastError=" << code << "\n";
    std::exit(2);
}

std::wstring join(std::wstring const& a, std::wstring const& b) { return a + L"\\" + b; }

void ensure_dir(std::wstring const& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) fatal_setup_failure("CreateDirectoryW", err);
    }
}

void write_text_file(std::wstring const& path, std::string const& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) fatal_setup_failure("CreateFileW(write)", GetLastError());
    DWORD written = 0;
    if (!WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
        written != content.size()) {
        fatal_setup_failure("WriteFile", GetLastError());
    }
    CloseHandle(h);
}

// Same "shell out to mklink /J" precedent test_worktree_mount_fs_escape_corpus.cpp already uses --
// junctions need no special privilege on Windows, unlike symbolic links.
bool create_junction(std::wstring const& link, std::wstring const& target) {
    std::wstring cmdline = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exit_code == 0;
}

bool has_entry(std::vector<DirEntry> const& entries, std::string const& name) {
    return std::any_of(entries.begin(), entries.end(), [&](DirEntry const& e) { return e.name == name; });
}

// `rmdir /s /q` on a junction removes the junction link itself, never recursing into whatever it
// points at -- the correct behavior here, since one of this file's own fixtures (`outside_target`)
// must survive a re-run even though `scratch/mount/escape` links to it. Best-effort: a first-ever run
// has nothing to remove, so a nonzero exit here is not itself a failure.
void remove_scratch_tree(std::wstring const& path) {
    std::wstring cmdline = L"cmd.exe /c rmdir /s /q \"" + path + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

}  // namespace

int main() {
    wchar_t temp_dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp_dir);
    if (n == 0 || n >= MAX_PATH) fatal_setup_failure("GetTempPathW", GetLastError());
    std::wstring scratch = join(std::wstring(temp_dir, n), L"ae_listdir_test");
    remove_scratch_tree(scratch);  // clean any leftover state from a prior run (crash, or a previous
                                    // pass's junctions/files this fresh run must not see).
    ensure_dir(scratch);
    std::wstring mount_root = join(scratch, L"mount");
    ensure_dir(mount_root);

    // ---- G2-LD1: an empty mount root lists as an empty (not error) result. ----
    {
        auto entries = list_within_mount_root(mount_root, "");
        AE_CHECK(entries.has_value() && entries->empty(), "G2-LD1: an empty mount root lists as empty, not an error");
    }

    // ---- G2-LD2: root listing sees files and subdirectories, with correct name/is_dir/size. ----
    write_text_file(join(mount_root, L"a.txt"), "hello");
    write_text_file(join(mount_root, L"b.txt"), "");
    ensure_dir(join(mount_root, L"sub"));
    {
        auto entries = list_within_mount_root(mount_root, "");
        AE_CHECK(entries.has_value() && entries->size() == 3, "G2-LD2: setup -- root now has 3 entries");
        if (entries) {
            auto find = [&](std::string const& name) -> DirEntry const* {
                for (auto const& e : *entries) {
                    if (e.name == name) return &e;
                }
                return nullptr;
            };
            auto const* a = find("a.txt");
            AE_CHECK(a && !a->is_directory && a->size_bytes == 5,
                     "G2-LD2: a.txt reports as a 5-byte file, not a directory");
            auto const* sub = find("sub");
            AE_CHECK(sub && sub->is_directory, "G2-LD2: sub reports as a directory");
        }
    }

    // ---- G2-LD3: listing a real subdirectory (non-root guest_path) works, through the SAME
    // segment-validation grammar `open_within_mount_root` already enforces. ----
    write_text_file(join(mount_root, L"sub\\c.txt"), "xyz");
    {
        auto entries = list_within_mount_root(mount_root, "sub");
        AE_CHECK(entries.has_value() && entries->size() == 1 && has_entry(*entries, "c.txt"),
                  "G2-LD3: listing 'sub' sees c.txt");
    }

    // ---- G2-LD4 (negative control): a `..`-escaping guest path is rejected before any Win32 call,
    // the same `split_mount_path`/`validate_fs_segment` grammar `open_within_mount_root` enforces. ----
    {
        auto entries = list_within_mount_root(mount_root, "../outside");
        AE_CHECK(!entries.has_value(), "G2-LD4: a '..'-escaping listing path is rejected, not walked");
    }

    // ---- G2-LD5 (negative control, paired 022 §5): a junction inside the mount pointing OUTSIDE it
    // is refused -- the escape is caught by the SAME handle-resolved containment check
    // `open_within_mount_root` uses, applied here to a directory HANDLE for the first time. ----
    std::wstring outside_target = join(scratch, L"outside_target");
    ensure_dir(outside_target);
    write_text_file(join(outside_target, L"secret.txt"), "should never be listed");
    std::wstring escape_link = join(mount_root, L"escape");
    bool junction_ok = create_junction(escape_link, outside_target);
    if (junction_ok) {
        auto entries = list_within_mount_root(mount_root, "escape");
        AE_CHECK(!entries.has_value() && entries.error().code == "worktree.mount_path_escapes_root",
                  "G2-LD5: a junction escaping the mount root is refused when listed, not walked");
    } else {
        std::cout << "  skip: G2-LD5 (mklink /J failed in this environment, not a listdir defect)\n";
    }

    // ---- G2-LD6 (positive control paired with LD5): a junction that stays INSIDE the mount root
    // lists normally -- proving the rejection above discriminates rather than refusing everything. ----
    std::wstring inside_target = join(mount_root, L"sub");
    std::wstring inside_link = join(mount_root, L"alias");
    bool inside_junction_ok = create_junction(inside_link, inside_target);
    if (inside_junction_ok) {
        auto entries = list_within_mount_root(mount_root, "alias");
        AE_CHECK(entries.has_value() && has_entry(*entries, "c.txt"),
                  "G2-LD6: a junction that stays inside the mount root lists normally");
    } else {
        std::cout << "  skip: G2-LD6 (mklink /J failed in this environment, not a listdir defect)\n";
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree_mount_fs listdir checks passed.\n";
    return 0;
}
