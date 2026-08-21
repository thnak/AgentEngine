// Implements native_worktree_bridge.hpp. See that header for the spec/ADR citation, the scope
// statement, and the honestly-named TOCTOU residual this file does not (and cannot, given real
// unsandboxed child processes take path arguments, not handles) eliminate.

#include "backends/native_process/native_worktree_bridge.hpp"

#include <windows.h>

#include "agentengine/core/worktree.hpp"
#include "agentengine/core/worktree_mount_fs.hpp"

namespace agentengine::native_process {

namespace {

bool contains_path_separator(std::string const& s) {
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
}

// A CLI FLAG, not a path -- must be let through unvalidated rather than misread as an absolute-path
// escape attempt. POSIX/GNU-style flags start with '-' ("-n", "-I/usr/include", "--flag=value").
// Windows-style flags start with '/' ("/c", "/k", "/v:on") -- the real disambiguator against an
// actual absolute-looking '/'-rooted path (e.g. "/etc/passwd", which DOES need rejecting) is that a
// flag has no FURTHER path separator after its leading '/': "/c" is a flag, "/etc/passwd" is not.
bool looks_like_flag(std::string const& s) {
    if (s.empty()) return false;
    if (s[0] == '-') return true;
    if (s[0] == '/') return s.find('/', 1) == std::string::npos && s.find('\\', 1) == std::string::npos;
    return false;
}

bool looks_like_absolute_or_drive_or_unc(std::string const& s) {
    if (s.size() >= 2 && s[1] == ':') return true;                 // "C:..." drive-letter form
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\') return true; // "\\server\share" UNC
    if (!s.empty() && s[0] == '\\') return true;                    // "\rooted" (drive-relative-root)
    return false;
}

std::string backslashes_to_forward(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

}  // namespace

result<void> validate_argv_path(std::wstring const& mount_root, std::string const& argv_entry) {
    if (!contains_path_separator(argv_entry)) {
        return {};  // bare filename -- can only land directly in the already-verified mount_root
    }
    if (looks_like_flag(argv_entry)) {
        return {};  // a CLI flag ("/c", "-n", "--flag=value"), not a filesystem path at all
    }
    if (looks_like_absolute_or_drive_or_unc(argv_entry)) {
        return std::unexpected(agentengine::error{
            failure_class::policy,
            "argument names an absolute/drive/UNC path, which cannot be confined to the worktree "
            "mount",
            "native_process.argv_path_absolute"});
    }

    auto segments = split_mount_path(backslashes_to_forward(argv_entry));
    if (!segments.has_value()) {
        return std::unexpected(agentengine::error{
            failure_class::policy,
            "argument is not a well-formed worktree-relative path: " + segments.error().message,
            "native_process.argv_path_malformed"});
    }
    if (segments->size() <= 1) {
        // A single segment with a (now-normalized) trailing/leading slash already failed
        // split_mount_path's own grammar above; reaching here with size() <= 1 means the original
        // string, after backslash normalization, was actually a bare filename in disguise (e.g. a
        // lone "foo" that happened to be routed through this branch) -- accept, same reasoning as
        // the no-separator case.
        return {};
    }

    std::string parent_guest_path;
    for (std::size_t i = 0; i + 1 < segments->size(); ++i) {
        if (i != 0) parent_guest_path += "/";
        parent_guest_path += (*segments)[i];
    }

    auto parent_handle = open_within_mount_root(mount_root, parent_guest_path, FILE_LIST_DIRECTORY,
                                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (!parent_handle.has_value()) {
        return std::unexpected(agentengine::error{
            failure_class::policy,
            "argument's containing directory does not already exist inside the worktree mount "
            "(intermediate directories are not created on the caller's behalf -- "
            "decisions/ADR-071-native-unsandboxed-process-execution-providers.md's named scope "
            "limit): " + parent_handle.error().message,
            "native_process.argv_parent_not_contained"});
    }
    return {};
}

}  // namespace agentengine::native_process
