// Implements mediated_filesystem_adapter.hpp -- see that header for the TOCTOU-safety rationale.

#include "backends/native_jail/mediated_filesystem_adapter.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "agentengine/core/worktree.hpp"           // split_mount_path -- reused, not duplicated
#include "agentengine/core/worktree_mount_fs.hpp"  // open_within_mount_root, SafeFileHandle

namespace agentengine::native_jail::mediated_shell {

namespace {

result<void> win_error_v(char const* what, DWORD code) {
    return std::unexpected(error{failure_class::fatal,
                                  std::string(what) + " failed: GetLastError=" + std::to_string(code),
                                  "shell.fs_win32_failure"});
}
template <class T>
result<T> win_error(char const* what, DWORD code) {
    return std::unexpected(win_error_v(what, code).error());
}

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}
std::string narrow(std::wstring const& s) {
    if (s.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace

result<MediatedFileSystemAdapter> MediatedFileSystemAdapter::create(std::filesystem::path root) {
    // Prove the root itself is a real, existing directory -- open_within_mount_root's own
    // "trusted baseline" step re-does this on every call anyway, but failing fast here at
    // construction (matching RealFileSystemAdapter's own fallible-factory shape) surfaces a
    // misconfigured root immediately, not on the adapter's first real use.
    // `root.c_str()` is already `const wchar_t*` here -- `std::filesystem::path::value_type` is
    // `wchar_t` on Windows, so this needs no explicit `.wstring()` conversion.
    HANDLE h = CreateFileW(root.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win_error<MediatedFileSystemAdapter>("CreateFileW(root)", GetLastError());
    CloseHandle(h);
    return MediatedFileSystemAdapter(std::move(root));
}

result<std::vector<std::byte>> MediatedFileSystemAdapter::read_file(std::string_view path) {
    auto h = open_within_mount_root(root_.wstring(), std::string(path), GENERIC_READ, OPEN_EXISTING);
    if (!h) return std::unexpected(h.error());

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h->get(), &size)) return win_error<std::vector<std::byte>>("GetFileSizeEx", GetLastError());
    std::vector<std::byte> out(static_cast<std::size_t>(size.QuadPart));
    std::size_t total_read = 0;
    while (total_read < out.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - total_read, 1u << 20));
        if (!ReadFile(h->get(), out.data() + total_read, want, &chunk, nullptr)) {
            return win_error<std::vector<std::byte>>("ReadFile", GetLastError());
        }
        if (chunk == 0) break;
        total_read += chunk;
    }
    out.resize(total_read);
    return out;
}

result<void> MediatedFileSystemAdapter::write_file(std::string_view path, std::span<std::byte const> data,
                                                     bool append) {
    auto h = open_within_mount_root(root_.wstring(), std::string(path), GENERIC_WRITE,
                                     append ? OPEN_ALWAYS : CREATE_ALWAYS);
    if (!h) return std::unexpected(h.error());
    if (append) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(h->get(), zero, nullptr, FILE_END);
    }
    std::size_t total_written = 0;
    while (total_written < data.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - total_written, 1u << 20));
        if (!WriteFile(h->get(), data.data() + total_written, want, &chunk, nullptr)) {
            return win_error_v("WriteFile", GetLastError());
        }
        total_written += chunk;
    }
    return {};
}

result<bool> MediatedFileSystemAdapter::exists(std::string_view path) {
    // Try as a file first, then as a directory (CreateFileW's OPEN_EXISTING against a directory
    // needs FILE_FLAG_BACKUP_SEMANTICS, already set internally by open_within_mount_root) -- either
    // succeeding means the path names something real; a policy-class error (escapes the mount, a
    // malformed segment) is propagated as a real error rather than silently reported as "false",
    // since that is a POLICY fact, not an "absent" fact.
    auto h = open_within_mount_root(root_.wstring(), std::string(path), GENERIC_READ, OPEN_EXISTING);
    if (h) return true;
    if (h.error().klass == failure_class::policy || h.error().klass == failure_class::contract) {
        return std::unexpected(h.error());
    }
    return false;
}

