#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §5's OS-level path-escape corpus requirement
// (021-Platform-Support-and-Portability.md §6 G3: "the path-escape corpus (`..`, symlinks,
// junctions, `\\?\`, ADS, case tricks, unicode normalization) fails to escape a mount on any
// platform") -- Milestone 3 Phase C2, decisions/ADR-014-worktree-mount-path-canonicalization.md.
//
// Scope, precisely: this is the primitive that turns a guest-relative path string into a real,
// already-verified-safe Win32 HANDLE rooted at a real OS directory (`mount_root`) -- the mechanism
// core/worktree.hpp's Phase C1 comment explicitly deferred ("the DIFFERENT, later mechanism that
// materializes a mount onto a real OS filesystem for a sandboxed process to see -- that mechanism
// doesn't exist yet"). It does NOT sync a content-addressed Tree onto `mount_root` (that
// materialization/sync mechanism is a Phase E dependency of PythonRunner/ShellRunner, not built
// here) and it does NOT itself decide read-vs-write policy (`cap::FsRead`/`cap::FsWrite`, already
// enforced by `mount_read`/`mount_write` one layer up, own that). What this file owns is narrower
// and sharper: given a real directory that some outer layer has already decided the guest may see,
// make it structurally impossible for a guest-relative path string to name anything outside it.
//
// Windows only (021 §2's platform-priority ordering, matching every prior milestone's Windows-
// first sequencing) -- linked into the existing `agentengine::worktree_store` CMake target
// alongside worktree_digest.cpp, same real-syscall, no-third-party-dependency posture.
//
// Relationship to `sandbox/filesystem_adapter.hpp`'s `FileSystemAdapter` (an M0-era interface
// stub, not yet implemented): a future real adapter -- Phase E's job, wired to `PythonRunner`/
// `ShellRunner` -- is the expected CALLER of `open_within_mount_root`, one layer up. This file does
// not implement that interface; it builds the one primitive an implementation of it cannot safely
// do without.

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"  // DirEntry -- list_within_mount_root (Milestone
                                                          // 3 Phase G2) reports in the SAME shape
                                                          // FileSystemAdapter::list_directory already
                                                          // uses, rather than inventing a second one.

#include <windows.h>

#include <string>
#include <vector>

