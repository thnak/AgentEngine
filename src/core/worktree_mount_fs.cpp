// Implements core/worktree_mount_fs.hpp -- see that header for scope. This file is where ADR-014's
// two competing designs actually live: `open_within_mount_root` (accepted) and
// `redteam::naive_check_within_root`/`naive_open_checked_path` (rejected, kept only as a red-team
// control -- see the header's warning not to call these outside a test binary).

#include "agentengine/core/worktree_mount_fs.hpp"

#include <vector>

#include "agentengine/core/worktree.hpp"  // split_mount_path -- reused, not duplicated

namespace agentengine {

namespace {

result<void> win_error(char const* what, DWORD code) {
    return std::unexpected(error{failure_class::fatal,
                                  std::string(what) + " failed: GetLastError=" + std::to_string(code),
                                  "worktree.mount_fs_win32_failure"});
}

template <class T>
result<T> win_error_t(char const* what, DWORD code) {
    return std::unexpected(win_error(what, code).error());
}

result<std::wstring> utf8_to_wide(std::string const& s) {
    if (s.empty()) return std::wstring{};
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return std::unexpected(error{failure_class::contract, "guest path is not valid UTF-8",
                                      "worktree.mount_path_invalid_encoding"});
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

// The extra, real-filesystem-specific hardening layer on top of `split_mount_path`'s existing
// "no leading/trailing/double slash, no `.`/`..`" contract: a segment that contains a backslash
// can smuggle a whole absolute Windows path (or a `\\?\` prefix, or a `..\..\` climb) through what
// `split_mount_path` sees as one opaque, non-empty, non-`..` segment, because that splitter only
// ever looks for `/`. A colon can smuggle a drive letter (`C:`) or an Alternate Data Stream
// (`file.txt:hidden`). A NUL truncates whatever Win32 API reads the string next. None of these
// three characters can ever be a legitimate single-component filename fragment inside a mount, so
// rejecting them here is not a defense against a plausible false positive -- it removes the
// smuggling surface structurally, before any syscall sees the string at all.
result<void> validate_fs_segment(std::string const& seg) {
    for (char c : seg) {
        if (c == '\\' || c == ':' || c == '\0') {
            return std::unexpected(error{failure_class::policy,
                                          "path segment contains a character not permitted in a mounted path",
                                          "worktree.mount_path_forbidden_character"});
        }
    }
    return {};
}

std::wstring strip_trailing_sep(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

std::wstring join_segments(std::wstring const& root, std::vector<std::wstring> const& wsegments) {
    std::wstring joined = strip_trailing_sep(root);
    for (auto const& seg : wsegments) {
        joined.push_back(L'\\');
        joined += seg;
    }
    return joined;
}

// `candidate` is considered within `root` iff it equals `root` exactly (the root itself) or begins
// with `root` followed immediately by a path separator -- a plain string-prefix check would treat
// `C:\mount2` as inside `C:\mount`, which is exactly the class of bug this comparison exists to
// avoid. Case-insensitive: NTFS is case-preserving but not case-sensitive by default.
bool is_within_root(std::wstring const& root, std::wstring const& candidate) {
    if (candidate.size() < root.size()) return false;
    if (_wcsnicmp(candidate.c_str(), root.c_str(), root.size()) != 0) return false;
    if (candidate.size() == root.size()) return true;
    return candidate[root.size()] == L'\\';
}

result<std::wstring> full_path_name_lexical(std::wstring const& input) {
    DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return win_error_t<std::wstring>("GetFullPathNameW", GetLastError());
    std::wstring buf(needed, L'\0');
    DWORD written = GetFullPathNameW(input.c_str(), needed, buf.data(), nullptr);
    if (written == 0 || written >= needed) return win_error_t<std::wstring>("GetFullPathNameW", GetLastError());
    buf.resize(written);
    return buf;
}

// The check-from-a-real-handle counterpart to `full_path_name_lexical` above: asks the filesystem
// what a HANDLE actually, currently resolves to -- reparse points already followed by the OS at
// open time, 8.3 short-name aliases already expanded to their real long form -- rather than
// re-deriving an answer from a string. `FILE_NAME_NORMALIZED | VOLUME_NAME_DOS` yields an ordinary
// `\\?\C:\...`-shaped absolute path, comparable with the same `is_within_root` used everywhere else.
result<std::wstring> final_path_name(HANDLE h) {
    DWORD needed = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) return win_error_t<std::wstring>("GetFinalPathNameByHandleW", GetLastError());
    std::wstring buf(needed, L'\0');
    DWORD written = GetFinalPathNameByHandleW(h, buf.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= needed) return win_error_t<std::wstring>("GetFinalPathNameByHandleW", GetLastError());
    buf.resize(written);
    return buf;
}

result<std::vector<std::wstring>> validate_and_widen(std::string const& guest_path) {
    auto segments = split_mount_path(guest_path);
    if (!segments) return std::unexpected(segments.error());
    if (segments->empty()) {
        return std::unexpected(error{failure_class::contract, "path names the mount root itself, not a file",
                                      "worktree.mount_path_is_root"});
    }
    std::vector<std::wstring> wsegments;
    wsegments.reserve(segments->size());
    for (auto const& seg : *segments) {
        auto ok = validate_fs_segment(seg);
        if (!ok) return std::unexpected(ok.error());
        auto wseg = utf8_to_wide(seg);
        if (!wseg) return std::unexpected(wseg.error());
        wsegments.push_back(std::move(*wseg));
    }
    return wsegments;
}

} // namespace

result<SafeFileHandle> open_within_mount_root(std::wstring const& mount_root, std::string const& guest_path,
                                               DWORD desired_access, DWORD creation_disposition,
                                               DWORD flags_and_attributes) {
    auto wsegments = validate_and_widen(guest_path);
    if (!wsegments) return std::unexpected(wsegments.error());

    // Open the root once and ask the filesystem (not a string transform) what it really is -- the
    // trusted baseline every candidate below is compared against.
    SafeFileHandle root_handle(CreateFileW(mount_root.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!root_handle.valid()) return win_error_t<SafeFileHandle>("CreateFileW(mount_root)", GetLastError());
    auto root_canonical = final_path_name(root_handle.get());
    if (!root_canonical) return std::unexpected(root_canonical.error());

    std::wstring joined = join_segments(mount_root, *wsegments);

    // ONE open call for the target. Windows resolves any reparse points encountered while walking
    // `joined` transparently as part of ordinary path parsing, arriving at a real handle to
    // whatever the path actually names -- there is no separate "check" step here for anything to
    // race against; the resolution and the acquisition of the handle are the same operation.
    // FILE_SHARE_DELETE alongside READ: a still-open guest handle must not be able to block the
    // host from deleting or replacing the underlying file (e.g. during a later sync/merge pass) --
    // found via this ADR's own C2-7e/C2-7f TOCTOU proof, where an open target handle's default
    // sharing silently defeated a same-name delete-then-recreate the test performed around it.
    SafeFileHandle target(CreateFileW(joined.c_str(), desired_access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                       creation_disposition, flags_and_attributes | FILE_FLAG_BACKUP_SEMANTICS,
                                       nullptr));
    if (!target.valid()) return win_error_t<SafeFileHandle>("CreateFileW(target)", GetLastError());

    // Verify what was ACTUALLY opened -- read from the handle, not re-derived from `joined` -- is
    // still inside the root. A reparse point crossing the boundary, an 8.3 short-name alias, or any
    // other resolution surprise is caught here because this check runs against resolved reality.
    auto target_canonical = final_path_name(target.get());
    if (!target_canonical) return std::unexpected(target_canonical.error());
    if (!is_within_root(*root_canonical, *target_canonical)) {
        return std::unexpected(
            error{failure_class::policy, "resolved path escapes the mount root", "worktree.mount_path_escapes_root"});
    }
    return target;
}

namespace redteam {

result<std::wstring> naive_check_within_root(std::wstring const& mount_root, std::string const& guest_path) {
    // Deliberately reuses the SAME segment validation as the accepted design so this control
    // isolates the one difference under test (string-check-then-reopen vs. handle-based
    // open-and-verify) rather than being a strawman that also skips ordinary `..`/absolute-path
    // rejection real naive code would not skip either.
    auto wsegments = validate_and_widen(guest_path);
    if (!wsegments) return std::unexpected(wsegments.error());

    std::wstring joined = join_segments(mount_root, *wsegments);

    auto canonical = full_path_name_lexical(joined);
    if (!canonical) return std::unexpected(canonical.error());
    auto root_canonical = full_path_name_lexical(mount_root);
    if (!root_canonical) return std::unexpected(root_canonical.error());

    if (!is_within_root(*root_canonical, *canonical)) {
        return std::unexpected(error{failure_class::policy, "resolved path escapes the mount root (lexical check)",
                                      "worktree.mount_path_escapes_root"});
    }
    // THE BUG: the check's result is a STRING, handed back for the caller to re-open later.
    // Nothing ties this answer to what the filesystem looks like at the moment it is actually used.
    return canonical;
}

result<SafeFileHandle> naive_open_checked_path(std::wstring const& canonical_path, DWORD desired_access,
                                                DWORD creation_disposition) {
    // Re-parses `canonical_path` as a fresh CreateFileW call. Whatever the filesystem looks like
    // RIGHT NOW -- not when `naive_check_within_root` ran -- determines what this opens. No
    // containment re-check happens here; this function trusts the string it was handed.
    SafeFileHandle h(CreateFileW(canonical_path.c_str(), desired_access, FILE_SHARE_READ, nullptr,
                                  creation_disposition, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) return win_error_t<SafeFileHandle>("CreateFileW", GetLastError());
    return h;
}

} // namespace redteam

} // namespace agentengine
