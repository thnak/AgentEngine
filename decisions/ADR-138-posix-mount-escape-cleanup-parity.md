# ADR-138 — POSIX `open_within_mount_root` gets the same creation-escape cleanup its Windows sibling already has

- **Status:** Proposed — implemented and **Linux-verified, ADR-143**: builds clean on real GCC
  14.2.0, the full `test_worktree_mount_fs_escape_corpus_linux` suite passes with zero regression, and
  a dedicated targeted probe confirms both the create-and-unwind fix (§7) and the deferred-truncate
  fix (§8) genuinely hold on real Linux. **Independent red-team round (§7) found and fixed one real,
  deterministic logic bug in the first draft; the Linux-verify pass itself (§8) found and fixed one
  further real, previously-undiscovered data-loss bug, independent of and predating both this ADR and
  its own red-team round.**
- **Date:** 2026-08-30/31.
- **Scope:** `src/core/worktree_mount_fs_posix.cpp` only (`open_within_mount_root()`).
- **Related specs:** `decisions/ADR-014-worktree-mount-fs-toctou.md` and its own addendum (the
  Windows-side fix this mirrors, `src/core/worktree_mount_fs.cpp`), `decisions/ADR-103-105-*` (this
  file's own Linux-parity lineage), `decisions/ADR-104-real-io-filesystem-linux-parity.md`.

## 1. The question

A final-review pass found that the same PR's Windows-side fix to `worktree_mount_fs.cpp` (unwinding a
just-created object through its own handle when a creating `CreateFileW` disposition plants something
outside `mount_root` before the containment check runs) was never ported to the POSIX sibling. POSIX
`open()` with `O_CREAT` (no `O_NOFOLLOW`) follows a symlink at the final path component and, if
dangling, creates a REAL file at the symlink's target — outside `mount_root` — before the post-open
containment check (`resolved_path_of_fd` + `is_within_root`) catches it and returns
`worktree.mount_path_escapes_root`; the old code never unwound the just-created file. Reachable
directly: `mediated_filesystem_adapter_posix.cpp:289-290`'s `write_file()` opens with `O_CREAT`
(without `O_EXCL`), and a dangling symlink pointing outside the mount can pre-exist in a checked-out
worktree without any shell access (git tracks symlinks).

## 2. Findings

Unlike Windows' `CreateFileW`, POSIX `open()` has no single-call way to report "did this call create a
new object." `O_EXCL` is the idiomatic way to get that signal atomically, but only for a call that
itself uses it — a caller here wants ordinary "create if missing, open if exists" semantics
(`O_CREAT` without `O_EXCL`), which needs a genuine two-step emulation. See §7 for the real correctness
gap this introduced in the first draft, and its fix.

## 3. What was built

When `open_flags` requests `O_CREAT` without the caller's own `O_EXCL`: probe first with `O_EXCL`
added. Success means this call created a new object — unwind-eligible. `EEXIST` means something already
sat at the final path component (regular file or symlink) — the code then falls back to a plain reopen
(original `open_flags`, no `O_EXCL`) to actually get the intended handle. A caller that already passed
`O_EXCL` itself keeps its own real create-or-fail semantics, unaffected. On the post-open containment
check finding an escape, `::unlink()` unwinds via the resolved, canonical path just verified — never a
re-parsed relative string — best-effort (a failed unlink still correctly rejects the call to the
caller).

## 4. Verification

Initially could not be compiled or executed on the Windows session that authored this fix
(`src/core/worktree_mount_fs_posix.cpp` is `if(NOT WIN32)` gated) — verified by hand at the time: every
branch of the `(O_CREAT present/absent) x (O_EXCL present/absent)` matrix traced for correct
`created_new_object` assignment; compared line-for-line against the Windows sibling's own equivalent
logic; the unrelated `redteam::naive_check_within_root`/`naive_open_checked_path` control functions
confirmed untouched and unaffected.

**Real compilation and execution verification completed in ADR-143**: full incremental build clean on
GCC 14.2.0 (zero errors); `test_worktree_mount_fs_escape_corpus_linux` (the existing, pre-ADR-138
corpus) passes completely, zero regression; a dedicated targeted probe (built and run, then deleted —
not part of the permanent suite) directly confirmed both (a) a dangling symlink inside the mount
pointing outside it, opened with `O_CREAT` and no caller `O_EXCL`, is rejected AND the newly-created
file at the escaped-to target is genuinely unlinked, not left behind, and (b) a symlink to an EXISTING
outside file is rejected WITHOUT deleting that pre-existing file — the exact two properties §7's
red-team round reasoned about but could not execute. See §8 for a third, real, previously-undiscovered
bug that same probe run surfaced and this ADR's own fix closed in the same pass.

## 5. Not done

