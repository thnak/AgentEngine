# ADR-014 — How does a real, OS-backed materialization of a worktree mount resolve a guest-supplied relative path so that `..`, absolute redirects, symlink/junction/reparse-point crossing, ADS, `\\?\` prefixes, unicode-normalization tricks, and TOCTOU re-resolution can never name anything outside the mount root?

- **Status:** **Judged.** Design A (lexical canonicalize-then-string-check, then reopen the checked
  string later) rejected — proven vulnerable to TOCTOU by construction, deterministically
  reproduced, not merely argued. Design B (single handle-based open, containment verified from the
  resolved handle) accepted, with a real production bug found and fixed during the same prove
  pass (§6 finding 1) and two residual risks named, not hidden (§8.3).
- **Date:** 2026-08-06.
- **Scope:** The primitive that turns a guest-relative path string into a real, already-verified-
  safe Win32 `HANDLE` rooted at a real OS directory (`core/worktree_mount_fs.hpp`,
  `open_within_mount_root`) — Milestone 3 Phase C2
  (`docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md`, decision 6). Windows
  first, matching 021 §2's own platform-priority ordering and every prior milestone's Windows-first
  sequencing (Linux parity is Phase C4, explicitly deferred). **Excludes**: syncing a content-
  addressed `Tree` (`core/worktree.hpp`, Phase C1) onto a real directory — that materialization
  mechanism is a Phase E dependency of `PythonRunner`/`ShellRunner` and does not exist yet; write-
  quota enforcement (Phase C3); read-vs-write capability policy (`cap::FsRead`/`cap::FsWrite`,
  already enforced one layer up by `mount_read`/`mount_write`); Linux (Phase C4); 8.3 short-filename
  aliasing (named as an untested residual, §8.3).
