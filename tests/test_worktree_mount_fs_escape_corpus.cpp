// Red-team corpus + executed evidence for Milestone 3 Phase C2 / ADR-014
// (decisions/ADR-014-worktree-mount-path-canonicalization.md), proving core/worktree_mount_fs.hpp's
// `open_within_mount_root` against 025-Worktree-and-Virtual-Filesystem.md §5's own named attack
// list, reused verbatim from 021 §6 G3: "`..`, absolute redirects, symlinks/junctions/reparse
// points crossing the boundary, ADS, `\\?\` prefixes, unicode-normalization tricks, and TOCTOU
// re-resolution."
//
// Every rejection is paired with a positive control (022 §5): a request that is IDENTICAL in shape
// but legitimately inside the root, proving the mechanism can both allow and deny rather than
// failing everything indiscriminately. The TOCTOU class is proven via a deterministic, single-
// threaded interleaving (this project's established discrete-event-simulation precedent from
// tests/test_worktree_branch_concurrency.cpp, Phase B4) rather than a real timing race: the
// filesystem state is mutated by hand between two calls, exactly the state a real concurrent
// attacker would need to win a genuine race, made reproducible instead of timing-dependent.
//
// All setup (directories, junctions, files) is real Win32 I/O against a scratch directory under
// the system temp path, removed best-effort at the end of the run.

#include <windows.h>

#include <cstdio>
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

std::wstring g_scratch_root;

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

std::string read_all_text(HANDLE h) {
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    std::string buf(4096, '\0');
    DWORD read = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    buf.resize(read);
    return buf;
}

// Runs `mklink /J link target` (junctions need no special privilege on Windows, unlike symbolic
// links) via a child cmd.exe process -- test-setup infrastructure only, matching this project's own
// precedent (ADR-004's spike shelled out to `icacls` the same way) rather than hand-rolling
// FSCTL_SET_REPARSE_POINT's REPARSE_DATA_BUFFER, which is a WDK type not declared by the plain SDK.
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

void remove_file(std::wstring const& path) { DeleteFileW(path.c_str()); }
void remove_dir(std::wstring const& path) { RemoveDirectoryW(path.c_str()); }  // also removes a junction's link node

std::string wide_to_utf8_test(std::wstring const& s) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

