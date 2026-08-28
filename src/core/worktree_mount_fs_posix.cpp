// Implements core/worktree_mount_fs_posix.hpp -- see that header for scope. This file is where
// ADR-014's two competing designs live for Linux, mirroring src/core/worktree_mount_fs.cpp's
// Windows implementation function-for-function: `open_within_mount_root` (accepted, Design B) and
// `redteam::naive_check_within_root`/`naive_open_checked_path` (rejected, Design A, kept only as a
// red-team control -- see the header's warning not to call these outside a test binary).

#include "agentengine/core/worktree_mount_fs_posix.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "agentengine/core/worktree.hpp"  // split_mount_path -- reused, not duplicated

namespace agentengine {

void SafeFileHandlePosix::reset() {
    if (valid()) ::close(fd_);
    fd_ = -1;
}

namespace {

// `native_code = err` (2026-08-28, ADR-103, the SandboxToolProvider Linux-parity pass): mirrors
// `worktree_mount_fs.cpp`'s own Windows `win_error()`, which already sets `native_code` so a
// caller (there, `mediated_python_runner.cpp`'s `raise_os_error`; here,
// `MediatedFileSystemAdapter`'s POSIX implementation) can classify a failure (e.g. "not found" via
// `ENOENT`) without string-matching `message`. This field did not exist here before -- the ORIGINAL
// (ADR-014, Judged) version of this function never needed it, since nothing yet called through it
// wanting anything more than pass/fail. Purely additive: every existing caller that only checked
// `has_value()`/`.error().message` is unaffected.
result<void> posix_error(char const* what, int err) {
    return std::unexpected(error{failure_class::fatal,
                                  std::string(what) + " failed: " + std::strerror(err),
                                  "worktree.mount_fs_posix_failure", err});
}

template <class T>
result<T> posix_error_t(char const* what, int err) {
    return std::unexpected(posix_error(what, err).error());
}

// The only real smuggling surface a single POSIX path COMPONENT has: an embedded NUL truncates
// whatever C API reads the string next (`std::string` is not itself NUL-terminated internally, so
// a caller-supplied guest_path segment could carry one). Unlike Windows, `\` and `:` are ordinary,
// legal filename characters here -- no drive-letter or Alternate-Data-Stream concept exists on a
// POSIX filesystem for either to smuggle, so restricting them here (as the Windows sibling does)
// would reject legitimate filenames for no real containment benefit.
result<void> validate_fs_segment(std::string const& seg) {
    if (seg.find('\0') != std::string::npos) {
        return std::unexpected(error{failure_class::policy,
                                      "path segment contains a character not permitted in a mounted path",
                                      "worktree.mount_path_forbidden_character"});
    }
    return {};
}

std::string strip_trailing_sep(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string join_segments(std::string const& root, std::vector<std::string> const& segments) {
    std::string joined = strip_trailing_sep(root);
    for (auto const& seg : segments) {
        joined.push_back('/');
        joined += seg;
    }
    return joined;
}

// `candidate` is considered within `root` iff it equals `root` exactly (the root itself) or begins
// with `root` followed immediately by `/` -- a plain string-prefix check would treat `/mount2` as
// inside `/mount`, which is exactly the class of bug this comparison exists to avoid (the identical
// reasoning as the Windows sibling's `is_within_root`). Case-SENSITIVE, unlike the Windows version:
// this compares real, kernel-resolved paths from a Linux filesystem, which is case-sensitive by
// default (ext4 et al.) -- matching each platform's own real semantics is what "isolation parity is
// a gate, not a goal" (CONVENTIONS.md) actually asks for; the CONTRACT (never escapes) is identical
// on both, the comparison mechanics are not.
bool is_within_root(std::string const& root, std::string const& candidate) {
    if (candidate.size() < root.size()) return false;
    if (candidate.compare(0, root.size(), root) != 0) return false;
    if (candidate.size() == root.size()) return true;
    return candidate[root.size()] == '/';
}

result<std::vector<std::string>> validate_segments(std::string const& guest_path) {
    auto segments = split_mount_path(guest_path);
    if (!segments) return std::unexpected(segments.error());
    if (segments->empty()) {
        return std::unexpected(error{failure_class::contract, "path names the mount root itself, not a file",
                                      "worktree.mount_path_is_root"});
    }
    for (auto const& seg : *segments) {
        auto ok = validate_fs_segment(seg);
        if (!ok) return std::unexpected(ok.error());
    }
    return segments;
}

// Reads what descriptor `fd` actually, currently resolves to -- via the kernel's own record of the
// open file description (the `/proc/self/fd/N` magic symlink), not a re-derived string -- the POSIX
// counterpart to the Windows sibling's `GetFinalPathNameByHandleW`-based `final_path_name`. Stable
// for the descriptor's whole lifetime: renaming or removing what `fd` pointed at later does not
// change what this call reported for `fd` at the moment it was called, because the descriptor
// references the kernel's open-file object, not a path.
result<std::string> resolved_path_of_fd(int fd) {
    std::string const proc_path = "/proc/self/fd/" + std::to_string(fd);
    std::string buf(4096, '\0');
    ssize_t n = ::readlink(proc_path.c_str(), buf.data(), buf.size() - 1);
    if (n < 0) return posix_error_t<std::string>("readlink(/proc/self/fd)", errno);
    buf.resize(static_cast<std::size_t>(n));
    return buf;
}

} // namespace

result<SafeFileHandlePosix> open_within_mount_root(std::string const& mount_root, std::string const& guest_path,
                                                     int open_flags, mode_t create_mode) {
    auto segments = validate_segments(guest_path);
    if (!segments) return std::unexpected(segments.error());

    // Open the root once and ask the kernel (not a string transform) what it really is -- the
    // trusted baseline every candidate below is compared against.
    int root_fd = ::open(mount_root.c_str(), O_RDONLY | O_DIRECTORY);
    if (root_fd < 0) return posix_error_t<SafeFileHandlePosix>("open(mount_root)", errno);
    SafeFileHandlePosix root_handle(root_fd);
    auto root_canonical = resolved_path_of_fd(root_handle.get());
    if (!root_canonical) return std::unexpected(root_canonical.error());

    std::string joined = join_segments(mount_root, *segments);

    // ONE open call for the target. The kernel resolves any symlinks encountered while walking
    // `joined` transparently as part of ordinary path resolution, arriving at a real descriptor to
    // whatever the path actually names -- there is no separate "check" step here for anything to
    // race against; the resolution and the acquisition of the descriptor are the same operation.
    // No O_NOFOLLOW: an in-mount symlink must be followed, not blanket-denied (the same "an
    // identical in-mount junction is followed" property ADR-014's Windows corpus proved) -- what
    // matters is where the FINAL resolved location lands, checked below, not whether a symlink was
    // involved along the way.
    int target_fd = ::open(joined.c_str(), open_flags, create_mode);
    if (target_fd < 0) return posix_error_t<SafeFileHandlePosix>("open(target)", errno);
    SafeFileHandlePosix target(target_fd);

    // Verify what was ACTUALLY opened -- read from the descriptor, not re-derived from `joined` --
    // is still inside the root. A symlink crossing the boundary, wherever in the path it sat, is
    // caught here because this check runs against resolved reality.
    auto target_canonical = resolved_path_of_fd(target.get());
    if (!target_canonical) return std::unexpected(target_canonical.error());
    if (!is_within_root(*root_canonical, *target_canonical)) {
        // native_code = EACCES (2026-08-28, ADR-103): mirrors the Windows sibling's own
        // ERROR_ACCESS_DENIED sentinel for the identical situation -- a mount escape is a policy
        // denial, not a real OS-level lookup failure, so there is no genuine errno behind it; EACCES
        // is the closest real occurrence of "you may not reach this."
        return std::unexpected(error{failure_class::policy, "resolved path escapes the mount root",
                                      "worktree.mount_path_escapes_root", EACCES});
    }
    return target;
}

namespace redteam {

result<std::string> naive_check_within_root(std::string const& mount_root, std::string const& guest_path) {
    // Deliberately reuses the SAME segment validation as the accepted design so this control
    // isolates the one difference under test (string-check-then-reopen vs. descriptor-based
    // open-and-verify), matching the Windows control's own precedent exactly.
    auto segments = validate_segments(guest_path);
    if (!segments) return std::unexpected(segments.error());

    std::string joined = join_segments(mount_root, *segments);
    std::string root_lexical = strip_trailing_sep(mount_root);

    // THE BUG, structurally: this check never calls open()/readlink() at all -- it is pure string
    // manipulation, so a symlink sitting on disk under `mount_root` (already there, or swapped in
    // between this call and the later reopen) is completely invisible to it.
    if (!is_within_root(root_lexical, joined)) {
        return std::unexpected(error{failure_class::policy, "resolved path escapes the mount root (lexical check)",
                                      "worktree.mount_path_escapes_root"});
    }
    // THE BUG's consequence: the check's result is a STRING, handed back for the caller to re-open
    // later. Nothing ties this answer to what the filesystem looks like at the moment it is used.
    return joined;
}

result<SafeFileHandlePosix> naive_open_checked_path(std::string const& canonical_path, int open_flags,
                                                      mode_t create_mode) {
    // Re-parses `canonical_path` as a fresh open() call. Whatever the filesystem looks like RIGHT
    // NOW -- not when `naive_check_within_root` ran -- determines what this opens. No containment
    // re-check happens here; this function trusts the string it was handed.
    int fd = ::open(canonical_path.c_str(), open_flags, create_mode);
    if (fd < 0) return posix_error_t<SafeFileHandlePosix>("open", errno);
    return SafeFileHandlePosix(fd);
}

} // namespace redteam

} // namespace agentengine