result<void> MediatedFileSystemAdapter::remove(std::string_view path, bool recursive) {
    auto h = open_within_mount_root(root_.wstring(), std::string(path), DELETE | FILE_LIST_DIRECTORY | GENERIC_READ,
                                     OPEN_EXISTING);
    if (!h) return std::unexpected(h.error());

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h->get(), &info)) return win_error_v("GetFileInformationByHandle", GetLastError());
    bool is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (is_dir && recursive) {
        auto entries = list_directory(path);
        if (!entries) return std::unexpected(entries.error());
        for (auto const& e : *entries) {
            std::string child = std::string(path) + (path.empty() ? "" : "/") + e.name;
            auto r = remove(child, true);
            if (!r) return r;
        }
    }

    FILE_DISPOSITION_INFO disp{};
    disp.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(h->get(), FileDispositionInfo, &disp, sizeof(disp))) {
        return win_error_v("SetFileInformationByHandle(FileDispositionInfo)", GetLastError());
    }
    return {};
}

result<void> MediatedFileSystemAdapter::rename(std::string_view from, std::string_view to) {
    // Implemented as copy-then-delete, not `SetFileInformationByHandle(FileRenameInfo)` --
    // ADR-001's own researched design already names copy+remove as an accepted (if non-atomic, no-
    // rollback-on-partial-failure) fallback pattern for `mv` (its own finding 8, "still open, not
    // closed by any amendment"), carried forward here as the PRIMARY implementation rather than a
    // cross-device special case, since it is simpler and more reliably correct than the handle-
    // relative rename API for this pass. A named, accepted residual: a crash between the copy and
    // the delete would leave both `from` and `to` present rather than exactly one, same as ADR-001's
    // own stated gap -- not silently claimed atomic.
    auto data = read_file(from);
    if (!data) return std::unexpected(data.error());
    auto write = write_file(to, std::span<std::byte const>(*data), false);
    if (!write) return std::unexpected(write.error());
    return remove(from, false);
}

result<void> MediatedFileSystemAdapter::copy_file(std::string_view from, std::string_view to) {
    auto data = read_file(from);
    if (!data) return std::unexpected(data.error());
    return write_file(to, std::span<std::byte const>(*data), false);
}