int main() {
    // ---- Scratch layout ---------------------------------------------------------------------
    // <scratch>/mount_root/         the mount root under test
    // <scratch>/mount_root/inside/secret.txt        = "INSIDE_SECRET"        (positive control)
    // <scratch>/mount_root/escape_link -> <scratch>/outside_target (junction, crosses boundary)
    // <scratch>/mount_root/inside_link -> <scratch>/mount_root/inside (junction, stays inside)
    // <scratch>/outside_target/secret.txt           = "OUTSIDE_SECRET"
    // <scratch>/mount_root/weird_dir (fullwidth U+FF0E U+FF0E name) /marker.txt = "UNICODE_OK"
    // <scratch>/mount_root/toctou_dir/secret.txt    = "TOCTOU_INSIDE" (mutated mid-test)
    // <scratch>/toctou_outside/secret.txt           = "TOCTOU_OUTSIDE_SECRET"
    {
        wchar_t temp_path[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, temp_path);
        if (n == 0) fatal_setup_failure("GetTempPathW", GetLastError());
        g_scratch_root = join(std::wstring(temp_path, n - 1),
                               L"ae_c2_" + std::to_wstring(GetCurrentProcessId()));
        ensure_dir(g_scratch_root);
    }
    std::wstring mount_root    = join(g_scratch_root, L"mount_root");
    std::wstring outside_root  = join(g_scratch_root, L"outside_target");
    std::wstring toctou_out    = join(g_scratch_root, L"toctou_outside");
    ensure_dir(mount_root);
    ensure_dir(outside_root);
    ensure_dir(toctou_out);
    ensure_dir(join(mount_root, L"inside"));
    write_text_file(join(join(mount_root, L"inside"), L"secret.txt"), "INSIDE_SECRET");
    write_text_file(join(outside_root, L"secret.txt"), "OUTSIDE_SECRET");
    write_text_file(join(toctou_out, L"secret.txt"), "TOCTOU_OUTSIDE_SECRET");

    bool have_junction_support = create_junction(join(mount_root, L"escape_link"), outside_root);
    if (have_junction_support) {
        create_junction(join(mount_root, L"inside_link"), join(mount_root, L"inside"));
    } else {
        std::cerr << "SKIP: mklink /J unavailable on this machine -- symlink/junction escape "
                     "corpus items are SKIPPED, not silently passed.\n";
    }

    std::wstring weird_name = L"\uFF0E\uFF0E";  // FULLWIDTH FULL STOP x2 -- NOT ASCII '..'
    std::wstring weird_dir  = join(mount_root, weird_name);
    ensure_dir(weird_dir);
    write_text_file(join(weird_dir, L"marker.txt"), "UNICODE_OK");

    // ============================================================================================
    // C2-1: lexical `..` traversal -- rejected structurally by split_mount_path's own contract
    // (reused, not duplicated), never reaching a syscall.
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, "../outside_target/secret.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_malformed", "C2-1a: leading .. rejected");

        auto r2 = open_within_mount_root(mount_root, "inside/../../outside_target/secret.txt", GENERIC_READ,
                                          OPEN_EXISTING);
        AE_CHECK(!r2.has_value() && r2.error().code == "worktree.mount_path_malformed",
                 "C2-1b: embedded .. rejected");
    }

    // ============================================================================================
    // C2-2: absolute redirect -- a drive-qualified or rooted guest path is rejected before any
    // syscall (the ':' in "C:" and the leading '/' both trip validation independently).
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, "C:/Windows/win.ini", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_forbidden_character",
                 "C2-2a: drive-qualified absolute path rejected");

        auto r2 = open_within_mount_root(mount_root, "/Windows/win.ini", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!r2.has_value() && r2.error().code == "worktree.mount_path_absolute",
                 "C2-2b: leading-slash absolute path rejected");
    }

    // ============================================================================================
    // C2-3: symlink/junction/reparse-point boundary crossing -- the flagship isolation claim.
    // Positive control (C2-3c) proves the SAME mechanism, same call shape, correctly ALLOWS a
    // request that stays inside the root, including through an internal (non-crossing) junction.
    // ============================================================================================
    if (have_junction_support) {
        auto escape = open_within_mount_root(mount_root, "escape_link/secret.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!escape.has_value() && escape.error().code == "worktree.mount_path_escapes_root",
                 "C2-3a: junction crossing the mount boundary rejected");

        auto inside = open_within_mount_root(mount_root, "inside/secret.txt", GENERIC_READ, OPEN_EXISTING);
        bool inside_ok = inside.has_value() && read_all_text(inside->get()) == "INSIDE_SECRET";
        AE_CHECK(inside_ok, "C2-3b (positive control): ordinary inside file reads correctly");

        auto via_internal_link =
            open_within_mount_root(mount_root, "inside_link/secret.txt", GENERIC_READ, OPEN_EXISTING);
        bool internal_link_ok =
            via_internal_link.has_value() && read_all_text(via_internal_link->get()) == "INSIDE_SECRET";
        AE_CHECK(internal_link_ok,
                 "C2-3c (positive control): a junction that stays INSIDE the root is followed, not blanket-denied");
    } else {
        std::cerr << "SKIP: C2-3a/b/c (no junction support on this machine)\n";
    }

    // ============================================================================================
    // C2-4: ADS (Alternate Data Streams) -- rejected structurally by the ':' character check,
    // before any syscall, regardless of whether the base file exists.
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, "inside/secret.txt:hidden", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_forbidden_character",
                 "C2-4: Alternate Data Stream suffix rejected structurally");
    }

    // ============================================================================================
    // C2-5: `\\?\` extended-length prefix -- rejected structurally (the embedded backslashes trip
    // the same forbidden-character check; there is no separate lexical parser for this class to
    // bypass).
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, "\\\\?\\C:\\Windows\\win.ini", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_forbidden_character",
                 "C2-5: \\\\?\\ prefix rejected structurally");
    }

    // ============================================================================================
    // C2-6: unicode-normalization tricks -- a real on-disk directory named with fullwidth dots
    // (U+FF0E x2, NOT ASCII '..') is treated as an ORDINARY opaque name, proven both directions:
    // it opens correctly when named explicitly, and literal ASCII ".." is never conflated with it.
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, wide_to_utf8_test(weird_name) + "/marker.txt", GENERIC_READ,
                                         OPEN_EXISTING);
        bool ok = r.has_value() && read_all_text(r->get()) == "UNICODE_OK";
        AE_CHECK(ok, "C2-6a: a fullwidth-dot directory name is treated as an ordinary literal name");

        auto dotdot = open_within_mount_root(mount_root, "../marker.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!dotdot.has_value() && dotdot.error().code == "worktree.mount_path_malformed",
                 "C2-6b: literal ASCII '..' is never conflated with the fullwidth name (still rejected)");
    }

    // ============================================================================================
    // C2-7: TOCTOU re-resolution. Design A (naive, redteam-only): check a path lexically, get back
    // a STRING, then reopen that string later. Between the two calls the filesystem is mutated by
    // hand -- a real directory is swapped for a junction pointing OUTSIDE the root -- proving the
    // check's result does not bind to what is actually opened. Design B (accepted): the same
    // mutation is applied around a single open_within_mount_root call and around an already-open
    // handle, proving neither is affected the same way.
    // ============================================================================================
    if (have_junction_support) {
        std::wstring toctou_dir = join(mount_root, L"toctou_dir");
        ensure_dir(toctou_dir);
        write_text_file(join(toctou_dir, L"secret.txt"), "TOCTOU_INSIDE");

        // --- Design A: naive check-then-reopen, proven exploitable ---------------------------
        auto checked = redteam::naive_check_within_root(mount_root, "toctou_dir/secret.txt");
        AE_CHECK(checked.has_value(), "C2-7a: naive check accepts the currently-real inside path");

        // Swap: the checked directory is removed and replaced with a junction to an OUTSIDE
        // location -- exactly the state a real racing attacker would need to win the race; made
        // deterministic here instead of timing-dependent (same precedent as test_worktree_branch_
        // concurrency.cpp's discrete-event simulation).
        remove_file(join(toctou_dir, L"secret.txt"));
        remove_dir(toctou_dir);
        bool swapped = create_junction(toctou_dir, toctou_out);
        AE_CHECK(swapped, "C2-7b (setup): toctou_dir successfully swapped for an outside junction");

        if (checked.has_value() && swapped) {
            auto reopened = redteam::naive_open_checked_path(*checked, GENERIC_READ, OPEN_EXISTING);
            bool leaked_outside = reopened.has_value() && read_all_text(reopened->get()) == "TOCTOU_OUTSIDE_SECRET";
            AE_CHECK(leaked_outside,
                     "C2-7c: naive design's reopen-by-string reads OUTSIDE content despite the check having "
                     "validated an inside path -- the TOCTOU vulnerability, reproduced deterministically");
        }

        // --- Design B: handle-based open, proven immune to the identical interleaving --------
        remove_dir(toctou_dir);  // drop the junction node from the previous phase
        ensure_dir(toctou_dir);
        write_text_file(join(toctou_dir, L"secret.txt"), "TOCTOU_INSIDE_B");

        auto pre_swap_handle = open_within_mount_root(mount_root, "toctou_dir/secret.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(pre_swap_handle.has_value() && read_all_text(pre_swap_handle->get()) == "TOCTOU_INSIDE_B",
                 "C2-7d: Design B opens the real inside file before the swap");

        remove_file(join(toctou_dir, L"secret.txt"));
        remove_dir(toctou_dir);
        bool swapped_b = create_junction(toctou_dir, toctou_out);
        AE_CHECK(swapped_b, "C2-7e (setup): toctou_dir swapped again for Design B's phase");

        if (pre_swap_handle.has_value()) {
            // The handle acquired BEFORE the swap refers to the file object itself, not to the
            // path -- re-reading it after the swap must still return the ORIGINAL inside content,
            // proving an already-validated handle is not retroactively redirected.
            std::string still = read_all_text(pre_swap_handle->get());
            AE_CHECK(still == "TOCTOU_INSIDE_B",
                     "C2-7f: an already-open Design B handle is unaffected by a later filesystem swap");
        }

        // A FRESH request for the same guest path, made AFTER the swap, must re-resolve current
        // reality and be rejected -- proving Design B never caches a stale validation result, so
        // racing gains an attacker nothing: a handle obtained before the swap is safely scoped to
        // what it was opened against, and a request made after the swap sees the swap.
        auto post_swap = open_within_mount_root(mount_root, "toctou_dir/secret.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!post_swap.has_value() && post_swap.error().code == "worktree.mount_path_escapes_root",
                 "C2-7g: a fresh Design B request made after the swap re-resolves and is rejected");
    } else {
        std::cerr << "SKIP: C2-7 TOCTOU corpus (no junction support on this machine)\n";
    }

    // ============================================================================================
    // C2-8: case tricks -- NTFS is case-insensitive by default; a differently-cased request for
    // the same inside file must still resolve, and still be recognized as inside (not a distinct
    // escape vector under this design, but named in 021 §6 G3's own checklist, so covered
    // explicitly rather than silently assumed).
    // ============================================================================================
    {
        auto r = open_within_mount_root(mount_root, "INSIDE/SECRET.TXT", GENERIC_READ, OPEN_EXISTING);
        bool ok = r.has_value() && read_all_text(r->get()) == "INSIDE_SECRET";
        AE_CHECK(ok, "C2-8: a differently-cased request for an inside file still resolves inside");
    }

    // ============================================================================================
    // C2-9: root itself and malformed-segment rejections (empty path, trailing slash, double
    // slash) -- reused directly from split_mount_path's own already-proven contract
    // (tests/test_worktree_mount.cpp C1-C1); re-asserted here through the real-FS entry point so a
    // future refactor of open_within_mount_root can't silently stop calling split_mount_path.
    // ============================================================================================
    {
        auto root = open_within_mount_root(mount_root, "", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!root.has_value() && root.error().code == "worktree.mount_path_is_root",
                 "C2-9a: the mount root itself is rejected (not a file)");

        auto trailing = open_within_mount_root(mount_root, "inside/", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!trailing.has_value() && trailing.error().code == "worktree.mount_path_malformed",
                 "C2-9b: trailing slash rejected");

        auto doubled = open_within_mount_root(mount_root, "inside//secret.txt", GENERIC_READ, OPEN_EXISTING);
        AE_CHECK(!doubled.has_value() && doubled.error().code == "worktree.mount_path_malformed",
                 "C2-9c: double slash rejected");
    }

    // ============================================================================================
    // C2-10: a CREATING disposition (CREATE_ALWAYS) through an escaping junction. Every prior case
    // in this corpus opens GENERIC_READ + OPEN_EXISTING, which cannot itself have a side effect on
    // rejection -- it either legitimately reads existing content or fails outright. A creating
    // disposition is different: Windows performs the create as PART OF resolving `joined`, before
    // this function's own containment check ever runs, so a real, previously-undiscovered gap let a
    // rejected write still plant a brand-new (empty) object at the escaped, outside-the-mount-root
    // location -- found by a sibling design's external-validation pass replaying this exact ADR's
    // own C2-7 technique against a DIFFERENT mediation primitive, then noticing the same creating-
    // disposition shape had never been exercised against THIS primitive either. Fixed by unwinding
    // the just-created object through the SAME handle just verified (never a re-parsed path string,
    // preserving this whole file's "the object verified is the object used" property for the
    // cleanup step too), not merely by detecting and rejecting the escape.
    // ============================================================================================
    if (have_junction_support) {
        std::wstring const planted_path = join(outside_root, L"c2_10_planted.txt");
        remove_file(planted_path);  // in case a prior interrupted run left one behind

        auto create_escape =
            open_within_mount_root(mount_root, "escape_link/c2_10_planted.txt", GENERIC_WRITE, CREATE_ALWAYS);
        AE_CHECK(!create_escape.has_value() && create_escape.error().code == "worktree.mount_path_escapes_root",
                 "C2-10a: a CREATING open through an escaping junction is still rejected");

        DWORD attrs = GetFileAttributesW(planted_path.c_str());
        bool const nothing_planted = (attrs == INVALID_FILE_ATTRIBUTES);
        AE_CHECK(nothing_planted,
                 "C2-10b: the rejected create left NOTHING planted at the escaped outside location "
                 "(previously: a real, empty file WAS left behind despite the rejection)");

        // Positive control: the identical creating disposition, for a path that legitimately stays
        // inside the root, must still work -- proving the cleanup-on-reject path does not make
        // ordinary inside creates spuriously fail or get cleaned up too.
        auto create_inside =
            open_within_mount_root(mount_root, "inside/c2_10_created.txt", GENERIC_WRITE, CREATE_ALWAYS);
        AE_CHECK(create_inside.has_value(), "C2-10c (positive control): an ordinary inside CREATE_ALWAYS still works");
        if (create_inside.has_value()) {
            DWORD written = 0;
            WriteFile(create_inside->get(), "OK", 2, &written, nullptr);
        }
        DWORD inside_attrs = GetFileAttributesW(join(mount_root, L"inside\\c2_10_created.txt").c_str());
        AE_CHECK(inside_attrs != INVALID_FILE_ATTRIBUTES,
                 "C2-10d (positive control): the legitimately-created inside file actually persists");
    } else {
        std::cerr << "SKIP: C2-10 (no junction support on this machine)\n";
    }

    // ---- Best-effort cleanup ------------------------------------------------------------------
    {
        std::wstring cmdline = L"cmd.exe /c rmdir /s /q \"" + g_scratch_root + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
        mutable_cmd.push_back(L'\0');
        if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
