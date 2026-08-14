# ADR-040 — Fixing the `FsRead`/`FsWrite` quota-capability gate (false denial + silent bypass)

**Status:** Judged (2026-08-14, project owner sign-off). Designed, red-teamed (self-authored, see §3),
implemented, and proven (real code + a new regression block, §5).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #12 (the finding this
ADR closes) and gap #3's deferred note (the symmetric `FsRead` bug, folded into this ADR's scope
rather than left for a second pass, see §2). Reuses, rather than reinvents, the mechanism Milestone 3
Phase G4 already established for `mediated_python_runner.cpp`'s write path (`CapabilitySet::
find_fs_write`, `capability.hpp:594-604`) — this ADR is that same fix applied to the places it was
missing, not a new design.

## 1. The question

**Stated so it has a wrong answer:** does every `cap::FsRead`/`cap::FsWrite` capability check in
`mediated_shell_dispatch.cpp` and `mediated_python_runner.cpp` correctly (a) accept a quota-capped
grant that has real headroom, and (b) deny a write once real, on-disk usage already exceeds the
grant's own quota, for every code path that claims to enforce §007's `size_cap_bytes`/`quota_bytes`/
`file_count_cap` axes?

Before this fix: **no, on both counts**, in different call sites.

## 2. What was actually wrong, re-verified against current code (not the 4-day-stale audit)

The 2026-08-10 gap audit's row #12 named "ShellRunner write builtins unconditionally deny writes
under any quota-capped FsWrite grant." Re-grounding against current code (post-ADR-037, which moved
large parts of the tree the audit never re-checked) found the bug real but more precisely located,
and a second bug the audit's own deferred notes named but didn't scope in:

1. **False denial (the audit's own finding, confirmed).** Every `cap::FsRead`/`cap::FsWrite` check
   in `mediated_shell_dispatch.cpp`'s `require_capability()` call sites (`ls`, `cat`, `mkdir`, `rm`,
   `mv`/`cp`, output redirects) built a synthetic `requested` capability with `std::nullopt` for
   `size_cap_bytes`/`quota_bytes`/`file_count_cap` and checked it via `CapabilitySet::contains()`.
   `capability.hpp`'s `cap_covers()` treats a capped parent against an uncapped (`nullopt`) *request*
   as a widening attempt and rejects it (`cap_covers`'s own header comment: "parent capped, request
   claims uncapped -- widening"). So a quota-capped grant was unconditionally unusable from the
   shell, regardless of actual usage — the identical bug class Milestone 3 Phase G4 already found and
   fixed once for `mediated_python_runner.cpp`'s Python `open()`/write bridge
   (`capability.hpp:579-593`'s own comment documents that earlier fix). The shell dispatcher was never
   updated to match.
2. **The identical false denial also existed on the FsRead side**, in both files — not scoped by the
   original audit's row #12 (its own deferred section named this as a *separate*, out-of-scope
   follow-up: "a symmetric FsRead/size_cap_bytes capped-grant gate bug... was not in scope for this
   pass"). Confirmed live in `mediated_shell_dispatch.cpp`'s `ls`/`cat`/`mv`/`cp`-read-half, and in
   `mediated_python_runner.cpp`'s `Internal_open` read branch and `Internal_listdir` — both still
   using `contains()` against a synthetic uncapped `cap::FsRead` request. Folded into this ADR's scope
   rather than deferred again, since it is the exact same fix applied to the read-side twin of the
   already-in-scope write-side bug, not new design work.
3. **Silent bypass (the audit's other stated concern, confirmed).** `MediatedFileSystemAdapter::
   write_file` (`src/backends/native_jail/mediated_filesystem_adapter.cpp`) is a direct Win32
   `WriteFile` call with no `CapabilitySet`/usage awareness at all. So the only grants that were
   actually *usable* before this fix (uncapped ones) enforced no live quota whatsoever — an uncapped
   grant and a "quota exists but is silently never checked" grant were indistinguishable in practice.

The audit's own recommended approach named the correct constraint: **"the gate fix and live usage
enforcement together"** — fixing only the false denial without also wiring live enforcement would
have converted a safe-but-annoying bug (a capped grant is unusable) into a silent quota bypass (a
capped grant becomes usable but nothing checks it), the exact "loud failure becomes silent" pattern
CLAUDE.md and this audit both flag as the thing to watch for.

**One part of the original audit did not survive re-verification and is corrected here rather than
carried forward:** the audit named "ShellRunner write builtins" broadly; the false-denial bug is
absent from the untouched `shell_dispatch.cpp`/`RealFileSystemAdapter` spike (`ADR-001`'s deliberately
kind-only `contains_kind()` path, which never claims to enforce quota at all) and present only in
`mediated_shell_dispatch.cpp`. This ADR names the file precisely to avoid the same ambiguity recurring.

## 3. The fix, and what red-teaming it changed

**Mechanism reused, not reinvented**: `CapabilitySet::find_fs_write(mount_id, path)` is a pure
lookup — "what's the grant's own ceiling for this mount/path", not `contains()`'s "is a synthetic
ceiling covered" question (`capability.hpp:579-593`'s own comment explains why this is a genuinely
different question from attenuation). This ADR adds its read-side twin, `find_fs_read()`, and applies
both wherever the false-denial bug was found.

**Live enforcement**, only where it was missing: `FileSystemAdapter` (the portable seam
`mediated_shell_dispatch.cpp` and any future backend depend on) gets one new, deliberately **non-pure**
virtual method, `usage() -> result<std::optional<MountUsage>>`, defaulting to "unavailable"
(`std::nullopt`, wrapped success) so `RealFileSystemAdapter` — which never claims to enforce quota —
needs no stub override. `MediatedFileSystemAdapter::usage()` overrides it to forward to
`mount_root_usage()` (the same Windows on-disk recursive scan `Internal_open`'s write branch already
uses), so the shell's live-quota check shares the exact scanning logic rather than a second,
independently-reasoned copy.

Design points a self-red-team pass changed before implementation, each because the first-pass shape
would have reintroduced a version of the audit's own "safe becomes silent" failure pattern:

1. **`usage()` unavailable must fail closed, not skip the check.** A caller that finds a
   quota-capped grant but gets back `nullopt` from `usage()` must treat that as "cannot verify the cap
   holds," not as "no cap to enforce" — conflating the two would silently bypass quota the moment any
   future `FileSystemAdapter` implementation inherited the default. `require_fs_write()`'s
   implementation returns `shell.fs_quota_unavailable` (fail closed) in that case rather than
   proceeding.
2. **`rm` must never be quota-gated.** Deletion can only reduce usage; denying it because the mount is
   already over quota would be actively counterproductive (a caller could never work back under quota)
   and is not the "safe→silent" pattern — but it would still be a real, avoidable correctness bug, not
   a security one. `require_fs_write()` takes an explicit `enforce_quota` bool; `rm`'s call site passes
   `false`.
3. **`mv` and `cp` share one call site in the existing code but need different quota treatment.** A
   rename adds no new bytes or file-count entries to the mount; a copy does. The shared `mv`/`cp`
   destination-write gate now passes `enforce_quota = (name == "cp")`, proven by a regression pair
   (§5, E3-Q4) that exercises both under the identical exhausted grant.
4. **`mount_root_usage()`'s full recursive rescan cost is inherited, not newly introduced** — the
   same cost the already-proven `Internal_open` write-mode check already pays on every quota-checked
   open. This fix adds more call sites paying that cost (`mkdir`, `cp`, output redirects), bounded to
   only grants that actually carry `quota_bytes`/`file_count_cap` (an uncapped grant pays nothing).
   Named honestly here rather than silently accepted: a mount with a very large file count under a
   quota-capped grant will rescan on every quota-checked write. Not fixed in this pass — the existing,
   already-Judged precedent has the identical characteristic, and changing that scanning strategy is a
   separate, larger design question this ADR does not reopen.
5. **`find_fs_write`/`find_fs_read`'s "first matching grant wins" behavior is inherited, not new.**
   Both are a linear scan returning the first capability in `granted_` whose `mount_id`/`path_prefix`
   covers the request. If a `CapabilitySet` ever held two overlapping `FsWrite` grants for the same
   mount with different quotas (e.g. a broad uncapped grant and a narrower capped one for a
   subdirectory), which one governs depends on grant order, not specificity. This is not a new
   regression — `find_fs_write` already had this shape when Judged at Milestone 3 Phase G4, and this
   ADR's `find_fs_read` deliberately matches it for consistency. **Named as a residual, not solved
   here**: today's real capability grants are single, flat `FsWrite`/`FsRead` entries per mount (one
   per workflow's declared `capability_ceiling`), so the ambiguous case doesn't arise in practice yet;
   if a caller ever needs genuinely overlapping grants with different quotas, this needs its own,
   separate design pass before that caller ships.

## 4. What changed

- `include/agentengine/trust/capability.hpp`: added `CapabilitySet::find_fs_read()`, the read-side
  twin of the already-Judged `find_fs_write()`.
- `include/agentengine/sandbox/filesystem_adapter.hpp`: `MountUsage` moved here from
  `core/worktree_mount_fs.hpp` (plain data, no reason to live in a Windows-only header); added the
  non-pure `FileSystemAdapter::usage()` virtual, defaulting to "unavailable."
- `include/agentengine/core/worktree_mount_fs.hpp`: `MountUsage` definition removed (now inherited
  from `filesystem_adapter.hpp`, already included there); `mount_root_usage()`'s signature unchanged.
- `src/backends/native_jail/mediated_filesystem_adapter.{hpp,cpp}`: `usage()` override, forwarding to
  `mount_root_usage(root_)`.
- `src/backends/native_jail/mediated_shell_dispatch.cpp`: two new helpers, `require_fs_read()` and
  `require_fs_write()` (the latter taking an `enforce_quota` bool), replacing the buggy
  `require_capability()`-based checks at every `FsRead`/`FsWrite` call site (`ls`, `cat`, `mkdir`,
  `rm`, `mv`/`cp`, output redirects). `require_capability()` itself is unchanged and still correctly
  used for `EnvWrite`/`RunnerCall` (no quota field, so it was never buggy for those kinds).
- `src/backends/native_jail/mediated_python_runner.cpp`: `Internal_open`'s read branch and
  `Internal_listdir` switched from `contains()` to `find_fs_read()` — the read-side mirror of the
  already-Judged Phase G4 write-side fix, same file.

## 5. Evidence

New regression block in `tests/test_mediated_shell_runner_smoke.cpp` (E3-Q0 through E3-Q5), against a
fresh mount with a known byte-exact baseline (one 10-byte seeded file), run under this project's real
`MediatedShellRunner`/`MediatedFileSystemAdapter`/`CapabilitySet` — not a mock:

- **E3-Q1** (the false-denial half): a `quota_bytes`-capped `FsWrite` grant with real headroom is
  usable at all — before the fix this was unconditionally denied regardless of usage.
- **E3-Q2** (the silent-bypass half): a grant whose `quota_bytes` is already exceeded by real,
  on-disk usage denies a further quota-checked write (`mkdir`). Confirmed the operation never reached
  the filesystem. A quota denial is not in `kHardStopCodes`, so — consistent with
  `mediated_python_runner.cpp`'s identical condition surfacing as an ordinary, catchable `OSError`,
  not a hard denial — it surfaces as an inspectable, non-ok `ExecOutcome`
  (`klass == policy_violation`, `stderr_text == "No space left on device"`), the same shape a missing-
  file `cat` already gets, not a propagated `std::unexpected`. (This is the one place the test's first
  draft was itself wrong — it assumed the wrong failure shape until a real run showed the actual,
  correct-per-precedent behavior.)
- **E3-Q3**: `rm` succeeds under the same exhausted grant (never quota-gated).
- **E3-Q4**: `mv` succeeds under the exhausted grant (rename adds no usage); `cp` is denied under the
  identical grant (copy does add usage) — the `enforce_quota = (name == "cp")` distinction proven
  directly, not merely asserted.
- **E3-Q5**: the `FsRead` false-denial fix — a `size_cap_bytes`-capped `FsRead` grant is usable
  (`cat` succeeds).

Full suite: 175/175 tests pass (`ctest`, this pass), zero regressions.

**Not locally verified this pass, named honestly rather than silently claimed**: the
`mediated_python_runner.cpp` fix (`Internal_open`'s read branch, `Internal_listdir`) is a mechanical,
exact pattern match against the same file's already-Judged, already-proven write-side fix (identical
`find_fs_*` substitution, no new logic) — but this environment builds with
`AGENTENGINE_BUILD_PYTHON_RUNNER=OFF` (no embedded-CPython dev install configured here), so it was not
locally build- or test-verified this session. A future session with that flag enabled should add the
read-side twin of `test_mediated_python_runner_error_mapping.cpp`'s existing Row 2 write-quota tests
before this residual is considered closed.

## 6. What this does not claim

- Does not change `find_fs_write`/`find_fs_read`'s first-match-wins semantics (§3 item 5).
- Does not change `mount_root_usage()`'s full-rescan cost characteristic (§3 item 4).
- Does not touch `shell_dispatch.cpp`/`RealFileSystemAdapter` (ADR-001's deliberately kind-only spike)
  — it never claimed to enforce quota, so it is out of scope, not silently left broken.
- Does not build or test the Python-runner half of the fix against a real embedded CPython (§5).
