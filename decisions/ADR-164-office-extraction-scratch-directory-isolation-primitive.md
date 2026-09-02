# ADR-164 — How does a mediated-Python-worker tool get exclusive, leak-free per-call scratch storage on Windows AppContainer, without either unbounded ACL growth or a hand-rolled concurrency primitive?

**Status:** Proposed — design, red-team, and prove phases complete; awaiting project-owner judgment.
This ADR covers the scratch-directory isolation primitive only, not the three office-document
extraction tools it was designed to support — those remain unimplemented, per `docs/planning/office-
document-extraction-design-draft.md`'s own scope (design draft, not an implementation ADR).

**Relates to:** `009-Plugin-and-Extension-System.md` §7 (the "Document extraction" catalog row's
DOCX/PPTX/XLSX third, deferred by `ADR-106` and picked up as this candidate), `ADR-004` (AppContainer
native-jail Windows backend — this ADR's `grant_ro_deduped()`/`shared_profile()` precedent),
`ADR-014` (`open_within_mount_root` — the TOCTOU-safe containment primitive this design's I/O reuses
via `MediatedFileSystemAdapter`), commit `40c573e` (the unrelated `NativeJailBackend::instances_`
data race this design's own investigation surfaced and fixed as a side effect, in already-shipped
Track B code), commits `0a357b0`/`f4eb2bf` (this ADR's own implementation and proof commits).
`docs/planning/office-document-extraction-design-draft.md` is the full record this ADR summarizes —
ten design revisions, nine red-team rounds, and three prove passes, all preserved in that file's own
history rather than restated here in full.

## 1. The question

**Stated so it has a wrong answer:** when a fixed, host-authored Python script running inside a
jailed `MediatedPythonRunner` worker needs to read a small parameter blob and a source document, and
write a result blob, through a scratch location the worker's AppContainer SID must be granted
filesystem access to — can that grant be made, used, and retired **per call**, safely, without either
(a) the granting AppContainer profile's DACL growing without bound over the process's lifetime, or
(b) needing a novel, hand-built concurrency/lease abstraction (a slot pool) to bound that growth?

## 2. The competing designs actually built and discarded

Four designs were built in sequence, each red-teamed before the next was attempted — not steelmanned
in the abstract, but actually specified in enough detail to find real defects in:

- **Design A — per-call-unique directory, deduped grant.** The first attempt: `unique_scratch_dir_
  name()` (pid + atomic counter) generates a fresh directory per call, granted via the existing
  dedup-on-repeated-path mechanism (`grant_ro_deduped()`). **Falsified**: dedup only helps for a
  *repeated* path; a genuinely unique path defeats it by construction, so this was actually an
  unbounded-grant design wearing a "deduped" label — found in round 4 (finding 14) while investigating
  a *different*, already-shipped concurrency bug (`NativeJailBackend::instances_`, fixed independently
  at `40c573e`).
- **Design B — one shared, inheritable root grant.** Grant a single `extract_scratch_root()` once;
  rely on Windows `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` so every call's unique subdirectory
  inherits access automatically. Bounds ACL growth (one grant, ever) but **falsified on I2 grounds**
  (round 5, finding 18): the AppContainer DACL is this codebase's own documented layer-3 *backstop*
  for a compromised worker, and a shared inheritable root gives every call standing OS-level reach
  into every other call's scratch data — the backstop stopped backing anything up.
- **Design C — fixed N-slot pool.** Pre-grant N slots once; lease them out with a mutex/condvar
  free-list, wipe-before/after-handout, quarantine-on-wipe-failure, a lease-attribution log. Bounds
  both axes Design A/B each only bounded one of, but is a genuinely new concurrency+security
  primitive built from scratch — and drew real defects on **every one of rounds 6 and 7's adversarial
  passes** (findings 20-27: an unsafe `ifstream`/`ofstream` read-back later fixed via
  `MediatedFileSystemAdapter`; a false FIFO-ordering claim about `std::condition_variable`; an
  underspecified handout-time wipe-failure path; no bound on `ToolCallHook`-based call-count budgets).
- **Design D — per-call grant, delete-retires-grant (accepted).** A "prove pass" (not another red-
  team round) re-read `AppContainerProfile::grant_path()`'s actual implementation and found Design B's
  own premise was never true: the ACE `grant_path()` adds lives on the **granted path's own** NTFS
  security descriptor — there is no separate, ever-growing profile-wide DACL. Deleting that directory
  destroys the ACE as a structural side effect of NTFS object deletion. Design A's original per-call
  directory was safe all along, *provided* cleanup (`remove_all()`, already designed one revision
  before Design B was ever introduced) actually runs. This collapsed ~1000 lines of Design C's pool
  machinery back to near-Design-A simplicity, keeping only the genuinely-earned fixes (adapter-
  mediated I/O, capability-composition rigor) — findings 14/18/19/21/22/23/25/26/27 all closed by
  eliminating the mechanism they were findings *about*, not by patching further.

## 3. Falsifiable claims and the executed evidence

Design D's safety rests on two claims that nine rounds of code-reading argued but did not, by
themselves, prove. Per this project's own "a test that cannot fail proves nothing" standard, two
positive-control tests were built and run (`tests/test_native_jail_grant_path_ace_lifecycle_windows.cpp`,
commits `0a357b0`/`f4eb2bf`):

- **Claim 1 — deletion destroys the grant.** *Verdict: CORRECT.* Real evidence: granted a directory,
  created a child file while the grant was live (confirming, by direct ACE enumeration, that the
  child independently materializes its own ACE at creation — Windows inheritance is a point-in-time
  copy, not a live reference, which is also what falsified Design C's `revoke_path()`-on-directory-
  only fallback at round 9, finding 33), deleted the directory, then re-created a fresh object at the
  identical path with no new grant — the fresh object carries zero ACE for the profile's SID.
- **Claim 2 — `revoke_path()` (the failure-path safety net for when `remove_all()` itself fails)
  actually strips access, including from already-materialized child ACEs.** *Verdict: CORRECT,
  fixed once.* First specified as directory-only (finding 31's Revision 9 fix); round 9 (finding 33)
  found this doesn't touch child files' own independently-materialized ACEs, since inheritance
  copies, it doesn't reference. Revision 10 extended the fallback to a fixed, small enumeration
  (directory + the three known children — `args.json`, `source`, `out.json`); the corrected version
  was proven by granting a directory with three children, confirming each independently carries the
  SID's ACE, calling `revoke_path()` on all four paths (plus a fifth, never-created path, confirming
  the "missing child is trivially satisfied" case doesn't error), and confirming zero ACEs remain
  for the SID on any real path afterward — while every *other* SID's ACE count is unchanged (proven
  by ACE enumeration, not a flat count, after the test's first draft caught a real subtlety: a single
  `grant_path()` call legitimately materializes *two* ACEs per SID, an effective one plus an
  inherit-only one for future children — genuine Windows ACL canonicalization, not a bug).
- **Claim 3 — `revoke_path()` succeeds even when `remove_all()` fails due to real open-handle
  contention** (the specific scenario the fallback exists to handle; argued from documented
  `WRITE_DAC`/`READ_CONTROL` semantics at round 9/Revision 10, finding 34, but not executed until
  now). *Verdict: CORRECT.* A file inside the granted directory was opened with a real Win32 handle
  excluding `FILE_SHARE_DELETE`; `remove_all()` was confirmed to genuinely fail while the handle was
  held (a real `std::error_code`, not assumed); `revoke_path()` was then called on the directory and
  the still-open file **while the handle remained open**, and both succeeded — confirmed by fresh
  `GetNamedSecurityInfoW` queries showing the SID's ACE gone from both, then the handle was closed and
  ordinary cleanup confirmed to succeed afterward, proving the earlier failure really was the held
  handle and not some other cause.

All three claims: **37 + N real assertions across the two test runs, 0 failures**, full
`native_jail`/`extract_pdf`/`pdf_text` suite (9/9) re-run clean after every change, no regressions.

## 4. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| Deletion destroys a per-call `grant_path()` ACE | **CORRECT** | Real ACE enumeration before/after deletion + re-creation at the same path |
| `revoke_path()` on directory + fixed child enumeration strips all materialized ACEs | **CORRECT** | Real ACE enumeration across 4 paths (3 real children + 1 missing), other-SID ACE count unchanged |
| `revoke_path()` succeeds under real open-handle contention that defeats `remove_all()` | **CORRECT** | Real held `HANDLE` without `FILE_SHARE_DELETE`; confirmed `remove_all()` failure; confirmed `revoke_path()` success against both the directory and the held-open file |

No claim in this ADR's scope came back WRONG or INCONCLUSIVE. The one claim this ADR does **not**
carry a verdict on is whether Design D's *aggregate* per-call OS-call cost (`grant_path()`/
`create_directories()`/`remove_all()` on every call, no longer amortized across Design C's pool) is
acceptable under real load — named, disclosed, and explicitly unmeasured (round 8, finding 30; the
design draft's §6).

## 5. Decision

**Design D — per-call directory, direct `grant_path()` grant, `MediatedFileSystemAdapter`-mediated
I/O, unconditional `remove_all()` cleanup with a bounded-retry-then-`revoke_path()`-fallback failure
path — is the accepted scratch-directory isolation primitive** for any first-party tool that needs a
`MediatedPythonRunner` worker to exchange small parameter/result data with the host through a
filesystem location, on Windows. `AppContainerProfile::revoke_path()` (commit `0a357b0`) is a new,
real, small, general-purpose primitive on the same footing as the existing `grant_path()` — usable by
future designs with the same failure-path-safety-net shape, not scoped narrowly to this candidate.

This binds the isolation mechanism; **it does not itself authorize implementing `ExtractDocxText`/
`ExtractXlsxCells`/`ExtractPptxText`**. Those three tools still need their own implementation pass
(the design draft's §3 already specifies their shape) and, per this project's design→red-team→prove→
judge discipline, their own red-team attention on tool-specific concerns this ADR's scope excluded
(XLSX/DOCX/PPTX-format-specific parsing risk, the `openpyxl`/`python-docx`/`python-pptx` dependency
vendoring itself, the transitive Python import allowlist, and §6's still-unmeasured resource caps).

**Not decided by this ADR, left for the implementation pass:**
- Real `exec_wall_ms`/`memory_bytes` numbers for the three tools (§6, never sized in the design draft).
- The transitive Python import allowlist's exact, complete membership (round 3, finding 6's
  descendant work — verified real but never exhaustively enumerated).
- §6's disclosed-but-unmeasured per-call OS-call cost and correlated-retry-under-load residual
  (round 8/9, findings 30/36).
- Linux support for this whole mechanism — `MediatedPythonRunner`/`create_python_worker()` remains
  Windows-only (a pre-existing, named platform gap this ADR inherits, does not solve).

**Awaiting the project owner's own review** — a security-critical isolation-boundary decision, per
this project's own norm (CLAUDE.md's "design → red-team → prove → judge" discipline; `decisions/
README.md`'s "any security-critical choice... isolation boundary" trigger) that such a choice gets
explicit owner sign-off before being treated as settled, even though the technical claims above are
each backed by real, executed, passing evidence rather than argument alone.