namespace {

// Mirrors core/worktree_mount_fs.cpp's own (internal-linkage) `validate_fs_segment` character class
// exactly: backslash, colon, and NUL are never valid within a single path SEGMENT, since Windows
// treats `\` as its own separator regardless of what a `/`-only tokenizer (`split_mount_path`) does.
// Duplicated rather than exported -- that function lives in a different, ADR-014-governed file with
// internal linkage; widening its visibility is out of scope for this fix. Code-review fix
// (2026-08-07): `create_one_directory`'s leaf segment had NO character validation at all before this
// -- see that function's own comment below for the escape this closes.
result<void> reject_forbidden_segment_chars(std::string const& seg) {
    for (char c : seg) {
        if (c == '\\' || c == ':' || c == '\0') {
            return std::unexpected(error{failure_class::policy,
                                          "path segment contains a character not permitted in a mounted path",
                                          "worktree.mount_path_forbidden_character"});
        }
    }
    return {};
}

// `CreateFileW` cannot itself CREATE a new directory (only `FILE_FLAG_BACKUP_SEMANTICS` OPENS an
// existing one) -- real directory creation needs the separate `CreateDirectoryW` API, which has no
// handle-relative form. This is a narrower TOCTOU guarantee than `open_within_mount_root`'s own
// open-then-verify pattern: the PARENT is verified via a real handle (`GetFinalPathNameByHandleW`,
// the same resolved-reality check ADR-014 uses), but `CreateDirectoryW` itself still takes that
// verified parent's path as a STRING, re-parsed by the OS on this second call -- a real, narrower-
// than-ADR-014 residual, named here rather than silently assumed equivalent (a candidate for E4 or
// a follow-up to close via a stronger primitive, e.g. an NT-native relative-create, not attempted
// this pass).
//
// Code-review fix (2026-08-07), CRITICAL: the LEAF segment used to be handed straight to
// `CreateDirectoryW` with zero validation -- `open_within_mount_root` below validates the PARENT
// (rejecting `\`/`:`/NUL per segment via its own internal `validate_fs_segment`), but the leaf was
// computed by a bare `find_last_of('/')` split and never ran through that or any equivalent check.
// Since this tokenizer only understands `/`, a guest segment like `"..\\..\\..\\Windows\\Temp\\evil"`
// contains no `/` at all -- it IS the whole leaf, verbatim -- and `CreateDirectoryW` then does its
// own ordinary Win32 backslash-delimited parsing on `parent_real + "\\" + leaf`, walking the
// embedded `..` components and creating a real directory OUTSIDE the mount root entirely. No race,
// no reparse point, no 8.3 alias needed -- a deterministic, guest-reachable mount escape via the
// shell's ordinary `mkdir` builtin. Now rejected up front with the same character-class rule every
// other mediated path segment is held to.
result<void> create_one_directory(std::wstring const& root, std::string const& relative_path) {
    auto slash = relative_path.find_last_of('/');
    std::string parent = slash == std::string::npos ? "" : relative_path.substr(0, slash);
    std::string leaf = slash == std::string::npos ? relative_path : relative_path.substr(slash + 1);

    if (auto ok = reject_forbidden_segment_chars(leaf); !ok) return std::unexpected(ok.error());
    if (leaf.empty() || leaf == "." || leaf == "..") {
        return std::unexpected(error{failure_class::contract,
                                      "'.'/'..' are not a valid directory name to create",
                                      "worktree.mount_path_malformed"});
    }

    std::wstring parent_real;
    if (parent.empty()) {
        HANDLE rh = CreateFileW(root.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (rh == INVALID_HANDLE_VALUE) return win_error_v("CreateFileW(root)", GetLastError());
        DWORD n = GetFinalPathNameByHandleW(rh, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        parent_real.resize(n);
        GetFinalPathNameByHandleW(rh, parent_real.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!parent_real.empty() && parent_real.back() == L'\0') parent_real.pop_back();
        CloseHandle(rh);
    } else {
        auto parent_handle = open_within_mount_root(root, parent, FILE_LIST_DIRECTORY | GENERIC_READ, OPEN_EXISTING);
        if (!parent_handle) return std::unexpected(parent_handle.error());
        DWORD n = GetFinalPathNameByHandleW(parent_handle->get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        parent_real.resize(n);
        GetFinalPathNameByHandleW(parent_handle->get(), parent_real.data(), n,
                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!parent_real.empty() && parent_real.back() == L'\0') parent_real.pop_back();
    }

    std::wstring full = parent_real + L"\\" + widen(leaf);
    if (!CreateDirectoryW(full.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS) return {};
        return win_error_v("CreateDirectoryW", err);
    }
    return {};
}

}  // namespace

result<void> MediatedFileSystemAdapter::make_directory(std::string_view path, bool parents) {
    if (!parents) return create_one_directory(root_.wstring(), std::string(path));

    auto segments = split_mount_path(std::string(path));
    if (!segments) return std::unexpected(segments.error());
    std::string prefix;
    for (auto const& seg : *segments) {
        prefix += (prefix.empty() ? "" : "/") + seg;
        auto probe = open_within_mount_root(root_.wstring(), prefix, GENERIC_READ, OPEN_EXISTING);
        if (probe) continue;  // already exists
        // Code-review fix (2026-08-07): a probe failure for any reason OTHER than "this segment
        // doesn't exist on disk yet" -- a forbidden character, an out-of-mount escape, any other
        // Win32 error -- must propagate as a real rejection, never be treated as "go ahead and
        // create it". Falling through unconditionally here (the previous behavior) was a second,
        // indirect route to the same mount-escape class create_one_directory's own leaf-validation
        // gap allowed directly: a rejected/malformed prefix would still reach CreateDirectoryW.
        bool const not_found_yet = probe.error().code == "worktree.mount_fs_win32_failure" &&
                                    (probe.error().native_code == ERROR_FILE_NOT_FOUND ||
                                     probe.error().native_code == ERROR_PATH_NOT_FOUND);
        if (!not_found_yet) return std::unexpected(probe.error());
        auto created = create_one_directory(root_.wstring(), prefix);
        if (!created) return std::unexpected(created.error());
    }
    return {};
}

result<std::vector<DirEntry>> MediatedFileSystemAdapter::list_directory(std::string_view path) {
    // `open_within_mount_root` structurally rejects an empty guest path (`worktree.mount_path_is_root`)
    // -- correct for `read_file`/`write_file`, where "the root" is never a valid FILE target, but
    // wrong here: listing the mount's own root ("" / ExecState.cwd before any `cd`) is an ordinary,
    // expected directory-listing target. Opened directly against `root_`, the same root-special-case
    // `create_one_directory` above already needs for the identical reason.
    SafeFileHandle h;
    if (path.empty()) {
        HANDLE rh = CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY | GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (rh == INVALID_HANDLE_VALUE) return win_error<std::vector<DirEntry>>("CreateFileW(root)", GetLastError());
        h = SafeFileHandle(rh);
    } else {
        auto opened = open_within_mount_root(root_.wstring(), std::string(path), FILE_LIST_DIRECTORY | GENERIC_READ, OPEN_EXISTING);
        if (!opened) return std::unexpected(opened.error());
        h = std::move(*opened);
    }

    std::vector<DirEntry> out;
    std::vector<std::byte> buf(64 * 1024);
    for (;;) {
        BOOL ok = GetFileInformationByHandleEx(h.get(), FileFullDirectoryInfo, buf.data(),
                                                static_cast<DWORD>(buf.size()));
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_FILES) break;
            return win_error<std::vector<DirEntry>>("GetFileInformationByHandleEx", err);
        }
        std::size_t offset = 0;
        for (;;) {
            auto* entry = reinterpret_cast<FILE_FULL_DIR_INFO*>(buf.data() + offset);
            std::wstring name(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
            if (name != L"." && name != L"..") {
                DirEntry de;
                de.name = narrow(name);
                de.is_directory = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                de.size_bytes = static_cast<std::uint64_t>(entry->EndOfFile.QuadPart);
                out.push_back(std::move(de));
            }
            if (entry->NextEntryOffset == 0) break;
            offset += entry->NextEntryOffset;
        }
    }
    return out;
}

result<std::string> MediatedFileSystemAdapter::canonicalize(std::string_view path) {
    auto h = open_within_mount_root(root_.wstring(), std::string(path), GENERIC_READ, OPEN_EXISTING);
    if (!h) return std::unexpected(h.error());

    DWORD needed = GetFinalPathNameByHandleW(h->get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) return win_error<std::string>("GetFinalPathNameByHandleW", GetLastError());
    std::wstring buf(needed, L'\0');
    DWORD written = GetFinalPathNameByHandleW(h->get(), buf.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= needed) return win_error<std::string>("GetFinalPathNameByHandleW", GetLastError());
    buf.resize(written);

    // Strip the (already-canonicalized) root prefix -- the adapter's own contract is a
    // root-relative canonical path, never a host path (filesystem_adapter.hpp's own comment:
    // "callers never see host paths").
    std::wstring root_final;
    {
        HANDLE rh = CreateFileW(root_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (rh == INVALID_HANDLE_VALUE) return win_error<std::string>("CreateFileW(root)", GetLastError());
        DWORD rn = GetFinalPathNameByHandleW(rh, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        root_final.resize(rn);
        GetFinalPathNameByHandleW(rh, root_final.data(), rn, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!root_final.empty() && root_final.back() == L'\0') root_final.pop_back();
        CloseHandle(rh);
    }
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    std::wstring relative = buf.size() > root_final.size() ? buf.substr(root_final.size()) : L"";
    while (!relative.empty() && relative.front() == L'\\') relative.erase(relative.begin());
    for (auto& c : relative) {
        if (c == L'\\') c = L'/';
    }
    return narrow(relative);
}

// Gap-12 fix (2026-08-14): `usage()`'s real answer for this adapter -- forwards to the same
// `mount_root_usage` scan `Internal_open`'s write branch already uses (Milestone 3 Phase G4),
// so `mediated_shell_dispatch.cpp`'s live-quota check shares the exact scanning logic rather than
// a second, independently-reasoned copy.
result<std::optional<MountUsage>> MediatedFileSystemAdapter::usage() {
    auto u = mount_root_usage(root_.wstring());
    if (!u) return std::unexpected(u.error());
    return std::optional<MountUsage>(*u);
}

}  // namespace agentengine::native_jail::mediated_shell