- No attempt to close the disclosed, narrower TOCTOU residual named in-comment (a concurrent process
  racing the exact window between the O_EXCL probe and the plain reopen) — a materially different,
  stronger threat model than the one this mediation layer defends against today (model output as
  untrusted data, not a live concurrent attacker racing this exact syscall sequence).

## 6. Residuals

- The disclosed TOCTOU residual named in §5 — unchanged, not attempted.

## 7. Independent red-team round (same day, this session's own consolidated final-review pass)

**Static-only review (execution not possible on this platform for this file) found one real,
deterministic bug in the first draft — not merely a theoretical concurrent-race case.**

In the `O_CREAT` without caller-`O_EXCL` branch, the `EEXIST` fallback reopen (`open(joined.c_str(),
open_flags, create_mode)`, no `O_EXCL`) never set `created_new_object = true`, unconditionally. But
`EEXIST` from the probe only proves SOME directory entry already sits at the final path component — it
does not distinguish "a symlink to something that already exists" (the fallback reopen creates
nothing) from "a DANGLING symlink" (the fallback's own `O_CREAT` is exactly the call that brings a new
object into existence at the symlink's target, single-threaded, no race required). The first draft
left `created_new_object` false unconditionally for this branch, meaning the escape was still correctly
*rejected* to the caller, but the newly-planted file was silently left on disk outside the mount root —
precisely the residual this whole ADR exists to close, still open for exactly this one deterministic
path.

**Fixed**: before the fallback reopen, `stat()` (follows symlinks, unlike `lstat()`) the pre-resolution
`joined` path. Success means the target already exists (the fallback will only reopen it — must NOT be
treated as "created," or the escape-unwind would delete an arbitrary PRE-EXISTING file reached through
the symlink, a real, WORSE bug than the one being fixed — confirmed by hand-tracing the alternative
"always treat fallback as created" fix that was considered and rejected for exactly this reason).
`ENOENT` means the symlink is dangling and the fallback's own `O_CREAT` is about to create a new object
at its target — `created_new_object` is set accordingly. A narrow TOCTOU window between this `stat()`
and the fallback `open()` is a strictly narrower instance of this function's own already-disclosed
residual (§5/§6), not a new one.

This fix is included in the diff described in §3 above (i.e., §3 already describes the corrected,
post-red-team logic). Confirmed by real execution in ADR-143 — see §4.

## 8. A real, previously-undiscovered bug found by this fix's OWN Linux-verify pass (ADR-143)

While building the targeted probe named in §4, its second scenario (a symlink to an EXISTING outside
file, confirming the §7 fix does not over-delete) surfaced a real, separate, previously-undiscovered
data-loss bug: `O_TRUNC` is destructive at `open()`-time — the kernel truncates an existing target as
an unconditional side effect of a successful `open()` call, before this function's own containment
check ever runs. A symlink inside the mount pointing to an EXISTING file outside it (the non-dangling
counterpart of the create-and-plant case §7 closes) had its real content silently wiped to zero bytes
even though the call was ultimately, correctly rejected as an escape. Reachable through the identical
`write_file()` path §1 already names, since `append=false` is this codebase's own default write mode
(`O_WRONLY | O_CREAT | O_TRUNC`).

**This predates ADR-138 entirely** — the original, pre-existing, single-`open()` design (before this
ADR's own fix existed at all) had the identical exposure, since `open_flags` (including any caller
`O_TRUNC`) was always passed straight through to the one real `open()` call. Not introduced by this
ADR's own create-and-unwind fix — found only because this fix's own verification probe happened to
test the non-dangling, EXISTING-target case for a different reason (confirming no over-deletion) and
incidentally exercised the truncation path too.

**Fixed in the same pass, not merely disclosed**: `O_TRUNC` is now stripped from `open_flags` before
every `open()` call in this function (`effective_open_flags = open_flags & ~O_TRUNC`) and applied via a
real `ftruncate()` on the resulting descriptor ONLY after the containment check confirms the target is
genuinely inside `mount_root` — deferring the destructive effect past the point where it is known to be
safe, rather than letting the kernel perform it speculatively before any application-level check can
run. Behavior-preserving for every legitimate in-mount caller: `open()` without `O_TRUNC` still
positions the file offset at 0 (POSIX guarantees this for non-`O_APPEND` opens regardless of
truncation), and a subsequent `ftruncate(fd, 0)` before any write reaches an identical end state.
Verified via the probe's own updated assertion (the pre-existing outside file's content, not merely its
existence, is confirmed untouched after a rejected escape attempt) — failed before this fix (content
wiped to empty despite the file surviving), passed after. Full `test_worktree_mount_fs_escape_corpus_
linux` and the whole Linux `ctest` suite re-run clean after this fix, zero regression (see ADR-143).