- **Related specs:** `025-Worktree-and-Virtual-Filesystem.md` §5 ("path escape is a security bug,
  not a bug"), §9 G2 · `021-Platform-Support-and-Portability.md` §6 G3 (the exact attack-class list
  this ADR's corpus is drawn from, verbatim) · `decisions/ADR-004-appcontainer-native-jail-windows-
  backend.md` §6 finding 1 (AppContainer's ACL model is not a sufficient filesystem boundary;
  interpreter-level path mediation must be primary — this ADR is the concrete mechanism that
  finding requires) · `sandbox/filesystem_adapter.hpp` (the M0-era interface stub this primitive is
  expected to sit beneath, once Phase E implements it — not implemented here).

## 1. The question

**Can a guest-relative path string be turned into a location on a real OS filesystem by
canonicalizing it as a string, checking that string is prefixed by the mount root, and then
separately opening that checked string — the natural-looking, single-pass-feeling approach — or
does that shape have a structural flaw regardless of how carefully the canonicalization step
itself is written?**

The wrong answer, stated up front because it is the one a plausible-sounding design gives:
**"`GetFullPathNameW` resolves `..` and relative segments correctly, and a case-insensitive
string-prefix check against the mount root after that is sufficient — the canonicalization is the
hard part, and once it's right, checking a string is trivial."** §5 below shows this is false, not
because the canonicalization is wrong, but because checking a *string* and later opening that
*same string* are two different operations with an arbitrary gap between them, and nothing forces
the filesystem to still agree with itself across that gap. This is exactly the "convenient-looking
change that breaks I2" pattern CLAUDE.md warns about: the check reads as correct, passes every
test that doesn't specifically attack the gap, and is wrong in a way that never shows up until
something is racing it — which is precisely when it matters.

## 2. Background

`core/worktree.hpp`'s Phase C1 (`mount_read`/`mount_write`) resolves guest paths against a purely
in-memory, content-addressed `Tree` — no real filesystem, hence structurally immune to `..`/
symlinks/ADS (there is nothing an OS resolves; C1's own header comment states this explicitly).
025 §7 requires the *opposite* experience for a sandboxed guest process: "Ordinary files. `open()`,
`pathlib`, `os.listdir`, `ls`, `cat` — all work, because the mount is a real filesystem view inside
the guest." Making that true means some layer eventually hands a real OS path or handle to guest
code (via `PythonRunner`/`ShellRunner`, Phase E, not yet built) — and `decisions/ADR-004`'s own §6
finding 1 already established, with executed evidence, that the sandbox's own OS-level isolation
(AppContainer ACLs) is **not** a sufficient filesystem boundary and that interpreter-level path
mediation must be the primary defense. This ADR builds that mediation's core primitive: given a
real directory some outer layer has already decided the guest may see, make it structurally
impossible for a guest-relative path string to name anything outside it.

## 3. The competing designs

### Design A — lexical canonicalize, string-check, reopen later (rejected)

1. Reject `.`/`..`/absolute paths lexically (reusing `core/worktree.hpp`'s `split_mount_path`).
2. Join `mount_root` and the guest segments into one string; canonicalize it with
   `GetFullPathNameW` (a pure string transform — no handle, no syscall against the actual target).
3. Case-insensitive prefix-check the canonical string against a similarly canonicalized
   `mount_root`. If it passes, **return the checked string**.
4. Later — potentially much later, potentially after other work — **re-open that string** with a
   fresh `CreateFileW` call.

This is the natural shape of "validate input, then use it" as most code is written, and it is the
shape real-world path-traversal CVEs in exactly this class repeatedly take: the validation logic
itself can be airtight and the bug is still there, because step 4 trusts a string produced by step
2/3 without re-deriving anything from the filesystem's *current* state.

### Design B — single handle-based open, verify from the resolved handle (accepted)

1. Reject `.`/`..`/absolute paths lexically (same reuse of `split_mount_path`), plus three
   additional real-filesystem-specific characters no legitimate single path component can contain:
   backslash (would smuggle a second, differently-delimited path through what the `/`-only splitter
   sees as one opaque segment — including a full `\\?\` prefix or a `..\` climb), colon (would
   smuggle a drive letter or an NTFS Alternate Data Stream), and NUL.
2. Open `mount_root` itself once; ask the filesystem — not a string transform — what it really is,
   via `GetFinalPathNameByHandleW`. This is the trusted baseline.
3. Build the joined path string and issue **one** `CreateFileW` call. Windows resolves any reparse
   points (symlinks, junctions) encountered while walking the path transparently, as ordinary path
   parsing — there is no separate "check" step here for anything to race against.
4. Ask the filesystem what the **resulting handle** actually resolves to
   (`GetFinalPathNameByHandleW` again), and verify *that* — not the string that was asked for — is
   still inside the root's canonical path.
5. Return the handle. There is no path re-opened later; the object verified is the object handed
   back.

The key structural difference from Design A: the thing that gets checked (a resolved handle) is
the same thing that gets used (that same handle) — not a string derived once and consumed later.

**Alternative considered and rejected without a spike**: hand-rolling `FSCTL_SET_REPARSE_POINT`'s
`REPARSE_DATA_BUFFER` to detect-then-reject every reparse point outright (deny all symlinks/
junctions unconditionally, rather than allowing ones that resolve to stay inside the root). Rejected
because 025 §5's own wording — "symlinks/junctions/reparse points **crossing the boundary**" — frames
crossing as the violation, not existence; a blanket ban would reject the legitimate case a sandboxed
mount may reasonably want (an internal junction used for its own organizational purposes, proven
allowed in §5's C2-3c below), for no isolation benefit Design B doesn't already provide.

## 4. Falsifiable claims

| # | Claim | Disproving experiment |
|---|---|---|
| MC-1 | `open_within_mount_root` rejects lexical `..` and absolute redirects before any syscall. | A guest path containing `..` or a drive-qualified/rooted path succeeds in opening anything. |
| MC-2 | A symlink/junction whose resolved target lies OUTSIDE `mount_root` is rejected. | The escape-link corpus item (§5 C2-3a) opens successfully. |
| MC-3 | A junction whose resolved target lies INSIDE `mount_root` is followed, not blanket-denied (the mechanism distinguishes crossing from mere existence). | The in-mount junction corpus item (§5 C2-3c) is rejected despite staying inside the root. |
| MC-4 | ADS suffixes and `\\?\` prefixes are rejected structurally, independent of whether the base path exists. | Either corpus item (§5 C2-4/C2-5) opens successfully or is rejected with a "not found"-class error instead of the forbidden-character code. |
| MC-5 | A real on-disk name built from non-ASCII, dot-like codepoints is treated as an ordinary opaque name, never conflated with `.`/`..` in either direction. | The fullwidth-dot corpus item (§5 C2-6) either fails to open the real name, or a literal `..` request is accepted as if it were that name. |
| MC-6 | Design A (naive check-then-reopen) is vulnerable to TOCTOU: a filesystem mutation between the check and the reopen changes what the reopen actually reads. | The naive design's reopened handle (§5 C2-7c), after the deterministic swap, still reads the originally-checked INSIDE content rather than the swapped-in OUTSIDE content. |
| MC-7 | Design B is immune to the identical interleaving MC-6 exploits: neither an already-open handle nor a fresh post-swap request is fooled by the same mutation. | Either the pre-swap handle (§5 C2-7f) reads swapped-in content after the swap, or a fresh post-swap request (§5 C2-7g) succeeds in opening the now-outside target. |

## 5. Executed evidence

Environment: Windows 11 (`10.0.26200.0`), MSVC (Visual Studio 18, toolset via `vcvars64.bat`),
Ninja, `cmake --build build --target test_worktree_mount_fs_escape_corpus -j4`, run directly and
via `ctest`. `core/worktree_mount_fs.cpp`/`.hpp` (new), linked into the existing
`agentengine::worktree_store` CMake target alongside `worktree_digest.cpp` — same real-syscall,
no-third-party-dependency posture. `tests/test_worktree_mount_fs_escape_corpus.cpp` builds a real
scratch directory tree under the system temp path (junctions created via `cmd.exe /c mklink /J` —
test-setup infrastructure only, matching `decisions/ADR-004`'s own precedent of shelling out to
`icacls` for spike setup rather than hand-rolling WDK-only reparse-point structs), and removes it
best-effort at the end of the run.

22 checks, all real filesystem operations, no mocks:

```
  ok: C2-1a: leading .. rejected
  ok: C2-1b: embedded .. rejected
  ok: C2-2a: drive-qualified absolute path rejected
  ok: C2-2b: leading-slash absolute path rejected
  ok: C2-3a: junction crossing the mount boundary rejected
  ok: C2-3b (positive control): ordinary inside file reads correctly
  ok: C2-3c (positive control): a junction that stays INSIDE the root is followed, not blanket-denied
  ok: C2-4: Alternate Data Stream suffix rejected structurally
  ok: C2-5: \\?\ prefix rejected structurally
  ok: C2-6a: a fullwidth-dot directory name is treated as an ordinary literal name
  ok: C2-6b: literal ASCII '..' is never conflated with the fullwidth name (still rejected)
  ok: C2-7a: naive check accepts the currently-real inside path
  ok: C2-7b (setup): toctou_dir successfully swapped for an outside junction
  ok: C2-7c: naive design's reopen-by-string reads OUTSIDE content despite the check having
      validated an inside path -- the TOCTOU vulnerability, reproduced deterministically
  ok: C2-7d: Design B opens the real inside file before the swap
  ok: C2-7e (setup): toctou_dir swapped again for Design B's phase
  ok: C2-7f: an already-open Design B handle is unaffected by a later filesystem swap
  ok: C2-7g: a fresh Design B request made after the swap re-resolves and is rejected
  ok: C2-8: a differently-cased request for an inside file still resolves inside
  ok: C2-9a: the mount root itself is rejected (not a file)
  ok: C2-9b: trailing slash rejected
  ok: C2-9c: double slash rejected
ALL PASS
```

**C2-7's TOCTOU proof, read carefully, is the load-bearing evidence for this ADR's decision.** The
interleaving is applied by hand — delete the checked/opened directory, replace it with a junction
to a different, pre-populated "outside" directory — rather than via real concurrent threads: this is
the same discrete-event-simulation precedent `tests/test_worktree_branch_concurrency.cpp` (Phase
B4) established for a different concurrency property, applied here because it makes the exact state
a real racing attacker needs reproducible instead of timing-dependent, without violating CLAUDE.md's
machine-safety constraint against spawning extra threads.

- **C2-7c is Design A caught in the act**: `naive_check_within_root` genuinely validated a path
  that, *at the moment it ran*, resolved inside the mount. The swap happens strictly after that
  validation returns. `naive_open_checked_path` then reopens the identical string the check
  approved — and reads `"TOCTOU_OUTSIDE_SECRET"`, content that lives entirely outside the mount
  root, never `"TOCTOU_INSIDE"`. The check was correct when it ran and the answer it gave was still
  wrong by the time it was used.
- **C2-7f/C2-7g are Design B's answer to the identical attack**: a handle obtained *before* the
  swap keeps reading the original inside content afterward (Windows handles reference the file
  object, not the path used to open it — a later directory-entry replacement does not retroactively
  redirect an already-open handle), and a *fresh* request made after the swap re-resolves current
  reality and is correctly rejected. Either way the attacker gains nothing: there is no window in
  which a "validated" answer is later consumed against a state that no longer matches it, because
  Design B never separates validation from use in the first place.

Full regression after landing: Windows `ctest -j4` **42/43**, the single failure being
`test_native_jail_backend_windows` — the already-documented, pre-existing OOM-vs-timeout
misclassification flake (this project's own memory record and every prior milestone's regression
notes). Re-confirmed **not** a side effect of this ADR's work: re-run standalone (`ctest -j1 -R
test_native_jail_backend_windows`), it fails on the identical assertion class alone, unrelated to
worktree/mount code and unchanged by this session's changes.

## 6. Red-team findings

Informal — this session's own adversarial pass over its own design (`decisions/README.md`'s
established practice for every ADR in this project so far except where noted otherwise), not an
independent red-team.

1. **[REAL BUG, FOUND AND FIXED]** The first implementation of `open_within_mount_root` opened the
   target with `FILE_SHARE_READ` only. Building C2-7e/C2-7f (deleting a file out from under a
   still-open Design B handle, to prove the handle itself is unaffected) failed at setup:
   `DeleteFileW` silently fails against a handle that was not opened with `FILE_SHARE_DELETE`, so
   the directory could never be removed and replaced. **This is a real production correctness gap,
   not a test artifact**: any future host-side layer that needs to delete or replace a file a guest
   currently has open (a sync pass, a merge, an artifact rotation) would unexpectedly fail against
   every open Design B handle. **Fixed**: the target `CreateFileW` call now requests
   `FILE_SHARE_READ | FILE_SHARE_DELETE`. Found by the ADR's own prove phase, not by a separate
   review pass — the kind of gap this project's positive-control discipline (022 §5) exists to
   surface, here surfacing a functional gap via a security-proof test rather than the reverse.
2. **[HOLDS]** MC-1 through MC-7 all held under executed evidence once the fix above landed. No
   claim required narrowing.
3. **[NAMED GAP, NOT TESTED]** 8.3 short-filename aliasing (e.g. `PROGRA~1`) is architecturally
   covered by the same mechanism — `GetFinalPathNameByHandleW` reports the real long-form resolved
   name of whatever a handle refers to, so an 8.3 alias pointing outside the root would still be
   caught at the containment check — but this was not verified against a real short name, because
   8.3 name generation is commonly disabled by default on modern NTFS volumes (`fsutil 8dot3name
   query`) and this machine's volume configuration was not controlled for. Reasoned coverage, not
   executed evidence, for this one sub-case.
4. **[NAMED GAP, NOT TESTED]** Real symbolic links (`CreateSymbolicLinkW`) were not exercised —
   only junctions, which need no special privilege and are the more universally exploitable
   primitive on a standard, non-Developer-Mode Windows install. Junctions and symlinks share the
   identical OS-level resolution mechanism this design's containment check depends on
   (`CreateFileW` transparently following a reparse point, `GetFinalPathNameByHandleW` reporting the
   real resolved target), so this is expected, not required, to generalize — stated as an
   expectation from the shared mechanism, not as tested behavior, matching `decisions/ADR-004`
   §6.4's identical honesty about its own untested LPAC case.
5. **[SCOPE, NOT A FINDING]** This ADR proves the primitive in isolation, against a hand-built
   scratch directory standing in for "a real materialized mount." It does not prove anything about
   the not-yet-built mechanism that would keep such a directory in sync with `core/worktree.hpp`'s
   content-addressed `Tree` (Phase E's job), nor about `PythonRunner`/`ShellRunner` actually calling
   this primitive for every `open()`/`os.*` operation a guest can reach (also Phase E). A future gap
   in *that* wiring — an entry point that bypasses this primitive entirely — is exactly the failure
   class `decisions/ADR-003`'s own history (`importlib.import_module` on a cache miss) already
   demonstrated for a structurally similar mediation design; this ADR's C2-7-style proof only covers
   what happens once a call reaches `open_within_mount_root`, not whether every guest-reachable path
   operation is wired to call it.

## 7. Per-claim verdicts

| Claim | Verdict |
|---|---|
| MC-1 (lexical `..`/absolute rejected pre-syscall) | **CORRECT** |
| MC-2 (crossing symlink/junction rejected) | **CORRECT** |
| MC-3 (in-mount junction followed, not blanket-denied) | **CORRECT** |
| MC-4 (ADS/`\\?\` rejected structurally) | **CORRECT** |
| MC-5 (unicode name treated as opaque, no conflation) | **CORRECT** |
| MC-6 (Design A vulnerable to TOCTOU) | **CORRECT** — reproduced deterministically, §5/§6 finding 1's discovery is a direct consequence of building this proof honestly. |
| MC-7 (Design B immune to the identical interleaving) | **CORRECT**, after the FILE_SHARE_DELETE fix (§6 finding 1). |
| 8.3 short-name aliasing coverage | **INCONCLUSIVE** — architecturally reasoned, not executed (§6 finding 3). |
| Real symlink (vs. junction) parity | **INCONCLUSIVE** — expected by shared mechanism, not executed (§6 finding 4). |

## 8. The decision

### 8.1 What is accepted

`open_within_mount_root` (`core/worktree_mount_fs.hpp`/`.cpp`) — single handle-based open, verify-
from-the-resolved-handle — is accepted as the containment primitive any future real-OS-backed
worktree mount materialization (Phase E's `PythonRunner`/`ShellRunner`) must call for every guest
path resolution. `redteam::naive_check_within_root`/`naive_open_checked_path` are kept, explicitly
marked test-only, as the permanent regression control for the TOCTOU class this ADR exists to
close — matching this project's `naive_last_writer_wins_merge` precedent (Phase B4).

### 8.2 What this decision does not claim

Not 025 §9 G2's full promotion gate (that gate is "fails to escape a mount on any platform" —
Linux is untested, Phase C4). Not a claim that any guest-reachable code path actually calls this
primitive yet — no such caller exists (Phase E). Not a claim about write-quota enforcement (Phase
C3) or about syncing a `Tree` onto the real directory this primitive assumes already exists (a
named Phase E dependency, §1 scope). Not a resolution of the 8.3-short-name or real-symlink
sub-cases (§6 findings 3/4) — both reasoned, neither executed.

### 8.3 Residual risks, carried forward explicitly

- 8.3 short-filename aliasing: reasoned-covered, not proven (§6 finding 3). Should be re-verified
  against a volume with `fsutil 8dot3name` generation explicitly enabled before this ADR is cited
  as closing that specific sub-case of 021 §6 G3's "case tricks" wording.
- Real symbolic links, as distinct from junctions: reasoned-covered by the shared OS resolution
  mechanism, not executed (§6 finding 4).
- **The largest residual risk is not in this primitive at all**: nothing yet forces every guest-
  reachable filesystem operation Phase E's `PythonRunner`/`ShellRunner` will expose to actually
  route through `open_within_mount_root`. `decisions/ADR-003`'s own history (a real, found-and-fixed
  missed entry point in an analogous mediation design) is the concrete precedent for why this is
  named here rather than assumed — Phase E's own prove phase must include an entry-point census,
  not just correctness of the primitive itself.
- Linux parity (Phase C4) is untouched; the mechanism here (handle-based open, verify from the
  resolved object) has a Linux analogue (`openat2(RESOLVE_BENEATH)` where kernel support exists,
  `/proc/self/fd`-based re-verification otherwise) that was not designed or spiked in this ADR.

### 8.4 What this binds

Evidence toward `025-Worktree-and-Virtual-Filesystem.md` §5/§9 G2 and
`021-Platform-Support-and-Portability.md` §6 G3, Windows only, scoped as a primitive rather than a
full gate closure (§8.2). Establishes the mechanism `decisions/ADR-004` §6 finding 1 named as
required ("interpreter-level `open()` mediation... must be the primary filesystem boundary") without
building it at the time.

### 8.5 What would reopen this

A found guest-reachable path operation in Phase E that does not route through
`open_within_mount_root` (the §8.3 entry-point-census risk, realized). An 8.3-short-name or real-
symlink test run that produces a different verdict than the reasoned expectation in §6 findings
3/4. A Linux implementation whose closest available primitive turns out not to offer an equivalent
handle-based verify-after-open guarantee, which would mean Phase C4 needs its own design pass
rather than a straightforward port (**resolved 2026-08-06 — it did not; see §9**).

## 9. Addendum (2026-08-06) — Phase C4, Linux parity closed

§8.3's own anticipated Linux analogue ("`/proc/self/fd`-based re-verification" where
`openat2(RESOLVE_BENEATH)` kernel support can't be assumed) is what was built, in
`core/worktree_mount_fs_posix.hpp`/`.cpp`: `open()` resolves symlinks transparently the same way
`CreateFileW` does, and `readlink("/proc/self/fd/N")` reads the kernel's own record of what the
resulting descriptor actually, currently refers to — the exact "verify from the object opened, not a
re-parsed string" structural property Design B's acceptance turned on, ported rather than
re-designed. `openat2(RESOLVE_BENEATH)` was considered and set aside for this pass: it would
additionally *prevent* an escaping resolution at the syscall level rather than open-then-verify, a
strictly stronger primitive where available, but it needs a newer kernel (5.6+) than this project
commits to as a floor; open-then-verify via `/proc/self/fd` needs nothing newer than a mounted procfs,
true on any real Linux install. Ordinary follow-on task, not a second ADR, per decision 6's own
framing (this ADR already settled the design question; nothing here is a first design pass).

Proven in `tests/test_worktree_mount_fs_escape_corpus_linux.cpp` (21 checks, real unprivileged Linux
filesystem I/O — `symlink()`, unlike Windows junctions, needs no special privilege at all, so the
Linux corpus is if anything less environment-dependent than the Windows one): the identical TOCTOU
interleaving reproduced deterministically (a real directory checked, then swapped for a symlink
pointing outside; the naive design's reopen reads the swapped-in outside content; a descriptor opened
before the swap is unaffected — on Linux this holds even more directly than on Windows, since an
unlinked file's already-open fd keeps referencing its original inode with no `FILE_SHARE_DELETE`-
equivalent flag needed at open time — and a fresh post-swap request is correctly rejected); a crossing
symlink rejected with an in-mount symlink followed as the paired positive control; an embedded-NUL
segment rejected structurally.

**Named platform divergences, not silently assumed to generalize either direction**: ADS and `\\?\`
prefixes have no Linux analog (not tested — N/A, not a gap); 8.3 short-name aliasing is Windows-only
(N/A); case handling is the *opposite* direction (ext4 is case-sensitive by default, NTFS is not) —
C4-8 tests this explicitly as a documented behavioral difference, not a security property. The two
residuals §8.3 named as Windows-only (8.3 aliasing, real-symlink-vs-junction parity) are unaffected by
this addendum — they remain open on Windows specifically. Full regression on Linux (WSL2 Ubuntu,
kernel 6.6.87.2, gcc 15.2.0): all worktree-store targets and this corpus build and pass clean; the
broader Linux `ctest` suite (native-jail's own privileged-only tests excluded by
`AGENTENGINE_LINUX_SANDBOX_TESTS` staying at its default OFF, unrelated to this primitive) shows no
regression.

## 10. Addendum (2026-08-27) — a real, previously-undiscovered gap in `open_within_mount_root` itself: rejection didn't undo the side effect

Found by an unrelated, independent design's own external-validation work
(`docs/planning/identity-native-sandbox-worktree-design.md` §38.6), which adopted this ADR's
`open_within_mount_root` for its own mediated-filesystem primitive and, in doing so, was the first
caller to route a real *write* (`GENERIC_WRITE`/`CREATE_ALWAYS`) through it against an escaping
junction. Every check in this ADR's own corpus (§6, C2-1 through C2-9, all Judged) uses
`GENERIC_READ`/`OPEN_EXISTING`, which cannot itself have a side effect on rejection. A creating
disposition is different: `CreateFileW(..., CREATE_ALWAYS, ...)` performs the create as part of
resolving the path — including transparently crossing a reparse point — *before*
`open_within_mount_root`'s own containment check runs, so a rejected escape attempt still left a
real, empty file planted at the escaped, outside-the-mount-root location. The verdict returned
(`worktree.mount_path_escapes_root`) was always correct; the side effect of getting there was not
undone.

**Fixed** in `src/core/worktree_mount_fs.cpp`: `open_within_mount_root` now requests `DELETE`
access alongside whatever the caller asked for, reads `GetLastError()` immediately after
`CreateFileW` to determine whether a creating disposition (`CREATE_ALWAYS`/`CREATE_NEW`/
`OPEN_ALWAYS`) actually planted a brand-new object (`ERROR_ALREADY_EXISTS` means it merely
truncated/reopened a pre-existing one — nothing to unwind), and, only when containment fails *and*
a new object was created, unwinds it via `SetFileInformationByHandle(FileDispositionInfo,
Delete=TRUE)` on the SAME handle already verified — never a re-parsed path string, so the cleanup
step itself introduces no new check-then-use gap, preserving this ADR's own "the object verified is
the object used" property one step further than before.

**Proven**: a new **C2-10** in `tests/test_worktree_mount_fs_escape_corpus.cpp` — a creating
`CREATE_ALWAYS` open through the existing `escape_link` junction is still rejected, and now leaves
nothing planted at the escaped location (`GetFileAttributesW` on the would-be planted path returns
`INVALID_FILE_ATTRIBUTES`), paired with a positive control proving an ordinary, legitimate inside
`CREATE_ALWAYS` still succeeds and its file still persists. Full pre-existing corpus (C2-1..C2-9,
22 checks) reverified green alongside the 4 new C2-10 checks — no regressions. Isolated before/after
proof (a minimal standalone probe against `open_within_mount_root` directly, independent of the
consuming design's own code): before the fix, `file exists at OUTSIDE location despite rejection: 1`
(0 bytes); after, `0`.

**What this shows, beyond the specific bug**: this ADR's own corpus, despite being real, executed,
and Judged, had a genuine blind spot — every crossing-junction case it exercises is a read. A
different caller exercising a different operation shape (write) against the same already-proven
primitive found a real gap the original red-team rounds did not, simply because that shape had
never been tried. Consistent with this whole project's standing practice of external, not purely
self-referential, validation.

**Named platform gap, not silently assumed closed**: this addendum's fix and its C2-10 proof are
Windows-only, matching this ADR's own scope. `worktree_mount_fs_posix.hpp`'s Linux analogue was not
audited or fixed for the equivalent gap (a POSIX `open()` with `O_CREAT` similarly creates before
any userspace verification could run) as part of this addendum — a real, named, not-yet-closed
residual for a future pass, not assumed safe by symmetry with §9's otherwise-clean Linux parity.