namespace agentengine {

// RAII wrapper around a real, already-verified-safe Win32 file/directory HANDLE -- the object
// `open_within_mount_root` hands back. Callers perform ReadFile/WriteFile/SetEndOfFile/etc.
// directly on `.get()`. There is deliberately no "reopen this by path" accessor: reopening a
// verified-safe location by re-parsing a path string is exactly the operation this module exists
// to make unnecessary (see `redteam::naive_open_checked_path` below for what that operation costs).
// ae-naming-lint: allow SafeFileHandle — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class SafeFileHandle {
public:
    SafeFileHandle() = default;
    explicit SafeFileHandle(HANDLE h) : handle_(h) {}
    ~SafeFileHandle() { reset(); }
    SafeFileHandle(SafeFileHandle const&) = delete;
    SafeFileHandle& operator=(SafeFileHandle const&) = delete;
    SafeFileHandle(SafeFileHandle&& other) noexcept : handle_(other.release()) {}
    SafeFileHandle& operator=(SafeFileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        HANDLE h = handle_;
        handle_  = nullptr;
        return h;
    }
    void reset() {
        if (valid()) CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

// Opens `guest_path` (mount-relative; same segment grammar `split_mount_path` in core/worktree.hpp
// already defines and enforces -- no leading/trailing `/`, no `//`, no `.`/`..` segment, non-empty)
// for real OS I/O against `mount_root`, an absolute Windows directory path. The returned handle is
// guaranteed, BY CONSTRUCTION rather than by a separate check that could go stale, to refer to a
// location still inside `mount_root` at the moment the handle was produced -- regardless of `..`,
// absolute redirects, symlinks/junctions/reparse points, ADS, `\\?\` prefixes, or unicode-
// normalization tricks anywhere in `guest_path` or already sitting on disk. See ADR-014 for the
// design rationale, the rejected alternative, and the red-team corpus this defeats.
//
// Mechanism, in one sentence: a single `CreateFileW` call lets Windows itself resolve whatever
// `guest_path` actually names (following any reparse points transparently, the same way any
// ordinary Windows path resolution does), and only THEN is the resulting HANDLE's real,
// filesystem-resolved location (`GetFinalPathNameByHandleW`) checked against the mount root -- so
// there is no window between "checked" and "used" for anything this function itself does: the
// object verified is the object returned, not a path string re-parsed afterward.
//
// `desired_access`/`creation_disposition`/`flags_and_attributes` pass straight through to the
// underlying `CreateFileW` once the containment property is established; this function does not
// interpret read-vs-write policy.
[[nodiscard]] result<SafeFileHandle> open_within_mount_root(std::wstring const& mount_root,
                                                              std::string const& guest_path,
                                                              DWORD desired_access,
                                                              DWORD creation_disposition,
                                                              DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL);

// Milestone 3 Phase G2 (026 §5's `agent.files` "listing" claim). Lists the immediate children of
// `guest_path` (mount-relative, same grammar as `open_within_mount_root` -- OR the empty string,
// meaning the mount root itself, which `open_within_mount_root` deliberately refuses since a root is
// never "a file"; listing the root is an ordinary, legitimate request this function must not reject
// the same way). Uses the IDENTICAL containment mechanism as `open_within_mount_root` -- one
// `CreateFileW` with `FILE_FLAG_BACKUP_SEMANTICS` (the documented way to open a directory HANDLE),
// then `GetFinalPathNameByHandleW`-verified against the root -- so a reparse point/junction/8.3-alias
// escape is caught here exactly the way it already is for a file open, not by a second, independently
// -reasoned check. Enumeration itself (`FindFirstFileW`/`FindNextFileW`) then runs against that
// ALREADY-VERIFIED canonical path, never the raw guest-derived one, preserving "the object verified is
// the object used" for the enumeration step too, not just the containment check.
[[nodiscard]] result<std::vector<DirEntry>> list_within_mount_root(std::wstring const& mount_root,
                                                                     std::string const& guest_path);

// Milestone 3 Phase G4 (026 §3's "Quota exhausted" row) -- live, on-disk usage of everything under a
// real, host-owned mount root, recursively. Deliberately NOT a guest-path-taking function like the
// two above: `mount_root` here is the whole materialized mount directory, always host-config, never
// guest input, so there is no containment property to prove -- what this walks is exactly what a
// future `harvest_mount` pass would also walk. Reparse points are never followed while recursing
// (their nominal directory entry is skipped, not counted or descended into) purely to keep this scan
// non-cyclic and bounded against a junction a prior write could have planted somewhere inside the
// mount -- an availability/correctness precaution for a usage COUNTER, not a second ADR-014-grade
// security boundary.
// `MountUsage` itself now lives in `sandbox/filesystem_adapter.hpp` (2026-08-14, gap-12 fix) — this
// header already includes it (above, for `DirEntry`), and the struct is plain data with no
// Windows-specific content, so `FileSystemAdapter::usage()` (the portable seam) and this
// Windows-only scan share one definition rather than two structurally-identical ones.
[[nodiscard]] result<MountUsage> mount_root_usage(std::wstring const& mount_root);

// TEST-ONLY, deliberately vulnerable control -- this project's established pattern for proving a
// containment check is a real gate rather than one that cannot fail (022 §5; the precedent is
// `redteam`-style naive_last_writer_wins_merge in tests/test_worktree_branch_concurrency.cpp).
// NEVER call this outside a test binary. It is declared here, once, rather than duplicated inside
// the test file, so ADR-014's evidence and any future regression both call the identical
// vulnerable implementation the ADR's red-team section describes.
namespace redteam {

// Design A, step 1: canonicalize `mount_root + "/" + guest_path` lexically (`GetFullPathNameW`,
// string manipulation only, no handle) and verify the result is string-prefixed by `mount_root`.
// Returns the canonical path STRING on success -- this is the bug's load-bearing detail: the
// result of the check is a string, not a handle, so nothing ties the check to what is later opened.
[[nodiscard]] result<std::wstring> naive_check_within_root(std::wstring const& mount_root,
                                                             std::string const& guest_path);

// Design A, step 2: open a PREVIOUSLY-CHECKED canonical path string by re-parsing it as a fresh
// `CreateFileW` call. Whatever the filesystem looks like at the moment THIS call runs -- not at
// the moment `naive_check_within_root` ran -- determines what gets opened. Nothing in this
// function re-verifies containment; it trusts the string it was handed.
[[nodiscard]] result<SafeFileHandle> naive_open_checked_path(std::wstring const& canonical_path,
                                                               DWORD desired_access,
                                                               DWORD creation_disposition);

} // namespace redteam

} // namespace agentengine
