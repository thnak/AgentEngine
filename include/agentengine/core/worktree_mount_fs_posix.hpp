#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §5's OS-level path-escape corpus requirement
// (021-Platform-Support-and-Portability.md §6 G3), Linux half -- Milestone 3 Phase C4, the parity
// pass over `worktree_mount_fs.hpp`'s Windows-only primitive (Phase C2,
// decisions/ADR-014-worktree-mount-path-canonicalization.md, Judged). Ordinary task, not its own
// ADR: this carries forward ADR-014's already-Judged design finding (open, then verify from the
// object actually opened, never from a re-parsed string) rather than re-litigating it -- the same
// treatment the M2 breakdown gave `LinuxNativeJailBackend` relative to ADR-004's Windows-only
// findings (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision 6's own
// framing: a fresh ADR is for a *first* design pass with nothing to carry forward; this has ADR-014
// to carry forward).
//
// Same scope boundary as the Windows header: this is the primitive that turns a guest-relative path
// string into a real, already-verified-safe POSIX file descriptor rooted at a real OS directory
// (`mount_root`). It does NOT sync a content-addressed Tree onto `mount_root` and does NOT decide
// read-vs-write policy -- both remain one layer up, same as on Windows.
//
// A DELIBERATE signature divergence from the Windows header, not an oversight: Win32's
// `CreateFileW` splits "how to open" into `desired_access` + `creation_disposition`; POSIX `open()`
// folds both into one `flags` bitmask (`O_RDONLY`/`O_WRONLY`/`O_CREAT`/`O_TRUNC`/...). Matching each
// OS's own idiomatic API shape here is what 025 §5 and CONVENTIONS' "isolation parity is a gate, not
// identical shape" actually ask for -- the CONTRACT (a guest-relative path can never resolve outside
// `mount_root`, by construction) must match; the parameter list naming how to open the result need
// not, and forcing a fake DWORD-shaped POSIX API would be the wrong kind of parity.
//
// Linux only -- linked into the existing `agentengine::worktree_store` CMake target's `NOT WIN32`
// branch, mirroring how the Windows branch links `worktree_mount_fs.cpp` alongside
// `worktree_digest.cpp`. Unlike the Windows branch, no digest provider is linked here yet (025 §2's
// Linux SHA-256 gap, decision 2, is a separate, still-open, tracked item -- nothing in this file or
// its test needs `compute_digest`/the content-addressed store at all, so it is not blocked on it).

#include "agentengine/core/error.hpp"

#include <sys/types.h>

#include <string>

namespace agentengine {

// RAII wrapper around a real, already-verified-safe POSIX file descriptor -- the POSIX counterpart
// to `SafeFileHandle` in worktree_mount_fs.hpp. Same deliberate omission: no "reopen this by path"
// accessor, for the identical reason (see that header's comment on the Windows type).
class SafeFileHandlePosix {
public:
    SafeFileHandlePosix() = default;
    explicit SafeFileHandlePosix(int fd) : fd_(fd) {}
    ~SafeFileHandlePosix() { reset(); }
    SafeFileHandlePosix(SafeFileHandlePosix const&) = delete;
    SafeFileHandlePosix& operator=(SafeFileHandlePosix const&) = delete;
    SafeFileHandlePosix(SafeFileHandlePosix&& other) noexcept : fd_(other.release()) {}
    SafeFileHandlePosix& operator=(SafeFileHandlePosix&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    int release() {
        int fd = fd_;
        fd_    = -1;
        return fd;
    }
    void reset();  // defined in the .cpp -- calls ::close, kept out-of-line so the header stays
                    // syscall-free (matches the rest of this header's shape)

private:
    int fd_ = -1;
};

// Opens `guest_path` (mount-relative; the SAME segment grammar `split_mount_path` in
// core/worktree.hpp already defines and enforces) for real OS I/O against `mount_root`, an absolute
// Linux directory path. The returned descriptor is guaranteed, BY CONSTRUCTION rather than by a
// separate check that could go stale, to refer to a location still inside `mount_root` at the
// moment the descriptor was produced -- regardless of symlinks anywhere in `guest_path` or already
// sitting on disk (POSIX has no `..`/absolute-redirect/ADS/`\\?\`/8.3-alias equivalents to worry
// about beyond what `split_mount_path` already structurally rejects -- see ADR-014's residuals list
// and this task's own test file for exactly which Windows corpus items carry over and which are
// N/A on this platform, named rather than silently skipped).
//
// Mechanism, in one sentence, mirroring ADR-014's accepted Design B exactly: a single `open()` call
// lets the kernel itself resolve whatever `guest_path` actually names (following any symlinks
// encountered while walking it, the same way any ordinary POSIX path resolution does), and only
// THEN is the resulting descriptor's real, kernel-resolved location (`readlink("/proc/self/fd/N")`)
// checked against the mount root -- so there is no window between "checked" and "used" for anything
// this function itself does: the object verified is the object returned, not a path string
// re-parsed afterward.
//
// `open_flags`/`create_mode` pass straight through to the underlying `open()` once the containment
// property is established; this function does not interpret read-vs-write policy.
[[nodiscard]] result<SafeFileHandlePosix> open_within_mount_root(std::string const& mount_root,
                                                                    std::string const& guest_path,
                                                                    int open_flags, mode_t create_mode = 0600);

// TEST-ONLY, deliberately vulnerable control -- the POSIX counterpart to
// `worktree_mount_fs.hpp`'s `redteam::naive_check_within_root`/`naive_open_checked_path`, proving
// the identical class of bug (a check whose result is a string, disconnected from what is later
// opened) is real on this platform too, not merely assumed to generalize. NEVER call outside a test
// binary.
namespace redteam {

// Design A, step 1: canonicalize `mount_root + "/" + guest_path` LEXICALLY -- string concatenation
// only, no syscall, no symlink resolution -- and verify the result is string-prefixed by
// `mount_root`. Returns the canonical path STRING on success -- the bug's load-bearing detail: the
// result of the check is a string, not a descriptor, so nothing ties the check to what is later
// opened. (There is no `.`/`..` to lexically collapse here the way Windows' `GetFullPathNameW`
// would -- `split_mount_path` already rejects those segments upstream on both platforms -- so this
// is a plain join, not a normalizer; kept as its own named step to mirror the Windows control's
// shape and make the parallel legible.)
[[nodiscard]] result<std::string> naive_check_within_root(std::string const& mount_root,
                                                             std::string const& guest_path);

// Design A, step 2: open a PREVIOUSLY-CHECKED canonical path string by re-parsing it as a fresh
// `open()` call. Whatever the filesystem looks like at the moment THIS call runs -- not at the
// moment `naive_check_within_root` ran -- determines what gets opened. Nothing in this function
// re-verifies containment; it trusts the string it was handed.
[[nodiscard]] result<SafeFileHandlePosix> naive_open_checked_path(std::string const& canonical_path,
                                                                     int open_flags,
                                                                     mode_t create_mode = 0600);

} // namespace redteam

} // namespace agentengine
