#pragma once
// Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md's worktree-
// confinement half. Worktree confinement stays mandatory for NativeShellProvider/NativeBashProvider/
// NativePythonProvider/NativeNodeProvider even though the OS sandbox jail is optional (explicit
// project-owner instruction). This file does NOT re-implement worktree materialization -- that is
// already real, tested code: src/backends/native_jail/worktree_mount_sync.hpp's
// materialize_mount()/harvest_mount(), built on core/worktree_mount_fs.hpp's `open_within_mount_root`
// (ADR-014, handle-anchored, TOCTOU-safe FOR ENGINE-MEDIATED OPENS). A caller (the provider layer)
// materializes a mount via the existing machinery and hands this file the resulting real host
// directory; this file owns exactly two things: (1) validating a path-shaped argv entry before it
// ever reaches a spawned child's command line, and (2) nothing else -- cwd assignment itself is
// native_process_spawn.hpp's `NativeExecRequest::cwd` field, set directly by the caller to the
// materialized directory.
//
// HONEST SCOPE LIMIT (named here, and in ADR-071's red-team section, not silently left implicit):
// `validate_argv_path` proves containment AT THE MOMENT OF THE CHECK, using the same handle-
// anchored primitive ADR-014 relies on for the DIRECTORY CHAIN LEADING TO the argument -- but the
// argument itself is handed to the child as a PATH STRING (real, unsandboxed programs take path
// arguments, not handles), and the child's own subsequent raw syscall reopens that string. There is
// therefore a genuine TOCTOU window this function cannot close (unlike `open_within_mount_root`'s
// own callers, which use the verified HANDLE directly, never a re-derived path) -- a symlink/
// junction swapped into the verified parent directory between this check and the child's own access
// could still redirect it. This is the accepted residual ADR-071 names explicitly (matching this
// project's disclosure norm for ADR-041's Windows ACE-leak residual): cwd confinement + argv
// pre-validation meaningfully narrows the attack surface without a jail, it does not eliminate it.

#include <string>

#include "agentengine/core/error.hpp"

namespace agentengine::native_process {

// Validates `argv_entry` before it is placed on a spawned child's command line:
//   - An entry with no path separator at all ('/' or '\\') is always accepted without a filesystem
//     check -- it can only ever name something directly inside the already-verified `mount_root`
//     itself (the child's cwd), so there is nothing further to escape through.
//   - An entry that LOOKS LIKE A CLI FLAG, not a path, is passed through unvalidated: POSIX/GNU-
//     style ("-n", "-I/usr/include", "--flag=value") or Windows-style ("/c", "/k", "/v:on") -- a
//     flag is not filesystem-authority-bearing, and misreading one as a path is a real false-
//     positive this validator must not produce (an argument this codebase's own native providers
//     hand cmd.exe/bash.exe every ordinary call, e.g. "/c", would otherwise be rejected outright).
//     Disambiguated from a genuine '/'-rooted absolute path (which DOES still get rejected below) by
//     whether a FURTHER separator follows the leading one: "/c" has none, "/etc/passwd" does.
//   - An entry naming a drive letter (":") or a UNC prefix ("\\\\...") is always rejected -- these
//     are absolute-path forms `core/worktree.hpp`'s `split_mount_path` grammar was not designed to
//     recognize, and an absolute path is definitionally not confined to the mount.
//   - Any other multi-segment entry must (a) pass `split_mount_path`'s grammar (no leading '/', no
//     '.'/'..' segment, no empty segment) and (b) have its CONTAINING DIRECTORY already exist and
//     resolve inside `mount_root`, verified via `open_within_mount_root` -- a not-yet-existing
//     intermediate directory is REJECTED outright (named scope limit above), not created on the
//     caller's behalf.
[[nodiscard]] result<void> validate_argv_path(std::wstring const& mount_root, std::string const& argv_entry);

}  // namespace agentengine::native_process
