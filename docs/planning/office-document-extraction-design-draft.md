# Design draft — office document (DOCX/XLSX/PPTX) structural extraction

**Status: Revision 10 — after red-team round 9, Prove pass 3 complete (revoke_path() proven under real
open-handle contention — see "## Prove pass 3" at the end of this file).** Revision 2 resolved red-team round 1's five findings,
following Track A (`read_content`, `include/agentengine/tools/read_content.hpp`), Track B (the four
`extract_pdf_*` tools, PDFium-backed, `docs/planning/pdf-text-extraction-design-draft.md` Revision 7
after six real red-team rounds), and the OCR draft (`docs/planning/ocr-text-extraction-design-draft.md`
Revision 1, shelved/deprioritized per commit `c82f018` in favor of this candidate). This revision (3)
resolves red-team round 2's three findings, all real, none re-flags. **Finding 6 (Critical)** — §4b's
finding-4 fix (a dedicated `MediatedPythonRunner` per call, `mount_roots` narrowed to one scratch
entry) was necessary but not sufficient: `native_jail_handle_relay.cpp`'s `dispatch_open` gates every
`open()` on a SECOND, independent capability-layer check (`ctx.capabilities->find_fs_read`/
`find_fs_write`) that §4b never granted, so the fixed script's own `open("args.json")` would fail
closed exactly as specified. §4b below now names the real, concrete fix — verified directly against
`capability.hpp`'s actual `cap::decl::FsRead<Mount>`/`cap::FsWrite<Mount>` shape and `CapabilitySet`'s
real lookup semantics, not invented — and explains precisely why it does not reintroduce the standing,
session-wide authority finding 4 eliminated. **Finding 7 (Medium-High)** — round 2's own GitHub-API
re-verification of DuckX (dormant: last push 2024-06-12, one tag from 2019-11-05, 34 open issues)
against `Aspose.Slides FOSS for C++` (actively committed through the week of that pass, 2 open issues,
no tag) exposed that Revision 2 applied a currency lens to the PPTX comparison it never applied to the
DOCX one. §2a/§2c/§3 below re-reason DOCX's route choice on the same, now-consistent evidence and reach
a real conclusion (below). **Finding 8 (Medium)** — no per-turn/session call-count budget existed for
a tool whose finding-4 fix pays a full interpreter-process-spawn cost on every call; §6/§8 below name
the real, checked answer (a genuine cross-cutting gap, plus a real, already-shipped extension seam a
host can use today, found by actually checking whether one exists rather than assuming). **This is a
design-fix pass responding to round 2, not a third red-team round** — no new adversarial review
happened this pass, same revision-numbering discipline Revision 2 itself named (Track B's own
precedent: a revision number increments for a design-fix pass distinct from a red-team round). The
"Red-team round 1" and "Red-team round 2" sections below are left unmodified as historical record;
this pass's fixes are in the numbered sections above them, cross-referenced back to "round 2 finding
6/7/8" the same way Revision 2 cross-referenced round 1's findings. No code, no vendoring, no ADR yet.

**This revision (4) resolves red-team round 3's four findings — a design-fix pass, not a fourth
red-team round, same discipline named above.** **Finding 11 (Medium)** — actually re-evaluated,
not just re-asserted: whether the two-Python/one-native hybrid was worth its doubled isolation-
mechanism surface once round 2 finding 7 made the Python-mediated plumbing unavoidable anyway for
DOCX/PPTX. §2c below reaches a real, evidence-based conclusion — **the hybrid is collapsed**:
`ExtractXlsxCells` moves to route (b) (`openpyxl`, already vetted clean in Revision 1's own §2b
table and never seriously reconsidered once route (a) was picked for it in Revision 2). Part of that
evaluation required checking, directly against `extract_pdf_text.hpp` and `native_jail_backend.cpp/
.hpp`, whether Track B's own scratch-file mechanism actually handles concurrent same-tool calls
safely under `07ae98e`'s shipped ADR-160 parallel-batch scheduler — it does, for the one thing it was
built to do (`unique_scratch_filename()`'s pid+atomic-counter naming), but that same investigation
surfaced a real, live, **already-shipped** defect one layer down (`NativeJailBackend::instances_`, an
unsynchronized `std::unordered_map` mutated by every `create()`/`exec()`/`destroy()` call, shared by
every caller of the process-wide static backend both `extract_pdf_text_detail::invoke_worker` and
`tools/cli_chat.cpp`'s `shared_python_runner()` construct) that concurrent calls under ADR-160's real,
shipped `ThreadPool` now actually race on. This is disclosed prominently below (§2c, §4b) as a real,
out-of-scope-for-this-draft defect in Track B's shipped code — this draft does not fix it, but it is a
genuine, checked, load-bearing fact in reaching finding 11's conclusion, not a hypothetical. **Finding
9 (Critical)** — §4b never named how "this call's own scratch directory" actually gets generated, and
the one precedent it cited does the opposite (shared directory + unique filename, where §4b's own
fixed-filename design needs the reverse). §4b now names the real mechanism: a `unique_scratch_dir_name()`
twin of Track B's own `unique_scratch_filename()` (same pid+atomic-counter construction, verified
thread-safe), producing a fresh, call-unique subdirectory under a shared, host-fixed scratch root —
composed explicitly against how `path_prefix`-narrowed `cap::FsRead`/`FsWrite` grants actually work
(finding 6's fix), plus a companion fix (found during finding 11's own investigation, not separately
numbered by round 3) requiring each call's dedicated `MediatedPythonRunner` to be backed by its own
call-local `NativeJailBackend` instance, never the existing shared static, closing the instances_-map
race for these tools specifically. **Finding 10 (Medium-High)** — `python-docx`'s real API silently
excludes headers/footers/footnotes/endnotes/tracked-changes text/nested tables from `Document.paragraphs`/
`Document.tables`. §3 now names this explicitly as a v1 scope limitation on `ExtractDocxText`'s own
Reply shape, the same "named, not silently dropped" discipline already applied to embedded images and
writing support. **Finding 12 (Medium-High)** — finding 8's own `PolicyDecider`-based call-count-cap
seam is structurally unreachable for `text_derived`-provenance calls and carries no turn/session
distinguishing signal. Checked directly against `rt/agent_session.hpp`'s `run_rounds()`: a genuinely
different, already-shipped seam exists — OQ-21's `ToolCallHook` (`core/tool_call_hook.hpp`,
`AgentSession::set_tool_call_hook()`) — invoked unconditionally for every call in a round with no
provenance-based skip (unlike `resolve_approval_outcome`'s `policy` gate), closing finding 12's
`text_derived` gap for real. §6 now recommends this instead of `PolicyDecider` for a host-wired
call-count cap. The turn-vs-session distinguishing gap is NOT solved by this either — `ToolCallHookContext`
carries no turn index and no `EffectContext&` — so §6 also states plainly, as finding 12's own fix
direction anticipated, that this remaining half is a genuine, engine-level gap this draft cannot solve,
not a placeholder. The "Red-team round 3" section below is left unmodified as historical record; this
pass's fixes are in the numbered sections above it, cross-referenced back to "round 3 finding 9/10/11/12"
the same way Revisions 2/3 cross-referenced their own round's findings.

**This revision (5) resolves red-team round 4's five findings — a design-fix pass responding to round
4, not a fifth red-team round, same discipline named above.** **Finding 13 (High)** — §2c/§4a's own "Net
evaluation" for collapsing the hybrid named the `NativeJailBackend::instances_` data race as one of
three coequal, still-live costs of keeping XLSX on route (a), calling it "the headline finding" of that
pass's own investigation. That race is now genuinely FIXED — commit `40c573e` (2026-09-01, the same day
Revision 4's own text was written), verified directly against the real diff, not trusted from its own
commit message: a `std::mutex instances_mutex_` now guards every structural access to `instances_`
(Windows) and its Linux equivalent, through `find_instance_locked`/`insert_instance_locked`/
`erase_instance_locked`, used by every one of `create()`/`exec()`/`destroy()`/`create_python_worker()`/
`exec_session()`. §2c/§4a below are corrected to state this plainly and re-run the collapse's own
cost/benefit weighing on the two costs that remain real — reaching a real, re-confirmed conclusion (the
collapse still holds), not left asserting a defect that no longer exists. **Finding 14 (Critical)** —
finding 9's own fix (a fresh, never-repeated per-call scratch DIRECTORY, specifically to close a
cross-call file-collision race) turns out to defeat the ONE mechanism (`grant_ro_deduped()`, keyed on
exact path string) this codebase has for keeping `AppContainerProfile`'s Windows DACL from growing
without bound: `create_python_worker()`'s own per-mount grant loop (`native_jail_backend.cpp:612`,
re-verified) calls `AppContainerProfile::grant_path()` directly, ungated by any dedup, for every mount in
`spec.mounts` — and because finding 9's own fix deliberately makes the `extract_scratch` mount's host path
unique on every call, that dedup can never fire, so every one of `ExtractDocxText`/`ExtractXlsxCells`/
`ExtractPptxText`'s calls appends one new, permanent ACE to the shared profile's DACL, forever, directly
contradicting §4b's own then-current claim that this is "read-only, deduped grant bookkeeping." §4b below
now names the real fix — verified against `grant_path()`'s own `OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE`
inheritance flags (`app_container_profile.cpp:165`) and `extract_pdf_text.hpp`'s own shipped
`grant_ro_path_once()` precedent, not invented. **Finding 15 (High)** — no cleanup of the per-call scratch
directory or its `args.json`/`out.json` contents was named anywhere in this draft, an unbounded disk-space
leak on top of finding 14's ACL leak; combined with `create_directories()`'s silent-success-on-existing
semantics, this also left a narrow, real stale-directory-reuse risk under pid reuse after a process crash.
§4b below adds a real, both-paths cleanup step plus a fail-closed existence check, with the reasoning for
why that combination is judged sufficient for v1 stated explicitly rather than left implicit. **Finding
16 (Medium)** — §6's Revision 4 edit widened the interpreter-startup-cost disclosure from "two tools" to
"three tools" as a pure headcount change, without re-examining that the collapse also changed the
WORST-CASE AGGREGATE CONCURRENCY picture in kind, not just per-call cost: an XLSX-only batch under
ADR-160's parallel-batch scheduler used to spawn zero `MediatedPythonRunner` processes and now spawns one
per call, the same as DOCX/PPTX always did. §6 below adds that qualitative disclosure explicitly. **Finding
17 (Medium-High)** — `ExtractXlsxCells` never got the same Reply-shape-level disclosure discipline
`ExtractDocxText`'s `body_text_only: true` field gave it (round 3 finding 10's fix): `openpyxl`'s
`read_only=True` mode silently drops merged-cell content below a merged range's top-left cell (worse than
its own normal-mode behavior) and trusts the workbook's own unverified `<dimension>` XML element for
`total_row_count`'s bound. §3 below names both explicitly, the same honesty standard DOCX already has. The
"Red-team round 4" section below is left unmodified as historical record; this pass's fixes are in the
numbered sections above it, cross-referenced back to "round 4 finding 13/14/15/16/17" the same way prior
revisions cross-referenced their own round's findings.

**This revision (6) resolves red-team round 5's two findings — a design-fix pass responding to round 5,
not a sixth red-team round, same discipline named above.** **Finding 18 (Critical, I2)** — round 5
confirmed finding 14's own Windows ACL-inheritance mechanics are real and correctly cited
(`app_container_profile.cpp:152-205`'s `grant_path()`, genuine Win32 creation-time inheritance, not a
broken or merely-asserted claim) but found that granting the shared `extract_scratch_root()` ONCE,
inheritably, converts `native_jail_backend.hpp`'s own documented layer-3 OS-level "BACKSTOP... never the
primary boundary" from a per-call-narrow boundary into a standing grant reachable, via the ONE shared
AppContainer SID every worker of these three tools runs under (`shared_profile()`, ADR-004 §3's own
"reused across sessions" design), by any worker compromised through a hostile input file — across every
call, every session, every tool, for the whole process's lifetime, not merely this one call's own data.
§4b below (immediately after finding 15's fix) now names the real fix — neither accepting the widening
as-is, nor reopening ADR-004 §3's own locked one-SID design, nor inventing an unproven `revoke_path()`
surface, but a fixed, small, pre-granted scratch-SLOT POOL: N host-chosen slot directories, each
individually granted once at pool-initialization time (no inheritable ACE on the root itself at all),
checked out under mutex/condition-variable discipline for the duration of one call and wiped both before
handout and after release — that bounds BOTH finding 14's ACL-growth axis (still zero unbounded growth:
exactly N grants, ever) AND finding 18's backstop-scope axis (bounded to N concurrently-live calls' worth
of exposure, not the whole process's lifetime) at once, rather than trading one for the other the way
Revision 5's fix and the pre-Revision-5 design each did. The real, disclosed residual — this still shares
one AppContainer SID, so the backstop layer still cannot distinguish between the N slots' own concurrent
occupants — is stated explicitly, not swept under the rug, alongside the real, checked precedent
(`decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md`) for why this codebase's own already-Judged
threat model does not require this backstop layer to be leak-free, only bounded and disclosed, provided the
SOFTWARE layer (`dispatch_open`, finding 6) remains the actual, independently-sound primary boundary —
which round 5's own investigation confirmed it still is ("the software layer... remain[s] genuinely sound
— no defect found there this round"). **Finding 19 (High)** — the shared-root design also turned out to
need a genuine three-step cold-start sequence (ensure the root exists, THEN grant it, THEN create any
subdirectory under it) that finding 14's own prose never named, a real, undesigned first-call reliability
gap. The slot-pool fix resolves this differently and more simply than sequencing three lazy steps
correctly: pool initialization (root-exists, then create-and-grant every one of the N slots) is now one
atomic, mutex-guarded, lazily-triggered-but-fully-sequenced startup step (mirroring `shared_profile()`'s own
lazy-singleton pattern, `native_jail_backend.cpp:110-123`, including that function's own already-fixed
"retry on transient failure, don't latch it forever" discipline) that completes in full, for every slot,
before the pool's free-list is ever populated — no call can ever acquire a slot that has not already been
granted, structurally, not by sequencing discipline alone. The "Red-team round 5" section below is left
unmodified as historical record; this pass's fixes are in §4b below (immediately after finding 15's fix),
cross-referenced back to "round 5 finding 18/19" the same way prior revisions cross-referenced their own
round's findings.

**This revision (7) responds to round 6's five findings not by patching the pool a further time on the
same terms, but by first asking round 6's own framing question directly: can a fundamentally different
I/O design avoid this whole class of problem, rather than fix it in place again?** Two real alternatives
were investigated against the actual shipped code, not this draft's own prose, before any fix was
written: **(2) whether `ExecRequest` has grown any field, since round 1's own finding 1, capable of
carrying `sheet_name`-shaped host-to-worker data into the worker's Python namespace without a shared
scratch file at all — checked directly, it has not.** `sandbox.hpp:298-313`, re-verified, still has
exactly `language`/`source`/`preseeded_answers`; `preseeded_answers` is wired, by its own header
comment, EXCLUSIVELY to `agent.ask()`'s replay semantics (`_ae_internal.ask_or_raise`,
`python_worker_mediation.cpp`) — repurposing it for `sheet_name` would require `expose_agent_ask = true`
(deliberately `false` for all three tools, finding 4's fix — neither script calls any `agent.*` module)
and would overload a human-in-the-loop-shaped channel to smuggle an ordinary parameter, a real semantic
mismatch, not a narrower version of the same mechanism. `run_capturing`'s only injection point into the
worker's `main_dict` remains `PyRun_String(source, ...)` — no other channel exists. **`args.json`/
`json.load()` (finding 1's fix) therefore cannot be eliminated — it is retained unchanged.** This also
answers, directly, whether the SOURCE document itself could bypass the scratch mechanism using Track B's
proven `fetch_via_url`/`fetch_via_path`/`grant_ro_path_once()` pattern unchanged: no, for a real,
structural reason, not an oversight — unlike Track B's one-shot native worker (which never routes
through `dispatch_open` at all, §4a), these tools' fixed scripts read the source document FROM INSIDE
the mediated Python interpreter (`docx.Document(path)` etc.), so that read is subject to the identical
two-layer `dispatch_open` check (finding 6) as `args.json`/`out.json` and must live inside the SAME
single `mount_roots` entry finding 4's fix narrows the runner to. The source document already,
correctly, shares one scratch mount/slot with `args.json`/`out.json` — not a missed simplification. **(1)
`ExecOutcome::artifacts` (`sandbox.hpp:341`), round 1 finding 2's own original, un-adopted suggestion,
succeeds partially, and the partial result is real and load-bearing.** `artifacts` is populated by
`harvest_mount`/`harvest_subtree` (`worktree_mount_sync.hpp`), which read/write through the generic
`FileSystemAdapter` interface — and the real, production conformer of that interface for a native-jail
mount, `MediatedFileSystemAdapter` (`mediated_filesystem_adapter.cpp`/`_posix.cpp`, both platforms
verified directly), builds every one of `read_file`/`write_file`/`exists`/`remove`/`list_directory`/
`canonicalize` on `open_within_mount_root` (`worktree_mount_fs.cpp:152`, ADR-014's TOCTOU-safe,
handle-then-verify primitive) — exactly the discipline finding 20 found missing from this draft's own
bare `std::ifstream`. So `ExecOutcome::artifacts`, when harvested through this adapter, IS already
structurally immune to finding 20's reparse-point/symlink attack — the safe primitive round 6 asked this
draft to check for is real. But `harvest_mount` itself is not adoptable as-is: it requires a real `Mount`
with an already-committed worktree Ref plus a `WorktreeObjectStore`/`AppendLogStore` pair, and commits
each harvested file into that Ref as it goes (`worktree_mount_sync.hpp`'s own "the agent saves a file,
the user receives an artifact," 025 §7) — genuine turn/session-artifact-tracking machinery, correctly
judged too heavy for an internal, single-call result-transport channel back in Revision 2's own original
rejection of this path (finding 2's fix, §4b) — a judgment this revision re-confirms, not reverses. **The
right-sized result is one layer down from `harvest_mount`, not `harvest_mount` itself**:
`MediatedFileSystemAdapter` is usable standalone, against any host directory, with NO worktree/Ref/
object-store dependency at all — exactly finding 20's own fix direction, discovered to already exist as
a ready, cross-platform, already-shipped wrapper class rather than something to call by hand. **Net
result: full collapse is not achievable** — investigation (2) closes off eliminating `args.json`, and the
source document cannot be pulled out of the shared mount either — **this is a partial collapse: the
scratch-slot pool (findings 18/19) and the `args.json`/`out.json` channels (findings 1/2/6/9) are
retained unchanged in shape, but every read/write against a slot's contents now goes through
`MediatedFileSystemAdapter` instead of bare `std::ifstream`/`std::ofstream`** (§4b's new fix for finding
20, immediately after the finding 18/19 pool fix). Findings 21 (wipe reliability/quarantine), 22 (the
incorrect `std::condition_variable` FIFO claim), and 23 (no lease attribution) concern the pool mechanism
itself, which survives this pass's investigation intact — they are fixed in place, not resolved by
elimination, in the same location. The "Red-team round 6" section below is left unmodified as historical
record; this pass's fixes are in §4b below, cross-referenced back to "round 6 finding 20/21/22/23" the
same way prior revisions cross-referenced their own round's findings. (Finding 24 — the ADR-041-analogy
weighing, and whether a positive-control test should gate shipping — is not addressed this pass; it
remains open, per its own round-6 text.)

**This revision (8) implements Prove pass 1's recommendation — a collapse, not another patch, credited to
that pass's own close reading of code this draft had cited but never checked against the specific question
that mattered.** Prove pass 1 (full text preserved below, after "Red-team round 7") read
`AppContainerProfile::grant_path()`'s real body (`app_container_profile.cpp:152-205`) against a question
seven rounds of red-teaming the scratch-slot pool never asked: where does the ACE `grant_path()` adds
actually live? The answer — on the granted path's OWN NTFS security descriptor, not on any
`AppContainerProfile`-wide ledger — means finding 15's cleanup fix (unconditional `remove_all()` on both
the success and failure path, designed in Revision 5, one section BEFORE finding 14's pool was ever
introduced) already destroys that ACE as a structural side effect of deleting the object it was granted
on, *provided the grant lands on each call's own leaf directory directly (finding 9's original shape)
rather than on a shared, never-deleted root*. Finding 14's own fix chose the shared root specifically to
avoid a per-call `grant_path()` call — that choice, not anything about per-call grants being unsafe, is
what manufactured finding 18's standing cross-call/cross-session/whole-process-lifetime backstop exposure,
which then required the entire pool/inheritance/quarantine/wipe/ring-log lineage (findings 14, 18, 19, 21,
22, 23, 25, 26, 27 — ten findings, roughly a thousand lines of design text and fixes across Revisions 5-7)
to bound. None of that machinery was ever necessary: it was built to bound a problem finding 14's own fix
created, not one finding 9's original design had. **This revision reverts §4b (and the §6/§8 bookkeeping
that referenced the pool) to finding 9's per-call-fresh-directory shape**: each call gets its own
uniquely-named scratch directory (`unique_scratch_dir_name()`, unchanged), granted directly via the
already-shipped `grant_path()` (a genuine per-call grant — no dedup, because the path is used by exactly
one call, ever, so there is nothing to dedup, and dedup is exactly what pushed finding 14's fix toward a
shared root in the first place), and retired by finding 15's already-designed unconditional `remove_all()`
on both paths. No new Win32 surface (not even the `revoke_path()` Prove pass 1 confirmed is small and
buildable but unnecessary) and no new mutex, condition variable, free-list, or attribution log — the whole
concurrency/security primitive rounds 6-7 spent nine findings hardening is not built at all, because
per-call-unique, self-deleting directories leave no reusable resource to protect. **This is not wasted
effort being discarded — it is why the simpler design can now be trusted rather than merely reasserted**:
finding 20's fix (routing `args.json`/`out.json`/the source document through `MediatedFileSystemAdapter`
instead of a bare `std::ifstream`/`std::ofstream`) is genuinely independent of the pool-vs-per-call
question and is kept unchanged; finding 6's capability-composition rigor (`path_prefix`-narrowed
`cap::FsRead`/`FsWrite` grants composing with whatever host directory a mount label currently resolves to,
regardless of how that directory was chosen) is what lets this revision state, rather than merely hope,
that the collapse is safe; and the "named, not silently asserted" honesty habit the pool's own seven rounds
enforced is what makes finding 15's residual (a crash that skips RAII cleanup) a disclosed, bounded
v1 posture instead of an unexamined one. Net effect: roughly a thousand-plus lines of pool-specific
design/findings/fixes collapse back down to something close to Revision 3/4's original per-call
simplicity — but a version of that simplicity earned by five rounds' worth of genuine correctness this
draft did not have the first time around. Findings 14, 18, 19, 21, 22, 23, 25, 26, and 27 are marked
"resolved by elimination — Revision 8, per Prove pass 1" in §8 below, not deleted from the record. Finding
24 (the ADR-041-analogy weighing) and finding 29 (finding 24's own tracking gap) are both re-assessed, not
carried forward unexamined — §8 below reasons through why neither is load-bearing once there is no
standing residual left to justify by analogy. Per Prove pass 1's own honesty about its limits, this
collapse is argued from `grant_path()`'s real code and ordinary NTFS/AppContainer semantics, not from an
executed test — a real red-team round of its own (Prove pass 1's own suggested checks: no ACE inheritance
across a deleted-and-recreated same-named directory; no post-deletion handle-retention surprise) remains
real, undesigned future work, named here rather than skipped past.

**This revision (9) resolves red-team round 8's three findings — a design-fix pass responding to round 8,
not a ninth red-team round, same discipline named above.** Round 8 independently re-verified Revision 8's
own central claim (the ACE `grant_path()` adds lives on the granted path's own NTFS security descriptor,
not on any profile-wide ledger) directly against the code a second time, not merely trusted from Prove pass
1's citation, and it held — this is real convergence, not a re-opened question. Three real, new findings,
all lower-severity than any prior round, none reopening the collapse itself. **Finding 31 (High, I2/I8)** —
finding 15's own residual framing (Revision 5, never reopened since) named only the CRASH-skips-cleanup
case; round 8 found a materially different, non-crash failure mode finding 15's text never considered:
`remove_all(dir, ec)` can run to completion normally (no crash, RAII fires, the calling code keeps
running) and still return a nonzero `ec` — a transient sharing violation (an AV/indexing scanner or a
not-yet-exited worker process briefly holding a handle inside the directory), a permissions edge case, or
any other real, non-catastrophic filesystem error `remove_all`'s non-throwing overload can report without
throwing. Because Prove pass 1's own "finding 14 resolved by elimination" argument makes this cleanup step
the SOLE mechanism retiring the per-call `grant_path()` ACE, an unchecked failure here silently strands a
live-granted directory — with no bound and no signal, reproducing finding 14's own unbounded-ACE-growth
shape through a different door than the one Revision 8 checked. §4b below (immediately after finding 15's
own discussion) now names the real fix: check `remove_all`'s own `ec`, retry a small, bounded number of
times (transient sharing violations typically clear within milliseconds), and, on exhausted retries, both
surface a loud, attributable failure event (finding 32's own mechanism, composed explicitly rather than
designed separately) and fall back to `AppContainerProfile::revoke_path()` — Prove pass 1 verified this
primitive is real, small, and buildable (the same three Win32 calls `grant_path()` already ships, with
`SetEntriesInAclW`'s `ACCESS_MODE` flipped to `REVOKE_ACCESS`) but declined to build it because the success
path never needed it; finding 31 is the one real case where deletion does NOT retire the grant, making this
the one place in the whole draft where `revoke_path()` earns its keep, as a narrowly-scoped, failure-path-
only safety net, never the primary mechanism. **Finding 32 (Medium, I4)** — eliminating the pool's own
lease-attribution ring log correctly closed finding 23's narrower shared-slot-reconstruction claim, but
silently dropped the more general property no replacement was ever named for: SOME record of which
call/session/tool created and deleted which scratch directory. §4b below adds a minimal, right-sized
replacement — not a reinvention of the eliminated ring log's own cross-call-attribution complexity, which
has no analog once every directory is single-use, but a structured event at directory-creation and
directory-deletion/cleanup-outcome time, reusing `EffectContext::report_progress` (ADR-060's real,
already-shipped, call-scoped reverse channel into the run event stream — already used by this exact backend
file, `native_jail_backend.cpp:901`, for a comparable per-call purpose) rather than inventing a new logging
mechanism, and composed explicitly with finding 31's fix: the failure-path event IS finding 31's own
"surface a real, attributable error" requirement, not a second, independently-designed mechanism.
**Finding 30 (Medium, I8)** — §6 below adds the honest disclosure round 8 found missing: the collapsed
per-call design pays `grant_path()`'s own three-call `GetNamedSecurityInfoW`/`SetEntriesInAclW`/
`SetNamedSecurityInfoW` cycle, plus `create_directories()`/`remove_all()`, on EVERY call, not amortized
across `N` slots the way the now-eliminated pool incidentally was — named as a real, unmeasured, disclosed
tradeoff of the collapse, the same "mechanism resolved, number not sized" honesty this section already
extends to every other cap. The "Red-team round 8" section below is left unmodified as historical record;
this pass's fixes are in §4b (findings 31/32, immediately after finding 15's own discussion) and §6
(finding 30), cross-referenced back to "round 8 finding 30/31/32" the same way prior revisions
cross-referenced their own round's findings.

**This revision (10) resolves red-team round 9's five findings — a design-fix pass responding to round 9,
not a tenth red-team round, same discipline named above, and intended as the LAST such pass before this
draft moves from pure design review to an actual, executed positive-control test.** **Finding 33 (High,
new — I2)** — round 9 verified that `grant_path()`'s `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` flags
(finding 14's own already-verified citation, `app_container_profile.cpp:165`) stamp an independent,
already-materialized copy of the SID's ACE onto every child object created inside the granted directory
while the grant is live — a point-in-time copy, not a live reference — so finding 31's own
`revoke_path(dir)` fallback, which touches only `dir`'s own security descriptor, blocks new directory
listing/enumeration under the shared SID but leaves `args.json`, `out.json`, and the source-document copy
each independently reachable by full-path open for as long as they survive on disk, directly falsifying
finding 31's own written claim that the fallback closes off "this stranded directory's contents." §4b
below (immediately after finding 32's fix) now names the real fix: the fallback calls `revoke_path()` not
once but against a fixed, small, enumerable set of FOUR known paths — `dir` itself plus its (up to) three
known children (`args.json`, a newly-named-for-the-first-time fixed literal `source` for the source
document copy — closing a small gap this draft left implicit since Revision 2, never a
`TreeSetNamedSecurityInfoW`-class recursive primitive this codebase does not have and does not need — and
`out.json`), with a missing child (the call failed before that file was ever written) treated as trivially
satisfied rather than an error. The design's own safety claim is corrected in place, not merely
re-asserted: it now states precisely that content reachability is closed only when all four revoke
attempts succeed or are trivially satisfied, and names exactly which object remains reachable when one
does not. **Finding 34 (Low-Medium)** — resolved as part of finding 33's own fix, not left open: `revoke_path()`'s
three Win32 calls request only `READ_CONTROL`/`WRITE_DAC`, access rights NTFS's `IoCheckShareAccess`
sharing-violation check does not arbitrate (the same real, documented reason `icacls`/`takeown` can repair
permissions on a file another process holds open) — this reasoning is identical for the directory-level
`revoke_path(dir)` call and every per-child `revoke_path()` call added by finding 33's fix, so it resolves
finding 34 uniformly for both, not just the directory, and is why finding 33's fix adds no new bounded-retry
loop around any of the four `revoke_path()` calls. This conclusion is argued from `revoke_path()`'s own real
body and documented Win32 access-check semantics, the same "argued, not yet measured" standard this draft's
own central claims already carry, not from an executed test — named explicitly, not left ambiguous.
**Finding 35 (Medium)** — §4b now states explicitly, next to finding 31/32's own discussion, the real
answer round 9 found by reading `tool_pipeline.hpp`'s deadline-enforcement point and `agent_session.hpp`'s
`report_progress` bracket directly: cleanup (including finding 31's own retry-plus-backoff loop and its
`revoke_path()` fallback) runs synchronously inside `Tool<>::invoke()`'s own call stack, entirely outside
both `exec_wall_ms` (which bounds only the worker's in-sandbox execution) and `ctx.deadline` (checked only
at the call boundary, never mid-call) — a slow cleanup sequence therefore cannot itself cause an
otherwise-successful call to fail on the deadline, but it does add real, unsized, uncapped latency to the
call's own Reply-return time on the failure branch specifically, not counted against or exempted from any
cap this draft names. **Finding 36 (Medium)** — §6 below adds, alongside finding 30's own cost disclosure,
the honest residual round 9 found: finding 31's own retry backoff is, on the natural reading, a blocking
sleep on whatever thread is running `Tool<>::invoke()` — a sequential caller's thread, or, under ADR-160's
parallel-batch scheduler, one of `rt::ThreadPool`'s own worker threads — and a transient
`ERROR_SHARING_VIOLATION` (an AV/indexing scanner sweeping a batch of just-created scratch directories, the
same failure class finding 31 already names) is plausibly a CORRELATED failure across several
concurrently-dispatched calls of the same batch, not an independent one; several worker threads could each
be independently blocked in their own bounded retry loop at once, a real, bounded-per-call but
unsized-in-aggregate contributor to worst-case batch latency, not sized or mitigated here. **Finding 37
(Low)** — the stale bolded conclusion round 9 found ("**`revoke_path()` is not built for v1**," in the "Why
not `revoke_path()` either" paragraph, left unedited by Revision 9's own "Correction" paragraph immediately
after it) is corrected in place, exactly as its own fix direction asked, to
"**`revoke_path()` is not built as the PRIMARY retirement mechanism for v1**" with an inline forward
pointer to the Correction paragraph that already explains the one exception, rather than left standing as a
now-false, unqualified stopping point. §4b's own heading and revision-history note above it, and §8's
"Resolved by Revision 10" entry, are updated to match; §8 also adds a short closing note stating plainly
that this is the last pure-design-review pass intended in this series — the central NTFS-deletion-destroys-
ACE claim, and now finding 33's own per-child-revoke extension of it, remain argued from code-reading and
documented Windows semantics, not from an executed test, and the real next step is building that
positive-control test, not another red-team round on paper. The "Red-team round 9" section below is left
unmodified as historical record; this pass's fixes are in §4b (findings 33/34/35/37, immediately after
finding 32's own discussion) and §6 (finding 36), cross-referenced back to "round 9 finding 33/34/35/36/37"
the same way prior revisions cross-referenced their own round's findings.

Continues `009-Plugin-and-Extension-System.md` §7's "Document extraction (text layer)" catalog row,
specifically the third this row names ("...and a DOCX/PPTX/XLSX structural parser") that
`decisions/ADR-106-document-extraction-catalog-candidate-license-substitution.md` §3 explicitly scoped
itself away from: "this pass did not do the research needed to find a permissively-licensed C++
equivalent... defers DOCX/PPTX/XLSX to a separate follow-up." This draft is that follow-up's design
half — a license/architecture ruling plus a tool shape, not an implementation.

## 1. What already exists, reused rather than re-invented

- **The result shape.** `ReadContent`'s bounded-preview + `BlobRef`-promotion pattern
  (`read_content_detail::build_reply`) and `ExtractPdfText`'s own standalone copy of it
  (`extract_pdf_text_detail::build_reply`, `include/agentengine/tools/extract_pdf_text.hpp`) — 006 §7's
  token-budget rule does not care what produced the extracted text. Whatever this draft's tools return
  goes through the same discipline (§5 below).
- **The sourcing.** `url`/`path` `Args`, exactly-one-of-two, with standalone `fetch_via_url`/
  `fetch_via_path` helpers reusing only `read_content.hpp`'s free `parse_read_content_url`/
  `net_out_target_string` parsing helpers, own capability checks (`find_net_out`/`find_fs_read`), own
  error codes — `ExtractPdfText`'s real, shipped choice (standalone over a shared `fetch_source_bytes`,
  per that draft's own §2) and the OCR draft's own re-confirmation that two real precedents choosing
  standalone is itself evidence for this codebase's judgment. This draft reuses that pattern a third
  time rather than reopening the question.
- **The companion-skill mechanism.** `builtin_skills_detail::parse_builtin()` + `InlineSkillSource`,
  `builtin_skills.hpp`'s real, tested pattern (§8f). Reused directly; see §7.
- **`NativeJailBackend::create()`/`exec()` one-shot isolation** (Windows) / `LinuxNativeJailBackend`
  (Linux) — Track B's real, verified-on-both-platforms conformer for a from-scratch native worker
  binary, IF this draft ends up recommending a native C++ parser (§2a). **Revision 4 (round 3 finding
  11): no v1 tool in this draft ends up on this route** — §2c collapses the hybrid to all-route-(b) —
  but this mechanism is still cited below because checking it directly (its `instances_` map, not just
  its scratch-filename helper) is exactly what finding 11's evaluation required, and what it found
  (§2c, §4b) is real, disclosed, and load-bearing to that conclusion, not moot.
- **`MediatedPythonRunner`/`NativeJailBackend::create_python_worker()`** — the OTHER real, shipped
  isolation mechanism this codebase has, backing the embedded CPython interpreter (008 §1b, 010
  Python-Code-Interpreter). Verified directly against the code (not assumed from the planning docs)
  because whether this draft's tools ride on THIS mechanism instead of Track B's is the headline
  question — see §2b/§2c.
- **`MediatedFileSystemAdapter`/`open_within_mount_root` (Revision 7, round 6 finding 20).** The same
  ADR-014-Judged, TOCTOU-safe, handle-then-verify-real-location primitive `dispatch_open` itself already
  relies on (`worktree_mount_fs.cpp:152`), wrapped as a ready, cross-platform (`mediated_filesystem_adapter.cpp`/
  `_posix.cpp`) `FileSystemAdapter` conformer usable standalone against any host directory, with no
  worktree/Ref/object-store dependency — verified directly this pass because whether the host's own
  `args.json`/`out.json`/source-document reads and writes could reuse it, instead of a bare
  `std::ifstream`/`std::ofstream`, was this revision's own headline question. See §4b, finding 20's fix.

## 2. The architecture fork — investigated, not assumed

009 §7's own catalog text names "Docling/MarkItDown-class" as the shape wanted, but both are Python
frameworks, not embeddable C++ libraries — ADR-106 §3's own words for exactly why it stopped short of
ruling here. Two real routes exist; both were actually checked, not guessed.

### 2a. Route (a): a native C++ `Tool<>`, Track B's PDFium pattern

Checked directly against each candidate project's own license file/PyPI-equivalent (npm/crates.io
analog: GitHub repo + README), the same discipline ADR-106 §2 and the OCR draft §2 used:

- **XLSX has a real, PDFium-tier candidate — in fact two.** `tfussell/xlnt` (MIT, verified against its
  own `LICENSE.md`, cross-platform C++14, xlsx-only, bundles PugiXML (MIT) + miniz (public domain/MIT)
  + utfcpp (Boost) for zip/XML — all permissive, no conflict) and `troldal/OpenXLSX`
  (BSD-3-Clause, actively maintained — a June 2026 release with conditional-formatting/worksheet-
  protection/comment-support commits through 2025-2026 — reads AND writes, also PugiXML-based with a
  miniz-wrapper ("Zippy") or optional libzip for the zip half). Both are ordinary CMake projects, real
  and checkable the way `bblanchon/pdfium-binaries` was for PDFium. **Revision 4 (round 3 finding 11):**
  this library-maturity case for XLSX staying native remains real and undisputed on its own terms —
  what changed is whether it is worth PAYING FOR, once weighed against the aggregate cost of a second,
  fully separate isolation mechanism (§2c below reaches the real, checked conclusion: no, not for v1 —
  `ExtractXlsxCells` moves to route (b) instead). xlnt/OpenXLSX are not disqualified, dropped, or found
  unsound here — they are named, per this draft's own disclosure discipline, as a real, live option to
  revisit (§2c, §8) if a future revision's actual measurement of the per-call interpreter-startup cost
  (§6) makes route (b) look worse for this one format specifically.
- **DOCX has one real candidate, materially smaller-scale than PDFium's ecosystem — and, checked
  directly against its live repository state (round 2 finding 7, GitHub API, not its own README),
  genuinely dormant, not merely "smaller-scale."** `amiremohamadi/DuckX` (MIT, verified against its own
  `LICENSE`) reads and writes .docx, depends on a zip utility + PugiXML — real, ~500 stars' worth of
  historical exposure — but its last push was **2024-06-12** (over two years before
  round 2's pass), its one tagged release (`v1.2.2`) is from **2019-11-05** (~7 years old), and it
  carries **34 open issues** with no recent maintainer activity closing any of them. This is a
  materially different trust posture than xlnt/OpenXLSX's more established, currently-maintained
  footing — the same "real but not a like-for-like substitute for a Chromium-fuzzed decoder" caution
  the OCR draft raised about unofficial Tesseract binary mirrors, now applying to source-code maturity
  rather than binary-distribution trust. §2c resolves what this means for the actual route choice,
  applying the identical currency-vs-exposure reasoning §2c already applies to PPTX's own
  Aspose.Slides FOSS comparison below (round 2 finding 7's own point: the two comparisons must use the
  same lens, not a star-count-only one for DOCX and a currency-aware one for PPTX).
- **PPTX has a real candidate — Revision 1's "no viable candidate at all" claim was wrong (red-team
  finding 5), corrected here by actually checking it.** `iharob/libpptx` still 404s (that part of the
  original finding holds). But `Aspose.Slides FOSS for C++` was actually checked this pass, not
  waved off as "awaiting the project owner's own review" the way Revision 1 did: its repo is
  `github.com/aspose-slides-foss/Aspose.Slides-FOSS-for-Cpp`; its `LICENSE` file, fetched directly
  (not a marketing page), is the real MIT License text ("Copyright (c) 2026 Aspose Pty Ltd... free of
  charge, to any person obtaining a copy..."); its README states it "builds the Office Open XML
  package itself, so it needs no Microsoft PowerPoint installation, no COM interop and no other
  proprietary runtime" — an explicit, direct denial of the "wrapper around a commercial SDK" trap this
  section's own methodology exists to catch. It depends on pugixml 1.14 (public headers) + miniz 3.0.2
  (linked privately) — the SAME permissive stack xlnt/OpenXLSX already use above, no license conflict.
  It is a genuine from-source CMake 3.20+ project (C++20, static archive, `FetchContent` integration),
  with real CI across Ubuntu 22.04/24.04 (GCC/Clang), macOS (AppleClang), and Windows 2022/2025 (MSVC),
  including AddressSanitizer runs and conformance testing against `python-pptx` output. No
  watermarking, no evaluation limit, no "community edition" functionality cap was found anywhere in
  the README or LICENSE — this is not the free-tier-of-a-commercial-product trap ADR-106 §4 and this
  draft's own methodology were built to catch; it is a real, separate, permissively-licensed project.
  **The real, disclosed caution that DOES apply**: 2 GitHub stars, 1 fork, 47 commits total, and no
  tagged release at all ("no release has been tagged" per the repo's own packaging notes) — a real,
  early-stage engineering-maturity concern for a catalog row 009 §7 itself frames as "parsing hostile
  files." **Round 2 finding 7 corrects the comparison this caution was originally weighed against**:
  Revision 2 called this "materially EARLIER-stage... than even DuckX's already-flagged caution" on
  star-count and release-tag presence alone; checked directly against both repos' live state (GitHub
  API, not either README), the real comparison is two DIFFERENT cautions, not a strictly-worse one —
  Aspose.Slides FOSS is untested/pre-release but under active, current, near-daily development;
  DuckX has an actual tagged release but has not been pushed to in over two years and carries 34 open,
  un-triaged issues. Neither is strictly safer than the other on the "parsing hostile files" axis: an
  unreleased library's API surface may still shift, but an unmaintained one's known bugs will not get
  fixed regardless of how long ago they were reported. This is not a license or existence problem
  either way — it is a real, disclosed, now-correctly-characterized engineering-maturity tradeoff. See
  §2c for how this reshapes the final recommendation for BOTH DOCX and PPTX, not just PPTX.
- **No prebuilt-binaries mirror exists for any of the three**, unlike PDFium's
  `bblanchon/pdfium-binaries`. Every real candidate here is an ordinary, from-source CMake build — the
  OCR draft's own "materially different, and in one way easier, vendoring shape than PDFium's" finding
  for Tesseract/Leptonica applies again: none of xlnt/OpenXLSX/DuckX need Chromium's `gn`/`ninja`/
  `depot_tools` toolchain, but none has PDFium's one-line `FetchContent_Declare(URL ...)` convenience
  either, and (unlike Tesseract/Leptonica, which share one build) this route would mean vendoring TWO
  OR THREE separate libraries from source for one catalog row.
- **Real advantage this route has that route (b) does not (§2b): ordinary cross-platform C++.** xlnt,
  OpenXLSX, and DuckX are all plain CMake C++ projects with no Windows-only build path — unlike route
  (b)'s inherited platform gap below.

**Corrected finding (Revision 2)**: this route delivers a real, PDFium-tier XLSX story, a
usable-but-smaller-scale DOCX story, and a real-but-early-stage PPTX story — not the "no PPTX story at
all" Revision 1 claimed. Route (a) is no longer disqualified by a dead end; §2c below reasons about
what to actually build from this corrected landscape, per format rather than as a single all-or-nothing
route choice, since §3 already committed to three independent tools with independent Reply shapes.

### 2b. Route (b): Python-mediated, via `python-docx`/`openpyxl`/`python-pptx`

**Revision 4 framing note (round 3 finding 11 — supersedes the Revision 3 note below, kept struck-through
in spirit rather than deleted per this draft's own history-preserving discipline)**: §2c now adopts
route (b) for **all three tools** — `ExtractDocxText`, `ExtractPptxText`, AND `ExtractXlsxCells`.
`openpyxl`'s entry below is no longer "kept for completeness... non-load-bearing" — it is a real,
load-bearing dependency now, evaluated with the same rigor as `python-docx`/`python-pptx` throughout
this section. (History: Revision 2 had narrowed this section to `python-pptx` only, moving
`python-docx`/`openpyxl` both out; Revision 3/round 2 finding 7 reopened `python-docx`, leaving
`openpyxl`/XLSX untouched; Revision 4/round 3 finding 11 reopens `openpyxl`/XLSX too, on real,
checked evidence — see §2c.)

`docs/planning/v1-office-user-toolkit.md` §3 names these three as "pure-Python packages (no native
extension to build or pin per platform)" and gives all three an MIT license. **Checked directly against
each package's own PyPI metadata (`requires_dist`), not trusted from that table:**

| Package | License (verified) | Real dependency chain |
|---|---|---|
| `openpyxl` | MIT | `et-xmlfile` (MIT) — genuinely pure Python, transitively, no native extension. The planning doc's claim holds for this one. |
| `python-docx` | MIT | `lxml>=3.1.0` (BSD-3-Clause but a **compiled C extension** wrapping libxml2/libxslt), `typing_extensions` (no declared license field, trivial pure-Python type stubs) |
| `python-pptx` | MIT | `lxml>=3.1.0` (same as above), `Pillow>=3.3.2` (native/compiled, already route (b)'s own §4 "native/binary-wheel" tier in the planning doc — for a DIFFERENT reason there), `XlsxWriter>=0.5.7` (BSD-2-Clause, pure Python) |

**Correction to the planning doc**: `python-docx` and `python-pptx` are **not** "no native extension to
build or pin per platform" — both transitively require `lxml`, a compiled extension, and `python-pptx`
also pulls in `Pillow`. This does not block the route (both already have widely-available prebuilt
wheels for Windows/Linux/macOS, unlike a from-source C++ build), but it is a real reclassification: all
three packages land closer to numpy/pandas's existing tier-3 vendoring shape (a pinned, wheel-resolved,
`pip install --target` step) than to a "genuinely zero extra build cost" pure-Python tier. All four
packages' own licenses (MIT ×3, BSD-3 lxml, BSD-2 XlsxWriter, Pillow's MIT-CMU "Pillow license" per the
planning doc's own already-checked §4 entry) are permissive — **no license conflict for any of them**,
same conclusion shape as the OCR draft's Tesseract/Leptonica finding, just for a different dependency
set.

**Real lxml version to pin (Revision 2, resolves red-team finding 3's version-floor gap)**: the
`lxml>=3.1.0` floor above and finding 3's own "pin `>=5.0`" suggestion are both superseded by a more
specific, freshly-checked fact: `resolve_entities` defaulting to `'internal'` (XXE-safe) became lxml's
default for the ordinary XML/HTML parsers in 5.0, but `iterparse()`/`ETCompatXMLParser()` kept the
UNSAFE `resolve_entities=True` default past that point — a separate, real vulnerability
(`CVE-2026-41066` / `GHSA-vfmq-68hx-4jfw`), fixed only in **lxml 6.1**, which made the safe default
apply to every parser including `iterparse()`. The latest stable release as of this pass is **6.1.2**
(2026-08-18, PyPI). **Design decision**: pin `lxml==6.1.2` (not merely "`>=5.0`"), specifically because
this closes the `iterparse()` gap too, regardless of whether `python-pptx`'s own internal parsing calls
`iterparse()` anywhere (not traced call-by-call this pass) — pinning past the fix is cheaper and safer
than auditing every internal call path for parser selection. This is a real, checkable, actionable pin,
not a floor with an unverified safety margin.

**Real `openpyxl` version to pin (Revision 4, round 3 finding 11 — needed now that `ExtractXlsxCells`
rides this route too)**: checked directly this pass (PyPI/`readthedocs.io`, not assumed from Revision 1's
older citation) — the latest stable release is **3.1.5**. **Design decision**: pin `openpyxl==3.1.5`,
the same "pin a real, current, checkable version" discipline as the `lxml==6.1.2` pin above, not a
floor. One real, disclosed difference from the `python-docx`/`python-pptx` legs: `openpyxl`'s own
dependency chain has no `lxml` in it at all (this table, unchanged since Revision 1) — it parses XML
via Python's stdlib `xml.etree.ElementTree`, not `lxml`, so the `lxml==6.1.2` pin offers `openpyxl`'s
own worker script no protection whatsoever, and round-1 finding 3's cited historical CVE
(`openpyxl<=2.3.5`'s XXE exposure, over 100 releases behind 3.1.5) is not itself evidence that the
*current* stdlib-`ElementTree` code path is safe by construction — it is evidence the OLD, different
code path this pin moves well past was not. Whether `xml.etree.ElementTree`'s own entity-handling needs
a comparable explicit guard for this specific worker script (the zip-bomb `infolist()` pre-check below
still applies regardless, since that guards the container, not the XML parser) is a real, checkable
question this pass does not chase further — named here as a genuinely open item, the same honesty this
draft already applies to unset numeric caps (§6), not assumed safe by omission.

**What wiring this into the "preinstalled policy image" concretely requires — read directly against the
real mechanism, not assumed:**

1. **Package vendoring (Revision 4, round 3 finding 11: scoped to ALL THREE tools' real transitive
   dependencies now that the hybrid is collapsed)**:
   `CMakeLists.txt`'s existing `AGENTENGINE_VENDOR_PYTHON` block (~line 736-793, verified directly this
   pass — its real, current pin is `set(AE_VENDORED_PACKAGES_PIN
   "numpy==${AE_VENDORED_NUMPY_VERSION};pandas==${AE_VENDORED_PANDAS_VERSION}")`, `2.3.3`/`2.3.3` today,
   installed via `execute_process(COMMAND ... python.exe -m pip install --no-input --target
   <vendored_python_packages> ...)`, guarded by a stamp file so a routine reconfigure doesn't re-hit
   PyPI) already does exactly this shape for numpy/pandas. Extending `AE_VENDORED_PACKAGES_PIN` with
   `python-docx==<pin>;python-pptx==<pin>;openpyxl==3.1.5;et_xmlfile==<pin>;lxml==6.1.2;Pillow==<pin>;
   XlsxWriter==<pin>;typing_extensions==<pin>` is the same mechanism, not a new one — real, low-novelty
   implementation work, with real, checked pins (above) rather than unpinned floors. `python-docx` and
   `python-pptx` share the SAME `lxml==6.1.2` pin (§2b's table above: both declare `lxml>=3.1.0`), so
   that is one shared entry, not two separate pins to keep in sync; `openpyxl`/`et_xmlfile` are a
   genuinely separate, `lxml`-free pair (this section's own `openpyxl` version-pin paragraph above).
2. **The import allowlist is a SEPARATE gate from being on disk, and covers TRANSITIVE imports too —
   verified against the real enforcement code, not assumed.** `python_worker_mediation.cpp`'s
   `is_name_allowed(top_level_name)` (called from the installed `sys.meta_path` finder's
   `Finder_find_spec`) checks against `g_package_policy_allowlist`
   (`MediatedPythonConfig::package_policy_allowlist`, a flat `std::unordered_set<std::string>`) for
   **every** import that reaches the finder — not just what the caller's own top-level `import pptx`
   names. Because a meta-path finder intercepts every `__import__` once installed, `pptx`'s own internal
   `import lxml.etree` would ALSO need `lxml` present in that same allowlist, and it would likewise
   need `PIL`/`xlsxwriter` present — a real, concrete consequence of the existing mechanism this draft
   is the first to spell out rather than leave implicit; `docx`'s own internal imports need the same
   treatment (`lxml`, `typing_extensions`); `openpyxl`'s own internal imports need `et_xmlfile` present
   (its one real transitive dependency, this section's own table) and nothing else, since it has no
   `lxml` dependency to worry about (this section's `openpyxl` version-pin paragraph above). Each
   dedicated per-call `MediatedPythonRunner` (§4b) is configured with the allowlist for ONLY the ONE
   library its own tool actually imports — `ExtractDocxText`'s worker never needs `pptx`/`PIL`/
   `xlsxwriter`/`openpyxl`/`et_xmlfile` on its allowlist, `ExtractXlsxCells`'s worker never needs
   `docx`/`pptx`/`lxml`/`PIL`/`xlsxwriter`, and so on for each of the three — a real, free narrowing the
   per-call-dedicated-runner shape (finding 4's fix) already gives for free, named here explicitly
   rather than left implicit. (Revision 4's all-route-(b) recommendation covers `docx`/`pptx`/`openpyxl`/
   `lxml`/`PIL`/`xlsxwriter`/`typing_extensions`/`et_xmlfile` in total, spread across three DISJOINT
   per-tool allowlists, never one shared allowlist across tools.)
3. **The worker process this rides on is the jailed `agentengine_python_worker`
   (`python_worker_main.cpp`/`python_worker_mediation.cpp`), spawned via
   `NativeJailBackend::create_python_worker()`** — a real, already-shipped, AppContainer+Job-Object-
   jailed OS process, the SAME class of isolation Track B's PDFium worker gets via a different
   mechanism (one-shot `create()`/`exec()`) — not a weaker boundary, a differently-shaped one (§4).

**Two real, disclosed problems with this route, found by reading the actual code, not assumed away —
one of them (worker lifecycle) is resolved for the tools that ride this route (§2c/§4), the OTHER
(capability-layer authority, round 2 finding 6) needed a further, separate fix on top of the
worker-lifecycle one — see §4b:**

- **The entire `MediatedPythonRunner`/`create_python_worker()` subsystem this route depends on is
  Windows-only today.** `native_jail_backend.hpp`'s own file-top comment says so directly ("...Windows
  half"); `create_python_worker()` exists only in `native_jail_backend.{hpp,cpp}`, not in
  `linux_native_jail_backend.{hpp,cpp}`; and `CMakeLists.txt`'s own `AGENTENGINE_BUILD_PYTHON_RUNNER`
  block hits `message(FATAL_ERROR ...)` on any non-Windows configure ("POSIX wiring
  (python3-config/pkg-config --embed...) is NOT ATTEMPTED this pass"), citing ADR-002 §8/§9's own named
  gap. This is a pre-existing residual of a subsystem this project already committed to building out —
  not a new gap this draft introduces. **Revision 2 narrowed its blast radius to one tool; Revision 3
  (round 2 finding 7) widened it back to two; Revision 4 (round 3 finding 11) widens it to all three** —
  since §2c now collapses the hybrid, this Windows-only gap affects `ExtractDocxText`, `ExtractPptxText`,
  AND `ExtractXlsxCells` — no tool in this draft's v1 scope keeps a cross-platform-native fallback. This
  is a real, named cost of finding 11's conclusion, not swept under the rug by it — see §2c's own
  weighing of this against the alternative (a second, fully separate isolation mechanism for one format).
- **No first-party `execute_code`-plumbing `Tool<>` exists yet to build alongside.** The only `Tool<>`
  wired to `MediatedPythonRunner` in this tree today is `ExecuteCodeTool` in `tools/cli_chat.cpp`, a
  manual CLI validation harness, not a shipped, tested, `include/agentengine/tools/`-resident first-party
  tool the way `ReadContent`/`ExtractPdfText` are. `ExtractDocxText`/`ExtractPptxText`/`ExtractXlsxCells`
  (Revision 4) would be the FIRST production `Tool<>`s built on this plumbing — whichever ships first
  inherits this residual; the other two get to reuse a proven pattern, not repeat the risk.
- **The scoping/cost tension flagged in Revision 1 is resolved at the worker-lifecycle level (§4b's
  finding-4 fix), with a SEPARATE capability-layer gap (round 2 finding 6) fixed alongside it, and a
  THIRD, per-call backend-freshness fix (round 3 finding 9 plus finding 11's own investigation) fixed
  alongside those.** ADR-002 §5.5.6 scopes `MediatedPythonRunner` to "one process per session," and
  `tools/cli_chat.cpp`'s own `CodeActRunnerBinding` enforces a session's shared CodeAct runner has
  exactly one exclusive claimant. §4b below resolves the share-vs-dedicate choice this section
  previously left open: a dedicated, narrowly-mounted `MediatedPythonRunner` per call (each of the three
  tools gets its own, never the session's shared CodeAct runner) — chosen specifically because narrowing
  an ALREADY-initialized runner's `mount_roots` after the fact is not something the current
  `initialize()`-time-only ACL-grant mechanism supports (red-team finding 4), so sharing cannot be made
  I2-safe without inventing a re-narrowing mechanism this codebase doesn't have. Round 2 finding 6 then
  showed narrowing `mount_roots` alone is necessary but not sufficient — `dispatch_open`'s SEPARATE
  capability-layer check needed its own explicit fix, §4b's own subsection below. Round 3 finding 9 then
  showed "a dedicated runner per call" was never itself grounded in a concrete scratch-DIRECTORY
  generation mechanism — §4b's `unique_scratch_dir_name()` fix below closes that. All three tools pay
  the CPython-interpreter-plus-package-import startup cost on every call (heavier than Track B's
  purpose-built, fast-starting `pdf_worker_main.cpp` binary) — an OCR-draft-style "new, real, per-call
  cost," not sized here (§6), now for three tools' calls, not the one Revision 2 narrowed to or the two
  Revision 3 widened to.

### 2c. Recommendation

**Revision 2 changed the recommendation to a hybrid, reached per-format on the actual evidence — not
route (b) for all three (Revision 1), and not a reflexive "route (a) is now unblocked so use it for
everything" either (red-team finding 5's own fix direction warned against exactly that kind of
un-reasoned swap). Revision 3 (round 2 finding 7) re-derived the DOCX leg of that hybrid on
corrected evidence, moving DOCX from route (a)/`DuckX` to route (b)/`python-docx`. Revision 4 (round 3
finding 11) goes one step further and asks the question no revision had actually asked yet: now that
route (b)'s full plumbing is unavoidable anyway for two of the three tools, is the hybrid's remaining
native leg (`ExtractXlsxCells`) still worth its own, fully separate isolation mechanism? Answered here
for real, not asserted — see the evaluation below, which reaches a real conclusion: no. The hybrid is
collapsed. All three tools move to route (b). Revision 5 (round 4 finding 13) re-runs this same
evaluation after one of its three original supporting facts changed upstream — see the "Net evaluation"
below, which reaches the same conclusion again on the two costs that remain real.**

**What XLSX gains by staying native (real, not dismissed)**: everything §2a already established —
xlnt/OpenXLSX are PDFium-tier-mature, cross-platform, permissively-licensed, currently-maintained
(OpenXLSX's own June 2026 release), ordinary CMake builds with no dead end. Staying native would also
mean `ExtractXlsxCells` alone keeps working on Linux today, without waiting on POSIX support for the
`MediatedPythonRunner`/`create_python_worker()` subsystem (§2b's own Windows-only disclosure, ADR-002
§8/§9), and it would avoid paying a per-call CPython-interpreter-plus-package-import startup cost that
route (b)'s other two tools already pay (§6). Both are real, genuine advantages — this is not a case
where route (a) turns out to have been a mistake all along.

**What XLSX loses by staying native — weighed against those gains for the first time, not left
unweighed as round 3 finding 11 found it**: §4a's own framing calls route (a) "already-solved... a
direct reuse of Track B's real, shipped pattern, not a new mechanism" — checked directly this pass
(not assumed from that framing), this is not accurate. Staying native for v1 actually requires, from
scratch, for ONE tool out of three:

1. **A real, separate vendoring effort** — `FetchContent`-ing xlnt or OpenXLSX from source (§2a: no
   PDFium-style prebuilt-binaries mirror exists for either), a brand-new CMake target, a brand-new
   native worker binary analogous to `pdf_worker_main.cpp` (§4a/§8's own admission: "not designed in the
   same code-level detail" as Track B's), and its own `NativeJailBackend` wiring — while the
   Python-mediated vendoring/allowlist/capability-grant plumbing (§2b items 1-3, §4b) is being built
   REGARDLESS, for the other two tools, whether XLSX joins it or not.
2. **A native-C++ zip-bomb guard written from scratch against miniz's own API** (§4a's own disclosed,
   still-open item) — a real, separate implementation task, distinct from and not reusable across the
   `zipfile.infolist()` guard §4b already designs for the Python-mediated tools.
3. **(Revision 4) A newly-discovered concurrency-safety fix Track B itself had never built, that a
   native XLSX worker copying "Track B's own, already-proven shape" (§4a's own words) would have
   silently inherited as broken — since fixed, see below.** This was the headline finding of that
   pass's own investigation into finding 11's own explicit ask ("does Track B's `ExtractPdfText`
   already handle its own concurrent-call scratch-uniqueness correctly under ADR-160's parallel-batch
   scheduler?"), checked directly against the real, current code, not assumed either way:
   - `extract_pdf_text.hpp`'s own `unique_scratch_filename()` (`:262-271`) — the ONE mechanism this
     draft cited as reused precedent (§1) — IS correctly concurrency-safe: a `static std::atomic<
     std::uint64_t> counter` plus the process pid, verified thread-safe by construction, does exactly
     what its own comment claims ("collision-safety across CONCURRENT calls within one process"). This
     half of Track B's precedent is real and safe to copy — see §4b's `unique_scratch_dir_name()` fix
     below, which does exactly that, adapted to directory-naming.
   - But the worker-INVOCATION half of the same pattern is not safe. `extract_pdf_text_detail::
     invoke_worker` (Windows) constructs `static native_jail::NativeJailBackend backend;` — ONE
     process-wide instance, shared by every call to `ExtractPdfText`/`ExtractPdfImages`/
     `ExtractPdfMetadata`/`ExtractPdfToc` (all four share this pattern, verified via the same file).
     `NativeJailBackend::instances_` (`native_jail_backend.hpp:311`) is a plain
     `std::unordered_map<std::string, std::unique_ptr<Instance>>` — verified directly against
     `native_jail_backend.cpp`'s `create()` (`instances_.emplace(id, ...)`, line 224),
     `create_python_worker()` (`instances_.emplace(id, ...)`, line 796), `exec()`/`exec_session()`
     (`instances_.find(...)`, lines 230/409/920/1047), and `destroy()` (`instances_.erase(...)`, line
     432) — **none of these accesses are guarded by any mutex**. `include/agentengine/rt/thread_pool.hpp`'s
     `ThreadPool` (the real, shipped executor `07ae98e`'s ADR-160 parallel-batch tool-call scheduler
     runs jobs on) genuinely runs jobs on separate OS worker threads concurrently, by design (`worker_loop`
     drives each job "OUTSIDE the lock... the entire point of having N workers," that file's own
     comment). **The consequence, checked, not hypothetical: two concurrent calls to any of Track B's
     four PDF tools today already race on this shared static's unsynchronized `instances_` map under
     ADR-160's real, shipped scheduler** — an unsynchronized `std::unordered_map` mutated from multiple
     threads is undefined behavior, not merely a theoretical hazard. **This is a real, live,
     already-shipped defect in Track B's code, found as a side effect of this evaluation, and it is
     explicitly OUT OF SCOPE for this draft to fix** — Track B's own drafts/ADRs own that code. It is
     disclosed here, prominently, because it directly bears on finding 11's own question: a native
     `ExtractXlsxCells` worker built "exactly Track B's shape" would inherit this exact race the moment
     two `.xlsx` extractions (or an `.xlsx` extraction racing a concurrent PDF-tool call sharing the
     SAME static backend, since all four PDF tools already share one) run concurrently — which
     ADR-160's own shipped scheduler makes a real, live scenario, not a hypothetical one. Fixing it
     would mean either adding synchronization to `NativeJailBackend::instances_` (Track B's own
     defect, not this draft's to design) or, for a NEW XLSX worker specifically, deliberately NOT
     reusing the shared static backend pattern — itself a deviation from "already-solved... direct
     reuse" that §4a's own framing claims, meaning route (a) was not free reuse the way it read.
   - **Correction (Revision 5, round 4 finding 13): this leg is now fixed, and the paragraph above is
     historical record of a defect that no longer exists, not a live cost.** Checked directly against
     `git show 40c573e` (not trusted from its own commit message) — landed 2026-09-01, the same day this
     draft's Revision 4 text was written, one commit later: `native_jail_backend.{hpp,cpp}` and the
     Linux equivalent now guard `instances_` with a real `std::mutex instances_mutex_`, and every one of
     `create()`/`exec()`/`destroy()`/`create_python_worker()`/`exec_session()`/`refresh_python_tools()`
     now goes through the new `find_instance_locked`/`insert_instance_locked`/`erase_instance_locked`
     helpers instead of touching the map directly — verified by reading the diff, not the message: every
     raw `instances_.find/emplace/erase` call site this draft's own investigation named above is
     replaced. The fix's own repro is the SAME class this draft's own investigation used to find the bug
     (8 threads × 40 create/exec/destroy cycles) — 5/5 segfaults before, 3/3 clean passes after, 320/320
     cycles ok, zero regressions in the full native_jail/extract_pdf/pdf_text suite. This is a real,
     complete fix, not a partial mitigation or a narrowing of scope. **A native `ExtractXlsxCells`
     worker built exactly Track B's shape TODAY — a process-wide `static NativeJailBackend backend;`,
     the identical idiom `extract_pdf_text_detail::invoke_worker` uses — would inherit the FIXED,
     mutex-guarded map, not the race.** This item is no longer a real cost of route (a); it is dropped
     from the "Net evaluation" below rather than left standing as a live concern.

**Net evaluation (Revision 5, round 4 finding 13 — re-weighed on the two costs that remain real, not the
three Revision 4 stated)**: item 3 above is fixed as of `40c573e` and is no longer a cost of route (a) —
Revision 4's own framing (calling it "the headline finding of this pass's own investigation," a full
enumerated point here, a full paragraph in §4a) gave a since-resolved defect outsized weight, and this
revision corrects that rather than leaving the stale prose standing or silently dropping the item without
saying why. What remains real and unaffected by `40c573e`, because neither was ever contingent on the
concurrency bug: (1) a genuinely separate, from-scratch vendoring effort — `FetchContent`-ing xlnt/OpenXLSX,
a brand-new CMake target, a brand-new native worker binary "not designed in the same code-level detail" as
Track B's own (§8's own admission), and its own `NativeJailBackend` wiring, while the Python-mediated
plumbing is being built REGARDLESS for the other two tools; and (2) a native-C++ zip-bomb guard written
from scratch against miniz's own API, distinct from and not reusable across §4b's `zipfile.infolist()`
guard. **Does this weaker, two-legged case still justify the collapse? Re-run here, not merely asserted:
yes.** The benefits route (a) alone would buy back for XLSX are bounded to ONE tool's share of a gap that
stays real for the catalog row regardless of XLSX's own routing — `ExtractDocxText`/`ExtractPptxText`
remain Windows-only and remain per-call-interpreter-startup-cost payers no matter what XLSX does (§2b), so
keeping XLSX native does not make this catalog row cross-platform or startup-cost-free, it only spares one
of three tools from a cost the row already carries in aggregate. Against that bounded benefit, costs (1)
and (2) are not incremental tuning — they are a full second, independently-vendored, independently-tested
engineering surface (a new CMake target, a new worker binary, a new hostile-input guard) for a catalog row
whose own methodology already names "the number of DISTINCT isolation mechanisms this catalog row needs
to get right" as the dominant cost driver (below), independent of which specific defect first prompted
noticing it. A full duplicate mechanism to save one tool out of three from a cost the row cannot escape in
aggregate is not a good trade even without the now-moot concurrency argument — the collapse conclusion
holds, on weaker but still sufficient grounds. **Real, evidence-based decision, ADR-106-style, reaffirmed
on the corrected two-cost evidence: the hybrid stays collapsed. `ExtractXlsxCells` moves to route (b),
`openpyxl`-backed (§2b), following the identical §4b job shape `ExtractDocxText`/`ExtractPptxText`
already have (dedicated per-call `MediatedPythonRunner`, `args.json`/`out.json` channels, zip-bomb
pre-check, its own real `openpyxl==3.1.5` pin) — a third library import onto ALREADY-designed plumbing,
not a fourth mechanism.**

- **`ExtractXlsxCells` → route (b), Python-mediated (`openpyxl`, §2b), moved off `xlnt`/`OpenXLSX`
  (round 3 finding 11's fix, Revision 4).** See the evaluation above for the real reasoning; xlnt/
  OpenXLSX are not silently dropped — named below as a real, live option to revisit.
- **`ExtractDocxText` → route (b), Python-mediated (`python-docx`, §2b) — unchanged by Revision 4**,
  reached in Revision 3 (round 2 finding 7) and untouched by this pass's own evaluation.
- **`ExtractPptxText` → route (b), Python-mediated (`python-pptx`, §2b) — unchanged by Revision 4**,
  reached in Revision 2/confirmed in Revision 3 and untouched by this pass's own evaluation.
- **Why unifying is the coherent choice now, not an aesthetic preference finding 5's own fix direction
  warned against**: finding 5's warning was against swapping to route (a) uniformly just because it had
  "become unblocked," without weighing per-format evidence — the opposite failure mode from what
  Revision 4 does here. This revision does NOT claim route uniformity is good in itself; it reaches
  route (b) for XLSX by the SAME per-format evidence-weighing discipline §2a/2c already used for DOCX
  and PPTX, applied to XLSX for the first time this pass (the evaluation above). That the answer this
  time happens to unify all three tools onto one route is a consequence of that evidence, not a design
  goal chased for its own sake — §3's three independent `Tool<>`s/Reply shapes are entirely unaffected;
  only the execution mechanism behind them is now uniform.
- **What this narrows, concretely — revised from Revision 3's count**: route (b)'s previously-disclosed
  problems (Windows-only subsystem, no shipped first-party `Tool<>` on this plumbing yet, per-call
  worker-lifecycle/interpreter-startup cost) now apply to ALL THREE tools — a real, disclosed cost of
  this revision's own conclusion (§2b), not a reduction. What DOES shrink to zero, for the first time
  across every revision of this draft: the number of DISTINCT isolation mechanisms this catalog row
  needs to get right (one, not two), the number of native worker binaries to build/vendor/benchmark from
  scratch (zero, not one), and the number of independently-designed zip-bomb guards (one Python-side
  mechanism, reused three times, not one Python-side mechanism plus one from-scratch native one).
- **Not thrown away**: `Aspose.Slides FOSS for C++` (PPTX) and `DuckX` (DOCX) remain named, not
  silently dropped, as real, live options to revisit under the same disclosure discipline as before.
  **New this revision**: `xlnt`/`OpenXLSX` (XLSX) join that same disclosed-not-dropped list — a real,
  live option to revisit specifically if a future revision's actual measurement (§6, still unsized) of
  the per-call interpreter-startup cost, or continued pressure from the Windows-only gap, makes route
  (b) look materially worse for this one format than this evaluation currently estimates. Revisiting it
  would mean redoing this evaluation's own cost side (the vendoring effort and the native zip-bomb guard —
  **Revision 5, round 4 finding 13: no longer a third, concurrency-safety cost; `NativeJailBackend::
  instances_`'s race is fixed as of commit `40c573e`, so a revisit inherits the fixed map for free, the
  same way Track B's own PDF tools do today**) with real numbers, not reopening it on vibes.

## 3. v1 scope — three tools, not one, matching how the formats actually differ

Following Track B's own precedent (four PDF tools, not one over-generalized `extract_pdf`), and because
DOCX/XLSX/PPTX's real internal structures are genuinely different shapes (paragraphs+tables+headings vs.
a 2-D cell grid across named sheets vs. slides+shapes+speaker notes) that a single flattened "office
document text" result would either lose or force into an awkward common shape:

```
ExtractDocxText: Args { url?, path? } ->
  Reply { preview, truncated, total_bytes, paragraph_count, tables_processed, truncated_tables,
          body_text_only, blob? }
  -- paragraph text, in document order; each table rendered as a delimited block (row/cell structure
     preserved in the preview text, not modeled as a separate typed field this revision -- v1 keeps
     this to TEXT, not a structured table object, matching Track B's own "text layer" scoping, not
     029 §5b's separate VLM-tier "layout-aware... table/form structure" row). Revision 3 (round 2
     finding 7): backed by route (b)'s Python-mediated `python-docx` worker (§2c), NOT route (a)'s
     `DuckX` -- Revision 2 had moved this to native `DuckX`; round 2's re-verification of DuckX's real
     repository state (dormant, §2a) reopened that choice and §2c now lands on `python-docx` instead,
     following §4b's already-designed dedicated-`MediatedPythonRunner`-per-call job shape
     (`args.json`/`out.json` channels, zip-bomb pre-check, `lxml==6.1.2` pin) verbatim, extended to a
     second library import rather than redesigned. The Reply shape itself is unchanged across both
     revisions' backend swaps -- only the backend that fills it moved twice. **Revision 4 (round 3
     finding 10) -- a real, checked coverage gap, named explicitly rather than left implicit, the SAME
     "named, not silently folded in" discipline this block already gives PPTX's speaker notes below**:
     checked directly against `python-docx`'s own documented API, `Document.paragraphs` returns only
     BODY-level paragraphs and explicitly excludes paragraphs inside tracked-changes revision marks
     (`<w:ins>`/`<w:del>`); `Document.tables` lists only TOP-level tables ("a table nested inside a
     table cell does not appear," per that library's own docs). Headers, footers, footnotes, and
     endnotes are not reachable through either accessor at all -- as of this pass `python-docx` has no
     first-class accessor for footnotes/endnotes at all, and headers/footers require a completely
     separate API (`document.sections[i].header`/`.footer`) this tool does not call. **v1 scope
     decision (preferred fix direction, round 3 finding 10)**: headers, footers, footnotes/endnotes,
     tracked-changes-inserted/deleted text, and tables nested inside another table's cell are ALL
     explicitly OUT OF SCOPE for v1 -- named here, not silently gapped. This tool's Reply carries a new
     `body_text_only: true` field (always `true` in v1 -- a fixed, self-documenting marker rather than
     a caller having to infer the gap from documentation alone) so a caller cannot mistake `preview`/
     `blob`'s completeness for "the whole document's text," the same honesty `truncated_tables` already
     gives for the tables that ARE processed but exceed a cap. Nothing here is silently dropped the way
     `python-docx`'s own API silently drops it: a DOCX whose substantive content lives in a
     letterhead/running-title header or in academic/legal footnotes/endnotes will have that content
     absent from this tool's output, by design, in v1. Extending coverage (headers/footers are a
     comparatively cheap, separately-callable API; footnotes/endnotes and nested tables are not, per
     the gap above) is real, undesigned future work, not attempted here.

ExtractXlsxCells: Args { url?, path?, sheet_name? } ->
  Reply { preview, truncated, total_bytes, sheet_names, total_row_count, rows_processed,
          truncated_rows, merged_cells_collapsed, blob? }
  -- cell values only (not formulas, not formatting/styles). sheet_name optional -- unset means the
     FIRST sheet (not "all sheets") -- a real, deliberate v1 narrowing, named here rather than silently
     assumed ("extract every sheet of an arbitrary xlsx" can be a much larger result than this tool's
     own bounded-preview discipline was sized for). **Revision 4 (round 3 finding 11) -- backed by
     route (b)'s Python-mediated `openpyxl` worker (§2c), NOT route (a)'s native `xlnt`/`OpenXLSX`.**
     Revision 2 had moved this tool to native `xlnt`/`OpenXLSX`; round 3 finding 11's own aggregate-cost
     evaluation (§2c) reopened that choice and Revision 4 lands back on `openpyxl` instead, following
     §4b's already-designed dedicated-`MediatedPythonRunner`-per-call job shape (`args.json`/`out.json`
     channels, zip-bomb pre-check, real `openpyxl==3.1.5` pin) -- a third library import onto
     already-designed plumbing, the identical treatment DOCX got in Revision 3. This REACTIVATES the
     original, openpyxl-specific framing Revision 2 had stripped out when it moved this tool off route
     (b): opening the workbook with `openpyxl.load_workbook(..., read_only=True, data_only=True)` --
     `read_only=True` avoids materializing the full style/formatting tree this tool never returns
     (matching the Reply's own "not formulas, not formatting/styles" scoping above), and `data_only=True`
     reads formula CELLS' last-CACHED computed value rather than the formula text itself (consistent
     with "cell values only, not formulas" -- a workbook that has never been opened/recalculated in
     Excel has no cached value to read, a real, disclosed edge case this pass does not design a fallback
     for). `sheet_name` -- unlike `ExtractPptxText`'s Args, which round 2's own "Investigated, no real
     defect found" section noted has no live per-call value flowing through finding 1's `args.json`
     mechanism because XLSX stayed native -- is now the FIRST real, model-influenced per-call value this
     mechanism actually protects: it reaches the fixed script via `_args["sheet_name"]` (a genuine
     Python string from `json.load()`, §4b's finding-1 fix), used as `wb[_args["sheet_name"]]` if set,
     `wb[wb.sheetnames[0]]` otherwise -- exactly the injection-safe shape finding 1 was designed for,
     exercised for real for the first time under this revision's own route change. **Revision 5 (round 4
     finding 17) -- the same Reply-shape-level disclosure discipline `ExtractDocxText`'s `body_text_only:
     true` field already gives its own coverage gap (round 3 finding 10), extended here rather than left
     as prose-only for a materially similar class of gap.** `openpyxl.load_workbook(..., read_only=True)`
     -- the mode this tool always uses (above) -- has two real, checkable behaviors beyond the
     "not formulas, not formatting/styles" scoping already stated, verified directly against `openpyxl`'s
     own documentation and issue tracker, not assumed: (1) **merged cells silently vanish in `read_only=True`
     mode specifically, worse than `openpyxl`'s own normal-mode behavior.** In normal mode, a non-top-left
     cell of a merged range comes back as a real `MergedCell` object whose value is `None` by design -- a
     genuine signal something was merged. In `read_only=True` mode, those same cells come back as an
     ordinary `EmptyCell`, indistinguishable from a cell that was simply always blank -- a workbook whose
     content lives partly inside a merged block (merged header rows, merged label cells -- an ordinary
     spreadsheet pattern) has that content silently absent from every row after the top-left one, with zero
     signal anywhere in `openpyxl`'s own API that anything was collapsed. This tool's Reply now carries a
     new `merged_cells_collapsed: true` field (always `true` in v1, the identical fixed-marker shape
     `body_text_only` already established for DOCX) so a caller cannot mistake `preview`/`blob`'s
     completeness for "every cell's actual content," the same honesty `truncated_rows` already gives for
     rows that ARE processed but exceed a cap. (2) **`read_only=True` mode's reported dimensions are not
     independently verified.** `max_row`/`max_column` -- which `total_row_count`/`rows_processed`'s own
     iteration bound derives from -- come directly from the workbook's own `<dimension>` XML element in
     read-only mode, per `openpyxl`'s own documentation, which explicitly warns some producing applications
     set this incorrectly and names `ws.calculate_dimension(force=True)` as the caller's own required
     workaround when it is wrong -- not called anywhere in this tool's fixed script. For a catalog row 009
     §7 itself frames as "parsing hostile files" (the same framing this draft's own §4b/§6 quote repeatedly
     for the zip-bomb mechanism), an attacker-controlled or merely non-Excel-produced `.xlsx` with a wrong
     declared dimension range would silently under- or over-report `total_row_count` with no cap or
     truncation signal catching it, since from `openpyxl`'s own point of view nothing was truncated. Neither
     gap is a security/I2/I3 defect -- both are named here, in the same "named, not silently gapped" spirit
     as `sheet_name`'s own disclosure immediately above and DOCX's own finding-10 gaps, as real v1 scope
     limitations, not fixed this pass (`ws.calculate_dimension(force=True)` is a real, cheap candidate fix
     for the dimension-trust half specifically, left for a future revision rather than designed here).

ExtractPptxText: Args { url?, path? } ->
  Reply { preview, truncated, total_bytes, slide_count, slides_processed, truncated_slides, blob? }
  -- per-slide text: title + body placeholder text + any other text-frame shape's text, in shape order;
     speaker notes included as a clearly-delimited trailing section per slide (a real, separate
     python-pptx API call per slide -- named, not silently folded in as if it were the same field).
     Unchanged by Revision 2: this is still route (b), Python-mediated, `python-pptx`-backed (§2c).
```

**Deliberately out of scope for v1, named rather than silently dropped:**

- **Embedded images/charts/objects** in any of the three formats — matches `extract_pdf_images`'s own
  metadata-only precedent in spirit, but this draft does not even design that far; a future revision's
  job.
- **Cell formulas, number formatting, conditional formatting, DOCX styles/track-changes, PPTX
  animations/transitions** — all real OOXML structure this draft treats as out of scope for a "text
  layer" catalog row, matching PDF's own "text layer, not layout tier" scoping (009 §7's own table).
- **DOCX headers, footers, footnotes, endnotes, and tables nested inside another table's cell**
  (Revision 4, round 3 finding 10) — a real gap in `python-docx`'s own `Document.paragraphs`/
  `Document.tables` API, disclosed explicitly on `ExtractDocxText`'s own Reply (`body_text_only: true`,
  §3 above) rather than left for a caller to discover; see that block for the concrete API-level reason
  each is excluded.
- **Writing/generating** any of the three formats — this catalog row, like Track B's, is read-only
  extraction; `python-docx`/`openpyxl`/`python-pptx` (Revision 4's actual chosen libraries, §2c) can all
  write too, but that is a different tool with a different (much larger) capability-authority footprint
  (I2), not this draft's concern.
- **A single unified cross-format Reply shape** (what 009 §7's own "Docling/MarkItDown-class" framing
  envisioned) — deliberately not attempted; three distinct Reply shapes, matching how the formats
  actually differ, is judged more honest than forcing one shape and losing structure to fit it.

## 4. Isolation / jail integration — one mechanism for all three tools as of Revision 4

Revision 1 designed one isolation story for all three tools, assuming a single route. Revision 2 split
this by §2c's then-current per-format routing. Revision 3 (round 2 finding 7) moved `ExtractDocxText`
from §4a to §4b, leaving `ExtractXlsxCells` alone on §4a (route (a), native). **Revision 4 (round 3
finding 11) moves `ExtractXlsxCells` to §4b too** — §2c's own evaluation collapsed the hybrid, so §4b
now covers all three tools, and §4a below is kept as a historical/contingency record only (§2c's own
"not thrown away" disclosure: xlnt/OpenXLSX remain a real, live option to revisit) rather than deleted,
matching this draft's own practice of not erasing superseded-but-real analysis. §4b below carries real,
concrete fixes for red-team findings 1, 2, 4, 6, and (new this revision) 9, plus a companion
backend-freshness fix found during finding 11's own investigation — none of which apply to §4a.

### 4a. `ExtractXlsxCells` — native, one-shot native-jail worker (Track B's pattern) — **superseded for v1 by Revision 4 (round 3 finding 11); kept as a historical/contingency record, see §2c**

**Not this draft's v1 recommendation as of Revision 4** — `ExtractXlsxCells` moved to §4b's
Python-mediated route (§2c's own aggregate-cost evaluation, round 3 finding 11). This subsection is
kept, unmodified in substance, because §2c's own disclosure discipline names xlnt/OpenXLSX as a real,
live option to revisit, not a dead end — if a future revision reopens that choice, the analysis below
(and its own real, disclosed zip-bomb residual) is exactly what it would build on. Read it as "what v1
would have looked like on route (a)," not as this draft's current recommendation.

Follows `ExtractPdfText`'s own, already-shipped, already-proven shape exactly: a fixed `xlnt`/`OpenXLSX`
worker binary, spawned per call via `NativeJailBackend::create()`/`exec()` one-shot isolation (§1's own
citation of this mechanism), fed the fetched bytes at a per-call scratch file via
`grant_ro_path_once(scratch_dir())` (the SAME narrow, already-I2-clean grant Track B's PDFium worker
gets — nothing broader), producing a worker-binary-framed output record (magic/kind/index/payload_len,
`ExtractPdfText`'s own `parse_worker_output` shape reused directly) that `build_reply` (§5) consumes.
**None of red-team findings 1/2/4/6 apply to this path**: there is no `ExecRequest::source`/
Python-source-injection surface at all (a native worker binary takes no interpreted source text, so
finding 1's whole failure mode doesn't exist here); output already goes through Track B's own
purpose-built binary framing, not `output_cap_bytes` (finding 2 doesn't apply); the OS-level grant is
already the narrow, per-call `grant_ro_path_once` Track B's own worker uses, not
`MediatedPythonRunner::initialize()`'s broad session-wide `mount_roots` (finding 4 doesn't apply); and
`grant_ro_path_once`'s own caller (Track B's `create()`/`exec()` one-shot path) never routes through
`dispatch_open`/`ctx.capabilities->find_fs_read` at all — that check is specific to the interactive,
syscall-relaying `MediatedPythonRunner` worker shape §4b rides on, not Track B's one-shot binary
invocation, so finding 6's capability-layer gap has no analogue here either. This is precisely why
earlier revisions called this path "already-solved" rather than new design work: it is a direct reuse
of Track B's real, shipped pattern, not a new mechanism. **Correction (Revision 4, round 3 finding 11):
"already-solved" overstated it — at the time.** §2c's own investigation, prompted by finding 11's own
question, found that "Track B's real, shipped pattern" included a genuinely un-synchronized
`NativeJailBackend::instances_` map (verified directly, `native_jail_backend.hpp:311`/
`native_jail_backend.cpp`) that raced under concurrent calls sharing the same process-wide static
backend — real under ADR-160's shipped `ThreadPool` scheduler, not hypothetical, when Revision 4 was
written. A worker built on this path exactly as described here would have inherited that race; making
it safe would have needed either a fix to `NativeJailBackend` itself (Track B's own defect, out of scope
here) or a deliberate deviation from "reuse the existing static-backend idiom." **Correction (Revision 5,
round 4 finding 13): that specific inaccuracy has itself since been corrected upstream, one commit after
this draft's own Revision 4 text.** `git show 40c573e` (2026-09-01), verified directly: `instances_` is
now guarded by a real `std::mutex instances_mutex_` across every structural access, in both the Windows
and Linux backends, with a real before/after repro (5/5 segfaults → 3/3 clean, 320/320 cycles). A worker
built on this path TODAY — the exact `static NativeJailBackend backend;` idiom this paragraph describes —
inherits the fixed, mutex-guarded map, not the race. "Already-solved... a direct reuse of Track B's real,
shipped pattern, not a new mechanism" is therefore materially true again FOR THIS SPECIFIC CONCERN
(concurrency-safety) — the Revision 4 correction above is left in place as an accurate record of what was
true when it was written, not deleted, but a reader building on this section today should not treat
concurrency-safety as a reason route (a) is not free reuse; it no longer is one. The genuinely-remaining
reasons route (a) is not free reuse for XLSX are the vendoring/build effort and the native zip-bomb guard
below — real, independent of this bug, and not affected by its fix. See §2c for the full, re-weighed
evaluation and why the two costs that remain are still why Revision 5 continues not to recommend this
path for v1.

**One real, new residual this path DOES introduce (see finding 3's zip-bomb concern, which is
format-inherent, not route-specific)**: `.xlsx` is a zip container the same way `.docx`/`.pptx` are: a
hostile file that zip-bombs `xlnt`/`OpenXLSX`'s own (miniz-based) decompression call has the same "no
per-item cap can bound the single decompress-and-parse call" problem §4b names for `python-docx`/
`python-pptx` below. The SAME mitigating mechanism (§4b's zip `infolist()`/total-uncompressed-size
pre-check) needs a native-C++ equivalent in whichever worker binary is built for XLSX (miniz exposes
per-entry compressed/uncompressed sizes the same way Python's `zipfile.ZipFile.infolist()` does) —
named here as a real, still-open implementation-ADR task (§8), not silently assumed solved by "this is
the native, already-proven route."

### 4b. `ExtractDocxText`/`ExtractXlsxCells`/`ExtractPptxText` — Python-mediated, `MediatedPythonRunner` (route (b), with red-team findings 1/2/4/6/9/15/20/31/32/33/34/35/37 fixed; findings 14/18/19/21/22/23/25/26/27 resolved by elimination, Revision 8, per Prove pass 1)

All three tools ride the identical mechanism below — one fixed script per tool (differing only in which
library it imports and calls: `docx.Document(...)` vs. `openpyxl.load_workbook(...)` vs.
`pptx.Presentation(...)`), the same dedicated per-call worker shape, the same three data channels
(`args.json` in, `out.json` out, the zip-bomb pre-check ahead of all three). Written once here and not
duplicated per tool. **Revision 3 added finding 6's fix** (a subsection below, after finding 4's) —
round 2 found that finding 4's own fix, as Revision 2 stated it, produces a script that cannot actually
open either `args.json` or `out.json`, because `mount_roots` narrowing alone satisfies only ONE of
`dispatch_open`'s two independent checks. **Revision 4 adds finding 9's fix** (a subsection below, after
finding 6's) — round 3 found that finding 4's own fix never named how "this call's own scratch
directory" is actually generated, plus a companion backend-freshness fix found during finding 11's own
investigation into this same mechanism — and extends every fix in this section to `ExtractXlsxCells`,
which now rides this same route (§2c). **Revision 5 adds findings 14 and 15's fixes** (two subsections
below, immediately after finding 9's) — round 4 found that finding 9's own fix, while correctly closing
the cross-call file-collision race it targeted, introduced a real, unbounded Windows-ACL-growth defect
one layer down (finding 14) and left the per-call scratch directory/its contents with no cleanup at all,
plus a narrow stale-directory-reuse risk (finding 15) — both closed below without reopening finding 9's
own fix. **Revision 6 adds the fix for findings 18 and 19** (one combined subsection below, immediately
after finding 15's, replacing the shared-inheritable-root shape finding 14's fix used) — round 5 confirmed
finding 14's own ACL-inheritance mechanics are real, but found the fix, precisely because that mechanism is
real, converts `native_jail_backend.hpp`'s own documented OS-layer "backstop" into a standing, cross-call/
cross-session/cross-tool grant (finding 18), and that the fix's own prose never named the real three-step
cold-start sequence its two-level directory structure actually needs (finding 19). A fixed, pre-granted
scratch-slot pool closes both without reopening findings 9's or 6's own fixes. **Revision 7 adds the
fixes for findings 20, 21, 22, and 23** (finding 20's fix is inline in the "Fix for finding 2" subsection
below; findings 21/22/23's fixes are three subsections immediately after the pool's own point 5, above
"Why this closes finding 14 fully...") — round 6 found the pool's own new mechanisms (the host's read-back
of `out.json`, the acquire/release wipe, and the wait/wake discipline) each had a real defect on their
first adversarial pass, the same "every new mechanism gets at least one real defect found on its first
red-team round" pattern this draft's own §8 already named before round 6 ran. Revision 7 also investigated,
first, whether a fundamentally different I/O design could avoid this whole class of problem rather than
patch the pool a further time — see the revision-history note at the top of this draft for what that
investigation found (a partial collapse: `args.json`/`out.json`/the pool are retained, but the read/write
PRIMITIVE they ride on changes from a bare `std::ifstream`/`std::ofstream` to the same
`MediatedFileSystemAdapter`/`open_within_mount_root` discipline `dispatch_open` itself already relies on) —
and only fell back to fixing findings 21/22/23 in place because no better alternative existed for the
pool's own concurrency/attribution concerns specifically, AT THE TIME — Revision 7 did not check what a
directory deletion actually does to that directory's own DACL, and that turned out to be the better
alternative. **Revision 8 (Prove pass 1) retires the pool entirely, rather than fixing it a further time**
— see the revision-history note at the top of this draft, and Prove pass 1's own full text below (after
"Red-team round 7"), for the investigation. The pool existed to bound a standing OS-backstop exposure that
finding 14's own fix (a shared, inheritably-granted parent root, chosen to avoid a per-call `grant_path()`
call) manufactured; Prove pass 1 found that finding 15's cleanup — already designed, in this same section,
one subsection before finding 14's pool was ever introduced — already retires a per-call grant as a
structural side effect of deleting the directory it lives on, provided the grant lands on the call's own
leaf directly rather than on a never-deleted shared root. The fixes below for findings 1/2/4/6/9/15/20
are UNCHANGED from Revision 7 (finding 9's own per-call-fresh-directory generation mechanism was never the
problem — only what got layered on top of it by finding 14's fix was). Findings 14/18/19/21/22/23/25/26/27
are removed from the active design below and marked "resolved by elimination" in §8. **Revision 9 adds
fixes for round 8's findings 31 (a non-crash `remove_all()` cleanup failure silently stranding a
live-granted directory) and 32 (no audit trail for scratch-directory lifecycle), immediately after finding
15's own discussion below** — the first real defects round 8 found in the collapsed per-call design itself,
not in the eliminated pool; finding 30 (the collapse's own unmeasured per-call OS-call cost) is disclosed in
§6. **Revision 10 adds fixes for round 9's findings 33, 34, 35, and 37, immediately after finding 32's own
discussion below** — round 9 found the first real defect in Revision 9's own finding-31 fix itself: the
`revoke_path()` fallback strips only the scratch DIRECTORY's own DACL, leaving the already-inherited,
independently-materialized ACE on `args.json`/`out.json`/the source-document copy untouched (finding 33),
resolved by extending the fallback to a fixed, four-path `revoke_path()` enumeration (the directory plus its
three known children) and correcting the fix's own safety-claim prose in place; finding 34 (whether
`revoke_path()` shares `remove_all()`'s own sharing-violation failure mode) is resolved as part of that same
fix, on documented Win32 access-check reasoning; finding 35 (cleanup's own wall-clock relationship to
`exec_wall_ms`/`ctx.deadline`) is stated explicitly; finding 37 (a stale, now-false bolded conclusion left
unedited by Revision 9's own correction paragraph) is corrected in place. Finding 36 (correlated concurrent
retry-loop thread occupancy under ADR-160 batching) is disclosed in §6, alongside finding 30.

The key design move that makes these tools compatible with I2/I3 despite riding on the CodeAct
interpreter plumbing, stated precisely this time (Revision 1's "`sys.argv`" framing — red-team
finding 1 — did not exist as a real mechanism; `ExecRequest` has exactly three fields, `language`,
`source`, `preseeded_answers` (`sandbox.hpp:298-313`, re-verified this pass), and
`python_worker_mediation.cpp`'s `run_capturing` (~line 1180-1277) calls `PyRun_String` directly against
`request.source` with no other injection channel):

**Fix for finding 1 (parameterization, without ever formatting a model-influenced value into source
text)**: each tool's fixed script's source text is a single, byte-identical-across-every-call string
constant, containing zero string interpolation of any kind — every path and filename it references is
a fixed Python string LITERAL, chosen by the host implementation at tool-authoring time, never built by
concatenation/f-string/`.format()` against any per-call value. The one per-call value any of the three
tools needs (`sheet_name`, `ExtractXlsxCells`'s own Args field — §3's Revision 4 note: this mechanism is
no longer hypothetical for it, since XLSX now rides this same route, round 3 finding 11 — DOCX/PPTX
carry no such field today, but the same mechanism generalizes if either ever grows one) reaches the
script through a DATA channel, not a CODE channel: the host writes a small JSON object to a fixed path
under the call's
scratch mount (e.g. always `<mount_guest_path>/args.json`, never a model-influenced filename — **Revision
7, finding 20's fix: this write goes through `MediatedFileSystemAdapter::write_file`, not a bare
`std::ofstream`, the same containment discipline as the `out.json` read below**), and the
fixed script's own first lines are always exactly:

```python
import json
with open("/extract_scratch/args.json", "r", encoding="utf-8") as _f:
    _args = json.load(_f)
```

`json.load()` turns whatever the file contains into a genuine Python value (a `str`, never source
text) — there is no way for any character `_args` might contain to break out of a string literal,
because it was never inside one. This is red-team finding 1's own named fix direction (a), made
concrete: a host-controlled parameter file at a FIXED, non-interpolated path, read by a script that
never interpolates anything. `/extract_scratch` itself is a host-fixed mount `guest_path` (mirrors
`mount_roots`' existing `"/" + mount_id` convention, `mount_id` chosen by the host implementation, not
derived from anything caller-supplied) — not a new grant shape.

**Fix for finding 4 (I2: no standing, session-wide authority)**: `MediatedPythonRunner::initialize()`
(re-verified this pass, `mediated_python_runner.cpp:30-80`) grants EVERY entry of
`config_.mount_roots` `read_write = true` at worker-init time, unconditionally, independent of any
`run()` call's own capability grant — real, standing authority baked into the AppContainer ACL once,
for the runner's whole lifetime. There is no mechanism in this codebase to re-narrow an
already-initialized runner's mounts, so the ONLY way to keep this call's OS-level authority matching
what its own `find_fs_read`/`find_net_out` check actually granted is: **a dedicated
`MediatedPythonRunner`, constructed fresh for this call, with `MediatedPythonConfig::mount_roots`
containing EXACTLY ONE entry** — `{"extract_scratch": <this call's own scratch directory, and nothing
else>}` — no `tool_bridge`, no `expose_agent_files_data`/`expose_agent_ask`/`expose_agent_output`/
`expose_agent_progress` (neither fixed script ever calls any `agent.*` module; those stay at their
`false`/`nullopt` defaults). This is deliberately NEVER the session's shared CodeAct runner (which, if
one exists, was initialized with that session's full CodeAct `mount_roots` — reusing it would silently
reintroduce the exact standing-authority gap finding 4 found, since there is no way to shrink its
grant after the fact). This also settles §2b's own "share vs. dedicate" worker-lifecycle question
(Revision 1's §8 item 1, previously unresolved) in favor of dedicate, specifically for this I2 reason,
not merely for isolation-cleanliness — the interpreter-plus-package-import startup cost this pays on
every call is a real, accepted tradeoff (§6), not an oversight, now paid by all THREE tools' calls, not
the one Revision 2 narrowed to or the two Revision 3 widened to (round 3 finding 11's `ExtractXlsxCells`
move onto this same route). `ExtractDocxText`, `ExtractXlsxCells`, and `ExtractPptxText` each get their
OWN dedicated runner per call — the "extract_scratch" mount label is reused verbatim by all three tools'
fixed scripts (simpler than inventing a third/second label), which is safe precisely because each
Tool<>'s own admission is gated independently by its own declared capability ceiling (below) and each
call's OS-level mount always resolves to that ONE call's own fresh scratch directory regardless of
which of the three tools spawned it — there is no cross-tool or cross-call reachability through the
shared label, PROVIDED that directory is actually fresh per call, which finding 4's own text asserts but
never grounds in a real generation mechanism — see finding 9's fix below, which is exactly the gap round
3 found in this paragraph's own claim.

**Fix for finding 6 (round 2, Critical — I2: `mount_roots` narrowing alone does not satisfy the
SEPARATE capability-layer check `open()` actually goes through)**: finding 4's fix above narrows the
OS-level ACL grant, but `native_jail_handle_relay.cpp`'s `dispatch_open` (re-verified this pass,
lines 145-236) performs a genuinely SECOND, independent check before honoring any `open()` call — this
is not a restatement of finding 4, it is a distinct gate on a distinct layer:

1. `ws.session_config.mount_roots.find(mount_id)` (`dispatch_open:154-158`) — the OS-level grant
   finding 4's fix narrows. Satisfied by that fix alone.
2. `ctx.capabilities->find_fs_read(mount_id, mount_relative)` (read, `dispatch_open:166`) /
   `find_fs_write(mount_id, mount_relative)` (write, `dispatch_open:172`) — an entirely separate
   check against the ordinary `agentengine::cap::FsRead`/`cap::FsWrite` capability
   (`capability.hpp:114-124`), looked up against `ctx.capabilities`. **Verified directly**: `ctx` here
   is the SAME `EffectContext&` `MediatedPythonRunner::run(request, state, ctx)`
   (`mediated_python_runner.cpp:82-93`) threads straight through to `backend_.exec_session(...)` and on
   into `dispatch_worker_query`/`dispatch_open` unmodified — and `ctx.capabilities` is the SESSION's
   full granted `CapabilitySet` (`agent_session.hpp:625-630`: "session-level, set once at wiring time"),
   NOT the narrow, per-call `bound` subset `run_admitted_call` computes from the calling tool's own
   declared ceiling (`ctx.bound_capabilities`, `tool_pipeline.hpp:677` — a DIFFERENT field `dispatch_open`
   never consults). Declaring the tool's own ceiling narrowly is therefore necessary for admission but
   NOT sufficient for `open()` to succeed inside the worker — a second, separate grant must independently
   exist in the SESSION's held `CapabilitySet` for `dispatch_open`'s check to pass at all.

Without both, `dispatch_open` returns `deny("tool.capability_not_held", ...)` on the very first line of
either fixed script — `json.load()` never runs. This is the literal, checked behavior of the mechanism
findings 1/2/4 already designed, run through the exact function that would execute it — not a
hypothetical.

**The real fix, grounded in what `cap::decl::FsRead`/`FsWrite` actually support (verified directly,
`capability.hpp:387-394` and `:457-463`), not invented:** `cap::decl::FsRead<Mount>`/`FsWrite<Mount>`
are parameterized ONLY by a compile-time `fixed_string` mount label — there is no existing mechanism in
this codebase for a capability *declaration* to vary per call (it is a template non-type parameter,
fixed at `Tool<>` authoring time, the same constraint every other `cap::decl::*` tag has). Two things,
both already-existing mechanisms, reused together — no new capability kind, no new dispatch step, no
new `CapabilitySet` method:

1. **Each tool declares a real capability ceiling naming the shared mount label**: `ExtractDocxText`,
   `ExtractXlsxCells` (Revision 4, round 3 finding 11), and `ExtractPptxText` each get
   `Capabilities<cap::decl::FsRead<"extract_scratch">, cap::decl::FsWrite<"extract_scratch">>` — the
   missing declaration finding 6 found absent. This is necessary (gates admission, `admit_call`'s
   step 4/7) but, per the analysis above, not sufficient by itself.
2. **The session must ALSO be granted the matching runtime capability**, host-authored, via the SAME
   `grant(Capability)` mechanism `session_builder.hpp`'s `QuickstartSessionBuilder`/
   `ComposedQuickstartSessionBuilder` already expose for exactly this purpose ("Escapes hatch for
   anything else the constructed session needs authorized... host-authored only" — `session_builder.hpp:748-754`):
   `cap::FsRead{.mount_id = "extract_scratch", .path_prefix = "args.json", .size_cap_bytes = <a small,
   host-chosen ceiling>}` and `cap::FsWrite{.mount_id = "extract_scratch", .path_prefix = "out.json",
   .quota_bytes = <small>, .file_count_cap = 1}` — narrowed via `path_prefix` to the ONE filename each
   direction actually needs (`capability_detail::path_prefix_covers`, `capability.hpp:532-540`,
   confirms an exact-filename `path_prefix` covers only that name and its own sub-paths, not the mount
   root), not the mount-root-covers-everything `path_prefix = ""` `ExecuteCodeTool`'s own `"work"` grant
   uses. This reuses `cap::FsRead`/`FsWrite`'s existing "parameterized, not boolean" per-file shape
   (`capability.hpp:110-124`'s own header comment) rather than inventing a narrower capability kind.

**Why this is NOT the same shape as `ExecuteCodeTool`'s `Capabilities<cap::decl::FsRead<"work">, ...>`
precedent, despite reusing the identical mechanism**: the *mechanism* is genuinely the same (a
compile-time mount-label declaration plus a matching host-authored runtime grant) — there is no other
mechanism this capability system offers, and inventing one would be new, unjustified surface. What
differs is what the label is ever allowed to mean at the OS layer. `"work"` is a persistent, long-lived
host directory an agent can accumulate arbitrary files under across an ENTIRE session's worth of
`ExecuteCodeTool` calls — the standing grant is standing authority over a genuinely large, growing,
shared area. `"extract_scratch"` is a label whose OS-level binding is re-derived FRESH by finding 4's
own fix on every single call — a brand-new `MediatedPythonRunner` with `mount_roots` containing exactly
one entry, pointing at that ONE call's own scratch directory, torn down (`~MediatedPythonRunner()`,
`mediated_python_runner.cpp:23-28`) once `run()` returns. A standing session-level capability grant on
that label therefore never carries reachable authority beyond "whatever the CURRENT call's own
worker maps `extract_scratch` to" — which, by construction, is always exactly that call's own
`args.json`/`out.json` pair, never a previous call's data, never another mount's data, never reachable
by a worker one of these three tools did not itself spawn for one of the other two, or vice versa (each
tool's own dedicated runner is a genuinely separate OS process with its own AppContainer ACL). The
label being "standing" at the capability layer is honest and safe here specifically because nothing
standing exists beneath it at the OS layer — the opposite of `"work"`, where both layers are standing
together. This is the real, disclosed distinction finding 6 asked for, not an assertion that reuse is
automatically safe. **Revision 4 (round 3 finding 9) makes one load-bearing premise of this whole
argument explicit rather than implicit — see the fix immediately below**: "by construction, is always
exactly that call's own... pair" is only true if the per-call scratch directory really is generated
fresh every call. Finding 4's fix above never named how; finding 9 does.

**Fix for finding 9 (round 3, Critical — I2/data-integrity: "this call's own scratch directory" was
asserted, never generated)**: checked directly against `mediated_python_runner.cpp:56-66` (re-verified
this pass): `MediatedPythonRunner::initialize()` performs no derivation, uniqueness-check, or
randomization of `config_.mount_roots["extract_scratch"]` at all — it grants exactly the host path it is
handed. "Fresh per call" is a property the CALLER must provide, not one this mechanism provides on its
own. The ONE real precedent this draft cites for a per-call scratch path — `extract_pdf_text.hpp`'s
`scratch_dir()`/`unique_scratch_filename()` — does the OPPOSITE of what this design needs: a SHARED,
fixed directory with a per-call UNIQUE FILENAME, while finding 1's fix above needs a FIXED filename
(`args.json`/`out.json`, byte-identical every call, by design) with a per-call UNIQUE DIRECTORY instead.
**The real, concrete mechanism, adapting the SAFE half of Track B's own precedent rather than inventing
a new one**: verified directly this pass, `unique_scratch_filename()` (`extract_pdf_text.hpp:262-271`)
IS genuinely thread-safe — a `static std::atomic<std::uint64_t> counter` plus the process pid, correct
by construction for concurrent calls within one process. A twin function, `unique_scratch_dir_name()`,
built the identical way (pid + the same atomic-counter idiom, a fresh `std::atomic` instance scoped to
this function), returns a directory name instead of a filename; the host creates that directory (
`std::filesystem::create_directories`) under a shared, host-fixed root (`extract_scratch_root()`,
mirroring `scratch_dir()`'s own shape, one root shared by all three tools rather than one per tool —
simpler, and each subdirectory is already the real isolation boundary, not the root), writes
`args.json` inside THAT unique subdirectory, and sets `MediatedPythonConfig::mount_roots["extract_scratch"]`
to that unique subdirectory's real host path for this one call — never the shared root itself. This
composes cleanly with finding 6's own `path_prefix`-narrowed grants, checked directly rather than
assumed: `cap::FsRead{.mount_id = "extract_scratch", .path_prefix = "args.json", ...}` narrows WITHIN
whatever host directory the `"extract_scratch"` mount currently resolves to for THIS call — the grant
says nothing about which real host directory backs the mount, only what's reachable inside it — so a
freshly-generated, call-unique subdirectory as the mount's target, with the SAME `path_prefix="args.json"`/
`"out.json"` narrowing inside it, is exactly consistent with how `path_prefix`-narrowed grants already
work, not a new composition this draft has to invent.

**A companion fix, found during finding 11's own investigation into this same mechanism, not separately
numbered by round 3, but necessary for "fresh per call" to hold end-to-end**: `MediatedPythonRunner`'s
constructor takes `NativeJailBackend& backend` BY REFERENCE (`mediated_python_runner.hpp`) — nothing in
this codebase forces sharing one. §2c's own investigation (finding 11) found that BOTH of this
codebase's existing idioms for supplying that reference — `extract_pdf_text_detail::invoke_worker`'s
`static native_jail::NativeJailBackend backend;` and `tools/cli_chat.cpp`'s `shared_python_runner()`'s
identical `static` — back onto the SAME un-mutexed `instances_` map every `create_python_worker()`/
`exec_session()`/`destroy()` call mutates (§2c's own evaluation). If an implementer followed either
existing idiom for these three tools' own dedicated runners, concurrent calls to the SAME tool (e.g. a
batch extracting several `.docx` files, a real ADR-160 scenario) would share one static backend and race
on its `instances_` map — reopening, for these three tools specifically, exactly the class of defect
§2c disclosed in Track B's code. **The fix**: each call constructs its OWN, call-local
`NativeJailBackend` — a plain stack variable in the `Tool<>::invoke()` call path, declared immediately
before the call-local `MediatedPythonRunner` that references it (so the runner, which holds the
reference, is destroyed first — ordinary reverse-declaration-order C++ destruction, no new lifetime
rule) — never a shared static. `shared_profile()`'s own AppContainer SID/profile OBJECT
(`native_jail_backend.cpp:110-123`, itself correctly `std::mutex`-guarded, verified directly) is still
shared process-wide, which is fine — the SID/profile identity itself is immutable once created, not a
mutable per-call instance map — but each call's own `instances_` entry now lives in an `Instance` map
that NO OTHER call, of this tool or any other, ever touches, closing the race structurally rather than
by convention. This is a small, real deviation from "exactly mirror the existing shared-static idiom,"
named explicitly here rather than left for an implementer to rediscover the hard way. **Correction
(Revision 5, round 4 finding 14, since retracted — Revision 8, Prove pass 1): the phrase this paragraph
previously used here — "read-only, deduped grant bookkeeping" — described the GRANTS placed on that shared
profile, not just the profile object's own identity, and that description was false as stated:
`spec.mounts`'s own per-mount grant loop is genuinely ungated by any dedup. Revision 5 read that as a
defect in THIS mechanism (finding 9's fresh-per-call directory) and rerouted the grant onto a shared,
inheritably-granted parent root instead (finding 14's fix) — Revision 6 then found that reroute
manufactured a standing backstop-exposure problem of its own (finding 18), and rounds 6-7 spent nine
further findings hardening a pool built to bound it. Prove pass 1 (full text below, after "Red-team round
7") found the premise underneath all of that was checkable and false: `grant_path()`'s ACE lives on the
granted path's OWN security descriptor, not on any profile-wide ledger, so finding 15's own
already-designed unconditional `remove_all()` (the fix a few subsections below finding 14's, in the very
same revision) already destroys that ACE as a side effect of deleting the object it was granted on —
PROVIDED the grant lands on this call's own leaf directory directly, which is exactly what THIS
paragraph's mechanism (`unique_scratch_dir_name()`, a per-call-fresh `NativeJailBackend` instance) already
produces. Revision 8 reverts to that: the per-call directory this paragraph generates is granted directly
via `grant_path()`, no dedup, no shared parent — see the fix immediately below, which replaces finding
14's shared-parent-grant design (and the pool findings 18/19/21/22/23/25/26/27 built on top of it) with
this direct grant, unchanged from what finding 9 always generated.** Everything else in THIS paragraph
(the per-call-fresh `NativeJailBackend` instance, never a shared static) is unaffected by any of this and
carries forward unchanged across every revision.

**Finding 14 — resolved by elimination (Revision 8, per Prove pass 1)**: checked directly against the real
grant call path finding 9's fix feeds into — `NativeJailBackend::create_python_worker()`
(`native_jail_backend.cpp:574-622`, re-verified) — its per-mount loop (`:612`) calls
`(*profile)->grant_path(host_path, mount.read_write)` DIRECTLY, for every entry of `spec.mounts`, with no
dedup of any kind. Finding 14 (round 4) read this as a defect: finding 9's own fix makes
`mount_roots["extract_scratch"]`'s host path unique on EVERY call, by design, so this per-mount grant loop
appends one new ACE to the shared `AppContainerProfile`'s DACL on every call, without bound, for the life
of the host process. Revisions 5-7 (findings 14, 18, 19, 21, 22, 23, 25, 26, 27) spent three revisions and
nine findings building — first a shared, inheritably-granted parent scratch root, then, once that was
found to widen the backstop layer into a standing cross-call/cross-session exposure (finding 18), a fixed
`N`-slot pool with its own acquire/release/wipe/quarantine/attribution machinery — to bound this concern.
**Prove pass 1 (full text below, after "Red-team round 7") found the concern was real but the fix
direction was wrong, by reading `grant_path()`'s own body (`app_container_profile.cpp:152-205`) against
a question no round had checked: where does the ACE it adds actually live?** The answer: on the granted
path's OWN NTFS security descriptor (`GetNamedSecurityInfoW`/`SetNamedSecurityInfoW` both operate on
`path`'s own `DACL_SECURITY_INFORMATION`) — there is no `AppContainerProfile`-wide DACL for grants to
accumulate on. What actually grows, ungated, is the number of distinct filesystem objects carrying that
SID's ACE — and `std::filesystem::remove_all` (`RemoveDirectory` under the hood) deallocates a deleted
object's MFT record, security descriptor included. **An ACE cannot outlive the object it was granted on.**
Finding 15's own cleanup fix (unconditional `remove_all()` on both the success and failure path, a few
subsections below, designed in Revision 5 — one subsection BEFORE finding 14's pool was ever introduced —
and never reopened by anything since) already destroys the ACE finding 14 worried about, as a structural
side effect of deleting the object, with zero new code: **the real fix is to grant the call's own leaf
directory directly.**

**The real, adopted design**: the host grants `unique_scratch_dir_name()`'s own per-call directory (finding
9's fix, unchanged — see immediately above) directly via the already-shipped `AppContainerProfile::
grant_path()`, exactly as `create_python_worker()`'s existing, unmodified per-mount loop (`:612`) already
does for every entry of `spec.mounts` — no dedup, no shared parent root, no inherited ACE, and no code
change to `create_python_worker()`, `AppContainerProfile`, or `grant_ro_deduped()` at all. This needs no
dedup because the path is used by exactly one call, ever — dedup exists to stop a REPEATED path from
re-appending a DACL entry (`grant_ro_deduped()`'s real, different job, for the three deployment-fixed paths
like `python_home`), and a per-call-unique path is never repeated. Cleanup (finding 15, unchanged) then
retires the grant when the call ends, on both the success and failure path — total live ACE count at any
moment is bounded by the number of calls genuinely in flight right now, not by cumulative call volume, and
shrinks to zero the instant the host is idle. This is the same conclusion finding 14's own fix reached
("bounded ACL growth") but achieved with zero new mechanism, rather than a shared-root inheritance scheme
that (finding 18) turned out to trade ACL-growth-boundedness for a standing backstop exposure it then took
a further ten findings' worth of pool machinery to re-bound.

**Why not `revoke_path()` either — Prove pass 1 checked this directly, not asserted it**: findings 14 and
18 both rejected a genuinely-per-call grant-plus-revoke design on the grounds that `AppContainerProfile::
revoke_path()` would be "real, new, unproven Windows-ACL-surgery surface" this codebase does not have.
Prove pass 1 read `grant_path()`'s actual body and found this premise false: `grant_path()` is exactly
three Win32 calls (`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW`), and
`SetEntriesInAclW`'s own `ACCESS_MODE` enum already has a documented `REVOKE_ACCESS` member — a
`revoke_path()` sibling would be the identical three-call shape with one enum flipped, ~15 lines, no new
synchronization needed (a race requires two calls targeting the SAME path concurrently, and finding 9's
own per-call-unique naming already rules that out for grant OR revoke). It would be real and buildable —
but it would be solving a problem `std::filesystem::remove_all` already solves for free, since deleting the
directory destroys the ACE as an intrinsic NTFS property, not a mechanism this draft has to build, test, or
trust for the first time. **`revoke_path()` is not built as the PRIMARY retirement mechanism for v1** (round
9 finding 37: this sentence previously read "is not built for v1" — an unqualified conclusion the
"Correction" paragraph immediately below narrows without ever editing this sentence to match, leaving it
standing as a now-false stopping point; corrected in place here rather than left for a reader to
re-derive — see that Correction for the one narrow case where it is, in fact, built) — not because it is
unproven, but because nothing in this design needs it AS THE PRIMARY MECHANISM once the grant lands on a
directory that gets deleted at call end.

**Correction (Revision 9, round 8 finding 31, since narrowed — not retracted)**: the conclusion above was
correct for the case it was reasoned about — the SUCCESS path, where `remove_all()` actually deletes the
directory and the ACE goes with it. Round 8 found a real case this paragraph never considered: a non-crash
`remove_all()` FAILURE, where the directory (and its ACE) survives despite cleanup running to completion
normally (finding 31, fixed below, immediately after finding 15's own discussion). For THAT case, and only
that case, deletion does not retire the grant, and `revoke_path()` is exactly the safety net this paragraph
already established is small, buildable, and race-free (a race requires two calls targeting the SAME path
concurrently, and finding 9's own per-call-unique naming rules that out for grant OR revoke either way — the
same argument, unchanged, now also covering the revoke call this correction adds). **Revision 9 builds it**,
scoped narrowly to finding 31's own retry-exhaustion path — never called on the ordinary success path, never
a substitute for `remove_all()` as the primary retirement mechanism, exactly as buildable and exactly as
unnecessary for every OTHER path through this design as this paragraph already argued. This is not a
reversal of the reasoning above; it is the one case that reasoning did not yet cover.

**Composition with finding 6's `path_prefix` narrowing, re-confirmed for the direct-grant design**: this
fix touches ONLY which directory gets an explicit `grant_path()` call at the OS-ACL layer — it changes
nothing about `MountSpec::guest_path`, `mount_roots` resolution, or `dispatch_open`'s own two-layer check
(finding 6's fix). `mount_roots["extract_scratch"]` still resolves to THIS call's own fresh, unique
directory exactly as finding 9 designed; `cap::FsRead{.path_prefix="args.json"}`/
`cap::FsWrite{.path_prefix="out.json"}` still narrow WITHIN whatever host directory `"extract_scratch"`
resolves to for this call, unaffected — finding 6's own fix already established that the grant says
nothing about which real host directory backs the mount, only what's reachable inside it. This composition
was, if anything, cleaner to state for the direct-grant design than it ever was for the shared-root or pool
designs: this is exactly the shape finding 6 was originally verified against (Revision 3, before finding 9
even introduced the per-call-directory mechanism), with no intervening inheritance or lease indirection to
re-derive the argument through.

**Fix for finding 15 (High, round 4 — I8: unbounded disk-space leak, plus a narrow stale-directory-reuse
risk under pid reuse)**: this draft names no cleanup of the per-call scratch subdirectory or its
`args.json`/`out.json` contents anywhere — a real gap even against Track B's own, weaker precedent
(`extract_pdf_text.hpp:445`'s `std::filesystem::remove(file, ec)`, "best-effort cleanup" for the one
scratch FILE it wrote). **The fix**: the host removes the whole per-call scratch subdirectory,
recursively (`std::filesystem::remove_all(dir, ec)`, best-effort, mirroring Track B's own discipline but
for a directory rather than a file), after `run()` returns — on BOTH the success path (after `out.json`
has been read into the Reply, finding 2's own fix) AND the failure path (a worker error, a timeout, a
denied capability check — any path that does not reach a normal `out.json` read), structured so cleanup
runs unconditionally once the call is done with its scratch directory (an RAII scope-guard around the
call-local scratch-directory handle is the natural shape, matching the same reverse-declaration-order
discipline finding 9's companion fix already uses for the call-local `NativeJailBackend`/`MediatedPythonRunner`
pair) rather than a single cleanup call on only the success branch.

For the narrower stale-directory-reuse risk (a process crash resets the in-memory pid+counter state,
and if the OS then reuses the exact same pid before this draft's own generated name is retired, the next
`unique_scratch_dir_name()` call in the new process could compute the same name a prior, uncleaned call
left behind): the fix above closes MOST of this window by construction — a directory that no longer
exists after every ordinary call cannot collide with the very next call that happens to reuse its name,
because there is nothing left to collide with. What real cleanup does NOT close is the narrower case a
crash (not a normal return) skips cleanup entirely, and the OS then reuses that exact pid before the
crashed run's own directory is ever manually reaped. For that residual case, the host additionally checks
`std::filesystem::exists(dir)` BEFORE calling `create_directories()`, and treats an already-existing
directory as a real, fail-closed error rather than silently proceeding into `create_directories()`'s
ordinary success-on-existing semantics (verified: `create_directories()` surfaces `ec` only on an actual
filesystem failure, not on pre-existence, the same as the one real precedent this draft's own text says
`extract_scratch_root()` "mirrors," `extract_pdf_text.hpp:420-427`). **Why cleanup plus a fail-closed
existence check is judged sufficient for v1, stated explicitly rather than hand-waved**: together they
convert the ONLY remaining collision scenario (a crash that skips cleanup, followed by an exact pid reuse
before that crashed run's own directory is reaped) from a SILENT cross-call data-integrity hole into a
LOUD, fail-closed tool error — the same fail-closed posture `MediatedPythonConfig`'s own never-zero
`exec_wall_ms`/`memory_bytes` defaults already establish as this draft's standard (§6). It does not make
collision literally impossible — a process that crashes, loses its pid to a reused process, and hits that
exact directory name before anything reaps it is a real, residual, narrow window — but converting a
silent data leak into a loud failure is judged an acceptable v1 posture, not a design gap left unexamined.
Strengthening `unique_scratch_dir_name()` itself (e.g. adding a boot-time/process-start-timestamp
component alongside pid+counter, so a NEW process after a crash never reuses a name a DIFFERENT process's
lifetime could have generated) remains a real, live option to revisit if a future revision's own
measurement finds the residual window is not narrow enough in practice — not designed here, named as the
real alternative this fix does not need to reach for.

**Correction (Revision 6, round 5 finding 19, since retracted — Revision 8, Prove pass 1)**: Revision 6
read the crash/pid-reuse residual named immediately above as a reason to retire
`unique_scratch_dir_name()`'s pid+counter naming scheme entirely in favor of the scratch-slot pool's own
fixed slot literals. Since the pool is eliminated (Prove pass 1; see finding 14's own resolution above),
that retirement is undone: `unique_scratch_dir_name()` is the mechanism, unchanged, and the crash/pid-reuse
residual accepted two paragraphs above — narrowed to a loud, fail-closed error by the pre-existence check,
never eliminated outright — is this design's real, final residual for v1, not superseded by anything
below.

**Fix for finding 31 (round 8, High — I2/I8: finding 15's own residual framing named only the CRASH-skips-
cleanup case; a non-crash `remove_all()` FAILURE leaves the same stranded-directory-with-a-live-grant
outcome, silently, with no bound and no signal)**: `std::filesystem::remove_all(path const&,
std::error_code&)` — the exact overload finding 15's fix already calls — never throws; on failure it sets
`ec` to a real error and returns `static_cast<std::uintmax_t>(-1)`, while running to completion normally on
both the ordinary success path (files and directories actually removed) and the harmless not-found path
(`ec` cleared, `0` returned, nothing to remove). Finding 15's own text calls this "best-effort... regardless
of outcome" and never inspects the return value or `ec` — for Track B's own precedent (a single stray FILE
inside an already-permanently-granted directory, `extract_pdf_text.hpp:445`) that omission is a disk-space
nuisance; for THIS design, where Prove pass 1's entire "finding 14 resolved by elimination" argument depends
on `remove_all()` actually deleting the object the grant lives on, an unchecked failure silently reproduces
exactly the unbounded-ACE-growth shape finding 14 originally named, through a different door (a failed
delete instead of a skipped dedup) that neither Revision 8's own text nor Prove pass 1's own honesty section
checked for.

**The real fix — check the outcome, retry the transient case, and treat exhausted retries as a loud,
attributable, partially-recoverable failure, not a swallowed one:**

1. **Check `ec` after every `remove_all(dir, ec)` call**, on both the success and failure branch of `run()`
   (the same two call sites finding 15's fix already names). Only a cleared `ec` — confirmed, not merely
   assumed, by re-checking `!std::filesystem::exists(dir, ec2)`, the same existence-check primitive the
   fail-closed pre-existence check already uses — counts as a real success.
2. **On a nonzero `ec`, retry a small, bounded number of times (3 attempts is real, tunable headroom for
   the specific failure class named here, not sized further this pass) with a short backoff between
   attempts (tens of milliseconds, not seconds)** — CLAUDE.md's own machine-safety framing this draft
   already applies to every other bound applies to a retry loop too: bounded, never open-ended. This is the
   concrete answer to round 8's own naming of the real, non-catastrophic failure class this closes: a
   transient `ERROR_SHARING_VIOLATION` from an AV/indexing scanner or a worker process that has not yet
   fully exited almost always clears within milliseconds, so a bounded retry converts the common case into
   an ordinary, silent success — the same "retry on transient failure, don't latch it forever" discipline
   `shared_profile()`'s own lazy-singleton pattern already established for a different mechanism (round 5
   finding 19's own citation, §4b above), reused here rather than invented.
3. **If every retry is exhausted, the directory — and its live `grant_path()` ACE — genuinely cannot be
   deleted right now. Two things happen, not one:**
   - **Surface a loud, attributable failure.** Finding 32's own audit-trail fix, immediately below, IS the
     mechanism — this is the primary way a stranded directory becomes discoverable, not a separately-
     designed alerting path. A silently-swallowed `ec` (finding 15's original, un-reopened text) is exactly
     the "soft warning nobody reads" this finding's own severity (High, not Medium) argues against: a
     stranded directory with a live OS-level grant is a security-relevant residual, and this draft's own
     "named, not silently absorbed" discipline (already applied to the crash/pid-reuse residual,
     `body_text_only`, `merged_cells_collapsed`, and every unsized cap in §6) requires the same treatment
     here.
   - **Fall back to revoking the grant even though the directory itself survives.**
     `AppContainerProfile::revoke_path(dir)` — the sibling Prove pass 1 verified is real and small
     (`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW`, the identical three-call shape
     `grant_path()` already ships, with `SetEntriesInAclW`'s own `ACCESS_MODE` flipped from `GRANT_ACCESS`
     to its documented `REVOKE_ACCESS` member) but did not build, because finding 14's resolution never
     needed it — the success path already retires the grant by deleting the object it lives on.
     **This is the one place in the whole draft where `revoke_path()` earns its keep**: not the primary
     retirement mechanism (deletion still is, on the overwhelmingly common success path) but a
     failure-path-only safety net for the specific case deletion cannot reach. Revision 9 builds it, scoped
     exactly this narrowly: called only after retries are exhausted, targeting only this one call's own
     directory (never a shared or ancestor path, so it carries none of finding 18's own cross-call
     reachability concern), racing nothing else (finding 9's own per-call-unique naming already rules out
     two concurrent callers ever targeting the SAME path for grant OR revoke, re-confirmed independently by
     round 8's own "Investigated, no real defect found" section for `grant_path()` and unchanged for
     `revoke_path()`, since both are `const` methods touching only the target path's own security
     descriptor). Stripping the SID's ACE off a directory that could not be deleted does not free its disk
     space (finding 30's own already-disclosed, orthogonal cost, §6) and does not resolve whatever transient
     condition made deletion fail (a handle still open inside it, e.g., keeps working until closed —
     ordinary Windows handle semantics this fix does not and cannot change).

     **Revision 10, round 9 finding 33 — `revoke_path(dir)` alone does not close content reachability, only
     directory listing/enumeration; the fallback is extended to a fixed, four-path enumeration, and the
     safety claim below is corrected to match.** `grant_path()`'s own `OBJECT_INHERIT_ACE`/
     `CONTAINER_INHERIT_ACE` flags (`app_container_profile.cpp:165`) mean the ACE it places on `dir` is also
     independently stamped, at creation time, onto every child object created inside `dir` while the grant is
     live — a point-in-time copy, not a live reference back to the parent. By the time cleanup's retries are
     exhausted and this fallback runs, `args.json`, the source-document copy, and (if the worker got far
     enough) `out.json` already exist inside `dir` and already carry their own independently-materialized
     copy of that ACE; `revoke_path(dir)` touches only `dir`'s own security descriptor and does nothing to
     theirs. Revision 9's own text here previously claimed the fallback means "no worker... can reach this
     stranded directory's CONTENTS... any more" — round 9 found that true only for directory
     listing/traversal, not for a worker that already knows (or simply guesses — these are fixed, literal,
     non-interpolated filenames, identical on every call, finding 1's own fix) the exact full path of
     `args.json`/`out.json`/the source document and opens it directly; Windows' default
     "bypass traverse checking" policy means a full-path open does not, in general, require traversal
     permission on ancestor directories at all. **The fix**: the fallback calls `revoke_path()` not once but
     against a fixed, small, enumerable set of up to FOUR paths — `dir` itself, `dir/args.json`,
     `dir/source` (the source-document copy; Revision 10 gives it this fixed, non-interpolated literal name
     for the first time — no extension needed, since none of `docx.Document`/`openpyxl.load_workbook`/
     `pptx.Presentation` require one, and a fixed literal keeps the source document's on-disk name subject to
     the same "host-chosen, never model-influenced" discipline `args.json`/`out.json` already have), and
     `dir/out.json` — never a general recursive directory walk and never a new
     `TreeSetNamedSecurityInfoW`-class primitive, exactly as narrow as the actual, fixed set of files this
     mechanism ever creates inside a call's scratch directory (§4b's own "Fix for finding 1"/"Fix for finding
     2"/"Sourcing" subsections, above). A child that does not exist yet at the time cleanup runs (the call
     failed before that file was ever written) is not treated as a failure: `GetNamedSecurityInfoW` against a
     nonexistent path returns `ERROR_FILE_NOT_FOUND`, which this fix treats as trivially satisfied — there is
     no ACE to strip because there is no object left to carry one, the same "nothing to collide with"
     reasoning finding 15's own cleanup fix already applies to a directory that was never created.

     **Does a per-child `revoke_path()` call share `remove_all()`'s own open-handle failure mode? Round 9
     finding 34, resolved here, not left open.** `revoke_path()`'s three Win32 calls
     (`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW`) request only `READ_CONTROL`/
     `WRITE_DAC` against the target path — access rights that ordinary Windows semantics do not route through
     NTFS's `IoCheckShareAccess` sharing-violation check, the same real, documented reason `icacls`/`takeown`
     can repair permissions on a file another process holds open without hitting a sharing violation. This
     reasoning does not depend on which path is the target — it applies identically to the directory-level
     `revoke_path(dir)` call and to each of the three per-child `revoke_path()` calls this fix adds, so it
     resolves finding 34 for both, not just the directory: a `revoke_path()` failure (directory or child) is
     LIKELY a low-likelihood tail, not a correlated repeat of whatever `ERROR_SHARING_VIOLATION` blocked
     `remove_all()` in the first place. This is argued from `revoke_path()`'s own real body and documented
     Win32 access-check semantics, not from an executed test — the same "argued, not yet measured" standard
     this draft's own central NTFS claims already carry, stated here explicitly rather than left silent, per
     round 9's own fix direction. **Because of this, no new bounded-retry loop is added around any of the
     four `revoke_path()` calls** — each is attempted once, the same one-shot discipline the directory-level
     call already had; keeping the fix this narrow is a direct consequence of the reasoning above, not an
     unexamined simplification.

     **Corrected safety claim (round 9 finding 33)**: after cleanup failure, `revoke_path()` is attempted
     against the directory and each of its (up to) three known children. If all four attempts succeed, or
     are trivially satisfied because the child in question never existed, no further AppContainer-level
     access to any of this call's data — LISTING of the directory, or the CONTENTS of `args.json`/the source
     document/`out.json` — survives through the shared `shared_profile()` SID, closing finding 18's own
     backstop-reachability concern for both the directory and its contents. **If any one of the four
     `revoke_path()` calls fails**: exactly that object — and only that object; the other three's own revoke
     outcome is unaffected — remains reachable by full-path open under the shared SID for as long as it
     survives on disk, until an operator (alerted by finding 32's own extended log entry, below) manually
     reaps it; no further automated recovery is designed here, named as real, live future work (the same
     periodic host-side sweep already named as a candidate for the directory-level case, now also covering
     an individually-stranded child).

     **Wall-clock cost of this whole sequence — round 9 finding 35, stated explicitly rather than left
     ambiguous.** Checked directly against `tool_pipeline.hpp:669-673` (`ctx.deadline` enforced only "at the
     call boundary... not preemptible mid-call") and `agent_session.hpp:1773-1784` (`report_progress` is
     rebound to this call's own closure immediately before `invoke_tool(...)` and reset to a no-op only
     immediately after it returns): the retry loop plus, on exhaustion, this fallback's now-four
     `revoke_path()` calls all run synchronously inside `Tool<>::invoke()`'s own call stack, entirely outside
     both `exec_wall_ms` (which bounds only the worker's in-sandbox execution) and `ctx.deadline` (checked
     only at the call boundary). Two consequences follow, stated plainly rather than left for a reader to
     re-derive: (1) a slow cleanup sequence cannot itself cause an otherwise-successful call to fail on
     `ctx.deadline` — a real, positive property, the deadline is not re-checked on the way out; and (2) that
     same slow sequence adds real, unsized, uncapped wall-clock latency to the call's own Reply-return time
     on the failure branch specifically, not counted against or exempted from `exec_wall_ms`, the zip-bomb
     threshold, or anything else this draft names in §6. This draft names no caller-side/orchestrator-level
     timeout this composition could interact badly with, but it also does not rule one out — an ambiguity
     disclosed, not a proven defect.

**Fix for finding 32 (round 8, Medium — I4: eliminating the pool's own lease-attribution ring log correctly
closed finding 23's own narrower shared-slot-reconstruction claim, but dropped the more general property —
SOME audit trail of which call created/granted/deleted which scratch directory — with nothing replacing it,
and no acknowledgment that this is a real degradation rather than a pure simplification)**: the fix is
deliberately NOT a reinvention of the eliminated ring log's own cross-call attribution machinery (slot
index, lease-start/end timestamps, quarantine outcome, a bounded most-recent-`N` ring buffer) — that
machinery was sized for a SHARED pool's own cross-call attribution problem ("who else touched this slot
recently enough to matter"), a question with no analog once every scratch directory is created once, used
by exactly one call, and deleted; rebuilding it here would be exactly the "reinvent the eliminated
mechanism's full complexity" this fix is asked not to do. What replaces it is proportionate to what the
per-call design actually needs: a structured event at directory-creation time and at
directory-deletion/cleanup-outcome time, each carrying the directory's own path, the call's `run_id`/
`principal.id` (already threaded onto every `EffectContext`, `core/effect_context.hpp` — I4's own mandatory
attribution parameter, not a new field to invent), the tool name, and, for the deletion event, finding 31's
own three-way cleanup outcome (clean success / retried-then-succeeded / retries exhausted, `revoke_path()`
outcome included).

**The mechanism reuses this codebase's own existing per-call event channel rather than inventing a new
logging primitive.** `EffectContext::report_progress` (ADR-060, `core/effect_context.hpp`) is a real,
already-shipped, call-scoped reverse channel: `ctx.report_progress(ContentItem{...})` pushes a
`run_event_kind::tool_call_delta` event onto the run's own event stream, carrying whatever attribution the
emitting `RunEvent` envelope already stamps on every event (`run_id`, `seq`) plus the `ContentItem` payload
itself. This is not a hypothetical reuse: `native_jail_backend.cpp:901` already calls exactly this —
`ctx.report_progress(ContentItem{.value = Text{text}})` — for the CodeAct `agent.progress` module's own
per-call output (`decisions/ADR-155-agent-progress-codeact-module.md`), inside the SAME backend file this
draft's scratch-directory lifecycle code lives beside. The fix here is the same call, a structured `Data`
payload (`core/content.hpp`'s `ContentItem::value` alternative for exactly "a structured fact," per
`EffectContext::report_progress`'s own file comment — "a plain text, a structured `Data` fact, or a
namespaced `Custom` payload") instead of `Text`, at two points in `run()`'s own control flow: immediately
after `create_directories()`+`grant_path()` succeed (an `office_extraction.scratch_dir_created` event: path,
tool name, call id) and immediately after cleanup resolves, on both the success and failure path (an
`office_extraction.scratch_dir_cleaned`/`office_extraction.scratch_dir_cleanup_failed` event carrying
finding 31's own outcome). **Revision 10, round 9 finding 33's own extension**: the
`scratch_dir_cleanup_failed` event's payload now also carries, alongside the directory's own
`remove_all()`/`revoke_path()` outcome, the individual outcome (skipped-nonexistent / succeeded / failed) of
each of the three per-child `revoke_path()` attempts finding 33's fix adds — the same event, a richer
payload naming exactly which object(s) remain reachable, not a second, independently-designed mechanism. No
new persistent-logging mechanism, no new ring buffer, no new bounded-history
data structure — the run event stream is already this codebase's own established per-call observability
channel, and a caller that wants durable, cross-run retention wires a consumer onto it the same way any
other `run_event_kind` consumer already would (013 §1's own "one internal run event stream ... adding a
surface is writing a projection"), not something this draft needs to build.

**Composition with finding 31, stated explicitly rather than left implicit**: the
`scratch_dir_cleanup_failed` event above IS finding 31's own "surface a real, attributable error"
requirement — one event satisfies both findings at once, not two independently-designed mechanisms. A host
that wires any consumer onto its run event stream (the same "no wiring means no behavior change, real wiring
means real observability" posture every other opt-in `EffectContext` field in this draft already carries)
gets, for free, both the general I4 audit trail finding 32 asks for AND the specific stranded-directory
signal finding 31's retry-exhaustion path needs — a host that wires none gets neither, the same honest,
disclosed residual this draft's own call-count-budget discussion (§6) already accepts for a host that does
not wire `ToolCallHook` either.

**Findings 18, 19, 21, 22, and 23 — resolved by elimination (Revision 8, per Prove pass 1)**: rounds 5-7
built, then hardened across three revisions, a fixed `N`-slot scratch pool (host-chosen SID-shared slot
directories, pre-granted once at pool-init time, checked out under mutex/condition-variable discipline,
wiped before handout and after release, quarantined on wipe failure, and logged in a lease-attribution
ring log) specifically to bound finding 14's own fix — a shared, inheritably-granted scratch root — from
widening `native_jail_backend.hpp`'s documented OS-layer backstop into a standing, cross-call/cross-
session/cross-tool exposure (finding 18), correctly sequencing that pool's own cold-start (finding 19),
then correcting three further real defects rounds 6-7 found in the pool's own machinery (finding 21's
wipe-failure handling, finding 22's false FIFO-ordering claim, finding 23's missing attribution log). Prove
pass 1 (full text below, after "Red-team round 7") found none of this was necessary: the standing exposure
the pool was built to bound was created by finding 14's OWN choice of a shared, never-deleted root — a
per-call grant directly on finding 9's own per-call-unique leaf directory (finding 14's resolution, above)
never has a standing grant to begin with, because the grant is retired by finding 15's cleanup the instant
the call that owns it ends. With no shared, reused resource to protect, there is nothing left for the
pool's own concurrency/attribution machinery to protect: no free-list to guard with a mutex, no wait to make
race-free (finding 22), no reused slot to wipe before handout or quarantine on wipe failure (finding 21),
and no cross-call lease to attribute in a ring log (finding 23) — a directory that is created once, used by
exactly one call, and deleted has no "next occupant" for any of that machinery to reason about. Investigated
and rejected: distinguishing workers by a per-call/per-session AppContainer SID (findings 18's own
fix-direction option), and a genuine per-call grant with `revoke_path()` (findings 14's and 18's own
rejected option) — both real, live alternatives Prove pass 1 re-examined directly; the per-SID path remains
rejected for the same reasons findings 14/18 gave (real, disproportionate new implementation surface, and a
direct reopening of ADR-004 §3's own locked design); `revoke_path()` turns out to be small and buildable
(Prove pass 1 verified its real body is the same three Win32 calls `grant_path()` already ships, with one
enum flipped) but unnecessary, since `std::filesystem::remove_all` already does the same job as a structural
side effect of deleting the object the grant lives on (finding 14's resolution, above, has the full
argument). The exposure this pool bounded to "N concurrently-live slots' worth" is, under the direct-grant
design, bounded more tightly still — to exactly the calls genuinely in flight right now, shrinking to zero
the instant the host is idle — without building any of the machinery that bound it to N. Finding 18's own
backstop-scope concern is therefore not merely re-bounded by this revision, it is closed more tightly than
the pool ever closed it, by a design with strictly less new surface, not more.

**Fix for finding 2 (output transport: never `stdout_text`/`result_repr`/`output_cap_bytes`)**:
`output_discipline.hpp`'s `cap_output()` (re-verified: `kDefaultOutputCapBytes = 64 * 1024`, applied to
`ExecOutcome::stdout_text`/`stderr_text`/`result_repr` specifically because that layer is sized for
"an interactive REPL session's console chatter," per that file's own header comment) is the wrong
channel for a structured extraction result and is never used for one. Instead: each fixed script writes
its full extracted result — every field its own Reply needs (`ExtractPptxTextReply`'s `slide_count`,
`slides_processed`, `truncated_slides`, and the full joined slide text; `ExtractDocxTextReply`'s
`paragraph_count`, `tables_processed`, `truncated_tables`, `body_text_only`, and the full joined
paragraph/table text; `ExtractXlsxCellsReply`'s `sheet_names`, `total_row_count`, `rows_processed`,
`truncated_rows`, `merged_cells_collapsed` (Revision 5, round 4 finding 17), and the full joined cell
text — Revision 4, round 3 finding 11) — as ONE JSON object
to a second fixed path under the same narrow scratch mount, always
`/extract_scratch/out.json`, via ordinary `json.dump()`.

**Fix for finding 20 (Critical, round 6 — I2/I3: the host's own `out.json` read must not be a bare,
unverified path open)**: the paragraph immediately above, unchanged since Revision 2, described the
host's read as "the same host-side `std::ifstream` pattern `ExtractPdfText`'s own `fetch_via_path`/
scratch-file write already uses in the other direction" — round 6 found this comparison false for the
direction that actually matters (Track B's own `std::ofstream` write is the HOST writing trusted bytes
the WORKER later reads; nothing in Track B ever reads a WORKER-writable file back as trusted host input,
so Track B has no equivalent exposure to point to), and found that a plain `std::ifstream`/`std::ofstream`
lets a worker compromised via a hostile input file (a real threat class for this layer regardless of the
pool question — `native_jail_backend.hpp:15-19`'s own file-top comment documents the AppContainer DACL as
"the layer-3 BACKSTOP for filesystem access, never the primary boundary," live for any worker of these
three tools under the shared `shared_profile()` SID, per finding 9's own companion fix, whether that
worker's own directory came from a pool slot or a per-call grant) redirect the host's own trusted read to
a reparse point/junction pointing at any path the shared AppContainer SID can reach — turning a passive
backstop exposure into an active content-injection path through the ordinary Reply pipeline. **The real
fix, found by Revision 7's own investigation into whether a fundamentally different I/O design could avoid
this class of problem entirely (see this draft's own revision-history note above), and unaffected by
Revision 8's elimination of the pool (Prove pass 1) — this fix is orthogonal to the pool-vs-per-call-grant
question and is kept unchanged**: every read/write against the call's own scratch directory's contents —
`args.json` (write), the source document's bytes (write, "Sourcing" bullet below), and `out.json` (read) —
goes through `agentengine::native_jail::MediatedFileSystemAdapter`
(`mediated_filesystem_adapter.cpp`/`_posix.cpp`), constructed directly against that directory's own host
path (`MediatedFileSystemAdapter::create(dir.host_path())`, no worktree/Ref/object-store dependency of any
kind), never a bare `std::ifstream`/`std::ofstream`. This is not a new mechanism: this adapter's
`read_file`/`write_file`/`exists`/`list_directory` are ALREADY, verified directly against both the Windows
and POSIX implementations, built on `open_within_mount_root` (`worktree_mount_fs.cpp:152`) — the same
ADR-014-Judged, TOCTOU-safe, handle-then-verify-real-location primitive `dispatch_open` itself relies on
one layer down, and the same primitive `ExecOutcome::artifacts`' own `harvest_mount` path rides when it
uses this same adapter (this draft's own investigation into round 6's brief, above) — reused here
directly, standalone, without pulling in `harvest_mount`'s own heavier worktree-Ref machinery this draft
already judged (Revision 2) too heavy for an internal result-transport channel. A worker that replaces
`out.json` with a reparse point pointing elsewhere is caught the same way `open_within_mount_root` catches
it everywhere else in this codebase: the handle Windows/the kernel actually resolves is re-verified, by
handle, to still resolve inside the call's own scratch directory before its bytes are ever trusted — the
file the host reads is provably the one the fixed script wrote, not merely the one its path claims to be.
**The coarse, host-side size cap this section already required is preserved, on a correspondingly
adapter-native mechanism**: `MediatedFileSystemAdapter::list_directory("")` on the directory returns each
entry's `size_bytes` (`DirEntry::size_bytes`, `filesystem_adapter.hpp`) — the host checks `out.json`'s
reported size against a fixed, host-chosen ceiling BEFORE calling `read_file` (which reads the whole file
into memory), the identical "check before you fully trust, before you fully read" posture as before, just
through the adapter's own listing rather than `std::filesystem::file_size` on a path the adapter never
opened. Fails closed with a resource error if exceeded, distinct from and independent of the 64 KiB
LLM-context cap (finding 2's whole point, unaffected by this fix). Only after that check-then-read does the
ALREADY-correct §5 discipline (`ctx.tool_result_byte_threshold`/`ctx.blob_sink`) ever see the text —
identically for all three tools, unaffected by this fix (§5).

**Zip-bomb/XXE mitigation (finding 3), a concrete mechanism plus a re-verified pin, not merely a
disclosed concern**: before its fixed script calls `pptx.Presentation(...)`/`docx.Document(...)`/
`openpyxl.load_workbook(...)` (Revision 4 adds the third) at all, it opens the scratch file with
`zipfile.ZipFile` and checks the total of every entry's `file_size` (uncompressed) from `infolist()`
against a fixed, host-authored threshold constant — the same "check before you fully trust" posture
Track B's own worker already takes toward PDF structure, and red-team finding 3's own named fix
direction (a). The exact byte threshold stays an unmeasured placeholder (§6, same honesty as every
other cap here) but the MECHANISM — a coarse total-uncompressed-size guard ahead of the single,
uninterruptible parse call — is now designed, not merely named as a gap, for all three tools. Separately,
`python-docx`/`python-pptx` pin their shared XML-parsing dependency to **`lxml==6.1.2`** (§2b), which
closes not just the lxml-5.0-era XXE default (finding 3's own citation) but also the narrower,
separately-fixed `iterparse()`/`ETCompatXMLParser()` gap (`CVE-2026-41066`, fixed in lxml 6.1) — a more
specific, more current fact than finding 3's own "pin `>=5.0`" suggestion, found by re-checking rather
than assumed to still be state of the art. `openpyxl` (Revision 4) has no `lxml` dependency at all (§2b)
— its own real, checked pin is `openpyxl==3.1.5`, a separate fact, not the same pin reused.

**Sourcing (capability check unchanged; write mechanism updated by finding 20's fix, Revision 7)**: the
capability-check half is identical to `ExtractPdfText`'s own `fetch_via_url`/`fetch_via_path` — the host
does the SAME `find_fs_read`/`find_net_out` check, at the SAME point in the call (before the worker ever
starts), same already-accepted `FileSystemAdapter::read_file()` size-gap residual on the FETCH side, not
re-solved here either. **What changes**: the host writes the fetched bytes into the call's own scratch
directory through the same `MediatedFileSystemAdapter` finding 20's fix now uses for `args.json`/`out.json`,
not a bare `std::ofstream` — one write mechanism for everything that lands in that directory, not two. **The
on-disk name is a fixed literal, `/extract_scratch/source` (Revision 10, round 9 finding 33 — the first
revision to name this explicitly rather than leave it an unstated "path" variable)**: never derived from the
caller-supplied `url`/`path` `Args` field, never carrying the original filename or extension, the same
"host-chosen, never model-influenced" discipline `args.json`/`out.json` already have (finding 1's fix) — no
extension is needed because none of `docx.Document(path)`/`openpyxl.load_workbook(path)`/
`pptx.Presentation(path)` require one; each opens the path purely as a zip container regardless of its name.
This also completes the fixed, enumerable, three-child set finding 33's own `revoke_path()` fallback
extension (§4b, finding 31's fix, above) targets by exact literal name.
**Revision 7's own investigation (see this draft's revision-history note above), unaffected by the pool's
elimination (Revision 8) since it concerns the shared `mount_roots` entry, not how that directory's OS-level
grant was obtained, confirms this write cannot be narrowed out of the shared mount into an independent,
Track-B-style read-only mount, for a real, structural reason**: unlike Track B's one-shot native worker
(which never routes any file access through `dispatch_open` at all), these tools' fixed scripts open the
source document FROM INSIDE the mediated Python interpreter (`docx.Document(path)`/
`openpyxl.load_workbook(path)`/`pptx.Presentation(path)`), so that read is subject to the identical
two-layer `dispatch_open` check (finding 6) as `args.json`/`out.json`, and must live inside the SAME single
`mount_roots` entry finding 4's fix narrows the runner to — a second, independently-granted read-only mount
would work in principle but adds a second `path_prefix`-narrowed capability grant for no real benefit over
what this draft already does. Unchanged by finding 6's fix either way: the `find_fs_read`/`find_net_out`
check this bullet names is a different check, at a different point in the call, from `dispatch_open`'s
IN-worker check finding 6 addresses (reading `args.json`/the source document/`out.json` back out of the
directory once the worker is running).

## 5. BlobRef / bounded-preview discipline (006 §7) — unchanged regardless of route

Whichever worker produces the extracted text, the Reply-building step is the SAME discipline Track A/B
and the OCR draft all share: `ctx.tool_result_byte_threshold` gates whether the full text fits in
`preview` directly or gets truncated with `blob` populated via `ctx.blob_sink`. Each of §3's three tools
gets its own small, standalone `build_reply`-shaped function (matching `extract_pdf_text_detail::
build_reply`'s own precedent of NOT sharing this logic across tools) — no new pattern, no cross-tool
coupling introduced. Revision 2 made explicit what feeds this step for each tool; Revision 3 (round 2
finding 7), then Revision 4 (round 3 finding 11), shifted which tools feed from which channel, not the
discipline itself: as of Revision 4, all three of §3's tools hand this step text read from their own
call's `out.json` off the scratch directory (§4b's finding-2 fix) — one upstream channel, not the
two-channel split earlier revisions had, but the SAME downstream discipline either way, exactly the
property that makes this section still have "no real findings" after any revision's routing change.

## 6. Resource caps (I8) — real numbers not sized here, same honesty as the OCR draft's §5; one route as of Revision 4 (§4)

- **All three tools (§4b, Python-mediated, dedicated per-call `MediatedPythonRunner` each — Revision 4,
  round 3 finding 11 collapses the earlier native/Python split)** — `MediatedPythonConfig`'s existing
  defaults (`exec_wall_ms` 30000 ms, `memory_bytes` 1 GiB, `pids` 8) are sized for a general CodeAct
  session, not benchmarked against what one dedicated worker parsing a `.docx` via `python-docx`, an
  `.xlsx` via `openpyxl`, or a `.pptx` via `python-pptx` (plus the zip-bomb pre-check, §4b) actually
  costs on a MUCH narrower, single-purpose worker than a general session's. Not measured here — real
  implementation-ADR work. These numbers now cover all THREE tools' one-shot dedicated workers, not the
  one Revision 2 narrowed to or the two Revision 3 widened to — still not sized. (§4a's own
  `ResourceLimits`/wall-clock/memory-ceiling framing, reused from Track B's PDFium worker, is no longer
  this draft's v1 concern — kept in §4a itself as the historical/contingency record.)
- **Interpreter/package-import startup cost (§4b's three tools now, not two or one)** — a real, per-call
  fixed cost each dedicated-per-call `MediatedPythonRunner` (§4b's finding-4 fix) pays, that would have
  been avoided for `ExtractXlsxCells` alone had it stayed on §4a's native path (§2c's own weighed, real
  tradeoff, not an oversight); not sized here, matching the OCR draft's own honest "model-load cost is a
  NEW, real... cost" disclosure for a structurally similar reason (heavy fixed cost per invocation of a
  general-purpose runtime, not a purpose-built binary). Revision 2 resolved WHICH lifecycle pays this
  cost (dedicated, always, per §4b's finding-4 fix); Revision 3 widened which tools pay it to two; Revision
  4 widens it to all three (round 3 finding 11) — the actual number stays open for all three. **Revision 5
  (round 4 finding 16) adds the qualitative fact the Revision 4 headcount edit did not**: this is not just
  a per-call-cost widening, it is a change in the WORST-CASE AGGREGATE CONCURRENCY picture, in kind, not
  merely in count. Before Revision 4, an agent batch-extracting a folder of `.xlsx` files under ADR-160's
  real, shipped parallel-batch scheduler (§2c) spawned ZERO `MediatedPythonRunner`/CPython-interpreter
  processes — every one of those calls rode §4a's cheap, purpose-built native worker instead, contributing
  nothing to `MediatedPythonConfig`'s shared 1 GiB `memory_bytes`/8 `pids` concurrent-resource-pressure
  class. As of Revision 4/5, the identical XLSX-only batch spawns one dedicated, full-CPython-plus-
  package-import worker PER call, exactly like DOCX/PPTX batches always did — an entire tool's worth of
  previously-cheap concurrent batches now contributes, for the first time, to the SAME aggregate
  memory/CPU pressure class the other two tools already created. This is a real, new fact about the WORST
  CASE this catalog row can now produce under concurrent load, distinct from "the per-call number is still
  unsized" (which was already true, for whichever tools paid the cost, in every prior revision) — named
  here explicitly so a future reader benchmarking this does not have to re-derive that the worst case
  changed in kind, not just in per-call weight.
- **Zip-bomb total-uncompressed-size threshold (§4b's `zipfile.infolist()` guard, finding 3)** — the
  MECHANISM is now designed for all three tools on the one route they share; the actual byte threshold
  it enforces is not sized here, same "mechanism resolved, number open" honesty as every other cap in
  this section. (§4a's own native-C++/miniz equivalent stays a real, disclosed, still-undesigned item
  in that historical/contingency subsection, not this draft's v1 concern.)
- **Scratch-slot pool size `N` and its acquire wait-ceiling — retired, Revision 8, per Prove pass 1
  (kept here only as a pointer to what replaces it, not a live cap any more).** The pool's own bounded
  wait/fail-closed-exhaustion mechanism (`office_extraction.scratch_pool_exhausted`) no longer exists —
  there is no shared pool of `N` slots to exhaust, so there is no wait-ceiling to size. **What replaces
  it, named honestly rather than left implicit**: each call now creates and grants its OWN scratch
  directory (§4b, finding 9's fix, direct-grant design), so the question this cap used to answer —
  "what happens when concurrent demand exceeds a fixed supply" — has no analog for a per-call resource
  that is created on demand rather than checked out from a fixed pool. This does NOT mean per-call
  scratch-directory/`grant_path()` creation is itself unbounded, though: it is bounded by whatever already
  bounds how many of these three tools' calls can be genuinely in flight at once, which this draft already
  names as a real constraint elsewhere rather than a new one — `rt::ThreadPool`'s own worker-count ceiling
  (ADR-160's parallel-batch scheduler, the same concurrency bound §4b's own pool-sizing prose cited as a
  reasonable default source for `N`) caps how many `MediatedPythonRunner`/`NativeJailBackend` pairs, and
  therefore how many scratch directories, can be genuinely concurrent; the per-turn/session call-count
  budget gap named later in this section (round 2 finding 8, round 3 finding 12) bounds — where a host
  actually wires it — cumulative call volume over time. Neither of those existed to solve THIS problem
  (they bound tool-call concurrency/volume generally, not scratch-directory creation specifically), but
  because scratch-directory creation happens exactly once per call and is retired (finding 15's cleanup)
  before the call returns, it cannot outpace whatever already bounds calls themselves — there is no
  additional, scratch-directory-specific ceiling this design needs to invent. **The honest residual**: a
  host that wires neither a `ThreadPool` concurrency cap nor a `ToolCallHook` call-count budget has no
  backstop against a caller creating many concurrent (or rapidly sequential) scratch directories/ACEs
  either — this is not a NEW gap Revision 8 introduces, it is the SAME already-disclosed cross-cutting gap
  this section's own call-count-budget discussion below already names as genuinely open, now also the
  answer to "what bounds scratch-directory churn" rather than a separate, unexamined question.
- **Per-call OS-call cost of the `grant_path()`/`create_directories()`/`remove_all()` cycle (round 8
  finding 30, Medium, I8) — a real, unmeasured, and, until now, undisclosed resource-consumption axis,
  disclosed here explicitly rather than left implicit.** Each call now pays, in sequence:
  `create_directories()` (an NTFS MFT-record allocation), `grant_path()` (a `GetNamedSecurityInfoW`/
  `SetEntriesInAclW`/`SetNamedSecurityInfoW` read-modify-write cycle against that object's own security
  descriptor — three kernel round-trips, not one), and, at call end, `remove_all()` (MFT-record
  deallocation) — plus, on the rare cleanup-failure path, finding 31's own bounded retry loop and, on
  retry exhaustion, one further `revoke_path()` call (the same three-call shape as `grant_path()`, §4b's
  finding-31 fix). **The eliminated scratch-slot pool paid this exact create/grant sequence exactly `N`
  times, ONCE, at pool-initialization** — every call after that reused an already-granted slot with zero
  further `SetNamedSecurityInfoW` calls; the direct-grant design pays it on EVERY call, forever, in
  exchange for the simplicity and the tighter backstop-exposure bound the collapse already argues for
  (§4b, "Finding 14 — resolved by elimination"). This is very likely small in absolute terms relative to
  the already-disclosed CPython-interpreter-startup cost paid in the same call, and
  `test_native_jail_teardown_cycles_windows.cpp`'s own 300-cycle census (G4) is real, positive-control
  evidence that `grant_path()` itself does not leak handles/memory/ACEs when called repeatedly on the SAME
  path — but that test exercises `grant_path()` on the SAME, never-deleted path across cycles, not this
  draft's own actual pattern (a FRESH directory created, granted, and deleted every call), so it does not
  speak to MFT churn or `SetNamedSecurityInfoW` latency under this draft's specific create/grant/delete-
  per-call shape, a real, distinct, currently-untested pattern. Not sized here — matching this section's
  own "mechanism resolved, number not sized" honesty for `exec_wall_ms`/`memory_bytes`/the zip-bomb
  threshold/the interpreter-startup cost above — named as a real, disclosed tradeoff of the collapse, not
  an unexamined one. **This cycle's own wall-clock relationship to the call's deadline is stated explicitly
  in §4b, not here (round 9 finding 35)**: the retry-loop-plus-`revoke_path()` cleanup sequence (finding 31's
  fix) runs entirely outside `exec_wall_ms`/`ctx.deadline`, so it cannot itself cause an otherwise-successful
  call to fail on the deadline, but it does add real, unsized, uncapped latency to the call's own
  Reply-return time on the failure branch — see §4b, finding 31's fix, "Wall-clock cost of this whole
  sequence," for the full reasoning.
- **Retry-backoff thread occupancy under ADR-160 concurrent dispatch, and correlated concurrent
  retry-loops (round 9 finding 36, Medium, I8) — a real residual this section did not previously disclose.**
  Finding 31's own retry loop specifies "a short backoff between attempts (tens of milliseconds, not
  seconds)" but never states whether that backoff is a blocking sleep on the thread running
  `Tool<>::invoke()` or some asynchronous mechanism; the natural, default reading — the only one consistent
  with `run()`/cleanup already being a synchronous, RAII-scoped call (finding 35, above) — is a blocking
  sleep on that same thread, which under sequential dispatch is whatever called `run_rounds()` and under
  ADR-160's parallel-batch scheduler is one of `rt::ThreadPool`'s own worker threads. A transient
  `ERROR_SHARING_VIOLATION` from an AV/indexing scanner sweeping a batch of just-created scratch directories
  is plausibly a CORRELATED failure across several concurrently-dispatched calls of the SAME batch, not an
  independent one — several `ThreadPool` worker threads could each be independently blocked in their own
  bounded retry-plus-backoff loop at once, each holding its own worker slot for longer than the call's own
  extraction work alone would need. This is real and bounded per call (3 attempts, tens of milliseconds
  each, finding 31's own fix), but its effect on concurrent-batch throughput under a correlated transient
  failure is not sized or mitigated here — the same class of aggregate-concurrency disclosure round 4's own
  finding 16 required this draft to state explicitly for a different, structurally similar shift (the XLSX
  interpreter-startup cost), now named for this mechanism too rather than left implicit.
- **Row/paragraph/cell/slide processing caps** (`rows_processed`/`paragraph_count`.../`slides_processed`
  stopping short of the real total) — same shape as `pages_processed`/`total_page_count` in Track B,
  driven by whichever of wall-clock, memory, or output-byte ceiling fires first; exact thresholds not
  sized here, for any of the three tools.
- **Per-turn/session call-COUNT budget (round 2 finding 8, corrected by round 3 finding 12) — a real,
  better mechanism found this revision, PLUS a genuine cross-cutting engine gap this draft still cannot
  solve.** Round 2 finding 8's own gap ("no bound on call count") still holds: `MaxTurns<N>`/
  `TokenBudget<N>` (`core/agent.hpp`/`core/tool.hpp`) bound overall turn count and cumulative
  model-token spend, neither of which is "how many times did this one tool get called";
  `cap::decl::Background<MaxConcurrent>` (`capability.hpp:434-435`) bounds CONCURRENT background calls,
  not a sequential running total; 023's turn-level `TokenBudget` plumbing "does not exist anywhere in
  this codebase yet" (`output_discipline.hpp`'s own file-top comment). **Round 3 finding 12, checked
  directly, found finding 8's own proposed fix (a `PolicyDecider` closure) has two real gaps**: (1)
  `resolve_approval_outcome` (`tool_pipeline.hpp:518-534`, re-verified) only consults `policy` when
  `provenance != call_provenance::text_derived` — a counting `PolicyDecider` has NO visibility into any
  call that arrives with `text_derived` provenance, exactly the trust class I3 cares most about; (2)
  `PolicyDecider`'s own signature, `(Principal const& caller, ToolDescriptor const& tool, bool
  arguments_tainted)`, carries no turn or call-index signal, so a closure built on it can implement a
  session-cumulative cap but not a genuine per-turn reset. **Revision 4's fix, a genuinely different
  mechanism, not a patched-up `PolicyDecider`**: checked directly against `rt/agent_session.hpp`'s
  `run_rounds()` (re-verified this pass, lines ~2534-2593), OQ-21's `ToolCallHook`
  (`core/tool_call_hook.hpp`, wired via `AgentSession::set_tool_call_hook()`) runs `for (std::size_t i =
  0; i < calls.size(); ++i)` — **once per call, in every round that has any tool calls at all, with NO
  provenance-based skip** (`ToolCallHookContext::provenance` is passed through READ-ONLY/informational;
  nothing in the loop that invokes the hook gates on its value the way `resolve_approval_outcome`'s
  `policy` gate does). This closes finding 12's gap (1) for real, checked directly against the exact
  loop that would run it, not asserted: a host-wired `ToolCallHook` closure closing over a mutable
  counter keyed by `tool_name` — denying via `hctx.denial = ...` once `ExtractDocxText`/`ExtractXlsxCells`/
  `ExtractPptxText`'s combined call count crosses a host-chosen threshold — sees EVERY call to these
  tools regardless of provenance, and requires no `approval_mode::policy_driven` declaration on the
  tools at all (`ToolCallHook` is a session-level opt-in, orthogonal to per-tool `approval_mode`,
  simpler to wire than finding 8's own recommendation). **Gap (2) is NOT solved by this either, and this
  draft states that plainly rather than papering over it**: `ToolCallHookContext` carries `call_id`/
  `tool_name`/`arguments`/`provenance`/`caller` and nothing else — no turn index, no `EffectContext&` (a
  deliberate I2/I3 scoping choice per that header's own file-top comment, not an oversight this draft
  could ask to be widened without its own new review). A host-wired counting closure built on this seam
  can therefore implement a real, working SESSION-cumulative cap covering every provenance — a genuine
  improvement over finding 8's `PolicyDecider`-based proposal, which covered only `vendor_structured`
  calls — but it cannot itself distinguish "the 4th call this turn" from "the 4th call this session"
  without an out-of-band turn-boundary signal that reaches NEITHER `ToolCallHook` NOR `PolicyDecider`
  today. **This is the genuine, cross-cutting engine gap round 3 finding 12 asked this draft to either
  solve or honestly name**: no mechanism in this codebase today lets a host-wired seam of any kind
  distinguish per-turn from per-session call counting for a tool of arbitrary call provenance. It is not
  a placeholder this draft could size away — it is a real, missing engine-level signal (a turn index, or
  equivalent, reaching whichever host-wired hook counts calls) that a future engine-level ADR would need
  to add; this draft's own tools inherit it like every other tool in this codebase, and this draft does
  not attempt to solve it.

CLAUDE.md's "Machine safety" section applies to all of the above — none of these caps are optional even
by default, matching `MediatedPythonConfig`'s own existing comment on why `exec_wall_ms`/`memory_bytes`
are never zero.

## 7. Companion skill — a new skill, not folded into `extracting-document-text`

`extracting-document-text` (Track B, §5 of that draft) is explicitly PDF-scoped in its own name,
description, and body ("instead of reading a PDF's raw bytes"). This draft's tools read a genuinely
different content type (OOXML office documents, not PDFs) — folding it in would mean a "document text"
skill silently starting to talk about spreadsheets and slide decks under a PDF-specific name, the same
content-mismatch reasoning the OCR draft used to justify its own new `extracting-image-text` skill
rather than reusing that name. Revision 2/3/4 note: §4's routing (Revision 2: native for two tools,
Python-mediated for one; Revision 3, round 2 finding 7: native for one tool, Python-mediated for two;
Revision 4, round 3 finding 11: Python-mediated for all three) is an execution-path detail invisible at
the tool-call boundary either way — all three tools still return the same `preview`/`truncated`/`blob`
shape family (§5), so this skill's own guidance needs no per-tool
carve-out for which backend answers which call, regardless of which revision's split is current.

**Recommendation**: a new skill, tentatively `extracting-office-documents`, `allowed-tools:
extract_docx_text extract_xlsx_cells extract_pptx_text`, teaching: (a) these tools return decoded
text/cell values, not files to parse yourself — never fetch a `.docx`/`.xlsx`/`.pptx` via
`read_content`'s raw bytes and attempt to unzip/parse OOXML in the code interpreter yourself; (b) the
byte-threshold preview/`blob` split is pageable, same `reading-large-content` guidance as every other
tool here; (c) `ExtractXlsxCells`'s `sheet_name` defaults to the workbook's first/active sheet, not
every sheet — say so explicitly, mirroring `extracting-document-text`'s own corrected (Revision 1→2)
honesty about what `truncated_pages` can and cannot resume. Final prose not written here — a rough
shape only, matching both prior drafts' own treatment of this section before later revisions.

## 8. What this draft does NOT decide

**Resolved by Revision 2 (no longer open — kept here only as a pointer to where the fix lives):** the
I3 parameterization mechanism (host-written `args.json`, read via `json.load()`, §4b's finding-1 fix);
the output-transport channel (`out.json` read directly off the scratch directory, §4b's finding-2 fix);
the route (a)-vs-(b) recommendation itself, per-format rather than uniform (§2c, finding 5's fix); the
lxml pin (`==6.1.2`, §2b, finding 3's version-floor half); worker lifecycle (dedicated per call, never
the session's shared CodeAct runner, §4b's finding-4 fix).

**Resolved by Revision 3 (round 2's three findings — no longer open):** the capability-layer grant
`dispatch_open` independently requires beyond `mount_roots` narrowing (`Capabilities<
cap::decl::FsRead<"extract_scratch">, cap::decl::FsWrite<"extract_scratch">>` on each tool plus a
matching host-authored session-level `cap::FsRead`/`cap::FsWrite` grant narrowed via `path_prefix` to
`args.json`/`out.json`, §4b's finding-6 fix); the DOCX route choice, re-derived on DuckX's real
(dormant) repository state rather than its README (`python-docx`, route (b), §2c, finding 7's fix); the
per-turn/session call-count question as it stood at that point (finding 8's own `PolicyDecider`-based
fix — since found incomplete by round 3 finding 12, see below).

**Resolved by Revision 4 (round 3's four findings — no longer open):** whether the hybrid was worth its
doubled isolation-mechanism surface once route (b) became unavoidable anyway — actually evaluated, not
just re-asserted, real evidence weighed on both sides including a live, disclosed, already-shipped
`NativeJailBackend` concurrency defect found during the evaluation itself; real, checked conclusion: the
hybrid is collapsed, `ExtractXlsxCells` moves to route (b)/`openpyxl` (§2c, finding 11's fix); the
per-call scratch-DIRECTORY generation mechanism finding 4's own fix never named (`unique_scratch_dir_name()`,
a directory-naming twin of Track B's own `unique_scratch_filename()`, composed with a per-call-fresh,
non-shared `NativeJailBackend` instance, §4b, finding 9's fix); `ExtractDocxText`'s real coverage gap
against `python-docx`'s own documented API (headers/footers/footnotes/endnotes/tracked-changes text/
nested tables named as an explicit v1 scope limitation, `body_text_only: true` on the Reply, §3, finding
10's fix); the per-turn/session call-count seam's own real gaps (a genuinely different, already-shipped
mechanism found — `ToolCallHook`, OQ-21 — that closes the `text_derived`-provenance gap for real, while
the turn-vs-session distinguishing half is named as a genuine, unsolved, engine-level gap rather than
claimed solved, §6, finding 12's fix).

**Resolved by Revision 5 (round 4's five findings — no longer open):** whether the collapse's own "Net
evaluation" still holds now that one of its three named costs (the `NativeJailBackend::instances_` race)
was fixed upstream by commit `40c573e` the same day Revision 4 was written — re-weighed on the two costs
that remain real, real, checked conclusion: yes, the collapse still holds, on weaker but still sufficient
grounds (§2c/§4a, finding 13's fix); the unbounded Windows-ACL growth finding 9's own scratch-directory
fix introduced (`create_python_worker()`'s per-mount grant loop bypassing `grant_ro_deduped()` because the
per-call path is unique by design) — real fix named: grant the shared parent scratch root once, deduped,
via a read-write twin of the already-shipped `grant_ro_path_once()`, and let each call's unique
subdirectory inherit that grant via NTFS's own `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` semantics,
composed against finding 6's `path_prefix` narrowing without disturbing it (§4b, finding 14's fix); the
missing per-call scratch-directory/`args.json`/`out.json` cleanup, plus the narrow stale-directory-reuse
risk under pid reuse — real fix named: recursive best-effort removal on both the success and failure path,
plus a fail-closed pre-existence check, with the "sufficient for v1" reasoning stated explicitly rather
than hand-waved (§4b, finding 15's fix); the qualitative (not just numeric) aggregate-concurrency shift the
collapse causes for XLSX-only batches, previously undisclosed — named explicitly (§6, finding 16's fix);
`ExtractXlsxCells`'s own missing Reply-shape-level disclosure for `openpyxl`'s `read_only=True`-specific
merged-cell and dimension-trust gaps — a new `merged_cells_collapsed: true` field plus explicit prose,
matching `body_text_only`'s own treatment for DOCX (§3, finding 17's fix).

**Resolved by Revision 6 (round 5's two findings — no longer open):** whether finding 14's own
shared-inheritable-parent-grant fix, while mechanically sound Windows ACL inheritance (round 5's own
confirmed-genuine investigation), quietly widened this codebase's own documented OS-layer "backstop"
(`native_jail_backend.hpp:15-19`) from a per-call-narrow boundary into a standing, cross-call/cross-
session/cross-tool grant reachable by any worker compromised via a hostile input file — real, checked
conclusion: yes, it did, because every worker of these three tools still authenticates as the SAME
process-wide `shared_profile()` SID (ADR-004 §3's own locked "one profile, reused across sessions"
design); real fix named: retire the shared-inheritable-root shape entirely in favor of a fixed, small,
host-chosen pool of `N` individually pre-granted scratch slots, checked out under mutex/condition-variable
discipline and wiped both before handout and after release — bounding BOTH finding 14's ACL-growth axis
(exactly `N` grants, ever) AND finding 18's backstop-scope axis (bounded to `N` concurrently-live calls'
worth of exposure, not the whole process's lifetime) at once, with the residual bounded-not-eliminated
exposure disclosed explicitly and argued from this codebase's own already-Judged
`decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md` precedent for why a bounded, disclosed
backstop residual is an acceptable v1 posture given the software layer (`dispatch_open`, finding 6) remains
the real, independently-sound primary boundary (§4b, finding 18's fix); whether finding 14's own fix ever
named the real bootstrap-ordering sequence its two-level directory structure needs — real, checked
conclusion: no, a genuine three-step cold-start dance was needed and never spelled out; real fix named:
the scratch-slot pool's own lazy, mutex-guarded, fully-sequenced initialization (mirroring
`shared_profile()`'s own lazy-singleton pattern, including its own already-fixed "retry on transient
failure" discipline) that never publishes a slot to any caller until every slot has already been created
AND granted, closing the cold-start hazard structurally rather than by getting a lazy sequence's ordering
right by convention (§4b, finding 19's fix).

**Resolved by Revision 7 (round 6's four findings — no longer open, after first investigating whether a
fundamentally different I/O design could avoid this class of problem rather than fix the pool in place a
further time; see the revision-history note at the top of this draft for the full investigation):**

- **Finding 20 (Critical) is resolved, not by eliminating the scratch-slot pool but by replacing the
  unsafe read/write primitive it (and `args.json`'s write, and the source document's write) rode on.**
  Investigated first: does `ExecOutcome::artifacts` (round 1 finding 2's own original, un-adopted
  suggestion) already use safe containment, making it structurally immune to the reparse-point attack
  finding 20 found? Yes — `harvest_mount`'s own `FileSystemAdapter` reads/writes are, for the real
  production adapter (`MediatedFileSystemAdapter`), already built on `open_within_mount_root`
  (ADR-014's TOCTOU-safe primitive) — verified directly. But `harvest_mount` itself needs a real,
  already-committed worktree `Mount`/`Ref`/`WorktreeObjectStore` — genuine turn/session-artifact-tracking
  machinery this draft already correctly judged (Revision 2) too heavy for an internal, single-call
  result-transport channel, a judgment re-confirmed, not reversed, this pass. Real fix: reuse
  `MediatedFileSystemAdapter` directly and standalone (`MediatedFileSystemAdapter::create(lease.host_path())`),
  with no worktree/Ref dependency at all, for `args.json`'s write, the source document's write, and
  `out.json`'s read — the same containment discipline `dispatch_open` itself relies on, applied to the
  host's own side of the channel for the first time, closing the exact gap a plain `std::ifstream`/
  `std::ofstream` left open. The coarse pre-read size cap is preserved via `list_directory("")`'s
  per-entry `size_bytes`, not `std::filesystem::file_size` on an unopened path (§4b, finding 20's fix).
- **Finding 21 (High) is resolved in place — the wipe's own "best-effort" vs. "provably-empty"
  self-contradiction is closed by checking the result, not asserting it.** `acquire()`/the lease
  destructor now inspect the `remove_all` error code AND independently verify emptiness via
  `list_directory("")` after wiping; either signal quarantines the slot (permanent removal from the
  free-list, never silently recirculated and never silently left in service) rather than leaving the
  outcome undecided between the two silent failure modes finding 21 named (§4b, finding 21's fix).
- **Finding 22 (Medium-High) is resolved in place — the incorrect `std::condition_variable` FIFO claim is
  withdrawn, not repaired, and the pool's actual wait idiom is named concretely for the first time.**
  Neither the C++ standard nor this codebase's real target platforms guarantee `notify_one()` wakes
  waiters in FIFO order; the fix drops that claim (accepting unfair-but-bounded waiting as a real, minor,
  disclosed residual rather than inventing a ticket/queue mechanism) and names the actual, race-free
  idiom `acquire()` uses: the predicate form of `wait_for`, the same concreteness `unique_scratch_filename()`'s
  own atomic-counter citation already gives its primitive (§4b, finding 22's fix).
- **Finding 23 (Medium) is resolved in place — a minimal, in-memory lease-attribution log closes the I4
  gap.** `acquire()`/release now record slot index, lease-start/end timestamps, the acquiring tool
  name/call id, and the release outcome (clean vs. quarantined, finding 21's own new outcome) in a
  bounded, most-recent-`N`-events ring log — no new persistent-logging mechanism, but enough to make
  finding 18's own "bounded to `N` slots' worth of exposure" claim reconstructible after the fact rather
  than merely asserted (§4b, finding 23's fix).

Finding 15 remains resolved exactly as Revision 5 left it — its cleanup fix was never reopened by any
later round, and Revision 8 (below) restores it as the mechanism's own real, final retirement step, not
merely as history. Findings 14, 18, and 19, by contrast, do NOT remain resolved as Revision 6/7 left
them — see "Resolved by Revision 8" immediately below.

**Resolved by Revision 8 (Prove pass 1 — no longer open):** whether the scratch-slot pool built across
Revisions 6-7 to bound findings 14/18/19, and hardened against findings 21/22/23/25/26/27, was solving a
problem that actually existed — actually re-checked, not re-patched: Prove pass 1 (full text below, after
"Red-team round 7") read `AppContainerProfile::grant_path()`'s own body
(`app_container_profile.cpp:152-205`) directly and found the ACE it adds lives on the granted path's OWN
NTFS security descriptor, not on any `AppContainerProfile`-wide ledger — so finding 15's own already-
designed unconditional `remove_all()` cleanup (Revision 5, one subsection before finding 14's pool was ever
introduced, never reopened since) already destroys that ACE as a structural side effect of deleting the
object it was granted on, PROVIDED the grant lands on the call's own leaf directory directly (finding 9's
original shape) rather than on the shared, never-deleted root finding 14's own fix chose specifically to
avoid a per-call `grant_path()` call. That choice is what manufactured finding 18's standing backstop
exposure, which then required the pool findings 18/19/21/22/23/25/26/27 spent three revisions building and
hardening to bound. **Real, checked conclusion: findings 14, 18, 19, 21, 22, 23, 25, 26, and 27 are all
resolved by elimination, not by further patching** — §4b now grants each call's own
`unique_scratch_dir_name()` directory (finding 9, unchanged) directly via the already-shipped `grant_path()`
(no dedup — the path is used by exactly once, ever, so there is nothing to dedup) and retires it via finding
15's already-designed cleanup (unchanged); no pool, no `revoke_path()` (real and small, per Prove pass 1,
but unnecessary), no new mutex/condition-variable/free-list/quarantine/ring-log. See §4b, "Finding 14 —
resolved by elimination," for the full mechanism, and "Findings 18, 19, 21, 22, and 23 — resolved by
elimination" for why none of the pool's own internal-reliability fixes have anything left to protect.
**Findings 6 and 20 remain resolved, restated for the simpler per-call design they now compose with rather
than because either changed**: finding 6's capability-layer composition
(`Capabilities<cap::decl::FsRead<"extract_scratch">, FsWrite<"extract_scratch">>`, narrowed via `path_prefix`
to `args.json`/`out.json`) is, if anything, cleaner to state now than for the shared-root or pool designs —
this is close to the exact shape finding 6 was originally verified against (Revision 3, before finding 9
even introduced a per-call directory), with no inheritance or lease indirection to re-derive the argument
through (§4b, finding 14's resolution, "Composition with finding 6's `path_prefix` narrowing"). Finding
20's fix (`args.json`/`out.json`/the source document routed through `MediatedFileSystemAdapter` instead of a
bare `std::ifstream`/`std::ofstream`) is genuinely orthogonal to the pool-vs-per-call-grant question — it is
kept unchanged, now constructed against the call's own scratch directory instead of a leased pool slot
(§4b, finding 20's fix).

**Finding 24 (the ADR-041-analogy weighing) and finding 29 (finding 24's own untracked-in-§8 gap) are both
re-assessed, not carried forward unexamined**: finding 24 challenged the pool's own citation of
`decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md` as justifying a bounded-not-eliminated
backstop residual, on the grounds that the pool's residual (self-inflicted, live per-tenant document data,
argued-by-analogy) was a materially different kind from ADR-041's own (OS-inherited, two static non-secret
files, proven-by-an-executed-test). That weighing was real and correct AT THE TIME, but it was a weighing
of a STANDING residual that no longer exists: the direct-grant design (finding 14's resolution) has no
cross-call OS-backstop sharing at all — each call's directory is uniquely granted and then deleted, so there
is no residual moment where a compromised worker of one call could reach another call's already-finished
data at the backstop layer, the way the pool's own `N` shared, reused slots could. There is nothing left for
an ADR-041-style analogy to justify, so finding 24 is now moot, not merely deferred — a real, checked
conclusion, not an assumption Revision 8 is asking to be taken on faith (see §4b, "Findings 18, 19, 21, 22,
and 23 — resolved by elimination," for the same point stated in-line with the mechanism it applies to).
Finding 29's own concern — that finding 24 was honestly disclosed as deferred in this draft's revision-
history prose but left untracked in §8's own ledger, with the specific sentence finding 24 disputed still
standing, unmarked — is resolved as a direct consequence: the sentence finding 24 disputed (finding 14's
own "permanently and irrevocably grows the shared `AppContainerProfile`'s Windows ACL" framing) is deleted,
not merely corrected, along with the shared-root design it described (§4b, finding 14's resolution), and
finding 24's own status is tracked here, in §8, in this same revision that resolves it — the process gap
finding 29 found is closed by the same act that makes finding 24 itself moot, not by a separate tracking
fix. **What is NOT claimed**: Revision 8's own conclusion is argued from `grant_path()`'s real code and
ordinary NTFS/AppContainer semantics, not from an executed test — Prove pass 1 says this plainly, and so
does this draft — a real red-team round of its own (checking, at minimum, ACE-inheritance-across-deletion
and post-deletion handle-retention behavior, Prove pass 1's own named checks) remains real, undesigned
future work before this ships, the same "argued, not yet measured" honesty finding 24 itself demanded of
the pool's own ADR-041 citation, now applied to this pass's own conclusion.

**Resolved by Revision 9 (round 8's three findings — no longer open):** whether finding 15's own cleanup
fix needed to guard the crash case alone or also a real, non-crash `remove_all()` failure — round 8 found
the latter was never named, and, because Prove pass 1's own collapse made cleanup success the SOLE
retirement mechanism for the per-call ACE, an unchecked failure here silently reproduces finding 14's own
unbounded-ACE-growth shape through a different door; real fix named: check `remove_all()`'s own `ec` (the
same non-throwing overload finding 15's fix already calls), retry a small, bounded number of times
(transient sharing violations typically clear within milliseconds), and on exhausted retries both surface a
loud, attributable failure event (finding 32's own mechanism, composed explicitly) and fall back to a
narrowly-scoped `AppContainerProfile::revoke_path()` — built for real this time, the one place in the whole
draft where the small, buildable-but-until-now-unnecessary primitive Prove pass 1 verified actually earns
its keep, as a failure-path-only safety net, never the primary mechanism (§4b, finding 31's fix, immediately
after finding 15's own discussion). Whether the per-call design carried any audit trail at all for
scratch-directory lifecycle, now that the pool's own lease-attribution ring log is gone along with the rest
of the pool — real, checked conclusion: no, and this is a real degradation, not a pure simplification,
correctly scoped down from the ring log's own cross-call-attribution complexity (which has no analog for a
single-use directory) rather than left unreplaced; real fix named: a structured event at
directory-creation and directory-deletion/cleanup-outcome time, reusing `EffectContext::report_progress` —
ADR-060's real, already-shipped, call-scoped run-event channel, already used by this exact backend file
(`native_jail_backend.cpp:901`) for a comparable per-call purpose — rather than a new logging mechanism,
composed explicitly with finding 31's fix so the failure-path event satisfies both findings at once (§4b,
finding 32's fix). Whether the collapsed per-call design's own `grant_path()`/`create_directories()`/
`remove_all()` cycle has a real, unmeasured OS-call cost the now-eliminated pool was incidentally
amortizing across `N` calls — real, checked conclusion: yes, and it was undisclosed; named explicitly,
unsized, matching this draft's own "mechanism resolved, number not sized" honesty for every other cap (§6,
finding 30's fix).

**Resolved by Revision 10 (round 9's five findings — no longer open):** whether finding 31's own
`revoke_path()` fallback actually closed off a stranded directory's CONTENTS, not merely its listing — round
9 found it did not: `grant_path()`'s `(OI)(CI)` inheritance flags independently materialize a copy of the
SID's ACE onto `args.json`/`out.json`/the source-document copy the moment each is created inside the granted
directory, and `revoke_path(dir)`'s own single, non-recursive call touches only `dir`'s own security
descriptor, leaving each child's own copy untouched and each child fully reachable by full-path open under
the shared SID for as long as it survives; real fix named: extend the fallback to a fixed, four-path
`revoke_path()` enumeration — the directory plus its (up to) three known children, `args.json`, a
newly-literal-named `source`, and `out.json` — never a recursive tree walk, with a missing child treated as
trivially satisfied rather than an error, and the design's own safety-claim prose corrected in place to state
precisely what content reachability is closed when all four attempts succeed and what remains reachable, by
exact object, when one does not (§4b, finding 31's fix, "Revision 10, round 9 finding 33"). Whether a
per-child `revoke_path()` failure shares `remove_all()`'s own open-handle root cause — real, checked
conclusion, resolved rather than left as a named-but-unexamined possibility: no, or at least very unlikely
to, for a real, stated reason — `revoke_path()`'s three Win32 calls request only `READ_CONTROL`/`WRITE_DAC`,
access rights NTFS's own sharing-violation check does not arbitrate, the same reason `icacls`/`takeown` can
repair permissions on a file another process holds open, a conclusion that applies identically to the
directory-level and every per-child `revoke_path()` call and is why no new bounded-retry loop is added around
any of them (§4b, finding 31's fix, "Does a per-child `revoke_path()` call share `remove_all()`'s own
open-handle failure mode"). Whether the cleanup-plus-revoke sequence's own wall-clock cost is bounded by, or
exempted from, this draft's own deadline enforcement — real, checked conclusion: it runs entirely outside
both `exec_wall_ms` and `ctx.deadline`, confirmed directly against `tool_pipeline.hpp`'s and
`agent_session.hpp`'s real enforcement/bracket code, so a slow cleanup cannot itself cause a deadline failure
but does add real, unsized, uncapped latency to the call's own Reply-return time on the failure branch,
stated explicitly rather than left for a reader to re-derive (§4b, finding 31's fix, "Wall-clock cost of this
whole sequence"; cross-referenced from §6 alongside finding 30). Whether the retry loop's own backoff blocks
the calling thread, and how that composes with ADR-160's `ThreadPool` worker-count ceiling under a correlated
transient failure across a concurrently-dispatched batch — real, disclosed residual, not resolved away: the
natural reading is a blocking sleep on the calling thread, real and bounded per call but unsized in
aggregate, named explicitly rather than left implicit (§6, finding 36's fix). Whether the "Correction
(Revision 9)" paragraph's own stale, unqualified bolded conclusion ("`revoke_path()` is not built for v1")
was left standing, false, after the reasoning around it was narrowed — real, checked conclusion: yes, and it
is corrected in place, not merely annotated nearby, per its own fix direction (§4b, "Why not `revoke_path()`
either").

**Still genuinely open:**

- **§2c/§8 (narrowed by Revision 4, round 3 finding 11 — no longer this draft's v1 recommendation, but
  a live option to revisit)**: the exact choice between `xlnt` and `OpenXLSX` for `ExtractXlsxCells`,
  IF a future revision reopens route (a) for this format — both remain real, live PDFium-tier
  candidates (§2a); this draft does not pick between them, and does not need to for v1's own
  recommendation (openpyxl, §2c).
- **§4a (moot for v1 as of Revision 4, kept as the historical/contingency record §2c's own disclosure
  discipline calls for)**: the native-worker-binary shape for `ExtractXlsxCells`, IF route (a) is ever
  reopened for this format — whether it gets a dedicated worker binary of its own (mirroring
  `pdf_worker_main.cpp`'s one-binary-per-format precedent), its own internal shape, and the native
  zip-bomb-guard equivalent named in §4a (a total-uncompressed-size check via miniz's own per-entry size
  fields) all remain undesigned in the code-level detail §4b gives the Python-side `zipfile.infolist()`
  guard. **Revision 5 (round 4 finding 13) update**: reopening this path no longer needs a NEW
  concurrency-safety design — `NativeJailBackend::instances_`'s own race is fixed as of commit `40c573e`,
  so a native worker built exactly Track B's shape would inherit the fixed, mutex-guarded map, same as
  Track B's own PDF tools do today (§2c/§4a). What genuinely remains undesigned if this path is reopened
  is only the vendoring/build effort and the native zip-bomb guard, as this bullet already states.
- **§2b item 2 (widened by Revision 4, round 3 finding 11, to cover all three tools)**: the exact,
  complete transitive package-policy allowlist for `ExtractDocxText` (`docx`, `lxml`,
  `typing_extensions`), `ExtractXlsxCells` (`openpyxl`, `et_xmlfile`), and `ExtractPptxText` (`pptx`,
  `lxml`, `PIL`, `xlsxwriter`) — and whatever ELSE any of the three packages imports internally that
  this pass did not exhaustively trace — is a real implementation task (actually running each package
  under its own dedicated worker's lockdown allowlist and observing what it needs) required before any
  of the three specific `package_policy_allowlist` sets can be trusted.
- **§2b/§4b (widened by Revision 4, round 3 finding 11, to cover all three tools)**: the Windows-only
  status of the whole `MediatedPythonRunner`/`create_python_worker()` subsystem — a real, pre-existing
  platform gap (ADR-002 §8/§9) this draft inherits rather than solves; Revision 2 narrowed its blast
  radius to one tool, Revision 3 widened it to two, Revision 4 widens it to all three (round 3 finding
  11) — no tool in this draft's v1 scope has a cross-platform-native fallback any more, a real, disclosed
  cost of finding 11's own conclusion (§2c). Whether this project wants to build POSIX support for that
  subsystem at all, and on what timeline relative to this catalog row, is a project-owner-level
  prioritization call, not a design-draft one.
- **§4b**: whether a dedicated, narrowly-`mount_roots`-scoped (and, per finding 6's fix, narrowly
  capability-granted, and, per finding 9's fix, freshly-directoried AND freshly-backend-instanced)
  `MediatedPythonRunner` per call, reused across calls to the SAME tool within the SAME session (cached
  rather than re-spawned every call) or genuinely re-created every single call, is worth designing as a
  further optimization once the per-call interpreter-startup cost (§6) is actually measured — this
  revision picks "dedicated, never the session's shared CodeAct runner, never a shared `NativeJailBackend`
  instance either" for I2/finding-9 reasons, but does not decide whether "dedicated but session-cached"
  is a safe, worthwhile refinement of that for any of the three tools. **New wrinkle this revision
  surfaces for that future exploration (finding 9's own fix)**: a session-cached dedicated runner would
  need to keep its OWN call-local `NativeJailBackend` instance alive across calls too, not just its own
  `mount_roots`/capability grant — reusing one instance across calls is exactly the kind of sharing this
  revision's own backend-freshness fix moves away from, so that future optimization would need its own
  fresh argument for why reintroducing a shared instance across calls (even if still session-scoped, not
  process-wide) does not reopen finding 11's own disclosed `instances_`-map race.
- **§4a/§4b**: the exact zip-bomb/total-uncompressed-size THRESHOLD (bytes) each mechanism enforces —
  the mechanism is designed in §4b for v1 (and in §4a's own historical/contingency record), the number
  is not.
- **§4b (finding 6's own residual)**: the exact `size_cap_bytes`/`quota_bytes`/`file_count_cap` values
  on the session-level `cap::FsRead`/`cap::FsWrite` grants finding 6's fix adds — named as real,
  parameterized fields the fix should set, not left uncapped, but the actual numbers are not sized
  here, same "mechanism resolved, number open" honesty as every other cap in this draft.
- **§4b (finding 14's own residual — moot, Revision 8, per Prove pass 1)**: Revision 5's own residual here
  (extending `grant_ro_deduped()`'s dedup bookkeeping to a prefix-covered check for `create_python_worker()`'s
  per-mount grant loop) does not apply to the direct-grant design either, for a different reason than
  Revision 6 gave — there is no inherited-ACE blind spot to close because there is no inheritance at all:
  each call's directory is granted directly, not via an ancestor. Nothing replaces this bullet; the
  question it asked no longer has a design surface to be asked about.
- **§4b (finding 15's own residual, restored — Revision 8, per Prove pass 1)**: Revision 6 treated this
  residual as retired by the pool's own fixed slot-literal naming; with the pool eliminated,
  `unique_scratch_dir_name()`'s pid+counter construction (finding 9, unchanged) is the mechanism again, and
  its own accepted residual is back exactly as Revision 5 stated it — whether the fail-closed `exists()`
  pre-check plus unconditional cleanup is a sufficient, permanent answer to the narrow crash/pid-reuse
  collision window, or whether `unique_scratch_dir_name()` should gain a boot-time/process-start-timestamp
  component to narrow that window further, remains a real, live option to revisit (§4b, finding 15's fix),
  not decided here.
- **§4b (findings 18/19's own residuals — moot, Revision 8, per Prove pass 1)**: the exact pool size `N`,
  the acquire wait-ceiling, and the mode-conflation gap in `grant_ro_deduped()`/`grant_rw_path_once()`'s
  shared dedup set are all moot — there is no pool, no wait, and no `grant_rw_path_once()` call to conflate
  a mode on (the direct-grant design calls the existing, unmodified `grant_path()` once per call, the same
  call `create_python_worker()`'s own per-mount loop already made before finding 14 ever existed). **What
  remains real, live future work in its place (Prove pass 1's own honesty about its limits, not this
  draft's)**: a real red-team round of the direct-grant design itself, checking, at minimum, that a
  freshly-recreated directory at a reused name never inherits a predecessor's ACE via any path other than
  `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` from a still-live ancestor grant (it should not, since the
  per-call leaf is granted directly, never inheriting from a granted root), and that a worker's open handle
  into its own directory, opened before a raced deletion, does not create a reachability surprise after the
  object is unlinked — both believed true from ordinary NTFS/AppContainer semantics and from `grant_path()`'s
  own code, neither literally executed and observed by Prove pass 1 (its own "What is not yet proven, named
  honestly" section, below "Recommendation," after "Red-team round 7").
- **§4b (finding 31's own residual, new — Revision 9, round 8; the "does `revoke_path()` need its own
  retry" half resolved by Revision 10, round 9 finding 34)**: the exact retry count and backoff
  duration for a failed `remove_all()` (this draft names "3 attempts, tens of milliseconds" as reasonable
  headroom, not a measured or sized value) remains unsized. Whether `revoke_path()` itself can fail in a way
  that also needs a bounded retry is now answered, not merely disclosed as an open possibility: reasoned
  no, or at least unlikely to for the same root cause, because its `READ_CONTROL`/`WRITE_DAC` access request
  is not arbitrated by NTFS's sharing-violation check (§4b, finding 31's fix, "Does a per-child
  `revoke_path()` call share `remove_all()`'s own open-handle failure mode") — argued from documented Win32
  semantics and `revoke_path()`'s own real body, not from an executed test, so the residual narrows to
  "unproven by execution," not "unexamined." If any of the (now four, per finding 33) `revoke_path()` calls
  fails anyway, the affected object remains both undeleted/still granted and, if it is the directory,
  unreachable-by-listing but not necessarily by content — finding 32's own extended log entry is the only
  signal, and no further automated recovery is designed here; whether a periodic host-side sweep of the
  shared scratch root for old, orphaned directories belongs in a future revision as a backstop independent of
  any single call's own retry loop — a real, live option named, not designed here.
- **§4b (finding 33's own residual, new — Revision 10, round 9)**: the per-child `revoke_path()` extension
  itself, and finding 34's own WRITE_DAC/READ_CONTROL-not-blocked-by-sharing-violation reasoning it depends
  on, are argued from `grant_path()`'s/`revoke_path()`'s own real code and ordinary, documented NTFS/AppContainer
  semantics, not from an executed test — the same "argued, not yet measured" status Prove pass 1's own
  central claim already carries, now extended to this claim too, not a weaker standard. This is the concrete
  gap this draft's own closing note (end of this section) names as the real next step.
- **§4b/§6 (finding 30's own residual, new — Revision 9, round 8)**: the actual per-call `grant_path()`/
  `create_directories()`/`remove_all()` OS-call cost, in wall-clock or kernel-time terms, relative to the
  already-unsized CPython-interpreter-startup cost it is paid alongside — not measured;
  `test_native_jail_teardown_cycles_windows.cpp`'s own 300-cycle census is relevant, positive-control
  evidence that `grant_path()` itself is not leaky, but is not dispositive for this draft's own
  create-grant-delete-per-call pattern specifically (round 8's own finding-30 text explains why the two
  patterns differ).
- **§6 (finding 12's own residual, Revision 4)**: whether a host actually wires
  `AgentSession::set_tool_call_hook()` at all (a prerequisite for finding 12's `ToolCallHook`
  call-count-cap seam to ever be consulted) is a real design choice this draft names but does not make;
  if a host does wire a counting hook, what session-cumulative threshold it should default to
  recommending is not sized here either; and — distinct from either of those, a genuine engine-level gap
  rather than a residual this draft could size away — no per-turn-vs-per-session distinguishing signal
  reaches ANY host-wired call-counting seam in this codebase today (§6's own finding-12 fix explains why
  this is not this draft's to solve).
- **§3**: the exact preview/table-rendering format for `ExtractDocxText`'s embedded (top-level, per
  finding 10's own v1 scope limit) tables (a delimited text block was named, not designed in detail);
  `ExtractXlsxCells`'s exact cell-to-text serialization (CSV-style? a fixed-width form? not decided, and
  now needs re-deriving against `openpyxl`'s own API per Revision 4/finding 11's route change, not
  `xlnt`/`OpenXLSX`'s — the SAME "not decided" status finding 11's route swap reopens, having briefly
  been assigned to xlnt/OpenXLSX's own API in Revision 2/3); whether `ExtractXlsxCells` should support a
  `max_rows` argument distinct from the wall-clock/memory-driven cap; whether `ExtractDocxText`'s v1
  scope limitation (finding 10) should ever be extended to headers/footers specifically (named in §3 as
  the comparatively cheap half of that gap) — not decided, a real future-revision question.
- **§6**: real `exec_wall_ms`/`memory_bytes` values for the one route now serving all three tools, and
  the interpreter-startup-cost number for all three tools specifically — explicitly unmeasured
  placeholders' absence, not placeholders themselves (this draft does not even guess numbers the way the
  OCR draft guessed 512 MiB/30000 ms for Tesseract). Revision 4 (round 3 finding 11) collapses WHAT is
  being measured back down to one route's three dedicated Python workers' limits — simpler than
  Revision 3's own native-plus-two-Python split, but still not measured.
- **§7**: the skill's own final prose.
- Whether this ships as a native `Tool<>` in the WASM-plugin-ABI sense at all versus waiting for the
  production plugin loader — same open question every prior draft in this series has left open,
  unchanged here.
- **Any red-team round 6.** Round 5 already happened (below) and found two real defects (findings 18/19),
  both design-fix-pass-resolved above (§4b), not merely re-disclosed — this is Revision 6, a design-fix
  pass responding to round 5, not a sixth adversarial pass. Track B was not implementation-ready until
  Revision 7, after six real adversarial rounds found real defects design-fix passes alone have no chance
  to catch. Treat every design choice above — especially §4b's new scratch-slot-pool fix for findings
  18/19 (the pool-initialization sequencing, the acquire/release RAII shape, the bounded-not-eliminated
  backstop-exposure argument grounded in `ADR-041`, and the rejected per-call-SID/`revoke_path()`
  alternatives) — as a real, considered position that has been checked against the actual code and actual
  external facts five times now (round 1, round 2, round 3, round 4's own fixes, and round 5's own
  findings), not as adversarially reviewed a sixth time. A round-6 red-team pass against THIS revision's
  own new mechanism — the pool's own concurrency discipline (acquire/release correctness under real
  concurrent load, the wipe-on-acquire/wipe-on-release ordering, the wait-ceiling's own fail-closed
  behavior) being the most obvious place for one to look, given every prior new mechanism in this draft
  has had at least one real defect found in it on its first adversarial pass — is the natural next step,
  not yet done here. **One item a prior pass surfaced that is no longer open, closed by events rather than
  by this draft, named here so the record is not stale**: `NativeJailBackend::instances_`'s own
  unsynchronized concurrent-access defect (§2c, §4a), disclosed by Revision 4 as a real, live, out-of-scope
  bug in ALREADY-SHIPPED Track B code, was fixed directly in that code by commit `40c573e` (2026-09-01),
  verified against the real diff and its own repro evidence in §2c/§4a above (finding 13). It no longer
  needs a separate bug report/ADR — it already got one, in the form of a real, tested, merged fix,
  independent of this draft. This draft's own contribution was disclosing it, not fixing it; noting that
  the disclosure's underlying defect is now resolved is the same "don't silently move on" discipline this
  draft applied when it first surfaced the bug, applied again now that the bug's status has changed. **Note
  (Revision 8, per Prove pass 1)**: this bullet's own text was already stale before this revision (it still
  says "this is Revision 6," unedited through Revision 7); its call for a round-6 pass against "the pool's
  own concurrency discipline" is now doubly moot, since round 7 already ran (below) and Revision 8 has since
  eliminated the pool entirely (§4b, "Findings 18, 19, 21, 22, and 23 — resolved by elimination"). Left here
  rather than rewritten, per this draft's own history-preserving discipline — the `instances_`-race item
  immediately above remains accurate and is unaffected.

**Closing note (Revision 10) — this is intended as the last pure-design-review pass in this series.** Ten
revisions and nine red-team rounds have converged on a design (§4b's direct-grant, self-deleting per-call
scratch directory, `MediatedFileSystemAdapter`-mediated reads/writes, and a narrowly-scoped, four-path
`revoke_path()` failure fallback) with no open Critical or High finding left unaddressed in the active
design text — every finding this pass could resolve by reading real code and documented Windows/NTFS
semantics more carefully has been. What remains is not another round of paper review; it is the one honest
gap that reading code alone cannot close: this draft's own central claim — that deleting an NTFS object
destroys the ACE granted on it, because the ACE lives on that object's own security descriptor rather than
on any profile-wide ledger (Prove pass 1) — and this revision's own extension of it — that a fixed,
four-path `revoke_path()` enumeration closes content reachability the way directory deletion normally does,
and that `revoke_path()` itself does not share `remove_all()`'s open-handle failure mode (finding 34) — are
both argued from `grant_path()`'s/`revoke_path()`'s real bodies and ordinary, documented Win32/NTFS access-
check semantics, not from anything this draft or any of its nine red-team rounds has actually executed and
observed. Nine rounds of adversarial reading have not falsified either claim, which is real evidence they are
probably correct — but "probably correct, argued from code" is a different, weaker thing than "proven, by a
positive-control test that would fail if the claim were wrong," and this draft has said so honestly at every
point this gap was found (Prove pass 1's own "what is not yet proven, named honestly" section; round 8's and
round 9's own "argued, not yet measured" framing, both restated above). The real next step is building that
test — at minimum: (1) grant a directory, create a child file inside it, delete the directory, and confirm
by direct enumeration/ACL query that no live grant on the child's own security descriptor survives the
deletion where expected, and does when the fallback's per-child `revoke_path()` is skipped; (2) confirm a
freshly-recreated directory at a reused name never inherits a predecessor's ACE from a still-live ancestor
grant; (3) confirm `revoke_path()` genuinely succeeds against a path another process holds open in a way that
blocks `remove_all()`, and fails or succeeds as this draft predicts when it does not — not another round of
red-teaming this draft's own prose.

## Prove pass 2 — positive-control test built and executed

Items (1) and (2) above are now proven, not argued: `AppContainerProfile::revoke_path()` was implemented
for real (`src/backends/native_jail/app_container_profile.hpp`/`.cpp`, mirroring `grant_path()`'s own
three-Win32-call shape with `SetEntriesInAclW`'s `ACCESS_MODE` flipped to its documented `REVOKE_ACCESS`
member — no other code path touched), and a real, executed positive-control test —
`tests/test_native_jail_grant_path_ace_lifecycle_windows.cpp`, registered in `tests/CMakeLists.txt`
immediately after `test_native_jail_grant_ro_path_once_windows` — proves both of this draft's central
claims by direct `GetNamedSecurityInfoW`/ACE-walk query, not by reading code:

- **Claim 1 (Prove pass 1's own foundational claim, "Fix for finding 14")**: `grant_path()` on a real
  directory, followed by creating a file inside it while the grant is live, followed by `remove_all()`,
  followed by re-creating a fresh object at the exact same path with no new grant call — the fresh
  object carries zero trace of the deleted object's ACE. Deletion genuinely destroys the grant; nothing
  survives at any profile-wide level. The same run also proves finding 33's own premise directly: a file
  created inside a granted directory independently materializes its own copy of the ACE (present on the
  child, not merely inherited-by-reference from the still-live parent).
- **Claim 2 (Revision 9/10's `revoke_path()` safety net, "Fix for finding 31"/finding 33)**: granting a
  directory, creating three children (`args.json`, `source`, `out.json` — the design's own fixed
  literals), confirming each independently carries the ACE, then calling `revoke_path()` against the
  directory and all three children (plus a fourth, deliberately never-created path, proving the
  "missing child, trivially satisfied" branch is a real no-op via `ERROR_FILE_NOT_FOUND`, not a
  failure) — afterward, zero ACEs for the profile's SID remain on any of the four, while every OTHER
  SID's own ACE count on the directory is unchanged (a real, surgical revoke, not a side effect of
  clearing the whole DACL). One real, initially-surprising finding surfaced by running this rather than
  reading it: a single inheritable `grant_path()` call legitimately materializes TWO ACEs for the same
  SID (an effective ACE on the object itself plus an inherit-only ACE for future children) — real
  Windows ACL canonicalization, not a defect — so the test asserts on ACE *count-for-our-SID*, not a
  hardcoded delta, once this was observed and understood rather than assumed away.

Both new tests pass; the full `native_jail`/`extract_pdf`/`pdf_text`-prefixed suite was re-run afterward
and shows zero regressions from adding `revoke_path()`.

**Item (3) remains unproven — named honestly, not silently dropped.** This pass did not construct a real
open-handle-blocks-`remove_all()` scenario (e.g. a second process or an unclosed handle holding a file
inside the scratch directory open) to confirm `revoke_path()` genuinely still succeeds where `remove_all()`
fails, the specific circumstance finding 31's whole fallback exists for. Finding 34's reasoning (that
`WRITE_DAC`/`READ_CONTROL` access is not routed through NTFS's sharing-violation check the way `DELETE`
is) is real, documented Win32 semantics, not a fabrication — but it is still argued, not executed, exactly
as this draft's own closing note already said before this pass. A future pass constructing that specific
contention (e.g. a helper process holding a handle open on one of the four paths while the host attempts
`remove_all()` then `revoke_path()`) is the one remaining item between this design and a fully proven, not
merely argued, safety story.

## Red-team round 1

Five real defects, found by reading the actual shipped code this draft cites (not by restating §8's
own disclosed open items). None of these are fatal to route (b) in principle, but two of them
(finding 1, finding 5) directly undercut claims the draft states as settled fact in §2c/§4, and must be
resolved — not merely re-disclosed — before an implementation ADR can build on this draft as written.

### Finding 1 (Critical — I3 near-miss). §4's "sys.argv" parameterization mechanism does not exist

§4 claims the fixed script is "parameterized only by the scratch-file path... passed as `sys.argv[1]`"
and frames this as architecturally identical to `ExecuteCodeTool::real_execute_code`'s use of
`ExecRequest`. Checked directly against the actual types: `ExecRequest`
(`include/agentengine/sandbox/sandbox.hpp:298-313`) has exactly three fields — `language`, `source`,
`preseeded_answers` — no `argv`, no parameter list, no way to inject a value into the worker's
namespace other than through `source` itself. A repo-wide search of `src/backends/native_jail/` for
`argv` returns zero hits. `python_worker_mediation.cpp`'s actual execution path
(`run_capturing`/dispatch, ~line 1239) calls `PyRun_String(source.c_str(), Py_file_input, main_dict,
main_dict)` directly against `request.source` — there is no separate channel that could populate
`sys.argv` per call.

This means "parameterized only by the scratch-file path" is not achievable the way §4 describes. The
ONLY way any per-call value — the scratch path, or `ExtractXlsxCells`'s `sheet_name` Args field — can
reach the executed Python is by being formatted directly into the `source` string that IS
`ExecRequest::source`. The scratch path itself is host-generated (`pid + "_" + counter`, no
attacker-controlled characters) so interpolating it is low-risk. `sheet_name`, however, is a
model-supplied `Args` field with no stated character restriction. An implementer following §4's own
description (a `docx.Document(sys.argv[1])`-shaped fixed script) has no way to actually build that
script for `ExtractXlsxCells` without either (a) inventing a real parameter-passing mechanism this
subsystem doesn't have, or (b) doing the obvious, wrong thing: string-formatting `sheet_name` into the
Python source (e.g. an f-string `wb["{sheet_name}"]`-shaped line). Option (b) is a direct Python
source-injection vector — a crafted `sheet_name` like `x"]; import os; os.system(...); y=wb["Sheet1`
breaks out of the string literal and runs as arbitrary Python inside the worker. This would falsify
§4's own headline claim ("these tools never execute model-supplied Python") in exactly the case the
draft was trying to rule out, not by a design decision but by the underspecified mechanism steering an
implementer toward the unsafe path. This is a real, concrete way I3 ("model output is data, never
authority") could be violated by a straightforward, unreviewed implementation of this draft as written.

**Fix direction**: name the actual mechanism explicitly, and make it injection-safe by construction.
Two real options exist in this codebase already: (a) have the host write a small, host-controlled
parameter file (JSON) to a *fixed*, non-interpolated scratch path (e.g. always
`<scratch_dir>/args.json`), and have the truly-fixed script (zero string interpolation, ever) read that
fixed path and `json.load()` it — `json.load` turns `sheet_name` into a genuine Python string value,
never source text, so no value it carries can break out of a string literal; or (b) confirm whether
`ExecRequest`/`MediatedPythonRunner::run()` could be extended with a real parameter-injection API (e.g.
setting entries directly in the worker's `main_dict` before `PyRun_String` runs, bypassing source-text
formatting entirely) and use that instead. Either way, §4 must stop describing a `sys.argv` mechanism
that isn't there.

### Finding 2 (High — architecture, breaks §5's own promise). `output_cap_bytes`/`output_discipline.hpp` is the wrong mechanism for what §4 claims it solves

§4's "Output" bullet claims route (b) reuses `MediatedPythonConfig::output_cap_bytes` and that this
"already solved, once" the same "worker-output-ceiling problem Track B spent two revisions solving from
scratch." Checked directly: `output_discipline.hpp`'s own file-top comment states its actual scope —
it is a layer *above* the raw host-safety pipe cap (`ResourceLimits::output_bytes`), applied to the
MODEL-VISIBLE fields of `ExecOutcome` (`stdout_text`, `stderr_text`, `result_repr`) specifically because
"even output that comfortably fits under the host-safety ceiling can still be far too large to hand an
LLM" (010 §3). `cap_output()` itself (`output_discipline.hpp:48-59`) is a blunt byte-offset truncation
(back off to a UTF-8 boundary, append a `"...[truncated: showing X of Y bytes]"` marker) at a *fixed*,
explicitly "provisional" `kDefaultOutputCapBytes = 64 KiB` — sized for keeping an interactive REPL
session's console chatter inside an LLM's context window, not for transporting a structured extraction
result out of a worker process.

This is a different problem from the one Track B actually solved. Track B's PDF worker built a
purpose-designed binary record framing (`extract_pdf_text.hpp`'s `parse_worker_output`: magic/kind/
index/payload_len) specifically so a truncated/killed worker's output still parses as a "real,
contiguous, verified-complete prefix" — the host can tell exactly how many pages survived
(`pages_processed` vs `total_page_count`) rather than getting a byte stream cut off mid-record. Nothing
in `cap_output()` provides that: it has no concept of records, and it applies to `result_repr`/
`stdout_text`, fields with no other channel back to the host in this API. If `ExtractDocxText` et al.
route their extracted text through `print()`/trailing-expression capture (the only channels §4 names),
the full text needed for §5's own BlobRef/bounded-preview discipline — which needs the COMPLETE
extracted bytes to decide whether to truncate and to populate `blob` with everything past the preview
— gets hard-truncated at 64 KiB by an unrelated, LLM-context-sizing cap *before* the host-side
`build_reply` logic (§5) ever sees it. That silently breaks the "the rest is retrievable via blob"
promise 006 §7 makes and Track A/B actually deliver on: for anything beyond ~64 KiB of extracted text,
route (b)'s blob would never contain the full document, unlike Track B's, which is bounded only by the
much larger 1 MiB raw host-safety cap (`kWorkerOutputBytes`) with page-level structure preserved.

**Fix direction**: don't route extraction output through `stdout_text`/`result_repr` at all. `ExecOutcome`
already has an `artifacts` field (`sandbox.hpp:335-341`) populated by a caller that harvests a
worker's *output mount* after `run()` returns (`worktree_mount_sync.hpp`'s `harvest_mount`,
already used elsewhere in this codebase) — the fixed script writes its extracted text/cells to a file
under a granted output mount, and the host reads that file directly, off disk, with no 64 KiB
LLM-context cap in the path at all. §4 should name this channel (or an equivalent one), not
`output_cap_bytes`.

### Finding 3 (Medium-High — I8/hostile-input, real gap not covered by §6's disclosed "not measured" caveat). Zip-bomb and XML-entity risk sits ahead of every cap this draft plans

DOCX/XLSX/PPTX are zip containers whose XML parts get read via `python-docx`/`openpyxl`/`python-pptx`
(lxml and/or Python's stdlib `zipfile`). All three constructor calls this draft's fixed scripts would
make — `docx.Document(...)`, `openpyxl.load_workbook(...)`, `pptx.Presentation(...)` — fully decompress
and parse the entire document into memory in ONE library call, before any row/paragraph/slide-level
iteration begins. §3's `rows_processed`/`paragraph_count`/`slides_processed` caps and §6's "row/
paragraph/slide processing caps... driven by whichever of wall-clock, memory, or output-byte ceiling
fires first" cannot bound this phase at all — a small, hostile file that decompresses to gigabytes (a
classic zip bomb) or that triggers XML entity expansion completes its damage inside that single,
uninterruptible parse call, before any per-item cap this draft designs ever gets a chance to fire. The
only backstop is the coarse, whole-process `memory_bytes`/`wall_ms` limit (§6 already flags these as
unmeasured for this workload) — but §6's framing ("real numbers not sized here") reads as "these caps,
once tuned, do the job," not "these particular caps are structurally powerless against this attack
class regardless of tuning, because the parse phase they're meant to bound doesn't exist as a
subdividable unit of work." That is a materially different, currently undisclosed claim this draft
should make explicitly.

Separately, spot-checked against current sources: this exact library family has real CVE history for
this exact issue (`openpyxl<=2.3.5` had an XXE vulnerability via its internal lxml usage), and lxml's
own "external entity expansion disabled by default" protection is a relatively recent default — it only
became lxml's *default* behavior starting at lxml 5.x; earlier 3.x/4.x releases required the caller to
pass `resolve_entities=False` explicitly to get the same protection. §2b's dependency table pins
`lxml>=3.1.0` as a floor without naming which version actually gets vendored/pinned by the
`AGAENGINE_VENDOR_PYTHON` mechanism (§2b item 1) or verifying the resolved version sits on the safe
side of that behavior change — a real, checkable fact this pass did not check, for a catalog row 009 §7
itself frames as "parsing hostile files."

**Fix direction**: (a) name the zip-bomb/decompression-bomb gap explicitly as a caps-can't-help residual
in §6, distinct from the "numbers not sized yet" item already disclosed there; consider whether
`zipfile.ZipFile.infolist()`'s `file_size`/`compress_size` fields can be checked (a total-uncompressed-
size guard) before handing the archive to `python-docx`/`openpyxl`/`python-pptx` at all, the same
"check before you fully trust" posture Track B's own worker takes toward PDF structure; (b) pin an
actual lxml version (>=5.0, not merely >=3.1.0) in the real implementation's vendoring step and record
that decision here or in the implementation ADR, rather than leaving the floor at a version that
predates the safe default.

### Finding 4 (High — I2 authority gap). A shared `MediatedPythonRunner` carries session-wide filesystem authority the per-call capability check never re-derives

§4's "Sourcing" bullet claims this is "identical to `ExtractPdfText`'s own `fetch_via_url`/
`fetch_via_path`" — implying the same narrow, per-call OS-level grant Track B's worker gets
(`grant_ro_path_once(scratch_dir())`, nothing else). Checked directly against
`MediatedPythonRunner::initialize()` (`mediated_python_runner.cpp:30-80`): it builds `spec.mounts` from
`config_.mount_roots` — the session's *entire* set of configured mount roots — and grants every one of
them `read_write = true` (`MountSpec{.source = ..., .guest_path = "/" + mount_id, .read_write = true}`)
at worker-initialize time, once per runner lifetime, completely independent of any individual `run()`
call's own capability grant. This is real, standing, broad authority baked into the worker process
itself, not narrowed per call the way Track B's PDFium worker is.

If route (b)'s `Extract*` tools take §4's cheaper, currently-favored-sounding lifecycle option — reuse
whatever `MediatedPythonRunner`/`CodeActRunnerBinding` a session already holds — the fixed extraction
script executes inside a worker process that already has read-write OS-level access to every mount the
session's CodeAct config grants (e.g. the `work` mount), regardless of what this specific
`ExtractDocxText` call's own `find_fs_read`/`find_net_out` check established for `path`/`url`. That is
standing authority reachable without it being re-derived from this call's own capability grant — the
shape I2 ("no ambient authority") exists to rule out. The draft's "own dedicated `MediatedPythonRunner`
per call" alternative doesn't obviously avoid this either: nothing in §4 says a dedicated runner would
be initialized with a `mount_roots` narrowed to just the fetched scratch file rather than mirroring the
session's normal CodeAct config (`shared_python_runner()`'s existing construction shape) — if an
implementer copies that existing pattern, the same over-broad grant reappears even for the "isolated"
option.

**Fix direction**: whichever lifecycle option §8's first open item eventually picks, state explicitly
that any `MediatedPythonRunner` backing an `Extract*` tool call must be initialized (or reconfigured)
with `mount_roots` narrowed to *only* the scratch directory the fetched bytes were written to — never
the session's full CodeAct mount set — so the worker's OS-level authority actually matches what this
call's own capability check granted, the same discipline Track B's per-call `grant_ro_path_once` already
enforces structurally.

### Finding 5 (High — factual error, undermines §2c's stated reasoning). The PPTX "no viable candidate" claim in §2a is wrong; the draft admits it didn't check the one candidate that would have shown this

§2a states PPTX "has no viable candidate at all — a real, disclosed dead end for this route," naming
`iharob/libpptx` (confirmed 404 — that part checks out) as the only hit, then dismisses "Aspose.Slides
FOSS for C++" as coming "from a commercial vendor whose free-tier scope and license terms need the
same 'awaiting the project owner's own review' caution... for reasons this pass did not chase further
given the more decisive finding below." §2a states its own methodology is to check "each candidate
project's own license file... the same discipline ADR-106 §2 and the OCR draft §2 used" — but by its
own admission, this is the one candidate that check was skipped for.

Checked directly (this round): `Aspose.Slides FOSS for C++` is a real, separate, MIT-licensed
repository (`github.com/aspose-slides-foss/Aspose.Slides-FOSS-for-Cpp`, LICENSE file present, MIT
badge), NOT a "free tier" of the commercial product with ambiguous terms — it is a standalone C++20
library with "no dependency on Microsoft Office, COM automation, or any proprietary runtime," built on
the same permissive stack (pugixml + miniz) xlnt/OpenXLSX already use elsewhere in §2a, with CMake
`FetchContent` integration, cross-platform CI (Ubuntu/macOS/Windows, GCC/Clang/MSVC, including a
`windows-2025` image suggesting current maintenance), 47 commits, and documented read/write `.pptx`
support with conformance tests. It is real and it is MIT-licensed — the exact bar §2a already applied
to xlnt/OpenXLSX/DuckX. (Its low visibility — 2 stars — is a legitimate trust-posture caution in the
same spirit as the draft's own DuckX caution, but that is a different concern from "no viable candidate
exists," which is what §2a and §2c's recommendation actually assert.)

This matters because §2c's recommendation for route (b) is reached specifically "the same way ADR-106
reached PDFium, on real, checked evidence" and names route (a)'s PPTX dead end as the decisive,
first-listed reason: "Route (a) has a hard, disclosed dead end on PPTX (§2a) — no viable library
exists... A route that cannot cover all three formats named in the catalog row it's answering is a
materially worse starting position than one that can." With a real, MIT-licensed, standalone PPTX
candidate on the table, that premise does not hold as stated — route (a) has a real (if lower-profile)
story for all three formats, the same "PDFium-tier XLSX, usable-but-smaller-scale DOCX" shape §2a
already accepted for the other two, not "two formats now, write one from scratch." This does not
automatically mean route (a) is the right recommendation — route (b)'s operational reuse of an
already-shipped jailed-worker mechanism (§2b) is a real, independent argument on its own merits, and
findings 1/2/4 above show that mechanism has real work of its own left to do — but §2c's specific,
stated chain of reasoning ("no PPTX story at all" ⇒ route (a) disqualified ⇒ route (b) by elimination)
is built on a premise this round found to be false.

**Fix direction**: a real revision pass (not this red-team pass) needs to actually check
`Aspose.Slides-FOSS-for-Cpp`'s license/maturity/API surface with the same rigor §2a gave xlnt/OpenXLSX,
and re-run §2c's recommendation on the corrected premise — which may still land on route (b), but for
its own real advantages, not on a disqualified-by-elimination argument against a route that in fact has
a PPTX story.

### Sections with no real findings this round

§1 (reuse inventory), §3 (v1 tool scope/shape), §5 (BlobRef discipline as a stated intent, independent
of finding 2's mechanism gap), and §7 (companion skill) were checked against the same cited code and
found to hold up — no defects beyond what's already disclosed in §8.

## Red-team round 2

Three real defects, found by reading the actual shipped code Revision 2 cites for its own fixes (not by
restating round 1 or §8's disclosed opens). Finding 6 is the headline: it shows finding 4's own fix, as
specified, produces a script that cannot open either of the two files findings 1/2's fixes depend on —
and the natural "fix" an implementer reaches for to make it work again is exactly the standing,
over-broad grant finding 4 was resolved to eliminate. Findings 7/8 are real but lower-stakes: one
factual correction to a trust-posture comparison Revision 2 itself re-verified but applied
inconsistently, one disclosed-but-incompletely-scoped cost gap.

### Finding 6 (Critical — reopens I2 the moment an implementer makes the script actually work). §4b's `mount_roots` narrowing (finding 4's fix) is necessary but not sufficient — the fixed script's own `open()` calls need a SEPARATE, undocumented capability grant, and the obvious way to add it silently reintroduces finding 4's own gap

§4b states the fix for finding 4 in terms of `MediatedPythonConfig::mount_roots` alone: a dedicated
runner "with `mount_roots` containing EXACTLY ONE entry." Checked directly against the code that
actually services the fixed script's `open("/extract_scratch/args.json")` / `open("/extract_scratch/
out.json", "w")` calls — `native_jail_backend.cpp`'s `dispatch_worker_query` (~line 865-874) routes a
`kQueryOpen` to `NativeJailBackend::dispatch_open` (`native_jail_handle_relay.cpp:145-236`) — and that
function does TWO independent checks, not one:

1. `ws.session_config.mount_roots.find(mount_id)` — the OS-level grant §4b's fix narrows. Satisfied by
   the fix as described.
2. `ctx.capabilities->find_fs_read(mount_id, mount_relative)` (read) / `find_fs_write(mount_id,
   mount_relative)` (write) — `dispatch_open:163-177` — a completely separate, capability-layer check
   against `ctx`, the SAME `EffectContext` `MediatedPythonRunner::run(request, state, ctx)`
   (`mediated_python_runner.cpp:82-93`) threads straight through to `dispatch_worker_query`
   unmodified. This is not the OS ACL grant `MediatedPythonRunner::initialize()` sets up — it is the
   ordinary `agentengine::cap::FsRead`/`FsWrite` capability (`include/agentengine/trust/capability.hpp:
   114-124`) that `tool_pipeline.hpp`'s dispatch binds from whatever `Capabilities<...>` the CALLING
   `Tool<>` itself declares. The one real precedent riding this exact plumbing today,
   `ExecuteCodeTool` (`tools/cli_chat.cpp:375-377`), declares `Capabilities<cap::decl::FsRead<"work">,
   cap::decl::FsWrite<"work">>` for precisely this reason — its own fixed mount id, `"work"`, has to
   appear in the tool's OWN capability ceiling or every `open()` a guest script makes fails closed.

§4b never states the analogous requirement for `ExtractPptxText`: that the tool itself must declare
`Capabilities<cap::decl::FsRead<"extract_scratch">, cap::decl::FsWrite<"extract_scratch">>` (or
equivalent), and that whatever grants the session/tool-pipeline binds into `ctx.capabilities` before
`invoke()` runs must include a live `FsRead{mount_id="extract_scratch"}`/`FsWrite{mount_id=
"extract_scratch"}` capability. Without it, `dispatch_open` returns `deny("tool.capability_not_held",
...)` on the very first line of the fixed script — `json.load()` never runs, `args.json` is never read,
`out.json` is never written. This is not a hypothetical: it is the literal, first-instruction behavior
of the script exactly as §4b specifies it, checked against the exact function that would execute it.

The severity is not merely "the design is incomplete" — it is that the natural, minimum-diff fix an
implementer reaches for when their script fails with `capability_not_held` is to give `ExtractPptxText`
the SAME capability `ExecuteCodeTool` already has and already works with: `Capabilities<cap::decl::
FsRead<"work">, cap::decl::FsWrite<"work">>`, reusing the session's existing, broadly-scoped `"work"`
mount rather than inventing a new narrowly-scoped `"extract_scratch"` tag nobody told them to add. That
"fix" compiles, runs, and silently reintroduces exactly the standing, session-wide authority finding 4
was resolved to eliminate — the fixed script would then execute inside a worker whose OS-level mount
(narrowed correctly, per §4b) still only exposes the scratch directory, but whose CAPABILITY-layer grant
(if copied from `ExecuteCodeTool`'s own precedent) reaches the session's general `"work"` mount, an
I2 regression the draft's own re-verification of `dispatch_open` would have caught had it traced `ctx`
one layer further than `mount_roots`.

**Fix direction**: §4b must name the exact capability declaration explicitly — `Capabilities<cap::decl::
FsRead<"extract_scratch">, cap::decl::FsWrite<"extract_scratch">>` on `ExtractPptxText` itself, with
`"extract_scratch"` as the SAME fixed string used for `MediatedPythonConfig::mount_roots`'s key — and
state that the session/tool-pipeline grant behind it must be scoped to that literal mount id, never
`"work"` or any other pre-existing session-wide mount. This is the same class of fix finding 1 needed
(name the real mechanism, not a plausible-sounding one) applied one layer deeper: to the capability that
gates the fixed mechanism finding 1/2 already designed, not to the mechanism itself.

### Finding 7 (Medium-High — factual inconsistency, undermines §2c's own re-verified trust-posture reasoning). DuckX's real repository state is materially more dormant than Revision 2 credits it, and Aspose.Slides FOSS's is materially more active — checked directly, not accepted from Revision 1's characterization

§2a/§2c's case for keeping DuckX over the freshly-found `Aspose.Slides FOSS for C++` rests on DuckX
having "actual dated releases and ~500 stars' worth of real-world exposure" against Aspose.Slides FOSS's
"2 stars, 1 fork, 47 commits total, and no tagged release at all." Checked directly against both repos'
real, current state (GitHub API, not the READMEs Revision 1/2 read):

| | DuckX | Aspose.Slides FOSS for C++ |
|---|---|---|
| Created | 2019-01-31 | 2026-03-19 |
| Latest push | **2024-06-12** (over two years before this pass) | **2026-08-28** (four days before this pass) |
| Latest tagged release | v1.2.2, **2019-11-05** (~7 years old) | none |
| Open issues | 34 | 2 |
| Commit cadence (recent) | none in >2 years | multiple commits per day through the week of this pass |

Revision 2's own methodology credits Aspose.Slides FOSS's CI matrix ("a `windows-2025` image suggesting
current maintenance") as a real currency signal, but never applies the same currency lens to DuckX — the
comparison it actually draws ("dated releases and real-world exposure" vs. "neither") is a STAR-COUNT
and RELEASE-TAG-PRESENCE comparison only, not a maintenance-currency one. On the currency axis
specifically, the two candidates' real postures are closer to inverted than Revision 2's prose suggests:
DuckX's one tagged release is seven years old and its repository has not been pushed to in over two
years, while Aspose.Slides FOSS — despite having no tagged release yet — is being actively committed to
in the same week as this red-team pass. For a catalog row 009 §7 itself frames as "parsing hostile
files," a library with no commits in two-plus years is not obviously a SAFER choice than one under
active, current development, even though it has more stars and an actual (very old) release tag — a
lapsed CVE would sit unpatched in DuckX for years with no maintainer signal it ever will be, a risk
profile the "dated release" framing does not surface. This does not automatically flip the
recommendation (DOCX's `Tool<>` still needs SOME native library, and Aspose.Slides FOSS is a PPTX-only
project — not a DOCX candidate at all, so this finding does not argue for swapping DOCX's own library)
— but it means §2a/§2c's stated reason for treating DuckX's trust posture as strictly better than
Aspose.Slides FOSS's ("dated releases... real-world exposure" vs. "neither") is not fully supported by
the real, current state of either repository, and the draft should say so rather than let the star-count
framing stand as the whole comparison.

**Fix direction**: name the actual push/release recency for DuckX (2024-06-12 push, 2019-11-05 latest
tag) alongside its star count in §2a, the same "checked directly, not accepted at face value" discipline
already applied to Aspose.Slides FOSS this revision — and treat "actively maintained but pre-release" vs.
"tagged but dormant" as two different, both-real cautions rather than treating one as strictly worse
than the other.

### Finding 8 (Medium — I8, real gap not covered by §6/§8's disclosed "cost not sized" caveat). No stated bound on how many times `ExtractPptxText` can be invoked per turn/session, despite finding 4's fix making each call a full interpreter-process spawn

§6/§8 honestly disclose that the per-call CPython-plus-package-import startup cost of finding 4's
"dedicated, never shared" fix is real and unsized — but every mention of this cost is about ONE call's
weight, never about the absence of any cap on HOW MANY such calls a single turn or session can trigger.
`MediatedPythonConfig`'s `exec_wall_ms`/`memory_bytes`/`pids` limits (§6) bound one dedicated worker's
own resource use; nothing bounds the count of dedicated workers spawned in sequence by repeated
`ExtractPptxText` calls (an agent processing many `.pptx` files, or retrying the same one). Checked
against `output_discipline.hpp`'s own file-top comment (re-confirmed this pass): 023's turn-level
`TokenBudget` plumbing "does not exist anywhere in this codebase yet — no `TokenBudget` value reaches
`EffectContext`/`ExecState`/`ResourceLimits` at any call site," so there is no existing, generic,
project-wide mechanism that would incidentally catch this either. This is a different claim from what
§6/§8 already disclose (an unsized per-call cost): it is a per-CALL-COUNT bound that is not merely
unsized but entirely unnamed, for a tool whose finding-4-driven design specifically chose to pay a full
process-spawn cost on every call rather than amortize it — a real, new-vs-Track-B cost-multiplication
shape (Track B's native workers are cheap enough per-call that this question was never load-bearing;
finding 4's fix makes it load-bearing for this one tool).

**Fix direction**: name this explicitly as a residual in §6 or §8 — either a per-turn/per-session
call-count ceiling on `ExtractPptxText` specifically, or an explicit acknowledgment that none exists yet
project-wide and this tool inherits that gap like every other tool, which finding 4's own cost tradeoff
makes more consequential here than for cheaper-per-call tools.

### Investigated, no real defect found

- **The args.json write side** (host-side JSON serialization): the codebase's real `json::Value`/
  `json::dump()` machinery (already used throughout `native_jail_backend.cpp`, e.g. `dispatch_listdir`'s
  `json::dump(json::Value::make_array(...))`) is a real, existing, correctly-escaping serializer — the
  draft's "the host writes... via `json.dump()`" claim is grounded in a real mechanism, not asserted.
- **Worker-lifecycle ordering for `out.json`** (investigation 2's teardown-ordering question): file
  writes the fixed script makes are host-mediated (`dispatch_file_write` performs the real `WriteFile`
  call in the HOST process, not the guest), so the file is already on real disk by the time `run()`
  returns — well before `MediatedPythonRunner`'s destructor (`~MediatedPythonRunner()`,
  `mediated_python_runner.cpp:23-28`) ever runs. No race between "host reads out.json" and "worker
  mount gets torn down" was found.
- **`sheet_name`-into-openpyxl injection/DoS** (investigation 1's `workbook[sheet_name]` question): moot
  under Revision 2's own hybrid recommendation — `ExtractXlsxCells` (the only tool with a `sheet_name`
  Args field) moved to route (a), native C++, which has no Python/openpyxl involvement at all. Worth
  noting for its own sake: this also means finding 1's args.json/`json.load()` mechanism currently has
  no live per-call value to protect in `ExtractPptxText`'s own Args (`url?`, `path?` only) — the fix is
  correctly designed but, as v1 is scoped, is never actually exercised against a real caller-controlled
  value passing through it.
- **§4a's native-worker specification depth** (investigation 4): already honestly left open at §8
  ("whether each format gets its OWN worker binary... or a single shared native worker binary... not
  decided") — re-checked and found genuinely open, not newly hand-wavy beyond what §8 already discloses.
- **Zip-bomb pre-check's scope across all three tools** (investigation 6): §4a explicitly extends the
  guard to the native route ("needs a native-C++ equivalent... named here as a real, still-open
  implementation-ADR task"), so the mechanism is named for all three tools, not only wherever it was
  first added — the missing piece (an actual byte threshold) is consistently disclosed as open in both
  §4a and §4b, not newly inconsistent.

## Red-team round 3

Four real defects, found by reading the actual shipped code Revision 3 cites for its own fixes (and, for
finding 10, by checking `python-docx`'s own documented API — the same external-fact-checking discipline
round 2 applied to GitHub repository state). Finding 9 is the headline: it shows finding 6's own fix, as
specified, still leaves the ONE piece of the mechanism that actually prevents cross-call data collision
completely unnamed — and the only real precedent in this codebase for "a per-call scratch file" does it
the OPPOSITE way §4b needs, which a literal-precedent-following implementer would not notice. Findings
10-12 are real but narrower: one factual gap in what the DOCX route actually recovers versus what PPTX's
route was designed to recover, one disclosed-but-unweighed cost question, and one gap in finding 8's own
proposed mitigation that a careful reading of the exact function it cites reveals.

### Finding 9 (Critical — I2/data-integrity: the fix that makes `open()` succeed says nothing about which call's file it opens). §4b never names how "this call's own scratch directory" is actually generated, and the one real precedent in this codebase for a per-call scratch file does it the opposite way this design needs

§4b's finding-4 fix states the dedicated runner's `mount_roots` contains "EXACTLY ONE entry — `{"extract_scratch": <this call's own scratch directory, and nothing else>}`" and finding 6's fix adds a session-level `cap::FsRead{.mount_id = "extract_scratch", .path_prefix = "args.json", ...}` / `cap::FsWrite{.mount_id = "extract_scratch", .path_prefix = "out.json", ...}` grant, arguing this is safe because "the OS-level binding behind the label is re-derived FRESH... on every single call." Checked directly against the code that actually builds that binding — `MediatedPythonRunner::initialize()` (`mediated_python_runner.cpp:56-66`, re-verified this pass) does exactly this:

```cpp
session.mount_roots = config_.mount_roots;
...
for (auto const& [mount_id, host_path] : config_.mount_roots) {
    spec.mounts.push_back(MountSpec{.source = narrow(host_path), .guest_path = "/" + mount_id, .read_write = true});
}
```

`host_path` is whatever the TOOL's own construction code puts into `config_.mount_roots["extract_scratch"]` — `MediatedPythonRunner`/`NativeJailBackend::create_python_worker()` (`native_jail_backend.cpp:587-599`) perform no derivation, uniqueness-check, or randomization of it at all; they grant exactly the host path handed to them. "Fresh per call" is therefore not a property this mechanism provides — it is a property the CALLER must provide, by choosing a distinct host directory each call. §4b never says how.

This matters because the ONE real, shipped precedent in this codebase for "a scratch file written before a per-call worker runs" — the exact mechanism §1 and §4b's own "Sourcing (unchanged)" bullet cite as what this design reuses — does the OPPOSITE of what §4b's fixed-filename script needs. `extract_pdf_text.hpp:251-271` (re-verified this pass):

```cpp
[[nodiscard]] inline std::filesystem::path scratch_dir() {
#if defined(_WIN32)
    return std::filesystem::temp_directory_path() / "agentengine_pdf_scratch";
#else
    return std::filesystem::path("/tmp/agentengine_pdf_scratch");
#endif
}
// pid + a process-local monotonic counter -- collision-safety across CONCURRENT calls within one
// process (the design draft's own "not decided" item); not a security boundary (the scratch
// directory's own access grant, not the filename, is what gates who can read it).
[[nodiscard]] inline std::string unique_scratch_filename() { ... "pdf_" + pid + "_" + counter + ".pdf" ... }
```

Track B's real collision-safety mechanism is a SHARED, fixed directory (`agentengine_pdf_scratch`, identical every call) with a per-call UNIQUE FILENAME (`pid_counter.pdf`) — the filename, not the directory, is what's re-derived fresh per call, by a real, named function whose own comment says exactly why ("collision-safety across CONCURRENT calls within one process"). §4b's design is the mirror image: a FIXED filename (`args.json`/`out.json`, byte-identical every call, by finding-1's own design) that depends on the DIRECTORY being unique per call instead — and names no `unique_scratch_filename()`-equivalent function, no call-ID, no random component, nothing that actually produces that uniqueness. "This call's own scratch directory" is asserted in prose three times across §4b (finding-4 fix, finding-6 fix, and the "Sourcing" bullet) and never once grounded in a concrete generation mechanism, unlike literally every other host-chosen value in this draft (the `/extract_scratch` mount label itself is explicitly "not derived from anything caller-supplied," `args.json`/`out.json` are explicitly fixed literals) — this is the one value the whole fix's safety argument depends on being NOT fixed, and it is the one value the draft never shows how to make so.

The consequence, checked against a real, already-shipped feature of this codebase, not a hypothetical: `07ae98e` ("ADR-160: real 006 §8 G4 parallel-batch tool-call scheduler") means concurrent tool calls within one turn are a live, shipped capability, not a future concern — an agent extracting several attached `.docx`/`.pptx` files in one batch is exactly the scenario ADR-160 was built for. If an implementer follows §4b "as designed" by literally reusing Track B's own `scratch_dir()`-style helper (the only real precedent named anywhere in this draft, §1's own "reused directly" language) for the DOCX/PPTX scratch root, two concurrent `ExtractDocxText` calls (or a retry racing a slow prior worker's `~MediatedPythonRunner()` teardown, `mediated_python_runner.cpp:23-28`) would both mount the SAME shared host directory as `extract_scratch`, and both fixed scripts read/write the SAME `args.json`/`out.json` filenames inside it — one call's `sheet_name`-analog args (were DOCX/PPTX ever to grow a per-call Args field, per §4b's own "the mechanism generalizes" note) or extracted output could be read by, or overwritten by, the wrong call. This is not a hypothetical implementer error invented by this round — it is the literal, most-obvious way to satisfy "reuse `ExtractPdfText`'s own scratch pattern" (§1) while also satisfying "always exactly `/extract_scratch/args.json`" (§4b's finding-1 fix), and the draft supplies no text that would stop an implementer from doing both at once.

**Fix direction**: §4b must name the actual per-call scratch-DIRECTORY generation mechanism explicitly — e.g., a `unique_scratch_dir()` twin of Track B's `unique_scratch_filename()` (`pid` + a monotonic counter, or the call's own idempotency key from `tool_pipeline.hpp`'s `derive_idempotency_key`, which is already computed per call and already available at the tool's call site) — and state, as directly as finding 1 states its own fix, that this is why the FIXED-filename design (unlike Track B's fixed-directory design) is safe: because the directory, not the filename, is what's unique here. Until this is named, "the OS-level binding is re-derived FRESH... on every single call" is an assertion about what the design NEEDS to be true, not a description of a mechanism that makes it true.

### Finding 10 (Medium-High — correctness, undisclosed scope gap). `python-docx`'s real API recovers materially less of a DOCX's actual text than the design's Reply shape and PPTX's own treatment imply

§2c/§3 present the DOCX move to `python-docx` as PPTX's job shape "extended to a second library import, not redesigned from scratch," and §3's `ExtractDocxText` Reply comment says "paragraph text, in document order; each table rendered as a delimited block." Checked directly against `python-docx`'s own documented API (`python-docx.readthedocs.io`, current as of this pass, the same external-fact-checking discipline round 2 applied to GitHub repository state rather than trusting a library's marketing surface):

- **`Document.paragraphs`** is documented as returning body-level paragraphs only, and explicitly EXCLUDES paragraphs inside revision marks (`<w:ins>`/`<w:del>` — i.e. tracked-changes insertions/deletions) from the list. Headers, footers, footnotes, and endnotes are, per the library's own user-guide framing ("styles and page headers and footers are contained separately from the main content"), not reachable through `Document.paragraphs` at all — they require entirely separate API surfaces (`document.sections[i].header`/`.footer`; footnotes/endnotes have no first-class `python-docx` accessor at all as of this pass).
- **`Document.tables`** is documented as listing only tables "appearing at the top level of the document" — "a table nested inside a table cell does not appear" in that list, silently.

None of this is named anywhere in §3, §4b, or §8. This is a real, format-specific wrinkle investigation 3 asked this round to check for, and it is a different KIND of gap than anything round 1/2 found: it is not an I2/I3 authority problem, it is a correctness/completeness gap in what the tool actually extracts versus what an implementer or caller would reasonably expect from "paragraph text, in document order" for a "text layer" catalog row. Two concrete consequences: (a) a DOCX whose substantive content lives partly or wholly in a header/footer (letterheads, running titles) or in footnotes/endnotes (common in academic/legal documents — exactly the kind of document this catalog row exists to serve) would have that content silently absent from `preview`/`blob`, with no `truncated`-style signal that anything was skipped, because from `python-docx`'s point of view nothing WAS truncated — the API simply never surfaces it; (b) a table nested inside another table's cell is silently dropped from `tables_processed`, not counted toward `truncated_tables` either, for the same reason. This is exactly the class of gap PPTX's OWN design in this same §3 was careful about — speaker notes are named as needing "a real, separate `python-pptx` API call per slide" precisely so they aren't silently missed — but the identical care was never applied to DOCX's headers/footers/footnotes/nested-tables, despite §2c's own framing that the two tools are "the identical §4b job shape... extended to a second library import."

**Fix direction**: name the gap explicitly in §3 (the same "deliberately out of scope, not silently dropped" discipline already used for embedded images/formulas) — decide whether headers/footers/footnotes/nested tables are v1-in-scope (each needs its own, separately-designed `python-docx` API call, the same treatment speaker notes already got) or v1-out-of-scope (in which case the Reply's own field/description should say so, the same way `ExtractXlsxCells`'s `sheet_name` omission is disclosed rather than left to be discovered).

### Finding 11 (Medium — I8/complexity, undisclosed cost tradeoff). The three-way isolation split's aggregate engineering cost — two independent from-scratch vendoring/isolation efforts for one catalog row — is never weighed against an all-route-(b) alternative that round 2's own finding 7 made newly available

§2c defends the now-two-Python/one-native hybrid on PER-FORMAT evidence (trust posture, maturity, license) and on internal coherence ("three independent `Tool<>`s... evaluating each format's real library landscape on its own merits... is a direct continuation of that discipline"). Nowhere in §2c, §6, or §8 is the AGGREGATE cost of the split itself named or weighed: shipping this catalog row now requires BOTH (a) a from-source CMake `FetchContent` vendoring effort for `xlnt`/`OpenXLSX` plus a brand-new native worker binary (§4a, §8's own "not designed in the same code-level detail" admission) plus a native-C++ zip-bomb guard written against miniz's API from scratch, AND (b) the full Python-package-vendoring-plus-capability-plumbing effort (§2b items 1-3, §4b) that DOCX and PPTX already require regardless. Once round 2 finding 7 moved DOCX onto route (b), the Python-mediated plumbing stopped being optional infrastructure for this catalog row — it is being built either way. §2b's own table already lists `openpyxl` as verified "genuinely pure Python, transitively, no native extension" and MIT-licensed — a real route-(b) candidate for XLSX too — but §2c's recommendation keeps `ExtractXlsxCells` on route (a) purely on the strength of xlnt/OpenXLSX's own maturity, without ever asking whether that maturity is worth PAYING FOR a second, fully separate vendoring/isolation mechanism (native `NativeJailBackend::create()`/`exec()` one-shot worker, its own resource-cap benchmarking, its own zip-bomb mechanism, its own worker-binary build) when the alternative — moving `ExtractXlsxCells` to `openpyxl` too — would let this entire catalog row ride on the ONE isolation mechanism (b) that DOCX/PPTX already force into existence, at the cost of `openpyxl`'s own real, already-disclosed caution (finding 3's cited historical XXE CVE, mitigated the same way DOCX/PPTX's XXE risk already is, by the `lxml==6.1.2` pin `openpyxl` would also need). This is a real, live tradeoff this revision had in hand (both facts — DOCX's move and `openpyxl`'s clean bill — are already IN the document) and never actually weighed against each other; §2c reasons per-format libraries but never re-asks the route-(a)-vs-(b) question at the CATALOG-ROW level now that the premise (route (b) is unavoidable anyway) changed.

**Fix direction**: add an explicit paragraph to §2c or §6 naming this tradeoff — either a reasoned "two isolation mechanisms is worth it because [X]" (e.g., xlnt/OpenXLSX's read-time streaming behavior genuinely outperforms `openpyxl`'s for large spreadsheets, or the native route avoids a second Windows-only-subsystem dependency for at least one tool) or a revised recommendation. Either is defensible; leaving the question unasked while both facts needed to ask it already sit in the same document is the actual gap.

### Finding 12 (Medium-High — I8, the proposed mitigation doesn't cover what it's offered as covering). Finding 8's `PolicyDecider` call-count-cap seam is unreachable for `text_derived`-provenance calls, and supports only session-cumulative counting, not per-turn counting, neither of which §6 discloses

§6's finding-8 fix recommends a host-wired `PolicyDecider` "closing over a mutable per-turn/per-session counter keyed by tool name," calling this "expressible TODAY with zero new pipeline machinery." Checked directly against `resolve_approval_outcome` (`tool_pipeline.hpp:518-534`, re-verified this pass) and `PolicyDecider`'s own declared signature (`tool_pipeline.hpp:377-378`):

```cpp
using PolicyDecider = std::function<policy_decision(Principal const& caller, ToolDescriptor const& tool,
                                                     bool arguments_tainted)>;
...
if (tool.approval == approval_mode::policy_driven &&
    provenance != call_provenance::text_derived && policy) {
    switch (policy(caller, tool, arguments_tainted)) { ... }
}
```

Two real gaps, both checkable directly against this exact function rather than assumed:

1. **`policy` is never consulted at all when `provenance == call_provenance::text_derived`** — the condition on the `if` requires provenance to NOT be `text_derived`. `007 §4`'s own closed-declassifier-list comment two lines above (already quoted in this draft's own finding-6 fix elsewhere) confirms this is deliberate: a `text_derived` call (one reconstructed from raw model text rather than a structured tool-call) is a weaker trust class that gets its OWN, unconditional gate, never `policy`. This means a host's counting `PolicyDecider` — finding 8's entire proposed fix — has NO visibility into, and NO ability to cap, any `ExtractDocxText`/`ExtractPptxText` call that arrives with `text_derived` provenance; those calls fall straight through to the ordinary `ApprovalDecider` path with no counting involved at all. Whether this pipeline permits `text_derived` reconstruction to reach these two tools in the first place is a separate, upstream question this draft does not investigate — but IF it does, finding 8's own proposed mitigation silently does not apply to exactly the trust class (model-influenced, not vendor-structured) I3 is most concerned with.
2. **The signature carries no turn or call-index signal** — only `(Principal const& caller, ToolDescriptor const& tool, bool arguments_tainted)`. A closure keyed on `caller`/`tool` alone can implement a session-cumulative cap (assuming `Principal` is stable for the session) but cannot itself distinguish "the 4th call this turn" from "the 4th call this session" — a genuine per-TURN reset (the other half of §6's own "per-turn/session" phrasing) would need an out-of-band turn-boundary signal this seam does not provide, not merely a threshold choice left to the host as §6 already discloses for the number itself.

**Fix direction**: narrow §6's claim to what `PolicyDecider` actually supports — a session-cumulative, `vendor_structured`-provenance-only call-count cap — and name both carve-outs explicitly (the `text_derived` gap and the missing per-turn signal) the same way §6 already names the threshold itself as unsized, rather than presenting the seam as covering the general "per-turn/session" case its own prose describes.

### Investigated, no real defect found

- **Finding 6's declare-vs-grant distinction (investigation 2)**: `tool_pipeline.hpp`'s `admit_call` (steps 4/7, `:602-612`) binds a tool's declared `Capabilities<...>` ceiling against the SESSION's held `CapabilitySet` via `CapabilitySet::bind()`/`contains()` (`capability.hpp:765-768`, `:983-989`) — a compile-time ceiling and a separate, runtime, host-authored grant are genuinely two different things in this codebase, exactly as `examples/06_capabilities_and_denial.cpp`'s own file-top comment states ("declaring a ceiling is not a grant"). §4b's finding-6 fix states both steps explicitly (item 1: tool declares; item 2: session must ALSO be granted) and does not conflate them. One real nuance worth naming for a future revision rather than as its own finding: `dispatch_open` (native_jail_handle_relay.cpp) checks `ctx.capabilities` (the session's FULL `CapabilitySet`) directly, never the tool's declared ceiling or the narrower `ctx.bound_capabilities` admit_call itself computes — so the declared ceiling is not what makes the worker's `open()` calls succeed (the session-level grant alone does that); it only gates the OUTER admission via `admit_call`. This doesn't change the outcome (both checks key off the identical grant, so in practice they succeed or fail together) but the draft's "necessary... but not sufficient by itself" phrasing slightly overstates the declared ceiling's causal role in fixing the worker-side failure finding 6 originally found. Not severe enough to number as a finding — an implementer who does exactly what §4b says ends up with a safe, working system either way.
- **`ExtractPdfText`/`ReadContent`'s own precedent (investigation 2, continued)**: both declare `Capabilities<>` (empty — `extract_pdf_text.hpp:386`, `read_content.hpp:208`) and perform their OWN manual `ctx.capabilities->find_fs_read(...)`/`find_net_out(...)` checks rather than an admit_call-bound ceiling — a genuinely different (and, per the point above, equally valid) pattern from what finding 6 prescribes for `ExtractDocxText`/`ExtractPptxText`'s scratch-file access. The draft does not need to reconcile these — they gate different things (sourcing the ORIGINAL document bytes vs. the worker's OWN internal scratch-file access) — but is worth a future revision naming explicitly, since §1 cites the former as reused precedent while §4b introduces the latter as new, for the same two tools, without commentary on why they differ.
- **Zip-bomb/XXE mechanism transfer to DOCX (part of investigation 3)**: `python-docx`'s dependency chain is identical to `python-pptx`'s for the load-bearing parts (`lxml`, same pin, same zip-container shape) per §2b's own table — the `zipfile.infolist()` pre-check and the `lxml==6.1.2` pin apply to `ExtractDocxText` exactly as designed for `ExtractPptxText`, with no format-specific wrinkle found on this axis. The wrinkle is in CONTENT COVERAGE (finding 10), not in the security mechanisms.
- **PolicyDecider/`Capabilities<...>` interaction (part of investigation 5)**: `admit_call`'s step ordering (capability bind at steps 4/7, approval/policy at step 5) means a policy `auto_deny` correctly revokes any already-bound capability ticket (`tool_pipeline.hpp:633`, `for (auto const& b : bound) b.revoke();`) before returning — no leaked capability, no ordering hazard between the two mechanisms. The real gap is finding 12's, not an interaction defect.

## Red-team round 4

Five real defects, found by reading the actual shipped code Revision 4 cites for its own fixes — including
the just-pushed `native_jail_backend.{hpp,cpp}` mutex fix (commit `40c573e`), read directly rather than
trusted from its own commit message or from this draft's own summary of it. Finding 14 is the headline:
it shows finding 9's own fix — the mechanism this revision adds specifically to make `open()` safe —
introduces a NEW, real, unbounded-resource defect one layer down, in exactly the kind of place this
draft's own round 3 finding 9 was itself found (a claim about "the OS-level binding" that turns out not
to hold the way the surrounding prose asserts). Finding 13 answers the prompt's own headline question
directly: the collapse's stated "Net evaluation" is now built in part on a fact that changed out from
under it since Revision 4 was written, and the draft has not been updated to reflect that. Findings
15-17 are real but narrower. Two investigation lines (the totality of the Windows-only gap, and the
`sheet_name` args.json plumbing plus the zip-bomb pre-check's uniformity) were checked directly and found
to hold up as stated — recorded at the end, not padded into findings.

### Finding 13 (High — the draft's own headline architectural conclusion rests in part on a fact that changed since this revision was written, and the draft has not been reconciled with it). §2c/§4a's "Net evaluation" treats the `NativeJailBackend::instances_` race as one of three coequal, still-live costs of route (a); it no longer is one

The prompt for this round asks the question directly, and it has a real, checkable answer: **no, the
collapse's own written justification does not currently hold up independent of the now-fixed native bug
— not because the underlying trade-off is necessarily wrong, but because the draft's own "Net evaluation"
paragraph (§2c) explicitly enumerates the concurrency-safety defect as one of "three real, substantial,
previously-unweighed costs," and calls discovering it "the headline finding of this pass's own
investigation" (§2c, §4a's own words). Checked directly against `git show 40c573e` (not assumed from its
own commit message): the fix is real, lands exactly where the draft says the defect lived
(`NativeJailBackend::instances_`, `native_jail_backend.cpp`/`.hpp`), and is scoped correctly — a
`std::mutex instances_mutex_` member guarding `find_instance_locked`/`insert_instance_locked`/
`erase_instance_locked`, used by every one of `create()`/`exec()`/`destroy()`/`create_python_worker()`/
`exec_session()`/`refresh_python_tools()` (verified: the diff replaces every raw `instances_.find/emplace/
erase` call site named in this draft's own §2c citation with the locked helpers). The fix's own commit
message states it was verified with the same repro class this draft's own investigation used to FIND the
bug (8 threads × 40 create/exec/destroy cycles, 5/5 segfaults before, 3/3 clean passes after, 320/320
cycles ok, zero regressions in the full native_jail/extract_pdf/pdf_text suite) — a real fix, not a
partial mitigation or a narrowing of the bug's scope.

This directly undercuts item 3 of §2c's "Net evaluation" list (a "newly-discovered concurrency-safety fix
Track B itself has never built, that a native XLSX worker copying 'Track B's own, already-proven shape'
... would silently inherit as broken") and the parallel paragraph in §4a ("route (a) is not free reuse the
way it reads"). A native `ExtractXlsxCells` worker built exactly Track B's shape TODAY — a process-wide
`static NativeJailBackend backend;`, the same idiom `extract_pdf_text_detail::invoke_worker` uses — would
now inherit the FIXED, mutex-guarded map, not the race. "Already-solved... a direct reuse of Track B's
real, shipped pattern, not a new mechanism" (§4a's original framing, which Revision 4's own text calls an
overstatement) is, as of `40c573e`, materially truer again than Revision 4's own correction claims — the
specific mechanism the correction pointed at is no longer broken.

What does NOT go away: §2c's other two costs — (1) a genuinely separate, from-source vendoring effort
(`FetchContent`-ing xlnt/OpenXLSX, a brand-new CMake target, a brand-new native worker binary "not
designed in the same code-level detail" as Track B's own, per §8's own admission) and (2) a native-C++
zip-bomb guard written from scratch against miniz's API, distinct from and not reusable across §4b's
`zipfile.infolist()` guard — are both real and untouched by `40c573e`; neither was ever contingent on the
concurrency bug. Round 3 finding 11's own original question (is a second, fully separate isolation
mechanism worth paying for once route (b) is unavoidable anyway for two of three tools?) also predates and
does not depend on the bug discovery. So the collapse is not left with NO justification — it is left with
a WEAKER one than the draft currently states: two real costs against the stated benefits (one tool's
share of Linux support, one tool's share of avoiding the per-call interpreter-startup cost), not three,
and the draft's own rhetorical framing ("the headline finding of this pass's own investigation," a whole
enumerated point in the Net evaluation, a full paragraph in §4a) gives the now-resolved item outsized
weight relative to what remains. Whether the two SURVIVING costs alone still tip the same way is a real,
open, re-weighable question this pass does not settle either way — it is a judgment call for whoever
reruns the evaluation, not something round 4 can resolve unilaterally — but the draft as currently written
presents settled, three-legged reasoning that is now measurably two-legged, and does not disclose that its
own supporting investigation has since been overtaken by events (`40c573e`, dated the same day as this
draft's own "Revision 4" content, 2026-09-01).

**Fix direction**: a Revision 5 pass must (a) update §2c's "Net evaluation" and §4a's parallel paragraph to
state that the `instances_` race is FIXED as of `40c573e`, not "already-shipped... out of scope," (b)
either re-run the cost/benefit weighing on the two surviving costs alone and state explicitly whether the
collapse conclusion still holds, or explicitly flag that the conclusion needs re-litigation now that a
load-bearing premise changed, and (c) correspondingly soften "already-solved... is not accurate" language
in §4a, since the specific inaccuracy that correction relied on has itself been corrected upstream. Note
for §8's own historical-record discipline: this does not mean the disclosure of the (formerly real, now
fixed) bug was wrong to make at the time — it means the draft has a live-document staleness problem
distinct from any of its arguments being wrong when written.

### Finding 14 (Critical, new — I8: budgets are enforced / unbounded-resource defect introduced by this revision's own fix). Finding 9's fresh-per-call scratch directory, combined with `create_python_worker()`'s real (non-deduped) mount-grant path, permanently and irrevocably grows the shared `AppContainerProfile`'s Windows ACL by one entry per call, forever — directly contradicting this section's own "deduped grant bookkeeping" claim

§4b's finding-9 companion-fix text states, of `shared_profile()`'s AppContainer SID: "it is read-only,
deduped grant bookkeeping, not a mutable per-call instance map." Checked directly against the actual grant
call path finding 9's own fix feeds into — `NativeJailBackend::create_python_worker()`
(`native_jail_backend.cpp:574-622`, the function `MediatedPythonRunner::initialize()` calls, re-verified
this pass) — that claim is false for the specific grant these tools' own scratch mount goes through:

```cpp
for (MountSpec const& mount : spec.mounts) {
    ...
    std::wstring host_path = widen(std::get<std::string>(mount.source));
    auto granted = (*profile)->grant_path(host_path, mount.read_write);   // line 612 -- NOT deduped
    ...
}
```

This is the SAME per-mount loop shape as `create()`'s own (`native_jail_backend.cpp:204-217`, line 214 —
identical pattern). `AppContainerProfile::grant_path()` is documented, in this exact file, as "explicitly
documented as additive, non-idempotent" (`native_jail_backend.cpp:125-133`'s own comment, describing a
DIFFERENT, already-fixed bug in this same function: repeatedly granting the SAME deployment-fixed path —
`python_home`/`extra_sys_path`/the worker binary directory — on every session creation grew the profile's
DACL "without bound... purely from ordinary session churn"). That earlier bug was fixed by routing those
specific paths through `grant_ro_deduped()` (`native_jail_backend.cpp:134-145`, called at line 622 for
exactly those three deployment-fixed paths) — a dedup keyed on the path STRING, which only helps when the
same path repeats across calls.

The mount-list loop at line 612 (`spec.mounts`, which is where `MediatedPythonConfig::mount_roots` — and
therefore `extract_scratch` — actually lands) does NOT go through `grant_ro_deduped()`. It never did, for
a real reason unrelated to this draft: session-scoped mount paths (e.g. a genuinely session-unique `work`
directory) are not supposed to repeat, so deduping them would be a no-op at best and a category error at
worst (`grant_ro_path_once()`'s own header comment, `native_jail_backend.hpp:185-198`, names its OWN dedup
guarantee as applying specifically to "a FIXED, host-controlled path," explicitly contrasted with anything
per-call). Track B's own worker never hits this cost because its worker uses one-shot `create()`/`exec()`
against a SHARED, fixed `scratch_dir()` granted via `grant_ro_path_once()` (dedup applies, path repeats
every call) — not a per-call-unique mount. **Finding 9's fix deliberately breaks that precedent's shape**:
it makes `mount_roots["extract_scratch"]` a genuinely fresh, never-repeated directory on every single
call, specifically to close the cross-call data-collision hole finding 9 was written to fix. That is
exactly the one property (`path` never repeats) that defeats path-string deduping — there is no
`grant_ro_deduped()`-shaped fix available for a path that is unique by design. Every `ExtractDocxText`/
`ExtractXlsxCells`/`ExtractPptxText` call therefore appends one new, permanent ACE to the shared
`AppContainerProfile`'s DACL. `destroy()` (re-verified against the `40c573e` diff, which touched this exact
function) only erases the `instances_` map entry and tears down the job/process — no `revoke_path`/
`ungrant`/DACL-shrink call exists anywhere in `native_jail_backend.cpp`/`.hpp` (grepped directly, zero
hits beyond the comment describing the ALREADY-fixed deployment-path bug).

Consequence, for the exact workload this catalog row targets (batch/repeated document extraction, the
same scenario ADR-160's scheduler and this draft's own finding 8/12 call-count discussion are both about):
a long-running host process serving many Extract calls accumulates one non-removable ACE per call, without
bound, for the life of the process. This is not merely a slow memory/DACL-parse-time leak — Windows
security descriptors have a finite practical size ceiling, so on a sufficiently long-lived host this is a
real path to `grant_path()` itself starting to fail outright, meaning every SUBSEQUENT extraction call
across ALL THREE tools fails closed — a self-inflicted denial-of-service introduced by the very fix
(finding 9) meant to make the mechanism safe, and worse under Revision 4 than any prior revision because
all three tools, not one or two, now generate this growth on every call.

**Fix direction**: name this explicitly as a new, real residual of finding 9's fix — either (a) revoke the
per-call ACE when `destroy()`/`~MediatedPythonRunner()` tears down that call's runner (a real capability
`AppContainerProfile` would need to expose, not proven to exist today — not investigated further this
pass), or (b) stop granting the unique per-call subdirectory directly and instead grant ONE shared,
host-fixed parent root (e.g. `extract_scratch_root()` itself) via `grant_ro_path_once()`-style dedup once,
with the per-call uniqueness living entirely in the subdirectory name/AppContainer-relative path under
that already-granted root — the OS grant stays fixed and deduped even though the logical mount target
still varies per call. Option (b) needs checking against whether `MountSpec`/`mount_roots` can express "the
guest sees a subdirectory of a granted root" rather than "the guest sees exactly the granted path" — not
resolved here, a real design question for whoever picks this up, but the current design's silence on
this cost is the actual defect this finding reports, not a specific fix's soundness.

### Finding 15 (High, new — I8: unbounded disk-space leak; composes with finding 14). No cleanup mechanism for the per-call scratch directory or its `args.json`/`out.json` contents is named anywhere in this draft, and `create_directories()`'s own success-on-existing semantics turn that gap into a latent stale-directory-reuse risk under the exact edge case this round was asked to check

The whole design draft — searched directly, zero hits for "cleanup"/"delete"/"remove"/anything like it
anywhere in §1-8 — never states that the per-call scratch subdirectory `unique_scratch_dir_name()`
produces, or the `args.json`/`out.json` files written inside it, are ever removed after a call completes.
This is a real gap even measured against Track B's own, admittedly weaker, precedent: `extract_pdf_text.hpp`
(`:445`) calls `std::filesystem::remove(file, ec)` — "best-effort cleanup" per its own comment — immediately
after `invoke_worker()` returns, for the ONE scratch FILE it wrote. §4b's design, which creates a whole
DIRECTORY per call (not a file in a shared directory), names no equivalent call at all — not even a
best-effort one.

Concretely, this is a plain, unbounded disk-space leak for a host serving the batch/repeated-call workload
this catalog row targets — one subdirectory plus two small JSON files, forever, per call, on top of finding
14's ACL-growth defect (a genuinely separate resource, but the same root cause: finding 9's fix names how
to CREATE a fresh per-call directory and never how to retire one).

It also reopens, narrowly, exactly the collision question this round's investigation 3 asked about (pid
reuse after a fast process exit). `unique_scratch_dir_name()` is specified as "the identical way"
`unique_scratch_filename()` is built (pid + a function-local `std::atomic` counter) — verified directly
against the real precedent (`extract_pdf_text.hpp:262-271`): a function-local `static std::atomic<
std::uint64_t> counter{0}` inside an `inline` header function IS genuinely one single, process-wide,
correctly-initialized counter (C++'s inline-function-static-is-shared-across-TUs guarantee, plus
thread-safe "magic statics" initialization) — this part of finding 9's premise holds up, confirmed by
reading the actual code, not assumed. The counter is therefore unique WITHIN one process's lifetime,
monotonically, with no reset short of process restart. The real gap is what happens ACROSS a restart: a
process crash or ordinary host restart resets the in-memory counter to 0; if the OS then reuses the exact
same pid for the new process (routine on both Windows and Linux once a pid range wraps, and more likely
the shorter-lived the intervening process is), the very next `unique_scratch_dir_name()` call in the NEW
process computes the SAME name (`pid_0`, in whatever concrete format the real function ends up using) a
PRIOR process may have generated and — per this finding's own headline point — never cleaned up.
`std::filesystem::create_directories()` treats "directory already exists" as ordinary success, not an
error (confirmed: the one real precedent this draft's own text says `extract_scratch_root()` "mirrors,"
`extract_pdf_text.hpp:420-427`'s `create_directories(dir, ec); if (ec) {...}`, only surfaces `ec` on an
actual filesystem failure — pre-existence is not one) — so a collision here fails silently, not loudly.
The practical consequence is narrow but real: a call whose generated directory happens to coincide with an
old, un-retired one from a crashed/restarted process could write its own `args.json` into a directory that
already contains a stale `out.json` from an unrelated, unrelated-tenant prior call — and if THIS call's own
fixed script errors out before writing a fresh `out.json`, the host-side read (§4b's finding-2 fix) would
pick up the stale file rather than fail, a real, if low-probability, cross-call data-integrity hole
narrower than but structurally the same class as finding 9's own original concern.

**Fix direction**: name an explicit cleanup step in §4b (recursive removal of the per-call subdirectory
after the host finishes reading `out.json`, mirroring Track B's own best-effort discipline but for a whole
directory, not one file) — this alone closes the disk-leak half. For the narrower collision half, either
have the host check-and-fail (not silently proceed) when `create_directories()` reports the directory
already existed (`std::filesystem::exists(dir)` checked BEFORE calling `create_directories`, or inspecting
the OS-level "already existed" signal `create_directories` itself does not surface via `ec`), or extend the
name with a component that does not reset across a process restart (e.g. a boot-time/process-start
timestamp component alongside pid+counter) — not designed here, named as the real gap this round's own
investigation asked about.

### Finding 16 (Medium, investigation 2 — I8, disclosed-but-unexamined). §6 widens the interpreter-startup-cost disclosure to "three tools, not two," but never re-examines the qualitative aggregate-concurrency picture the collapse itself changes

§6's finding-11 update is, verified directly against its own text, a pure headcount edit: "now for three
tools' calls, not the one Revision 2 narrowed to or the two Revision 3 widened to — still not sized." That
is honest as far as it goes, and matches every other "mechanism resolved, number open" disclosure in this
section — but it is not the same claim as actually re-examining what changed in KIND, not just count, once
`ExtractXlsxCells` joined route (b). Before Revision 4, an agent batch-extracting a folder of `.xlsx` files
under ADR-160's real, shipped parallel-batch scheduler (cited elsewhere in this same draft, §2c) spawned
ZERO `MediatedPythonRunner`/CPython-interpreter processes — every one of those calls rode §4a's cheap,
purpose-built native worker instead. As of this revision, the identical batch spawns one dedicated,
full-CPython-plus-package-import worker PER call, exactly like DOCX/PPTX always did. §6 states the resulting
per-call cost is unsized for "all three" but never names the qualitatively new fact that an entire tool's
worth of previously-cheap batches now contributes, for the first time, to the SAME aggregate concurrent
memory/CPU pressure class (`MediatedPythonConfig`'s 1 GiB `memory_bytes`/8 `pids` defaults, §6's own
citation) the other two tools already created — a worst-case-concurrency statement, not a per-call-cost
statement, and a different thing to disclose than "the number is still open." This is not a defect in the
collapse decision itself (§6 was never sized before either, for any tool count), but it is a real gap in
whether this section's own re-derivation for Revision 4 actually re-examined the aggregate picture, as
opposed to inheriting the prior revision's prose with a word changed from "two" to "three."

**Fix direction**: add one explicit sentence to §6 naming the qualitative shift — an XLSX-only batch, which
previously paid none of this cost, now pays all of it — alongside the existing, still-accurate "not sized"
disclosure, so a future reader benchmarking this doesn't have to re-derive that the worst case changed in
kind, not just in per-call weight.

### Finding 17 (Medium-High, investigation 4 — correctness, undisclosed scope gap, same class as round 3 finding 10 but left unapplied to XLSX). `ExtractXlsxCells` does not get finding 10's own Reply-shape-level disclosure discipline, and two real, checkable `openpyxl` `read_only=True`-specific gaps go unnamed

§3's `ExtractXlsxCells` block does re-derive the `openpyxl.load_workbook(..., read_only=True, data_only=True)`
call correctly against the Reply's own "cell values only, not formulas, not formatting/styles" scoping —
investigation 4's own narrow question (did the collapse silently drop or alter that scoping) turns up
nothing wrong; the prose is accurate and specific. But unlike `ExtractDocxText`'s `body_text_only: true` —
a machine-readable Reply FIELD added specifically so "a caller cannot mistake `preview`/`blob`'s
completeness for 'the whole document's text'" (§3, finding 10's fix) — `ExtractXlsxCellsReply` has no
equivalent field; the `read_only=True`/`data_only=True` disclosure lives only in prose, one abstraction
level less durable than what DOCX got for a materially similar class of gap.

Two real, checkable gaps this leaves unnamed, verified directly against `openpyxl`'s own documented
behavior (`openpyxl.readthedocs.io`, its own GitHub issue tracker — the same external-fact-checking
discipline round 3 finding 10 applied to `python-docx`, not assumed from the planning doc or from
Revision 1's citation):

- **Merged cells silently vanish in `read_only=True` mode specifically** — worse than normal-mode
  `openpyxl`, not merely absent formatting: in normal mode, non-top-left cells of a merged range come back
  as a real `MergedCell` object whose value is `None` by design (a real signal something was merged); in
  `read_only=True` mode, those same cells come back as an ordinary `EmptyCell` — indistinguishable from a
  cell that was simply always blank. A workbook whose content lives partly inside a merged block (a common,
  ordinary spreadsheet pattern — merged header rows, merged label cells) has that content silently absent
  from every row after the top-left one, with literally zero signal in `openpyxl`'s own API that anything
  was collapsed, let alone in this tool's Reply.
- **`read_only=True` mode's reported dimensions are not independently verified** — `max_row`/`max_column`
  (which `total_row_count`/the iteration bound driving `rows_processed` would have to derive from) come
  directly from the workbook's own `<dimension>` XML element in read-only mode, per `openpyxl`'s own
  documentation, which explicitly warns some producing applications set this incorrectly and names
  `ws.calculate_dimension(force=True)` as the caller's own required workaround when it is wrong. Nothing in
  §3/§4b's description of the fixed script calls this — for a catalog row 009 §7 itself frames as "parsing
  hostile files" (a framing this draft's own §4b/§6 quote repeatedly for the zip-bomb mechanism), an
  attacker-controlled or merely non-Excel-produced `.xlsx` with a wrong declared dimension range would
  silently under- or over-report `total_row_count` with no cap or truncation signal catching it, since from
  `openpyxl`'s own point of view nothing was truncated — the same shape of silent gap finding 10 found for
  DOCX's headers/footers, unexamined here.

Neither gap is a security/I2/I3 defect — both are correctness/completeness gaps in the same spirit finding
10 named for DOCX, and finding 10's own "named, not silently dropped" fix direction was never re-applied to
the sibling tool this round asked about directly.

**Fix direction**: give `ExtractXlsxCellsReply` an explicit disclosure field (e.g. a boolean or small enum
naming that merged-cell content beyond the top-left cell is not recovered, matching `body_text_only`'s
shape) and name the read_only-mode dimension-trust gap in §3's own prose next to `sheet_name`'s existing
disclosure, the same "named, not silently gapped" treatment already given to embedded images/formulas/DOCX's
own finding-10 gaps elsewhere in this same section.

### Investigated, no real defect found

- **Investigation 1 (Windows-only platform gap totality)**: checked directly against §2b, §2c, §4b, and
  §8's own "Still genuinely open" list — the draft already states, explicitly and repeatedly (not merely
  implied), that ALL THREE tools now share the Windows-only `MediatedPythonRunner`/`create_python_worker()`
  gap and that "no tool in this draft's v1 scope has a cross-platform-native fallback any more" (§8). This
  is a TOTAL, not partial, platform gap for this catalog row as of Revision 4, and the draft says so in
  its own words in at least three places (§2b's bullet, §2c's "what this narrows" bullet, §8's own open-item
  bullet) — nothing found here that the draft itself doesn't already disclose at least as honestly as this
  round would have asked for.
- **Investigation 5 (`ExtractXlsxCells`'s `sheet_name` through the `args.json`/`json.load()` channel)**:
  checked directly against §3's own XLSX block — `_args["sheet_name"]` used as `wb[_args["sheet_name"]]` (a
  genuine Python `str` from `json.load()`, dict-style lookup, never formatted into source text) is exactly
  finding 1's own injection-safe shape, correctly re-derived for XLSX specifically, not merely asserted by
  analogy to DOCX/PPTX (which have no live Args field to exercise it). One minor, non-security robustness
  note not risen to a finding: an unrecognized `sheet_name` raises a Python `KeyError` inside the fixed
  script with no stated handling, but that is an ordinary error-propagation question this draft's fixed
  scripts already leave open uniformly (how a worker-side exception surfaces back through `out.json`/the
  Reply is not designed for any of the three tools, not a new or XLSX-specific gap).
- **Investigation 6 (zip-bomb pre-check uniformity across all three tools)**: checked directly against
  §4b's own "Zip-bomb/XXE mitigation" subsection — it names `openpyxl.load_workbook(...)` alongside
  `pptx.Presentation(...)`/`docx.Document(...)` explicitly as the third call the `zipfile.infolist()` guard
  runs ahead of ("Revision 4 adds the third"). The mechanism is applied uniformly across all three tools in
  one shared subsection, not left to only some of the three formats' own per-tool blocks — no inconsistency
  found.

## Red-team round 5

Two real, new defects, found by reading the actual shipped code Revision 5 cites for its own fixes — not
by restating rounds 1-4 or §8's already-disclosed opens. Finding 18 is the headline: it answers the
prompt's own central question (does the finding-14 inheritance mechanism actually hold up) with "yes, the
Windows ACL-inheritance mechanics are real and correctly cited" — but shows that the fix, precisely
*because* that mechanism is real, trades finding 14's resource-exhaustion bug for an undisclosed
I2-relevant widening of the OS-level AppContainer boundary this codebase's own comments call a real,
load-bearing security layer, not a decorative one. Finding 19 is a narrower, "assertion, not a mechanism"
gap in the same shape round 3's finding 9 and round 4's finding 13 were themselves found in — a two-level
directory structure (shared root + per-call subdirectory) that Track B's own single-level precedent never
had to solve, with a cold-start ordering hazard the fix's own prose glosses over. One further, smaller
technical residual (mode-conflation in the proposed dedup extension) is folded into finding 19 rather than
numbered separately, since it shares the same root cause and the same fix-direction discussion. Three of
the five investigation lines this round was asked to run (the AppContainer inheritance mechanics
themselves, the create-then-grant race, and the cleanup RAII question) are recorded at the end as
genuinely checked and found sound — not padded into findings for their own sake.

### Finding 18 (Critical, new — I2: no ambient authority / real, undisclosed OS-layer authority widening). Finding 14's shared-parent-grant fix is mechanically sound Windows ACL inheritance, but it converts this codebase's own documented "layer-3 backstop" from a per-call-narrow boundary into a standing, cross-call/cross-session/cross-tool one — exactly the shape I2 exists to prevent, and exactly the layer this codebase's own comments say exists to catch a compromised worker, not merely a well-behaved one

**Investigation #1's answer, stated plainly first, since the round's own instructions ask for it clearly:
the AppContainer inheritance mechanism finding 14's fix relies on is real and correctly cited, not broken.**
Checked directly against `app_container_profile.cpp:152-205`'s real `grant_path()`: it sets
`ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE` (line 165) on the `EXPLICIT_ACCESSW` it
builds, then calls `SetEntriesInAclW`/`SetNamedSecurityInfoW` to write that ACE onto the granted
directory's real, on-disk security descriptor — an ordinary Win32 DACL mutation, not a virtualized or
in-process-only construct. This is genuine, standard Windows ACL-inheritance behavior: when a *new* child
object is created under a directory whose DACL carries an ACE with `OBJECT_INHERIT_ACE`/
`CONTAINER_INHERIT_ACE`, the OS security reference monitor computes the new object's own DACL by merging
in that inheritable ACE **at creation time**, for any child created via the ordinary `CreateDirectoryW`/
`CreateFileW` path with no explicit security descriptor supplied — `std::filesystem::create_directories()`
is exactly such a call (the C++ standard library exposes no way to pass a security descriptor through it,
so it always takes the default-inheritance path on Windows). This holds **regardless of whether the child
existed at grant time** — inheritance is evaluated fresh, from the parent's *current* ACL, every time a
new child is created, by any process, not "baked in" only for objects that existed when `grant_path()` ran.
The round's own worry — "does inheritance only apply to objects that exist AT GRANT TIME" — is a real
question to ask but the answer, checked against the actual Win32 semantics this file invokes, is no: this
part of finding 14's safety claim holds. (The one genuine gap in the surrounding mechanism is finding 19
below — not in this inheritance mechanic itself.)

**What the fix's own "Composition with finding 6" paragraph does not check, and what turns out to matter**:
that paragraph (§4b, immediately after finding 14's fix) reasons entirely about the SOFTWARE layer —
`dispatch_open`'s `mount_roots` + `cap::FsRead`/`FsWrite` two-step check (finding 6's fix) — and correctly
shows that layer is unaffected by *how* the OS-level ACE got set. It never asks the symmetric question
about the OTHER, INDEPENDENT layer this codebase's own architecture names for exactly this kind of design:
`native_jail_backend.hpp:15-19`'s file-top comment states, verbatim, **"AppContainer's ACL denial is the
§1b layer-3 BACKSTOP for filesystem access, never the primary boundary"** — i.e. this codebase's own
documented threat model treats the OS-level AppContainer DACL as a real, load-bearing SECOND line of
defense, independent of and not replaced by the interpreter-level `open()` mediation (`dispatch_open`,
layers 1-2, per that same comment). CLAUDE.md's own "No `microvm` sandbox profile" locked decision states
the identical architecture at the project level: "isolation strength... comes from treating the whole
execution environment as the sandbox... with CPython's dangerous entry points mediated at the point of
use **and the OS-level jail as a second layer**." A backstop layer exists specifically for the case where
the first layer is bypassed — the textbook scenario here is a memory-corruption bug in `lxml`/`Pillow`/
`openpyxl`'s C extensions, triggered by a **hostile, attacker-controlled `.docx`/`.xlsx`/`.pptx` file**
(the exact threat class this draft's own §4b/§6 invoke repeatedly, verbatim, for the zip-bomb mechanism:
"a catalog row 009 §7 itself frames as 'parsing hostile files'"), giving an attacker code execution
*inside* the worker process, outside of `dispatch_open`'s Python-level mediation entirely — at which point
whatever the worker's raw OS-level filesystem authority is (regardless of what its own `mount_roots`
Python-level config claims) is exactly what a compromised worker can actually reach via raw Win32 calls.

**Checked directly, what that raw OS-level authority becomes under finding 14's fix**: `shared_profile()`
(`native_jail_backend.cpp:110-123`, already re-verified by this draft's own finding-9 companion-fix text)
is a single, process-wide `static std::optional<AppContainerProfile>` — **one AppContainer SID, shared by
every `NativeJailBackend` instance in the process**, including each of these three tools' own call-local
instances (finding 9's companion fix deliberately gives each call its own `NativeJailBackend` *object*, to
close the `instances_`-map race — but that object still calls the SAME `shared_profile()`, the SAME SID,
every time; finding 9's own text already says as much: "the SID/profile identity itself is immutable once
created, not a mutable per-call instance map"). Before finding 14's fix, this shared-identity fact was
harmless in practice at the OS layer: each call's scratch directory got its own individually-granted,
non-overlapping ACE (finding 9's original, unpatched design) — so even though every worker ran under the
same SID, no worker's OS-level grant reached any OTHER call's directory, because no common ancestor
carried an inheritable ACE covering more than one call's data. **Finding 14's fix is what changes this**:
granting `extract_scratch_root()` itself, once, with `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE`, means
every subdirectory ever created under it — every call's own scratch directory, from `ExtractDocxText`,
`ExtractXlsxCells`, and `ExtractPptxText` alike, across every session, for the entire lifetime of the host
process — carries the SAME SID's real, OS-level read-write access. A worker process that has been
exploited via a hostile input file can therefore reach, at the raw filesystem layer (bypassing
`dispatch_open` entirely, the same way a genuine backstop layer is supposed to be reached), every other
concurrently-running OR not-yet-cleaned-up (finding 15's own residual: a crash skips cleanup) call's
`args.json`/`out.json` — a real cross-call, cross-session, cross-tool confidentiality/integrity exposure
through the one layer this codebase's own comments call a real backstop, not decorative.

This is not a hypothetical this draft invented — it is the literal, direct consequence of the two verified
facts finding 14's own fix cites (`grant_path()`'s real inheritance flags, and `grant_ro_deduped()`'s real
process-wide dedup scope) combined with a third, already-documented fact (`native_jail_backend.hpp`'s own
"layer-3 BACKSTOP... never the primary boundary" framing) that finding 14's own "Composition" reasoning
never brings into the same paragraph. **Answering investigation #4's own question directly**: "grant once"
here means once per HOST PROCESS (not per session, not "once ever" across restarts) — `grant_ro_deduped`'s
dedup set is a plain function-local `static`, scoped to the process, exactly like `shared_profile()` itself
— so yes, multiple concurrent sessions genuinely share ONE `extract_scratch_root()`, and the isolation
between them is enforced ENTIRELY by the software layer (`mount_roots` + `path_prefix`-narrowed
capabilities, finding 6/9's own fixes, which remain genuinely sound — no defect found there this round)
with NOTHING independent backing it up at the OS layer any more for this specific mount, the opposite of
what `native_jail_backend.hpp`'s own architecture comment says this codebase is supposed to provide.

**Fix direction**: name this cost explicitly, the same "not swept under the rug" discipline finding 13's
own re-weighing already models — either (a) accept it as a real, disclosed residual specific to this
mount (the software layer alone carries the isolation burden for `extract_scratch`, same as it already
does for `"work"`, per finding 6's own "work" vs "extract_scratch" distinction — except finding 6's own
argument for why `"extract_scratch"` was safe specifically rested on "nothing standing exists beneath it
at the OS layer," a premise finding 14's fix now falsifies for the OS layer while leaving the capability
layer's own text unchanged), or (b) revisit option (a) from finding 14's own fix-direction list (an actual
per-call-ACE-revoke mechanism) despite the new `AppContainerProfile::revoke_path()` surface it would need,
specifically because it is the one alternative that keeps the backstop layer's own scope matching the
software layer's per-call narrowing — a real, load-bearing property this draft's own architecture
elsewhere treats as worth having, not this round's invention.

### Finding 19 (High, new — correctness/reliability, same "assertion, not a mechanism" shape as round 3 finding 9 and round 4 finding 13). Finding 14's fix asserts `extract_scratch_root()` "was already granted... BEFORE that subdirectory is created," but never names the mechanism that makes the ROOT itself exist and be granted before the very FIRST subdirectory anyone ever creates under it

Checked directly against `AppContainerProfile::grant_path()` (`app_container_profile.cpp:172-181`): it
opens `GetNamedSecurityInfoW(path.c_str(), ...)` on the target path FIRST, before doing anything else — a
Win32 call that requires the path to already exist on disk; it returns `app_container.get_security_info_failed`
otherwise. This means `grant_rw_path_once(extract_scratch_root())` cannot succeed until
`extract_scratch_root()` itself already exists as a real directory — a precondition finding 14's fix text
never names as its own, separate step.

Track B's own precedent, checked directly (`extract_pdf_text.hpp`'s `invoke_via`, lines 420-428, and
`invoke_worker`, line 291): `create_directories(scratch_dir(), ec)` runs FIRST, in `invoke_via`, unconditionally,
on EVERY call; `grant_ro_path_once(scratch_dir())` runs SECOND, inside `invoke_worker`, also on every call,
made cheap by the dedup set after the first success. That precedent works because Track B's scratch area
is exactly ONE level deep — the directory that gets created is the SAME directory that gets granted, every
time, in the same two-step create-then-grant order. **Finding 14's design is two levels deep** — a shared
`extract_scratch_root()` (created/granted once) and a per-call subdirectory underneath it (created every
call, never granted directly) — and this is new territory Track B's single-level precedent never had to
solve. `std::filesystem::create_directories()` (plural — the same function finding 14's own text cites)
recursively creates ANY missing intermediate directories, including a not-yet-existing root, in one call.
An implementer reaching for the obvious, idiomatic call — `create_directories(extract_scratch_root() /
unique_scratch_dir_name())`, exactly the shape finding 15's own fail-closed `exists()`-then-`create_directories()`
sequence is written around — creates the ROOT and the FIRST-EVER SUBDIRECTORY in the same call, on the
very first extraction call after a fresh process start or first-ever deployment. At that moment the root
has NOT yet been granted (finding 14's own fix hasn't run yet — nothing has triggered it), so the
subdirectory created alongside it inherits nothing; and by the time `grant_rw_path_once(extract_scratch_root())`
subsequently runs and succeeds (root now exists, so `GetNamedSecurityInfoW` no longer fails), it is too
late for the subdirectory already created in the same operation — Windows' creation-time inheritance
(finding 18's own confirmed-sound mechanism) only reaches children created AFTER the ancestor's ACE
exists, not ones that already existed when the ACE was added. **The reverse ordering has the identical
bootstrapping problem**: granting `extract_scratch_root()` BEFORE ever creating a subdirectory under it
still requires the root to exist first for `grant_path()`'s own `GetNamedSecurityInfoW` call to succeed at
all — and on a fresh process, it does not yet.

The only sequence that actually works is a genuine THREE-step dance finding 14's own prose never spells
out as three steps: (1) ensure `extract_scratch_root()` itself exists — its OWN, separate
`create_directories(extract_scratch_root())` call, idempotent, safe to run every time, exactly mirroring
how `scratch_dir()` gets created in Track B's precedent; (2) THEN `grant_rw_path_once(extract_scratch_root())`
(now safe to succeed, and cheap-after-first via the dedup set); (3) ONLY THEN
`create_directories(extract_scratch_root() / unique_scratch_dir_name())` for the per-call subdirectory,
which by now correctly inherits. Steps (1)+(2) are a genuinely new pair this draft's design needs that
Track B's single-level precedent never had to name, because Track B's "the directory that gets created"
and "the directory that gets granted" were always the same path. Getting this wrong is self-healing after
the very first call in a process's lifetime (once the root exists and is granted, every subsequent call's
subdirectory creation correctly inherits) — a narrow, first-call-only window, similar in shape and
narrowness to finding 15's own disclosed pid-reuse residual — but unlike that residual, this one is not
named anywhere in §8, and its failure mode (a spurious, real `open()` Access-Denied failure inside the
FIRST worker any process ever spawns for these tools) is a genuine, undesigned reliability gap, not merely
a theoretical corner case.

**A smaller, related technical residual, worth folding in here rather than numbering separately**: §8's own
finding-14 residual already names extending `grant_ro_deduped()`'s dedup bookkeeping to a read-write twin
(`grant_rw_path_once()`) "reusing the identical dedup `std::unordered_set<std::wstring>` and mutex."
Checked directly against `grant_ro_deduped()` (`native_jail_backend.cpp:134-145`) and `grant_ro_path_once()`'s
own header comment (`native_jail_backend.hpp:184-198`, "Calling this repeatedly with the SAME `path` is a
safe no-op"): the dedup set is keyed on the path STRING alone, with no mode/access-level component. A
`grant_rw_path_once()` that reuses this SAME set would silently no-op if the identical path had previously
been granted READ-ONLY via `grant_ro_path_once()`/`grant_ro_deduped()` (or vice versa) — the second call's
different access level would never actually reach `grant_path()`. This is inert for `extract_scratch_root()`
specifically, since it is a brand-new, dedicated path never granted for any other purpose today — but it is
a real, unaddressed gap in the general mechanism the fix proposes reusing verbatim, and the SAME gap would
also affect the "prefix-covered check" extension §8 already names as open for closing `create_python_worker()`'s
per-mount loop blind spot (a prefix granted read-only should not silently suppress a later read-write mount
under that same prefix) — worth naming alongside that residual, not a new item needing its own number.

**Fix direction**: name the three-step sequence explicitly in §4b (ensure-root-exists, as its own
idempotent call, distinct from granting it, distinct from creating any subdirectory under it) — the same
"mechanism resolved, not merely asserted" discipline finding 9's own fix was written to model — and add a
mode component (or a second, mode-aware set) to whatever dedup bookkeeping `grant_rw_path_once()` ends up
sharing, so a future read-only grant on a since-read-write-granted path (or vice versa) fails loud rather
than silently no-opping.

### Investigated, no real defect found

- **Investigation #1 (AppContainer inheritance mechanics)**: see finding 18's own opening paragraph —
  checked directly against `app_container_profile.cpp`'s real `grant_path()`, the inheritance mechanism
  finding 14 relies on is genuine, standard Win32 ACL-inheritance behavior, not broken and not merely
  "asserted." The defect this round found is not in whether inheritance works, but in what its own
  correctness costs at a layer the fix's own reasoning never checks (finding 18) and in a sequencing
  precondition the fix's own prose never names (finding 19).
- **Investigation #2 (TOCTOU race between grant-once and concurrent per-call subdirectory creation)**:
  checked directly against `unique_scratch_dir_name()`'s own cited construction (a function-local `static
  std::atomic<std::uint64_t>` counter, the identical, verified-thread-safe idiom `unique_scratch_filename()`
  already uses, `extract_pdf_text.hpp:262-271`) — two concurrent calls within one process can never
  generate the same subdirectory name, by construction of the atomic `fetch_add`, so there is no genuine
  concurrent-creation collision to race on. `grant_ro_deduped()`'s own dedup set is mutex-guarded
  (`native_jail_backend.cpp:136-144`), so concurrent callers calling the deduped grant function on the
  (unique-per-call, but same ROOT) path are correctly serialized, not racing — and Track B's own precedent
  (`extract_pdf_text.hpp`'s `invoke_via`/`invoke_worker` split) already establishes the safe pattern of
  calling create-then-grant on every single call, relying on internal dedup, rather than a one-time
  out-of-band "session/tool-registration time" call — the scenario this investigation asked about checking
  for. No new race found beyond the already-disclosed, narrower, sequential pid-reuse window (finding 15)
  and the cold-start ordering gap this round found instead (finding 19), which is not a concurrency race.
- **Investigation #3 (`remove_all()` cleanup reliability across all real call-termination paths)**: the
  fix's own text specifies an RAII scope-guard around the call-local scratch-directory handle, matching
  the reverse-declaration-order discipline finding 9's companion fix already uses — checked against what
  that actually covers: normal return (success), any early `return` on an error path, and a worker
  crash/wall-clock timeout (both of which cause `exec_session()`/`run()` to return an error result rather
  than hang or crash the HOST — the watchdog phases visible in `native_jail_backend.cpp`'s poll loop are
  what make this true) are all ordinary C++ stack-unwinding/return paths an RAII guard fires on. The one
  termination mode no in-process RAII mechanism of any kind can cover — a HOST process crash — is not
  silently unhandled: it is the exact scenario finding 15's own fail-closed `exists()`-before-`create_directories()`
  check already exists to convert from a silent collision into a loud error. No gap found beyond what §8's
  own finding-15 residual already discloses.
- **Investigation #5 (§3/§4 Reply-shape vs. mechanism consistency for the new disclosure fields)**:
  `merged_cells_collapsed` appears both in §3's `ExtractXlsxCells` Reply field list (line ~601) and in
  §4b's finding-2 "output transport" field enumeration (which explicitly lists it, "Revision 5, round 4
  finding 17"), the same treatment `body_text_only` already got for DOCX in both places. No drift found
  between what §3 claims the Reply contains and what §4b's mechanism actually produces for either field.

## Red-team round 6

Round 6's own brief: treat the scratch-slot pool — the one genuinely new, from-scratch concurrency/
resource-pool mechanism this draft has introduced, not reused from an existing shipped pattern — with the
same skepticism due any new pool/lease abstraction on a security-relevant path. Five real, new defects
found, by reading the actual mechanism the pool's own fix text specifies and the actual precedent
(`extract_pdf_text.hpp`, `worktree_mount_fs.hpp`) it claims to follow — not by restating rounds 1-5 or
§8's already-disclosed opens. Finding 20 is the headline: the host's own `out.json` read — trusted,
unmediated, and never checked against this codebase's own real ADR-014-grade path-containment
discipline — is a materially different, and materially weaker, channel shape than the Track B precedent
it claims to reuse, and it turns finding 18's already-disclosed backstop exposure into an active
content-injection path through the ordinary Reply pipeline. Finding 21 shows the wipe mechanism's own
prose is self-contradictory ("best-effort" vs. "provably-empty") with no failure path named. Finding 22
catches one affirmatively incorrect technical claim about `std::condition_variable` ordering. Finding 23
is a smaller I4 gap. Finding 24 answers the prompt's own investigation #6 directly: the ADR-041 analogy
is textually accurate but rests on a residual of a different kind, and the draft leans on it more heavily
than the difference supports. Two of the prompt's six investigation lines (cross-tool handout ordering,
and capability-grant composition with per-slot mounting) are recorded at the end as genuinely checked and
found sound.

### Finding 20 (Critical, new — I2/I3: the host's own `out.json` read is an unverified, worker-writable file-path read, not a kernel-mediated channel — a compromised worker can redirect the HOST's trusted read, not merely a hypothetical raw-backstop reader, to arbitrary content reachable under the shared SID)

**What the fix text actually specifies, checked directly (§4b, "Fix for finding 2," the paragraph
containing "the HOST reads `out.json` directly off the real host directory backing `extract_scratch`")**:
after `run()` returns success, the host opens `out.json` with "the same host-side `std::ifstream` pattern
`ExtractPdfText`'s own `fetch_via_path`/scratch-file write already uses in the other direction," checks
`std::filesystem::file_size` against a coarse cap, then reads the whole file into memory and hands it to
§5's `build_reply`. No file-type check (`std::filesystem::is_regular_file`/`symlink_status`), no
containment re-verification of any kind, appears anywhere in this description or in any of Revisions 2-6's
text about this step.

**Checked directly against the ACTUAL Track B precedent this cites, not assumed equivalent**:
`extract_pdf_text.hpp:420-430` (the host writing the fetched INPUT bytes to a scratch file via
`std::ofstream`) is the HOST writing trusted bytes that the WORKER later reads — the vulnerable direction
(a worker redirecting a read to attacker-chosen content) does not exist there, because the worker is the
reader, not the writer, of that file. The worker's OWN OUTPUT, in Track B, is never read back off a
worker-writable file at all — it comes back over `outcome->stdout_text`, a size-capped
(`kWorkerOutputBytes`, `extract_pdf_text.hpp:276`), OS-pipe-mediated channel the process-exec plumbing
itself owns end-to-end (`extract_pdf_text.hpp:307,324`), not a named path the worker could turn into a
reparse point, junction, or symlink. **This draft's own `out.json` channel is not "the same pattern... in
the other direction" — it is a different channel shape than Track B uses for the direction that actually
matters here (worker-produced output the host will trust).** Track B has no analogous exposure to point to
as already-accepted precedent, because Track B never puts a worker-writable file in the host's own trusted
read path for its result.

**Checked directly against what this exact codebase already builds for exactly this threat class**:
`include/agentengine/core/worktree_mount_fs.hpp:1-125` — ADR-014, `021-Platform-Support-and-Portability.md`
§6 G3's own "path-escape corpus (`..`, symlinks, junctions, `\\?\`, ADS, case tricks, unicode
normalization) fails to escape a mount on any platform" requirement — exists specifically because a naive
open-by-path is not safe against a writer-controlled reparse point: `open_within_mount_root`'s own header
comment states the mechanism precisely ("a single `CreateFileW` call lets Windows itself resolve whatever
`guest_path` actually names (following any reparse points transparently...), and only THEN is the
resulting HANDLE's real, filesystem-resolved location (`GetFinalPathNameByHandleW`) checked against the
mount root"). This is a real, shipped, ADR-014-Judged primitive in this exact tree, built to close exactly
the class of gap the office-extraction draft's own `out.json` read reintroduces by using a plain
`std::ifstream` instead. Nothing in §4b's finding-2 text, or in any later revision's discussion of the
pool, checks the `out.json` read against this precedent — an omission this draft's own "verified directly
against the actual code, not assumed" discipline should have caught, the same way it caught `dispatch_open`'s
second capability check (finding 6) by checking the real function rather than trusting `mount_roots`
narrowing alone.

**Why this is exploitable under this draft's own already-stated threat model, not a new hypothetical**:
finding 18's own text already establishes the operative scenario — "a memory-corruption bug in
`lxml`/`Pillow`/`openpyxl`'s C extensions, triggered by a hostile input file, giving an attacker code
execution *inside* the worker process, outside of `dispatch_open`'s Python-level mediation entirely." That
compromised worker already has ordinary OS-level read-write authority over its own leased slot directory
(finding 4/9's own design: the mount is granted read-write so the fixed script can write `out.json` at
all). Nothing about `dispatch_open`'s in-worker mediation (finding 6, confirmed sound again this round —
see "Investigated, no real defect found" below) prevents a worker with that authority from replacing the
regular file `out.json` with a reparse point/junction pointing at any path the shared AppContainer SID can
read — which, per finding 18's own analysis, includes every one of the pool's other `N-1` slots' CURRENT
contents (live, possibly different-session, different-tool data), plus ADR-041's own already-disclosed
`win.ini`/`hosts` leak. Whether AppContainer's restricted token specifically blocks reparse-point/junction
creation via `FSCTL_SET_REPARSE_POINT` on a file the worker itself owns is a real, checkable Windows-
security question this draft does not examine either way (the same "check the actual API, don't assume"
standard this draft holds itself to for `grant_path()`'s inheritance flags) — but the underlying gap is
broader than that one escalation vector and platform-independent: **nothing in the design verifies that the
file the host is about to trust and return to the caller is the actual regular file the fixed script wrote,
of the size it claims, before reading it.** The consequence, if the redirection vector is real: the HOST
ITSELF — not merely a hypothetical attacker independently reaching the raw OS layer, which is all finding
18 disclosed — reads attacker-chosen content and returns it through the ordinary `preview`/`blob` Reply
channel to whichever caller/session made THIS call, which may be a different, higher-trust caller than
whoever supplied the hostile document to some other concurrent call sharing the same slot pool. This
escalates a passive, backstop-only exposure into an active exfiltration/content-injection primitive
reachable through the host's own trusted code path, with no independent attacker action against the OS
layer required.

**Fix direction**: route the `out.json` (and, for symmetry, `args.json`) read/write through
`worktree_mount_fs.hpp`'s own `open_within_mount_root` primitive (or an equivalent open-then-verify-handle
check) instead of a bare `std::ifstream`/`std::ofstream`, so the file actually opened is verified, by
handle, to still resolve inside the leased slot directory before its bytes are trusted — the identical
"verify the object actually used, not the path requested" discipline `open_within_mount_root`'s own header
comment already states as its whole reason to exist. This is a real, scoped fix reusing an
already-shipped, already-Judged mechanism, not a new one — the same "reuse what exists" discipline
findings 6/9/14/18 already modeled.

### Finding 21 (High, new — I2/I8: the wipe mechanism is specified as "best-effort" while being the sole basis for a "provably-empty slot" guarantee; no failure path is named for either direction)

**The self-contradiction, quoted directly**: point 4 of the pool's own fix text says `acquire()` "recursively
removes that slot directory's CONTENTS... (`std::filesystem::remove_all` over its children, best-effort) so
a caller always receives a **provably**-empty slot regardless of what a previous occupant left behind" —
and separately, that the lease destructor "recursively removes the slot's contents again (best-effort,
mirroring finding 15's own release-time cleanup)." "Best-effort" (the `std::error_code` overload of
`remove_all`, ignoring the result — finding 15's own established meaning of that word, cited by name here)
and "provably-empty" cannot both be literally true of the same operation: an ignored error code proves
nothing about the outcome, it only avoids throwing. This is not a wording nitpick — it is exactly the kind
of "assertion, not a mechanism" gap rounds 3/4/5 each found once already (findings 9, 13, 19) in this
draft's own prior fixes, now present in the pool's own core safety claim.

**What is genuinely unspecified, checked against the fix text and found absent**: neither the acquire-side
nor the release-side wipe has any stated behavior for the case where `remove_all` fails to fully empty the
slot — a transient file lock left by a just-killed worker process (the wall-clock-timeout kill path finding
15's own text already names as a real termination mode), or a **poisoned** file: since the worker itself
holds read-write OS authority over its own slot (finding 4/9's grant), a compromised worker could, before
crashing, set a DACL entry denying delete/write access to a file it creates inside its own slot — plausible
because the worker is creating that ACE as the SAME SID the AppContainer identity already carries, not as
an outside attacker fighting the grant. If the wipe silently fails in either of these ways, the design as
written gives exactly two possible outcomes and never says which: (a) `acquire()`/the destructor silently
treats the ignored error as success — a slot returns to the free-list, or gets handed out, still carrying a
previous tenant's data, defeating the entire reason findings 18/19 replaced the shared-root design with a
pool in the first place; or (b) nothing quarantines the slot, so it remains in circulation indefinitely,
silently dirty, with no monitoring anywhere in this draft that would ever surface that it happened. A third,
undesigned possibility — permanently poisoning one slot so it can never again be verified clean — would
mean a single hostile document shrinks the pool's effective `N` by one, forever, with no quarantine-and-
replace story; enough hostile documents over the process's lifetime silently walk the pool toward
`office_extraction.scratch_pool_exhausted` for every subsequent call, a slow self-inflicted denial of
service this draft's own I8 discipline elsewhere (fail-closed caps, explicit disclosure of every unsized
ceiling) does not apply to this specific failure mode.

**Why this also reopens the cross-tool concern investigation #3 asked about**: since one pool is shared by
all three tools (confirmed below, "Investigated, no real defect found"), a wipe failure of either kind
means the NEXT tenant of that slot — which could be any of `ExtractDocxText`/`ExtractXlsxCells`/
`ExtractPptxText`, not necessarily the same tool or even the same session — is the one exposed, not just a
same-tool repeat caller.

**Fix direction**: check the `remove_all` result on both sides rather than discarding it; on a failed wipe,
fail closed for that specific slot — do not return it to the free-list, and do not hand it out — logging or
otherwise surfacing the quarantine event (feeds finding 23 below) rather than silently shrinking capacity;
decide, and state, whether a quarantined slot is ever recovered (a host-triggered re-wipe/retry with
elevated privilege, or a permanent removal from the pool with `N` decremented and disclosed) rather than
leaving it undecided which of the two failure modes above actually happens.

### Finding 22 (Medium-High, new — correctness: the pool's own condition-variable wait mechanics contain one affirmatively incorrect claim about `std::condition_variable`'s ordering guarantees, and are not grounded in a concrete, race-free wait idiom the way this draft's other primitives are)

**The incorrect claim, quoted directly**: point 5 of the pool's fix text states "A FIFO wait order on the
pool's condition variable (ordinary `std::condition_variable::wait`, no priority scheme) avoids starvation
without needing anything more elaborate." **This is not true of `std::condition_variable`.** Neither the
C++ standard nor any mainstream implementation this codebase targets (Windows `CONDITION_VARIABLE`,
pthread condition variables on Linux) guarantees any ordering among waiters woken by `notify_one()` — which
waiter wakes is implementation- and scheduler-dependent, not FIFO by construction. A design that actually
wanted FIFO fairness would need an explicit ticket/queue mechanism (e.g., each waiter takes a sequence
number and only proceeds when it is next), which this draft does not describe. In practice, with a small
`N` and short slot-hold durations, unfair wakeup is unlikely to cause visible starvation — but the text
asserts a specific, checkable API guarantee that does not exist, in a draft whose entire methodology is
built on not asserting facts about real mechanisms without checking them (the identical standard this round
already used against finding 20's channel-shape claim and every prior round used against this draft's own
earlier fixes).

**The separate, related spec gap**: nowhere does the fix text name the actual wait idiom `acquire()` uses.
The race-free way to implement "wait up to `wait_ceiling` for a free slot, and never hand a slot to a
waiter that has already timed out" is the predicate form of `wait_for` —
`cv.wait_for(lock, wait_ceiling, [&]{ return !free_list.empty(); })` — which internally loops through
spurious wakeups and re-checks the predicate under the same lock before returning, so a `false` return
(timeout) and a successful pop of the free-list happen atomically with respect to each other; done this
way, a timed-out waiter genuinely cannot also receive a slot, because the pop and the timeout decision are
the same critical section. This draft names the exact atomic-counter construction behind
`unique_scratch_filename()` as "verified thread-safe by construction" (§4b, finding 9's fix) when reusing
it — but gives the pool's own, brand-new concurrency primitive no equivalent grounding: it is described only
in prose, with one piece of that prose demonstrably wrong about the primitive's own guarantees. This is not
a proven live bug (a correctly-written predicate-form `wait_for` is the obvious, standard choice any
competent implementer would reach for) — it is a specification gap inconsistent with this draft's own
stated bar for every other mechanism in it.

**Fix direction**: drop the "FIFO" claim (or replace it with an actual ticket-based fairness mechanism, if
FIFO ordering is genuinely wanted); name the predicate-form `wait_for` explicitly as the required
implementation shape, the same concreteness `unique_scratch_filename()`'s citation already gives its own
primitive, so a "lost wakeup / handed-a-slot-after-giving-up" implementation error is ruled out by the
design text itself, not left to an implementer's own judgment to get right.

### Finding 23 (Medium, new — I4: no attribution/audit mechanism for slot lease/release is named anywhere, undermining the falsifiability of the pool's own "bounded to `N` concurrently-live calls" exposure claim)

Finding 18's own accepted-residual argument (reaffirmed by finding 24's own re-verification below) is a
quantitative claim: exposure is "bounded to `N` slots' worth of concurrently-live... call data," not
unbounded. Nothing in the pool's design records which call/tool/session held which slot index, when, or for
how long — no log line, no counter, no `EffectContext`-visible attribution of a lease to the call that holds
it. This matters for two concrete reasons beyond ordinary observability nice-to-haves: (1) if a
backstop-layer leak or a finding-21-style wipe-quarantine event is ever actually suspected or observed in
production, there is no way, as designed, to reconstruct which OTHER call/session/tool shared the affected
slot recently enough to matter — the exact information CLAUDE.md's own I4 invariant ("every effect is
attributable") calls for and that this draft's own §4b elsewhere treats attribution as worth designing for
(e.g., the per-call `NativeJailBackend` instance existing specifically so one call's own state is never
confused with another's, finding 9's companion fix); (2) unlike ADR-041's own residual — a fixed, static,
non-sensitive OS file set that never changes and needs no per-instance attribution to reason about — this
draft's own residual is live, call-specific tenant data whose sensitivity is exactly why "which calls
shared this slot" is an operationally necessary question, not an optional diagnostic.

**Fix direction**: have `acquire()`/`ScratchSlotLease` record, at minimum, a lease-start timestamp and the
tool name/call id that acquired each slot index (in-memory, most-recent-N-events is sufficient for v1, no
new persistent-logging mechanism required) — real, low-cost bookkeeping distinct from anything findings
18-22 already ask for, but a genuine gap given what the "bounded exposure" argument is actually claiming.

### Finding 24 (High, new — the ADR-041 analogy, re-verified directly against ADR-041's own text, is textually accurate but rests on a residual of a materially different kind, and the fix leans on it more heavily than the difference supports)

**Investigation #6's own question, answered directly**: `decisions/ADR-041-appcontainer-ace-leak-accepted-
residual.md` was read in full this round, not trusted from the draft's own quotes. The quotes the fix text
uses ("the model does not claim the backstop is leak-free, only that mediation (layer 1) is what actually
has to hold, with the backstop catching what mediation itself doesn't reach") are verbatim and accurate —
this is not a misquote, and the citation is not fabricated the way the Aspose.Slides README trap this
draft's own methodology exists to catch would have been. **But the two residuals are not the same kind of
thing, on three load-bearing axes ADR-041 itself makes explicit:**

1. **Who controls the exposure.** ADR-041's leak is Windows' OWN default DACL on a curated OS file set
   (`win.ini`, `hosts`) — "independent of anything this backend grants or withholds" (ADR-041 §2 item 3,
   quoted verbatim) — AgentEngine's code never grants it and cannot avoid it short of a fundamentally
   different sandboxing technology. This draft's own residual is the OPPOSITE: `grant_rw_path_once()`
   granting all `N` slots is AgentEngine's OWN explicit, first-party design choice, made anew by this fix,
   entirely within this draft's own control to make narrower, differently-shaped, or not at all. "We
   inherited this from the OS and can't fix it" (ADR-041's actual justification for not building a new fix)
   is not available as a reason to accept a residual this draft's OWN new mechanism manufactures.
2. **What is exposed.** `win.ini`/`hosts` are static, non-secret, identical-on-every-Windows-install files
   with no application data, no per-tenant content, and no confidentiality value — reading them leaks
   nothing about any user, session, or document. The pool's `N` slots hold **live, current, per-call
   application data** — the actual content of whatever `.docx`/`.xlsx`/`.pptx` a caller submitted, from
   whichever session/tool most recently used that slot. A leak of the first is a curiosity; a leak of the
   second is exactly the cross-tenant confidentiality exposure I2 exists to prevent. ADR-041's own §5 "Not
   decided, explicitly out of scope" list does not claim to have settled anything about leaking
   caller-supplied data — only about two fixed OS files.
3. **What evidence backs "bounded."** ADR-041's acceptance is not argued by analogy or intent alone — it
   rests on an EXECUTED, currently-green, positive-control regression test
   (`tests/test_native_jail_abuse_corpus_windows.cpp`, Case 4) that proves, empirically, both that the leak
   exists AND that it does not reach beyond the documented `win.ini`/`hosts` set to an arbitrary secret the
   test itself plants. This draft's own fix text explicitly has NO equivalent test — it names one, in the
   same paragraph that invokes the ADR-041 precedent, as "real, implementation-ADR-level future work, named
   here rather than designed." Citing ADR-041 as the reason a bounded-not-eliminated residual is "an
   acceptable v1 posture" while simultaneously admitting the one thing that makes ADR-041's own residual
   actually verified-bounded (the test) has not been built yet is citing the CONCLUSION of a precedent
   without yet having done the WORK that precedent's own argument actually rests on.

**Net assessment**: this is not a fabricated or misquoted citation, and it is not as weak as the
Aspose.Slides README trap (round 2 finding 7's own re-verification target) — the mechanism-level parallel
(a documented, real, load-bearing OS-layer backstop that isn't required to be leak-free) is genuine and the
draft is right that ADR-041 establishes THAT principle for this codebase. But the draft's own text ("argued
from this codebase's own already-Judged precedent, not asserted") overstates how much weight the specific
analogy can bear: it is being used to justify accepting a NEW, self-inflicted, live-data exposure on the
strength of a precedent about an OLD, environment-inherited, non-sensitive one, without yet having built the
one piece of evidence (a positive-control test proving the new residual's own boundedness, the same way
ADR-041 proves its own) that would make the analogy actually load-bearing rather than rhetorical.

**Fix direction**: either (a) treat the positive-control test named in §8's own finding-18/19 residual list
as a precondition for shipping this design, not merely "real, valuable future work," specifically because
it is what would make the ADR-041 analogy actually hold rather than merely gesture at holding; or (b) if
shipping without it, restate the justification honestly as "argued by structural analogy to ADR-041's
accepted principle, pending equivalent empirical verification" rather than "argued from this codebase's own
already-Judged precedent" — a real, disclosed difference in how strong the justification currently is, the
same "don't oversell what's actually been checked" discipline this draft applies to every other claim in it.

### Investigated, no real defect found

- **Investigation #3, first half (does the pool serve all three tools from one shared pool, or one per
  tool)**: confirmed directly — one shared `extract_scratch_root()`/pool serves `ExtractDocxText`,
  `ExtractXlsxCells`, and `ExtractPptxText` alike (§4b's finding-9 fix text, unchanged by the pool
  redesign: "one root shared by all three tools rather than one per tool"; the pool's own fix text reuses
  the identical root). §6's own resource-cap accounting (`N` total grants) is stated correctly against this
  shared-pool shape — it never double-counts or under-counts by assuming three separate pools.
- **Investigation #3, second half (handout-before-previous-wipe-completes ordering)**: checked directly
  against the fix text's own sequencing — `acquire()` pops a free index UNDER THE MUTEX, then performs its
  own wipe BEFORE returning the lease to the caller, and the release-side destructor removes contents BEFORE
  pushing the index back onto the free-list (so no index is visible to a new acquirer until after the
  releasing call's own wipe attempt has already run). As sequenced, there is no window where a new lease is
  handed out before some wipe of the previous tenant's data has been attempted. The real gap here is not an
  ordering race — it is finding 21's own gap (what happens when that attempted wipe fails), not a
  handout-before-wipe-runs race.
- **Investigation #4 (composition with finding 6's capability-grant design under per-slot mounting)**: no
  defect found. `cap::FsRead{.mount_id="extract_scratch", .path_prefix="args.json"}`/
  `cap::FsWrite{.path_prefix="out.json"}` are, per finding 6's own already-established property, narrowed
  relative to WHATEVER host directory the `"extract_scratch"` mount currently resolves to — the grant never
  encodes a host path itself. This means the capability declaration/grant needs no per-slot awareness or
  re-derivation at all: the "host learns which slot a call holds" step is nothing more than the ordinary
  per-call config assignment `mount_roots["extract_scratch"] = lease.host_path()` that finding 9's own fix
  already performed for a freshly-generated directory, now performed for a leased slot's path instead — no
  new mechanism, no new capability-layer logic, and no re-derivation of the capability-grant call site's own
  logic is actually required. The draft's own "unaffected" claim for this composition (§4b, "Composition
  with finding 6's `path_prefix` narrowing, re-checked for the pool shape specifically") holds up under
  direct re-checking, not merely asserted.
- **Investigation #5 (is pool size `N` honestly disclosed as unmeasured, or asserted with unjustified
  precision)**: no defect found. The fix text's own number is explicitly hedged ("an illustrative starting
  point is the low tens," "a reasonable default, not designed further here") and §6 lists `N` and its
  acquire wait-ceiling among the draft's own explicitly-unsized caps — the same "mechanism resolved, number
  open" honesty this draft has applied consistently since Revision 2, not the over-precise, unjustified-number
  pattern this series corrected once already (the pre-correction lxml version-floor citation, round 1).

## Red-team round 7

Round 7's own brief: verify Revision 7's own fixes for round 6's findings 20-23 against the real code they
cite, rather than trusting the fix text's own prose a second time — plus check whether finding 24, left
explicitly open, is honestly tracked as such. Five real, new defects found, one of them (finding 25) as
serious as anything findings 18/20 found: the release-time half of finding 21's own wipe-failure fix is
real and correctly specified, but the ACQUIRE-time half — the handout path that determines what the
CURRENT caller actually receives — is never disambiguated, the same "assertion, not a mechanism" shape
this draft's own prior rounds (findings 9, 13, 19, 21 itself) have each found once already, now present
inside finding 21's own fix for finding 21. Finding 26 shows finding 20's fix is narrower than its own
summary sentence claims: the pool's wipe primitive, the actual basis for "provably empty," was never
switched off the bare `std::filesystem::remove_all` finding 20's whole investigation was about, even
though the adapter class the fix already adopted ships an equivalent, TOCTOU-safe `remove()` unused for
this purpose. Finding 27 shows the lease-attribution log (finding 23's fix) does not record the one field
that would let it actually do the cross-session reconstruction its own justifying prose claims. Finding 28
is a minor, repeated citation-accuracy slip in a namespace this revision cites four times. Finding 29 shows
finding 24 is honestly named as deferred in the revision-history prose, but is not tracked anywhere in §8's
own designed-for-this ledger, and the exact sentence finding 24 quoted and disputed is still standing,
verbatim, with no inline pointer to that dispute. Three of the prompt's own investigation lines (the
adapter's handle-based TOCTOU-closure, the adapter's standalone-construction cost, and the `wait_for`
predicate's own race-freedom) are checked directly against the real code and found sound — recorded at the
end, not restated as findings.

### Finding 25 (Critical, new — I2/I8: finding 21's own fix specifies the RELEASE-time wipe-failure outcome but never the ACQUIRE-time one — the path that determines whether the calling code receives a dirty slot)

**What finding 21's fix text actually says, quoted precisely**: "`acquire()` and the `ScratchSlotLease`
destructor both CHECK the `remove_all` result... and, additionally, verify emptiness afterward... On
either signal..., the slot is QUARANTINED... it is removed from the pool's free-list permanently... and
never handed out again." This sentence correctly answers the RELEASE-time case: a slot the destructor
fails to wipe is pulled from the free-list before the NEXT acquirer could ever reach it — investigation
into that half (round 6's own "Investigated, no real defect found," re-confirmed here) shows the handout
sequencing is real and correct for that direction.

**What it never answers**: `acquire()` itself pops a free index UNDER THE MUTEX (point 4, unchanged since
Revision 6), THEN wipes it, BEFORE returning control to the CALLER THAT IS WAITING ON THIS VERY `acquire()`
CALL. If that wipe fails — the exact failure mode finding 21 itself names as real (a transient lock from a
just-killed worker, or a compromised worker's own DACL-based poisoning of a file inside its slot) — the
slot is "removed from the free-list... and never handed out again," but that sentence describes what
happens to FUTURE callers, not what happens to THIS ONE, which already holds the popped index and is
sitting inside the very `acquire()` call that just discovered the problem. Three genuinely different
implementations are all consistent with the fix text as written, and the text never says which is
required:

1. **`acquire()` propagates a distinct failure to its caller** (not `office_extraction.scratch_pool_exhausted`,
   a different, not-yet-named error) and the tool call fails closed without a slot — the safe outcome, but
   never specified.
2. **`acquire()` quarantines this slot and internally retries** — pops another free index (or re-enters its
   own `wait_for` if none remains) and returns a DIFFERENT, actually-clean slot to the caller transparently
   — also safe, but a materially different implementation shape (recursion or a retry loop inside `acquire()`
   itself, with its own re-entrancy and wait-ceiling-accounting questions: does the retry get a fresh
   `wait_ceiling`, or must it share the original deadline? Not named either way) — never specified.
3. **`acquire()` returns the lease it already popped anyway**, because the sentence's own literal scope
   ("never handed out again") is naturally read as "not a SECOND time," not as "not even the FIRST time,
   right now, mid-handout" — the naive reading an implementer reaches for BECAUSE the index has already left
   the free-list and the function is already committed to constructing a `ScratchSlotLease` for it. This is
   the one outcome that defeats the entire point of findings 18-21: the calling code — and, through it,
   whichever session's `.docx`/`.xlsx`/`.pptx` extraction is running — receives a slot this draft's own
   design just proved is not provably empty.

**Why this is not a nitpick**: this is the exact "assertion, not a mechanism" gap rounds 3/4/5/6 each found
once already in this draft's own prior fixes (findings 9, 13, 19, and finding 21 itself, which this very
finding is now inside) — the fix text states an outcome ("never handed out") without naming the concrete
control-flow that produces it for the one call path (acquire-time) where getting it wrong is worst. Nothing
elsewhere in §4b, §6, or §8 disambiguates this; §8's own "Resolved by Revision 7" bullet for finding 21
repeats the same "removed from the free-list... never handed out again" phrasing without adding the missing
half either.

**Fix direction**: state explicitly that a wipe failure discovered DURING `acquire()`'s own handout path
(before the lease is constructed/returned) causes `acquire()` to quarantine that index and retry against the
free-list (option 2 above, the one that preserves both safety and the caller's own experience of "I asked
for a slot and got one, eventually, within the wait ceiling") rather than either failing the caller's whole
call for a problem it didn't cause (option 1) or — the design's real, current silent risk — handing back the
very slot just proven unsafe (option 3). If option 2 is chosen, name whether the retry consumes the original
`wait_ceiling` budget or is granted a fresh one, the same "don't leave a control-flow detail to an
implementer's own judgment" standard finding 22's own fix already met for the pool's wait idiom.

### Finding 26 (Medium-High, new — I2: finding 20's fix does not actually cover the wipe, the operation finding 21's own "provably-empty" guarantee is built on, even though the adapter it adopted already ships a safe equivalent for it)

**The claim, quoted**: finding 20's fix states "every read/write against a slot's contents — `args.json`
(write), the source document's bytes (write...), and `out.json` (read) — goes through
`agentengine::native_jail::MediatedFileSystemAdapter`... never a bare `std::ifstream`/`std::ofstream`." This
list is accurate as far as it goes, but the acquire-time and release-time WIPE — `std::filesystem::remove_all`
over the slot's contents, point 4 of the pool's own fix text, unchanged since Revision 6 — is not a
read or a write, so it is not covered by this sentence's own literal scope, and it is not switched to the
adapter anywhere in Revision 7's text. Only the POST-wipe verification (`list_directory("")`, finding 21's
own new addition) goes through the adapter; the deletion itself still goes through the same class of bare,
path-based, non-handle-anchored primitive finding 20's entire investigation determined is unsafe for exactly
this threat model (a compromised worker with read-write OS authority over its own slot, finding 4/9's own
grant).

**Why this matters given what's already in this file, not merely in the abstract**: `MediatedFileSystemAdapter`
already ships a `remove(path, recursive)` method (verified directly,
`mediated_filesystem_adapter.cpp:116-197`) built on the identical open-then-verify-by-handle discipline as
`read_file`/`write_file` — including its own iterative, TOCTOU-safe post-order recursive-deletion algorithm
(a `std::vector<std::string>` stack of mount-relative paths, each child re-opened and re-verified by handle
before its type is trusted, explicitly built, per that file's own comment, to close "the exact same structural
hazard ADR-103's own MUST-FIX closed for the Linux sibling's `usage()`"). Routing the wipe through
`adapter.remove("", /*recursive=*/true)` instead of a bare `std::filesystem::remove_all` call needs no new
code in this codebase — the safe primitive is already sitting in the exact same class this fix already
adopted for the other three operations, unused for the fourth. This is not a demonstrated live escape this
pass: `std::filesystem::remove_all`'s default `recursive_directory_iterator` behavior does not follow
directory symlinks/junctions, so a worker-planted reparse point inside its own slot is deleted as a link, not
traversed into and used to reach content outside the slot — the worst-case "recursive deletion of arbitrary
host content" escalation is unlikely under the default options this draft doesn't say it overrides. What is
real, not speculative: the design's own core safety claim (finding 21's "provably empty") now rests on two
DIFFERENT primitives that were never checked against each other for consistency — a bare deletion, followed
by a mediated verification — rather than one mediated operation end to end, in a section of this draft whose
own methodology is "reuse what's already proven safe, don't leave a newer mechanism half-migrated."

**Fix direction**: route the wipe itself through `MediatedFileSystemAdapter::remove("", true)`, the same
adapter instance finding 20's fix already constructs per lease, rather than a separate
`std::filesystem::remove_all` call — one mediated primitive for every operation the slot's contents go
through (open, read, write, list, AND remove), closing the gap between the two halves of the fix text
rather than leaving read/write mediated and delete unmediated.

### Finding 27 (Medium-High, new — I4: the lease-attribution ring log records `call_id`, which is round-scoped and not guaranteed cross-session-unique, and never records the run/session identity finding 18/23's own justifying prose relies on being reconstructable)

**What finding 23's fix actually records, quoted**: "the slot index, a lease-start timestamp, the acquiring
tool name and call id... and, on release, an end timestamp plus... clean release... or quarantined." Checked
directly against `EffectContext` (`include/agentengine/core/effect_context.hpp:37-212`) and
`ToolCallHookContext` (`include/agentengine/core/tool_call_hook.hpp:49-57`, its own header comment: "`call_id`
correlates a decision back to one call in a round that may... carry several parallel `ToolCall`s"): `call_id`
is scoped to identify one call WITHIN one round of one session — nothing in `tool_call_hook.hpp` or
`tool_pipeline.hpp` (`call_id` declared as a plain `std::string` at both `tool_pipeline.hpp:263` and `:386`)
states or implies any cross-session or cross-run namespacing of this string. `EffectContext` separately
carries `run_id` (`effect_context.hpp:65`, "real 001 §1/§2 Run/Turn identity") and `principal.id`
(`principal.hpp:28`, "stable identity") at the exact call site finding 9's own companion fix already threads
a call-local `NativeJailBackend` through — neither is named among the fields finding 23's fix records.

**Why this defeats the log's own stated purpose, not merely narrows it**: finding 23's own justifying text
says the log is "what lets a host reconstruct which other call/session/tool shared the affected slot
recently enough to matter" — directly answering finding 18's own scenario, which is explicitly cross-session
("live, possibly different-session, different-tool data"). But if two DIFFERENT sessions' rounds happen to
produce calls carrying the same `call_id` string — plausible, since nothing in this codebase namespaces
`call_id` by session and the field's own header comment describes its scope as "one round," not "one
deployment" — the log's two entries for those two calls are, by the fields finding 23 actually specifies,
indistinguishable by session. A host investigating a suspected backstop leak or a finding-25/26-style dirty
slot could correctly identify "the tool was `ExtractDocxText`, the call id was `call_3`" and still be unable
to say which of potentially several sessions that call id belongs to, undermining the exact reconstruction
finding 18/23 name as the reason this log exists.

**Fix direction**: add `run_id` and `principal.id` to the recorded tuple — both already live on the same
`EffectContext&` available at the `acquire()` call site, no new plumbing required, the same "reuse what's
already threaded through" discipline this draft applies to every other fix in §4b.

### Finding 28 (Low-Medium, new — citation accuracy: `MediatedFileSystemAdapter`'s real namespace is misquoted, repeatedly, in the one revision whose entire contribution is verifying this class directly)

Checked directly against `mediated_filesystem_adapter.hpp:36-59`: the class is
`agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter` — inside a nested `mediated_shell`
namespace, not directly inside `agentengine::native_jail`. Revision 7's own new text cites it as
`agentengine::native_jail::MediatedFileSystemAdapter` (§1's new bullet, and twice in §4b's fix for finding
20) — omitting the `mediated_shell` component every time it appears this revision. This has no design
consequence (the class is the right one; an implementer will find it regardless), but this draft's own
standing methodology is "verified directly against the actual code, not assumed" — a repeated,
un-self-caught namespace slip in the one class this exact revision's headline investigation is built around
is worth a plain correction, the same standard this draft has held every other citation to since round 2's
own DuckX/Aspose re-verification.

**Fix direction**: correct all three citations to `agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter`.

### Finding 29 (Medium-High, new — process integrity: finding 24 is honestly described as deferred in the revision-history prose, but is untracked in §8's own designed-for-this ledger, and the specific sentence finding 24 disputed is still standing, unmarked)

**What §8 actually contains, checked directly**: every other numbered finding from rounds 1-6 gets an
explicit bullet, by number, in either a "Resolved by Revision N" list or the "Still genuinely open" list —
this is the pattern §8 has followed since Revision 2. Finding 24 gets neither. It is not in "Resolved by
Revision 7" (correct — it wasn't resolved), but it is also absent from "Still genuinely open," the section
whose entire purpose is enumerating exactly this kind of item. The only places finding 24 is named as
currently unresolved are the top-of-file revision-history paragraph ("Finding 24... is not addressed this
pass; it remains open, per its own round-6 text") and the round-6 section itself (historical record, per
this draft's own discipline, never edited by a later revision). Neither is the ledger a future reader
checking "what does this draft still owe" would consult first.

**Compounding this**: finding 24's own text quotes and disputes one specific sentence — "argued from this
codebase's own already-Judged precedent, not asserted" — as overstating how much weight the ADR-041 analogy
can bear absent the positive-control test ADR-041's own acceptance actually rests on. Checked directly
against §4b's current text (the paragraph beginning "Why a bounded-not-eliminated residual is judged an
acceptable v1 posture"): that exact sentence is still present, verbatim, unchanged by Revision 7, with no
"Correction (Revision 7, round 6 finding 24)" marker of the kind every other disputed sentence in this
draft's history gets the moment a later revision leaves it standing on purpose (compare finding 18's own
superseded shared-parent-grant text, or finding 9's superseded scratch-directory-naming text, both of which
carry an explicit forward-pointer to where they were revisited). A reader who reaches this sentence in §4b
without having separately read round 6 in full has no signal, at that point in the document, that the claim
it makes is disputed and still unresolved.

**Why this is worth flagging now rather than waiting for a future revision to notice**: CLAUDE.md's own
standard for this project is that "a security claim needs positive controls — a test that cannot fail
proves nothing," and finding 24 is precisely a claim that this draft's most novel, most-red-teamed mechanism
(the pool) satisfies a Judged precedent's own bar without yet having built the one thing that precedent's
acceptance rests on. Leaving that specific, disputed claim untracked in §8 and unmarked at its source is a
real, structural risk that it gets carried into an implementation ADR unchallenged — not because anyone
decided it was fine, but because nothing in the document's own tracking apparatus surfaces it at either of
the two places (the open-items list, or the sentence itself) a future implementer is likeliest to actually
look.

**Fix direction**: add finding 24 to §8's "Still genuinely open" list with its own bullet (distinct from the
finding-18/19 residual bullet that currently carries the closest related item, the positive-control test
itself, without naming finding 24), and add an inline "Correction/Note (Revision 7, round 6 finding 24):
this framing is disputed, not yet resolved" marker at the disputed sentence itself in §4b — this draft's own
established discipline for every other sentence a later round leaves standing on purpose.

### Investigated, no real defect found

- **Investigation (finding 20's own TOCTOU-within-the-adapter question — does the adapter hold a validated
  handle across the whole operation, or re-resolve the path between validation and use?)**: no defect found.
  Checked directly against `mediated_filesystem_adapter.cpp`'s `read_file`/`write_file`
  (`:60-100`): both call `open_within_mount_root` exactly once to obtain a single `SafeFileHandle`, then
  perform every subsequent operation (`GetFileSizeEx`, `ReadFile`/`WriteFile`, in a loop for large files) on
  that SAME handle — never re-opening or re-resolving the path. Once Windows hands back an open handle to the
  real file object, a worker's later rename/symlink/reparse-point swap of the PATH does not redirect
  operations already bound to that handle — the window finding 20's own investigation asked this round to
  check (a race between validate and use INSIDE the adapter, distinct from the validate-at-path-resolution
  window `open_within_mount_root` itself already closes) does not exist for `read_file`/`write_file` as
  implemented.
- **Investigation (does `MediatedFileSystemAdapter` require a `Mount`/`WorktreeObjectStore` context the
  scratch-slot pool doesn't have, the same heavy machinery round 7's own top-of-file note judged too costly
  for `artifacts`?)**: no defect found. Checked directly against `mediated_filesystem_adapter.hpp:36-57`:
  `create()` takes a single `std::filesystem::path root` and the private constructor stores only that path —
  no `Mount`, `Ref`, or `WorktreeObjectStore` parameter anywhere in the class's real, shipped shape. The
  fix's own claim that this adapter is "usable standalone, against any host directory, with NO worktree/Ref/
  object-store dependency at all" holds up under direct re-checking of the actual constructor, not merely
  asserted from the header's prose.
- **Investigation (finding 22's own `wait_for` fix — does a thread's predicate becoming true and waking it
  race against another thread grabbing the slot first, after wakeup but before the woken thread acts?)**: no
  defect found, GIVEN the fix is implemented as specified. `cv.wait_for(lock, wait_ceiling, predicate)`
  re-checks its predicate under the SAME lock it holds on return (whether returning due to a real notify, a
  spurious wakeup, or a timeout) — the calling code pops the free-list entry inside that same critical
  section, before releasing the lock, so no other thread can observe or mutate `free_list` between the
  predicate's last true check and this thread's own pop. The design text's own description ("the pop and the
  timeout decision share one critical section") states this correctly; the only residual is the same one
  finding 22 already disclosed (unfair-but-bounded wakeup order among multiple waiters), not a fresh
  correctness gap in the primitive itself.

## Prove pass 1 — evaluating path forward after round 7

**Method.** This pass did not run a further red-team round against the pool. It read the pool's own real
dependency, `AppContainerProfile::grant_path()` (`src/backends/native_jail/app_container_profile.cpp:152-205`),
and `grant_ro_deduped()` (`native_jail_backend.cpp:134-145`) verbatim, off disk, to answer a question every
prior round asserted rather than checked: is a real `revoke_path()` actually the "unproven Windows-ACL-surgery
surface" findings 14 and 18 twice declined to build (parenthetical: "Investigated and rejected... a genuine
per-call grant with revocation," and finding 14's own fix-direction option (a)), or is it a small, already-
precedented addition this draft talked itself out of for a reason that does not survive reading the code it
was reasoning about? The second turned out to be the case, and reading `grant_path()` closely enough to answer
that question surfaced a second, larger fact this whole pool lineage (findings 14, 18, 19, 20, 21, 22, 23, 25,
26, 27) never checked: what a directory deletion actually does to that directory's own DACL. That fact changes
the recommendation from "which of the three named paths" to a fourth, evidence-derived one.

### Is `revoke_path()` actually unproven? No -- it is the same three Win32 calls `grant_path()` already ships, with one enum flipped

`grant_path()`'s real body (`app_container_profile.cpp:152-205`) is exactly three Win32 calls:
`GetNamedSecurityInfoW` reads the path's current DACL, `SetEntriesInAclW` merges in one `EXPLICIT_ACCESSW`
entry with `ea.grfAccessMode = GRANT_ACCESS`, and `SetNamedSecurityInfoW` writes the merged ACL back. This is
the `aclapi.h` `SetEntriesInAclW`/`ACCESS_MODE` family Microsoft documents specifically for editing an existing
ACL without hand-walking `GetAce`/`DeleteAce`/`AddAce` -- and that same enum has a `REVOKE_ACCESS` member,
documented to strip every existing explicit-access ACE for the named trustee from the ACL `SetEntriesInAclW`
returns, regardless of the permission mask supplied. A `revoke_path()` sibling is therefore:

```cpp
result<void> AppContainerProfile::revoke_path(std::wstring const& path) const {
    // identical body to grant_path() above, down to the GetNamedSecurityInfoW/SetNamedSecurityInfoW pair,
    // except ea.grfAccessMode = REVOKE_ACCESS and ea.grfAccessPermissions left 0 (ignored for a revoke).
}
```

-- same header (`app_container_profile.hpp`), same three-call shape, same error-handling pattern, ~15 lines.
This is not a different API family from what `grant_path()` already ships and this codebase's own CI already
exercises (`test_native_jail_teardown_cycles_windows.cpp` covers `grant_path`); it is the same function's
counterpart enum value. This draft's own repeated claim -- "real, new, unproven Windows-ACL-surgery surface"
(SS4b, in both finding 14's and finding 18's fix text) -- does not survive being checked against the actual
`aclapi.h` surface `grant_path()` already calls: nothing about `REVOKE_ACCESS` is unproven relative to
`GRANT_ACCESS`; both are the same documented enum, exercised through the same three functions, on the same
file objects. Findings 14 and 18 rejected `revoke_path()` on a premise (real-but-unproven new surface) that
does not hold up against the function they were both citing directly.

### Does a real `revoke_path()` reintroduce the `instances_`-shaped race? Not for this draft's own per-call-unique paths, and here is why concretely

`grant_path()`/`revoke_path()` hold no shared mutable state of their own -- no static, no member bookkeeping,
just a read-modify-write against the ONE named `path`'s own security descriptor. A race exists only if two
calls target the SAME `path` concurrently (the `instances_` shape: shared mutable state, unsynchronized
writers). `grant_ro_deduped()`'s own `granted_mutex`/`granted` set (`native_jail_backend.cpp:136-137`) already
serializes the one place in this codebase that repeats a path across calls -- deployment-fixed paths
(`python_home` etc.), granted at most once, guarded correctly today. The pool's own `spec.mounts` loop
(`:612`) is NOT behind that mutex and never has been -- and that is safe today only because finding 9's own
`unique_scratch_dir_name()` (pid + a verified-thread-safe `std::atomic` counter, `extract_pdf_text.hpp:262-271`'s
pattern) guarantees every call's own `host_path` is a distinct string. Two concurrent calls therefore never
call `grant_path()` on the same object, so there is nothing for a read-modify-write race to clobber -- **this
is already true of the shipped code today**, not a new property a `revoke_path()` would have to establish.
The same guarantee extends to `revoke_path()` for free: as long as revoke (like grant) is only ever called on
a call's own unique leaf path -- never on a path two calls could simultaneously hold -- no new mutex is needed,
and none of this draft's own existing per-call-uniqueness machinery has to change to make that true. (The
external-process caveat the prove-pass brief raised -- some OTHER process mutating the same DACL concurrently
-- does not apply here either: nothing outside this codebase's own scratch mechanism has any reason to touch a
directory this codebase generates, grants, and deletes entirely within one call's lifetime; this is a
materially different situation from ADR-041's `win.ini`/`hosts`, which are genuinely OS-owned, externally
mutable objects.)

**So Path 2 as literally posed -- build `revoke_path()`, keep the pool, revoke a slot's grant instead of
leaving it standing -- is real and buildable**: small (one new function, same file, same pattern as
`grant_path()`), needs no new synchronization, and needs no change to `AppContainerProfile`'s object lifecycle
or `NativeJailBackend`'s cleanup ordering beyond calling it once more per slot release. That said, building it
this way (revoke, but keep the slot object alive and in the pool) is not actually the strongest design
available -- see below.

### The fact that reframes findings 14-27: deleting a directory deletes its DACL, because the DACL lives ON that directory, not on the profile

`grant_path()`'s own body is unambiguous about WHERE the ACE it adds lives: `GetNamedSecurityInfoW(path, ...)`
reads, and `SetNamedSecurityInfoW(path, ...)` writes, `path`'s own `DACL_SECURITY_INFORMATION` -- an ordinary
NTFS security descriptor attached to that one filesystem object's own MFT record. There is no
"`AppContainerProfile`-wide DACL" for grants to accumulate on -- finding 14's own opening sentence ("permanently
and irrevocably grows the shared `AppContainerProfile`'s Windows ACL") names a structure that does not exist in
the code it cites; what actually grows is the *number of distinct filesystem objects* carrying that SID's ACE.
That distinction matters because of ordinary, uncontroversial NTFS semantics this draft never checked against
this specific question: deleting a filesystem object (`RemoveDirectory`, which is what `std::filesystem::remove_all`
calls) deallocates its MFT record -- security descriptor included. **An ACE cannot outlive the object it was
granted on.** Finding 15's own fix -- recursive removal of the per-call directory, unconditionally, on both the
success and failure path, already designed and already adopted this revision (SS4b, "Fix for finding 15") --
therefore already destroys the very ACE finding 14 worried about, *as a structural side effect of deleting the
object*, with no additional code and no new primitive, **provided the grant lands on the call's own leaf
directory directly (finding 9's original shape) rather than on a shared root that is never deleted.**

Finding 14's own fix chose the opposite of that: it pushed the grant onto `extract_scratch_root()` -- a shared
directory that (unlike a per-call leaf) is *never* deleted -- specifically so a single dedup-guarded grant could
cover every future call via inheritance, avoiding a `grant_path()` call per call. That choice is what MANUFACTURED
finding 18's own standing, cross-call, cross-session, whole-process-lifetime backstop exposure: the root's
inheritable ACE has no object-deletion event to ever expire it, because the root itself never goes away. Finding
18's fix (the N-slot pool) then had to build an entire new concurrency/lease/wipe/quarantine/attribution
mechanism (findings 18-23, then 25-27 patching that same mechanism) to bound an exposure that finding 14's own
fix chose to create. Finding 15's cleanup -- sitting right next to finding 14's fix in the very same revision --
already contained the actual fix to finding 14's concern, and neither Revision 5 nor any later round checked
whether it did. This is a real, checkable gap in this draft's own investigation, not a difference of opinion:
finding 15's own text explicitly calls the ACL leak "a genuinely separate resource" from the disk-space leak
cleanup addresses -- that premise is what this pass's direct reading of `grant_path()` shows is false. It is the
same object's same security descriptor.

### A fourth path: grant the call's own leaf directly, delete it when the call ends, build nothing new

Concretely, this reverts SS4b to finding 9's original per-call-fresh-directory shape (`unique_scratch_dir_name()`,
unchanged, already thread-safe) and keeps finding 15's cleanup (unconditional `remove_all` on both the success
and failure path, RAII-scoped, unchanged) -- but **drops finding 14's shared-inheritable-root fix and the entire
N-slot pool findings 18-27 built on top of it.** Each call: creates its own uniquely-named leaf directory under
`extract_scratch_root()`; grants it directly via the existing `grant_path()` (no dedup needed -- the path is used
by exactly one call, ever, so there is nothing to dedup); runs; and deletes the directory unconditionally when
done (finding 15's own already-designed step). No new Win32 surface (not even `revoke_path()` -- deletion already
does that job as an intrinsic NTFS property, not a new mechanism this draft would have to build or trust for the
first time). No new mutex, condition variable, free-list, wipe-before/after step, quarantine state, or lease
ring-log -- the entire genuinely-new concurrency/security primitive rounds 6-7 spent ten findings (18, 19, 21,
22, 23, 24, 25, 26, 27, and 20 to the extent it touches pool-adjacent reads) hardening is not built at all,
because there is no reusable resource left to protect: every directory is single-use and self-destructs.

This is not merely "as good as" the pool on finding 18's own axis -- it is **strictly tighter**. The pool
commits to `N` slots granted for the whole process's lifetime, occupied or not; a compromised worker can reach
any of those `N` slots' current contents at any time the process is running, load or no load. A per-call leaf
exists, and carries a live grant, only for the duration of the one call that owns it -- the backstop-reachable
set shrinks to exactly the calls genuinely in flight right now, and to zero when the host is idle. The only
residual this reintroduces is the one finding 15 already named and already accepted as a v1-acceptable residual:
a crash that skips the RAII cleanup leaves one directory (and its one ACE) stranded until reaped, mitigated by
finding 15's own fail-closed `exists()` check before `create_directories()`. That residual is strictly narrower
than anything the pool's own quarantine machinery (findings 21/25/26) was built to bound, and it is already
designed, in this draft, today.

**What this closes, concretely, by elimination rather than by fix:**
- **Finding 14** -- closed structurally (deletion destroys the ACE; no accumulation, bounded by concurrently-live
  calls, not "forever").
- **Finding 18** -- closed more tightly than the pool's own fix claims to close it (exposure window shrinks to
  per-call, not per-`N`-slots-forever); the ADR-041 analogy finding 24 challenged is no longer load-bearing,
  because there is no longer a standing residual that needs an accepted-precedent argument to justify.
- **Findings 19, 21, 22, 23, 25, 26, 27** -- moot. Each is a defect in the pool's own lease/wipe/wait/attribution
  machinery; none of that machinery exists in this design.
- **Finding 24, 29** -- moot for the same reason (nothing to bound by analogy, nothing to leave untracked).
- **Finding 20** -- NOT closed by this change, and should not be dropped: it concerns the `out.json` READ channel
  (a worker-writable file the host trusts back), which is orthogonal to whether the directory that file lives in
  came from a pool or a per-call grant. Finding 20's fix (`MediatedFileSystemAdapter`/`open_within_mount_root`
  mediation instead of a bare `std::ifstream`) should be kept in this design exactly as designed -- it is the one
  real, still-necessary red-team output from rounds 6-7 that survives this reframing untouched, and it composes
  identically: construct the adapter against the call's own leaf directory instead of a leased slot's directory,
  same `create(lease.host_path())`-shaped call, same TOCTOU-safe primitive underneath.
- **Finding 15's own crash/pid-reuse residual** -- carries forward unchanged, already-accepted, already-designed.

This is offered as a genuinely different, better-evidenced fourth path, not a repackaging of Path 2: it needs
*less* new engineering than Path 2 (no `revoke_path()` to write, test, or reason about at all -- reuses
`grant_path()` and `std::filesystem::remove_all`, both already shipped and already exercised), and it produces a
*smaller* residual than Path 3 was ever going to be asked to accept. It should still get its own, real red-team
round before being adopted -- in particular, verifying empirically (a positive-control test, the same discipline
finding 24 asked the pool for) that a deleted-and-recreated directory under `extract_scratch_root()` truly never
carries over a stale ACE from a same-named predecessor, and that AppContainer-token filesystem checks are
actually re-evaluated per open rather than cached per-handle across a directory's deletion/recreation -- both
believed true from ordinary NTFS/AppContainer semantics and from `grant_path()`'s own code, but neither
literally executed and observed by this pass, the same "argued, not yet measured" gap finding 24 flagged
against the pool's own ADR-041 analogy, now flagged against this pass's own conclusion with the same honesty.

### Path 1 (keep patching the slot pool) -- evidence against convergence, not vibes

The finding-type sequence inside the pool's own lineage (14, 18, 19, then 20, 21, 22, 23, then 25, 26, 27) is
not independent, occasionally-recurring gaps -- it is one gap recurring in a fixed shape: **each round's fix
for the pool states an outcome in prose without naming the concrete control-flow that produces it, and the next
round finds a case the prose doesn't cover.** Findings 9, 13, 19, and 21 are each explicitly named, by the
draft's own later rounds, as instances of this exact "assertion, not a mechanism" pattern -- and finding 25
(round 7) is explicitly the SAME pattern found a fifth time, this time inside finding 21's own fix for finding
21, one round after finding 21 itself was fixed. Three of round 7's five findings (25, 26, 27) are new bugs in
round 6's own FIXES for round 6's findings, not fresh gaps in old, unreviewed prose -- meaning the round-over-
round trend for the pool specifically is not narrowing, it is finding fresh defects in the immediately-prior
round's repairs at roughly the same rate every round has since the pool was introduced (round 5: 2 findings
introducing the pool; round 6: 4 findings in the pool's first adversarial pass; round 7: 4 of 5 findings inside
round 6's OWN fixes). That is the signature of a hand-rolled concurrency-plus-security primitive whose surface
area (mutex + condition variable + free-list + two-sided wipe + quarantine + attribution log, all novel to this
draft) is large enough that each close reading finds a new seam, not a primitive converging toward soundness.
Track B's own six rounds, by contrast (protocol framing, resource caps, worker-lifecycle correctness), each
closed a structurally different kind of gap using already-proven primitives (`unique_scratch_filename()`'s
atomic counter, ordinary pipe-mediated `stdout_text`) -- the comparison this draft's own round 7 asked for is
not fair to extend to the pool: Track B was getting USE of existing mechanisms right; the pool is a NEW
mechanism, and this project's own CLAUDE.md standard ("a hot path without a bench... is not done," "a test that
cannot fail proves nothing") is exactly calibrated to expect a novel security-relevant concurrency primitive to
take materially more rounds than a checklist of already-proven pieces used correctly. **Verdict: Path 1 is not
close to converging in 1-2 more rounds on its own evidence, and the pool problem is categorically, not just
anecdotally, harder than Track B's.** This is moot in practice once the fourth path above removes the pool
entirely, but it directly answers the question asked: continuing to patch the pool, rather than removing it, is
the weakest of the three originally named options on the pool's own round-over-round record.

### Path 3 (accept the pool's residual per ADR-070) -- does not fit, on its own terms, before even reaching the residual

Checked directly: `ADR-070-host-configurable-responsibility-boundary.md` is never cited anywhere in this draft,
and for a structural reason, not an oversight -- this pass re-read ADR-070 in full. Its "Delegated Decision
Seam" (SS4) is a named shape for a *host-facing, explicit-opt-in decision function* that narrows or decides
among authority the host's own session already possesses (`PolicyDecider`, `ApprovalDecider`, `SandboxBackend`
are its real instances). The office-extraction pool's backstop residual is not that shape at all: there is no
host-facing seam, no opt-in decision function, and no responsibility being offered to a consumer dev to accept
or decline -- it is a purely engine-internal implementation residual on a backstop isolation layer. The draft's
own actual, correctly-chosen precedent is `ADR-041-appcontainer-ace-leak-accepted-residual.md` (cited by name,
SS4b, "Why a bounded-not-eliminated residual is judged an acceptable v1 posture") -- and finding 24 (round 6)
already re-verified that citation directly against ADR-041's own text and found it textually accurate but
resting on a residual of a *materially different kind* (self-inflicted vs. OS-inherited; live per-tenant
document data vs. two static, non-secret OS files; argued-by-analogy vs. proven-by-an-executed positive-control
test). Finding 24's own fix direction -- either (a) treat a positive-control test analogous to
`test_native_jail_abuse_corpus_windows.cpp` Case 4 as a shipping precondition, or (b) restate the justification
honestly as "pending equivalent empirical verification" -- is still, per finding 29, untracked in SS8 and
unresolved at its source sentence in SS4b as of this pass. **Path 3, evaluated on the pool as designed, is not
currently available**: the correct doctrine for it (ADR-041, not ADR-070) has an explicit evidentiary bar this
draft's own round 6 found unmet, and finding 25 (a still-open Critical acquire-time ambiguity that can hand a
caller a not-provably-empty slot) is exactly the kind of spec-clarity gap that has to close before ANY "ship
this as designed" judgment could be asked for, independent of which path is chosen -- a residual can only be
judged acceptable once the mechanism producing it is fully specified, and finding 25 shows the pool's acquire
path currently is not. Both objections evaporate under the fourth path above (no residual needing ADR-041-style
justification, no acquire-time ambiguity to resolve, because there is no pool), which is a further, independent
point in its favor.

### Recommendation

**Adopt the fourth path**: drop the N-slot pool and finding 14's shared-inheritable-root design; revert to
finding 9's original per-call-fresh-directory shape, grant each call's own leaf directly with the
already-shipped `grant_path()`, and rely on finding 15's already-designed unconditional cleanup
(`remove_all`, both paths, RAII) to retire the grant as a structural side effect of deleting the object it was
made on. Keep finding 20's fix (route `args.json`/`out.json`/the source document through
`agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter` instead of a bare `std::ifstream`/
`std::ofstream`) unchanged -- it is orthogonal to the pool-vs-per-call question and remains necessary either
way. Do not build a new `AppContainerProfile::revoke_path()` -- it is real and small (confirmed: the same
`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW` triple `grant_path()` already ships, with
`grfAccessMode = REVOKE_ACCESS` instead of `GRANT_ACCESS`), but it would be solving a problem deletion already
solves for free.

**The single strongest piece of evidence**: `grant_path()`'s own body (`app_container_profile.cpp:172-195`)
proves the ACE it adds is part of `path`'s own security descriptor, not some separate, profile-wide ledger --
so finding 15's cleanup, already designed and already adopted this revision to close a disk-space leak, already
closes finding 14's ACL-growth concern too, as an intrinsic property of NTFS object deletion, with zero new
code. Finding 14's own fix (push the grant onto a never-deleted shared root) is what manufactured finding 18's
standing exposure and the ten-finding pool-hardening lineage built to bound it (findings 18, 19, 21, 22, 23, 24,
25, 26, 27, plus 20 to the extent it touches pool-adjacent reads) -- all of it addressing a problem this draft's
own already-adopted fix for a DIFFERENT finding had already solved, one section earlier in the same file,
unrecognized until this pass read `grant_path()`'s actual body against that question directly.

**What is not yet proven, named honestly**: this recommendation is argued from `grant_path()`'s real code and
ordinary, well-established NTFS/AppContainer semantics, not from an executed test -- the same "argued, not yet
measured" gap this pass's own reasoning has in common with the very over-claim finding 24 caught in the pool's
ADR-041 citation. Before this fourth path is adopted as the design, it should get one real red-team round of
its own (checking, at minimum: does a freshly-recreated directory at a reused name ever inherit a predecessor's
ACE via any path other than `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` from a still-live ancestor grant -- it
should not, since the per-call leaf itself is granted directly, not inheriting from a granted root; and does a
worker's open handle into its own slot, opened before a raced deletion, retain access after the object is
unlinked -- ordinary NTFS answers yes transiently and harmlessly, since the handle closes with the worker
process at call end, but this pass did not execute anything to confirm it for this codebase specifically) --
the same discipline this draft has applied to every mechanism in it since Revision 2, now due once more, on a
smaller, more clearly favorable design than the one seven rounds already spent on.

## Red-team round 8

Round 8's own brief: this is the "one real red-team round" Prove pass 1 itself named as still owed before
its own collapse could be trusted — verify the central factual claim independently (not by re-reading Prove
pass 1's own citation a second time), check whether `grant_path()` called directly carries any precondition
or shared-state hazard the eliminated dedup wrapper was quietly providing, check whether the collapse traded
away a resource-exhaustion property the pool was incidentally also solving, re-verify finding 20's
composition against a fresh-per-call directory rather than assuming it, and spot-check whether eliminating
findings 18-27's own MACHINERY also eliminated their underlying CONCERNS, or only the specific shape those
concerns took inside the pool. Three real, new findings — none of them reopens the central collapse, but two
of them (30, 31) are real gaps in what "finding 15's cleanup already closes finding 14 fully" actually needs
to be true, that neither Revision 8's text nor Prove pass 1's own honesty section named.

**Investigation #1 (the central factual claim) — independently re-verified, holds up.** Read
`app_container_profile.hpp:27-55` and `app_container_profile.cpp:152-205` directly, not from Prove pass 1's
own citation. `AppContainerProfile` has exactly one data member, `PSID sid_` — no `granted_paths_` list, no
map, no ledger of any kind tracking what has been granted to what. `grant_path()`'s real body is exactly the
three Win32 calls Prove pass 1 described: `GetNamedSecurityInfoW(path, ..., DACL_SECURITY_INFORMATION, ...,
&existing_dacl, ...)` reads, `SetEntriesInAclW` merges, `SetNamedSecurityInfoW(path, ..., new_dacl, ...)`
writes back — every one of these three calls targets `path`'s own `DACL_SECURITY_INFORMATION`, never any
`AppContainerProfile`-owned structure. There is no other persistent state this call touches. Prove pass 1's
central claim is correct: the ACE lives on the granted path's own NTFS security descriptor, and deleting that
object destroys it as a structural, not merely likely, consequence.

### Finding 30 (Medium, new — I8: the per-call `grant_path()`/`create_directories()`/`remove_all()` cycle's own OS-call cost is a real, unmeasured, and — unlike every other unsized cap in this draft — undisclosed resource-consumption axis, one the eliminated pool was incidentally also amortizing)

§6 already discloses, honestly, that the CPython-interpreter-plus-package-import startup cost is unmeasured
for all three tools, and that XLSX-only batches now pay it where they didn't before (round 4 finding 16).
Investigation #3's brief asked whether the direct-grant design has its OWN resource-exhaustion axis the pool
was ALSO accidentally bounding — checked directly, it does, and it is not named anywhere in §6's cap list or
§8's residual ledger. Each call now performs, in sequence: `create_directories()` (an NTFS MFT-record
allocation), `grant_path()` (a `GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW` read-modify-
write cycle against that object's own security descriptor — three kernel round-trips, not one), and, at call
end, `remove_all()` (MFT-record deallocation). The eliminated pool paid this exact sequence exactly `N` times,
ONCE, at pool-initialization — every one of the (unboundedly many, over a long-running host's lifetime) calls
after that reused an already-granted slot with zero further `SetNamedSecurityInfoW` calls. The direct-grant
design pays it on **every single call**, forever — a real, structural cost trade the collapse makes in
exchange for the simplicity and the tighter backstop-exposure bound (both real, both already argued), but a
trade this draft's own "mechanism resolved, number not sized" honesty discipline (applied to `exec_wall_ms`,
`memory_bytes`, the zip-bomb threshold, and the interpreter-startup cost, all in §6) has not yet been applied
to. This is very likely small in absolute terms relative to the already-disclosed CPython startup cost paid
in the same call — but "very likely small" is exactly the kind of unmeasured assumption this draft otherwise
refuses to let stand without saying so explicitly (compare §6's own refusal to even guess a number the way
the OCR draft guessed one). Checked for existing coverage: `test_native_jail_teardown_cycles_windows.cpp`'s
own 300-cycle census (G4) proves `grant_path()` called repeatedly on the SAME, never-deleted path across
`create()`/`exec()`/`destroy()` cycles does not leak handles/memory/ACEs — real, relevant, positive-control
evidence that `grant_path()` itself is not leaky — but it does not exercise this draft's own actual pattern
(a FRESH directory created, granted, and deleted on every cycle, not one path granted repeatedly), so it does
not speak to fragmentation/MFT churn or `SetNamedSecurityInfoW` latency under this draft's specific create/
grant/delete-per-call shape, a real, distinct, currently-untested pattern.

**Fix direction**: add a bullet to §6 naming this explicitly — "the per-call `grant_path()`/directory-create/
directory-delete cycle has its own real, unmeasured OS-call cost, additional to and smaller than the already-
disclosed interpreter-startup cost, previously amortized to `N` total calls by the now-eliminated pool and now
paid once per call instead" — the same honesty §6 already extends to every other unsized cost in this draft,
not a new mechanism to design.

### Finding 31 (High, new — I2/I8: finding 15's own residual framing names only the CRASH-skips-cleanup case; an in-process, non-crash `remove_all()` FAILURE — a real, disclosed-elsewhere-in-this-codebase failure mode — leaves the same stranded-directory-with-a-live-grant outcome, silently, with no bound and no signal, and Prove pass 1's whole argument now depends on this cleanup step succeeding)

§4b's fix for finding 15, re-read carefully against exactly what it names: the residual it accepts is framed
entirely around "a crash (not a normal return) skips cleanup entirely, and the OS then reuses that exact pid
before the crashed run's own directory is ever manually reaped" — a discrete, hard process-death event. It
does not name a materially different, softer failure mode: the process does **not** crash, `remove_all(dir,
ec)` genuinely **executes** (RAII fires normally), but returns a non-zero `ec` — e.g. `ERROR_SHARING_VIOLATION`
because a just-exited worker process (or, on Windows, an AV/indexing scanner) still transiently holds an open
handle inside the directory, or a permissions edge case. "Best-effort... regardless of outcome" (the same
phrase this draft's own real precedent, `extract_pdf_text.hpp:445`'s `std::filesystem::remove(file, ec)`, uses
for a single scratch FILE) is honest about what happens on failure — nothing — but that same "silent, no
retry, no log" posture has a **categorically different consequence** here than it does for Track B's own
precedent. Track B's `scratch_dir()` root is a single, dedup-granted-once directory (finding 14's own kind of
mechanism, `grant_ro_deduped`) — a failed `remove(file, ec)` there leaks a stray FILE inside an
already-permanently-granted directory: a disk-space nuisance, nothing more, because the grant was never
per-file to begin with. In THIS draft's own design, by contrast, Prove pass 1's entire argument for why
finding 14 is "resolved by elimination" is that `remove_all()` is the **sole** mechanism retiring the
per-call `grant_path()` ACE — "an ACE cannot outlive the object it was granted on," which is only true if the
object is actually deleted. A `remove_all()` that fires but fails leaves the directory, and therefore its
live ACE, on disk — reachable by the shared `shared_profile()` SID's own backstop (finding 18's own threat
model, unaffected by which fix eliminated the pool) — for as long as the host process runs, with:

1. **No bound.** Unlike the crash/pid-reuse residual (bounded by "this exact pid gets reused before manual
   reaping"), a non-crash cleanup failure has no comparable narrowing mechanism — `unique_scratch_dir_name()`'s
   pid+counter naming guarantees the STRANDED directory is never collided with by a later call (good, no
   data-integrity hole), but says nothing about how many such stranded directories can accumulate over a
   long-running host's lifetime if `remove_all()` has any nonzero real-world failure rate. This is the same
   "permanently and irrevocably grows... forever" shape finding 14 originally raised against the un-deduped
   mount loop — re-manufactured here through a different door (failed cleanup instead of skipped dedup) that
   Revision 8's own elimination text never checked for.
2. **No signal.** Nothing observes, counts, or logs a `remove_all()` failure — an operator has no way to know
   this is happening, let alone how often, on the exact codebase's own established "best-effort" pattern.

**Why §8's own claim needs re-examining, not just this finding**: §8 states "Finding 15 remains resolved
exactly as Revision 5 left it — its cleanup fix was never reopened by any later round." That was true when
finding 15 was written (pre-pool, when the grant's own retirement mechanism was not yet load-bearing on
cleanup succeeding) and remained true through the pool era (when the grant's retirement never depended on
`remove_all()` at all — the pool retired grants at pool-shutdown, not per-call). Prove pass 1's own collapse
is what makes `remove_all()`'s SUCCESS newly load-bearing for security, not merely for disk hygiene — a stake
increase in what finding 15's fix needs to guarantee that no round, including this collapse's own, has
re-examined. This is not a reason to doubt the collapse's central mechanism (deletion does retire the ACE,
investigation #1 above), but it is a real gap in what has to be true, reliably, for that mechanism to actually
bound exposure the way §4b's "Finding 14 — resolved by elimination" claims.

**Fix direction**: distinguish, explicitly, "cleanup was never attempted" (the crash case, already accepted)
from "cleanup was attempted and failed" (this case) — at minimum, log a `remove_all()` failure (directory
path, ec, timestamp) so a host has SOME way to observe and bound accumulation, and consider whether a fail-
open metric/counter belongs in §6 next to the other caps this draft already tracks. A retry-with-backoff
before giving up is a real, live option to consider, not designed here. This does not need new Win32 surface
or a new primitive — it needs the same "name the residual, don't let 'best-effort' silently absorb it"
discipline finding 15's own text already applies to the crash case, extended to the case it currently omits.

### Finding 32 (Medium, new — I4: eliminating the pool's lease-attribution log correctly closes finding 23's own specific claim, but drops the more general property — SOME audit trail of which call created/granted/deleted which scratch directory — with no replacement named, and no acknowledgment that this is a real degradation rather than a pure simplification)

§4b's "Findings 18, 19, 21, 22, and 23 — resolved by elimination" and §8's matching bullet both argue,
correctly, that finding 23's own SPECIFIC concern — reconstructing "which other call/session/tool shared the
affected slot recently enough to matter," a question that only makes sense for a REUSED resource — has no
analog once every scratch directory is single-use. That argument is sound: re-reading finding 23's own
original text (round 6) confirms its fix direction was scoped specifically to shared-slot reconstruction
("what lets a host reconstruct which OTHER call/session/tool shared the affected slot"), not to general
scratch-directory observability. But this is a narrower question than the one finding 23's elimination
answers by implication. Checked directly against what actually exists in the per-call design: **nothing**
records which session/call/tool created, granted, or deleted any given scratch directory — no log line, no
counter, no `EffectContext`-visible attribution of any kind — where the pool at least had the ring log
(imperfect per finding 27, but real). `unique_scratch_dir_name()`'s own naming scheme (pid + atomic counter,
unchanged since finding 9) carries no session id, call id, or tool name in the directory name itself, so a
directory stranded by finding 31's own newly-identified failure mode (or found by an operator for any other
reason) cannot be traced back to the call that created it from the filesystem alone. CLAUDE.md's I4 ("every
effect is attributable") applies to `grant_path()`/`create_directories()`/`remove_all()` as ordinary effects
regardless of whether the pool's own specific security claim needed a log to back it — the per-call design's
total absence of any attribution for these effects is a real, disclosable gap in its own right, not merely
the pool-specific gap finding 23 named, and it is not listed in §8's "still genuinely open" ledger under this
framing (the ledger's finding-23-adjacent bullets all frame the pool's own elimination as mooting the concern
entirely, not as narrowing it and leaving a smaller piece open).

**Fix direction**: name this explicitly as a real, disclosed residual — a minimal, low-cost option already
consistent with this draft's own discipline elsewhere: log scratch-directory creation/grant/cleanup-outcome
events (directory path, `run_id`, `principal.id`, tool name, call id, cleanup outcome) through whatever
ordinary tool-call-scoped logging this codebase already has, no new ring-log/pool-shaped mechanism required
— sized as a "real gap, mechanism not designed here" item, the same honesty §6/§8 already extend to every
other unsized or undesigned item in this draft.

### Investigated, no real defect found

- **Investigation (does `grant_path()` called directly, bypassing `grant_ro_deduped()`, skip any precondition
  the deduped wrapper was silently providing)**: no defect found. Checked directly against
  `native_jail_backend.cpp:134-145` (`grant_ro_deduped`) and `:603-615`/`:611-614` (the per-mount loop both
  `create()` and `create_python_worker()` already call directly, unchanged by this draft): `grant_ro_deduped`
  does exactly one thing beyond calling `grant_path()` — check-then-insert against a `static
  std::unordered_set<std::wstring>` under a `static std::mutex`, purely to avoid re-granting a REPEATED path
  (deployment-fixed paths like `python_home`, granted at most once for the process's life). It performs no
  validation, no directory-existence check, no ordering guarantee `grant_path()` itself does not already
  provide on its own. The direct per-mount call this draft's own design already rides on (unchanged since
  before finding 9 existed) has never gone through `grant_ro_deduped()` and was never missing anything it
  provides — dedup exists to solve a different problem (repeated paths) this design's per-call-unique paths do
  not have.
- **Investigation (does `grant_path()`, or anything in its call path, mutate shared state in a way concurrent
  per-call invocations under ADR-160's parallel-batch scheduler could race on)**: no defect found. `grant_path()`
  is a `const` method (`app_container_profile.hpp:48`) touching only `sid_` (read-only, immutable once the
  profile is constructed) and `path`'s own security descriptor via three Win32 calls with no static/shared
  buffers — two concurrent calls race only if they target the SAME `path`, and finding 9's own
  `unique_scratch_dir_name()` (pid + a verified-thread-safe `std::atomic` counter) already guarantees they
  never do, the same conclusion Prove pass 1 itself already reached for `revoke_path()` and confirmed here
  independently for `grant_path()` too. `authorize_spec()` (`sandbox.hpp:210-244`), the other function every
  `create_python_worker()` call runs first, is a free function operating purely on its `SandboxSpec const&`
  parameter — no static, no shared mutable state, `inline`, stateless by construction. `shared_profile()`
  itself (`native_jail_backend.cpp:110-123`) only needs its mutex during lazy CONSTRUCTION; once built, every
  caller's use of it is through `const` methods on an object whose only member is an immutable `PSID`, so
  concurrent readers need no further synchronization — a subtlety this draft's own text has never spelled out
  explicitly but which holds up under direct checking, not a defect.
- **Investigation (finding 20's fix — does `MediatedFileSystemAdapter` compose identically against a
  freshly-created, never-before-used per-call directory as it did against a reused pool slot, or does
  anything about the adapter assume a longer-lived/reused root)**: no defect found. Checked directly against
  `mediated_filesystem_adapter.cpp:46-58` (`create()`): construction re-validates the root exists via a fresh
  `CreateFileW`/`CloseHandle` pair every time, caching nothing but the root path itself; every subsequent
  operation (`read_file`/`write_file`/`list_directory`/etc., verified via their own `open_within_mount_root`
  calls) re-resolves fresh, by handle, on every call — nothing in the adapter's real, shipped shape
  distinguishes "root that has existed and been reused for a while" from "root created moments ago for this
  one call," because it holds no state across operations beyond the root path string itself. Composes exactly
  as this draft's own text already claims, verified rather than assumed.
- **Investigation (any stray reference to "the pool," "slot," "quarantine," or the ring log left in the
  ACTIVE design text — §1-8 — that should have been removed or updated by Revision 8's collapse)**: no defect
  found. Every "pool"/"slot"/"quarantine"/"ring log" occurrence in lines 1-1890 (the revision-history preamble
  through §8) is either inside prose explicitly narrating the pool's OWN history (correctly framed as
  superseded, with an explicit "resolved by elimination"/"Revision 8, per Prove pass 1" marker adjacent) or
  inside §8's own "Resolved by Revision 6/7" bullets, which are historical-record entries by this draft's own
  established discipline (the same pattern applied to every earlier revision's superseded text). §3, §5, and
  §7's own bodies carry no pool/slot references at all needing cleanup. Revision 8's edit is, on this specific
  axis, clean.

## Red-team round 9

Round 9's own brief: verify Revision 9's own newest mechanism — `revoke_path()`, actually built for real
this round, plus the retry loop and `report_progress` reuse it composes with — the way round 6 verified
`grant_path()`'s real behavior rather than trusting a citation. Method: read `AppContainerProfile::
grant_path()`'s real body again (`app_container_profile.cpp:152-205`, re-verified, unchanged since round 8)
against a question no prior round asked — what does `grfInheritance = OBJECT_INHERIT_ACE |
CONTAINER_INHERIT_ACE` (`:165`) actually do to files created inside the granted directory AFTER the grant is
made, and does a later revoke on the directory undo that — plus reading `agent_session.hpp`'s real
`report_progress` bracket discipline and `tool_pipeline.hpp`'s real deadline-enforcement point directly,
not assumed from §4b's own prose. One real, load-bearing defect (finding 33) in the mechanism the task
asked about most directly; three smaller, real gaps (34, 35, 36) in how it composes with existing
mechanisms; one real editorial defect (37). Two of this round's four investigation questions (3's "does
`report_progress` fire too late" and part of 4) turned up no real defect — checked directly against code,
not asserted, and reported honestly below rather than padded into findings.

### Finding 33 (High, new — I2: finding 31's own `revoke_path()` fallback strips the ACE from the scratch DIRECTORY's own DACL, but the files a compromised worker would actually want — `args.json`, `out.json`, the source document — already carry their OWN, independently-materialized copy of that same ACE, stamped at creation time by the very `OBJECT_INHERIT_ACE`/`CONTAINER_INHERIT_ACE` flags `grant_path()` sets, and revoking the parent does not retroactively strip it)

`grant_path()`'s real body (`app_container_profile.cpp:165`, re-verified this round) sets
`ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE` on the ACE it adds to the scratch
directory's DACL — necessary and deliberate: it is exactly what lets the fixed Python script's own
`open("args.json")`/`open("out.json")` calls succeed inside a directory granted only once, at the parent
level, by finding 9's own per-call design. But `(OI)(CI)` inheritance on Windows is a **point-in-time
copy**, not a live reference: when `args.json`/`out.json`/the fetched source document are created inside
the granted directory (ordinary `CreateFile` calls from inside the worker, or the host's own
`MediatedFileSystemAdapter::write_file`), NTFS computes the applicable inherited ACEs from the parent's
current DACL and **stamps a copy of each onto the new object's own security descriptor**, flagged
`INHERITED_ACE` but otherwise a fully independent, standing grant on that file. This is standard,
well-documented Windows ACL behavior — it is exactly why Windows' own "Advanced Security Settings" UI
needs an explicit "Replace all child object permission entries with inheritable permissions from this
object" action, and why the only programmatic way to retroactively strip an already-materialized inherited
ACE from every child is a recursive tree operation (`TreeSetNamedSecurityInfoW` with `TREE_SEC_INFO_RESET`,
or an equivalent manual walk-and-`SetNamedSecurityInfoW`-per-child) — **not** the single, non-recursive
`GetNamedSecurityInfoW`/`SetEntriesInAclW`/`SetNamedSecurityInfoW` triple against the DIRECTORY path alone
that Prove pass 1 verified for `grant_path()` and that finding 31's fix reuses, unchanged, for
`revoke_path()`.

Concretely: by the time `remove_all()` has failed three times and `revoke_path(dir)` runs, `args.json` and
`out.json` (and, per §4b's "Sourcing" bullet, the fetched document) already exist inside `dir`, already
carry their own independently-stamped copy of the `shared_profile()` SID's ACE, and `revoke_path(dir)`
touches only `dir`'s own security descriptor — it does nothing to theirs. `revoke_path()` genuinely does
block a NEW open of the directory object itself (enumeration, or opening a not-yet-existing name under it),
but it does **not** block a worker that already knows (or can enumerate before revoke, or simply guesses —
`args.json`/`out.json` are fixed, non-interpolated literal filenames per finding 1's own fix, identical on
every call) the exact full path from opening `args.json`/`out.json`/the source document directly — Windows'
default "bypass traverse checking" policy (`SeChangeNotifyPrivilege`, granted to Everyone/Authenticated
Users out of the box) means a full-path open of a file does not, in the general case, require any
traversal permission on its ancestor directories at all, independent of this specific ACE question.

This directly falsifies the specific, written security claim finding 31's fix makes for this fallback:
"it DOES close finding 18's own backstop-reachability concern for that directory going forward: **no
worker running under the shared `shared_profile()` SID, spawned by any later call, can reach this stranded
directory's contents through the OS-level grant any more**" (§4b, "Fix for finding 31," point 3). That is
true for the directory LISTING, and false for the CONTENTS — exactly the two nouns that sentence
distinguishes and gets backwards. The stranded directory's most sensitive content (`out.json`'s extracted
document text, `args.json`'s parameters, the source document's own bytes) remains reachable by any later
worker under the same shared SID that already knows the fixed filenames, for as long as the directory
itself survives (which, per finding 31's own framing, is indefinitely, until an operator manually reaps
it) — the exact residual finding 18 was raised to bound, reopened through the one fallback path this
draft added specifically to close it.

This is scoped narrowly (only the double-failure path: `remove_all()` exhausts its retries AND the
directory already contains worker-written files at that point, which is the ordinary case by the time
cleanup runs) and does not reopen the SUCCESS path — `remove_all()` on success deletes the files along
with the directory, so no independently-materialized child ACE survives to matter. It also does not reopen
Prove pass 1's own central claim (an ACE lives on the object it was granted on, not a profile-wide ledger)
— that claim is still correct for the directory's OWN ACE; what it does not extend to, and what this
draft never checked, is that `(OI)(CI)` inheritance means the SAME logical grant also lives, independently,
on every child created underneath while it was active.

**Fix direction**: either (a) name this fallback honestly as "blocks future directory listing/enumeration
under the shared SID, does NOT retroactively revoke already-inherited child-object ACEs on `args.json`/
`out.json`/the source document" rather than "no worker... can reach this stranded directory's contents...
any more," or (b) if closing the CONTENT reachability is the actual goal (the more defensible reading of
what finding 18 was raised to bound), extend the fallback to also call `revoke_path()` (or an equivalent)
against each of the up-to-three known, fixed child filenames inside `dir` — a bounded, enumerable set
(`args.json`, `out.json`, the one source-document path), not a recursive tree walk, so still small and
buildable without a new `TreeSetNamedSecurityInfoW`-class primitive. Either is a real, undesigned choice
this draft has not yet made; leaving the current sentence as written is the one option that is actually
wrong.

### Finding 34 (Low-Medium, new — I8/documentation: the draft names "if `revoke_path()` itself fails" as a live possibility without ever examining whether it shares `remove_all()`'s own root cause, in contrast to how carefully it names `remove_all()`'s own failure mode)

Finding 31's fix names `ERROR_SHARING_VIOLATION` precisely as the failure this retry loop targets for
`remove_all()` — a `DELETE`/data-access operation that Windows' NTFS sharing-violation check
(`IoCheckShareAccess`) genuinely blocks when another handle holds the object without compatible share
flags. `revoke_path()`'s own three calls (`GetNamedSecurityInfoW`/`SetEntriesInAclW`/
`SetNamedSecurityInfoW`) request only `READ_CONTROL`/`WRITE_DAC` against the path — access rights that, on
ordinary Windows semantics, are not part of the read/write/delete triad NTFS's sharing check keys off of
(this is the same reason `icacls`/`takeown` can repair permissions on a file another process holds open
without a sharing violation). If that holds here, `revoke_path()` is LIKELY largely immune to the exact
open-handle/AV-scanner condition that caused `remove_all()` to fail in the first place — meaning finding
31's own "if `revoke_path()` itself fails... no further automated recovery is designed here" residual is
probably a low-likelihood tail, not a correlated repeat of the SAME failure. But this draft never states
that reasoning, in either direction — it names the possibility of a second failure with no analysis of
whether it is likely, unlikely, or unrelated to the first, an inconsistency with this draft's own
established discipline of stating precisely WHY a failure class is common or rare (as finding 31's own text
does for `ERROR_SHARING_VIOLATION`). Not a proven defect — this round did not execute anything to confirm
the sharing-check claim for this codebase's specific call shape, the same "argued, not yet measured" gap
Prove pass 1 and round 8 both already apply to themselves — but a real, checkable gap in the draft's own
reasoning that is inexpensive to close by simply stating it.

**Fix direction**: add one sentence to finding 31's fix (or finding 34's own residual bullet in §8) stating
the reasoning above and its unverified status explicitly, the same way every other "argued from real code
and ordinary semantics, not executed" claim in this draft (Prove pass 1's own two, round 8's own) is already
flagged rather than left silent.

### Finding 35 (Medium, new — I8: the retry-loop-plus-`revoke_path()` cleanup sequence's own wall-clock cost is never examined against this draft's own deadline/wall-clock enforcement points, in either direction)

Checked directly, not assumed: `tool_pipeline.hpp:669-673` enforces `ctx.deadline` only "at the call
boundary... not preemptible mid-call" (the file's own header comment, line 23), and
`mediated_python_runner.cpp:67`'s `exec_wall_ms` bounds only `spec.limits.wall_ms` — the WORKER's own
in-sandbox execution, not any host-side code that runs before or after `run()`. §4b's own RAII-scope-guard
design for cleanup (finding 15's own phrasing, reused for finding 31) means the retry loop (up to 3 attempts
plus backoff) and, on exhaustion, `revoke_path()`, all run synchronously inside the same call stack as
`Tool<>::invoke()`, before it returns — confirmed against `rt/agent_session.hpp:1773-1784`, where
`effect_context_.report_progress` is bound to this call's closure immediately before `invoke_tool(...)` and
reset to a no-op only immediately AFTER `invoke_tool` returns, meaning cleanup (including its own
`report_progress` calls, finding 32's fix) executes and completes before the caller ever sees a `Reply`.
Two real, disclosed-nowhere consequences follow: (1) a slow cleanup sequence cannot itself cause an
otherwise-successful call to fail on `ctx.deadline` (a real, positive property — the deadline is not
re-checked on the way out), but this draft never states it, so a reader has to re-derive it rather than
being told; and (2) that same slow sequence adds real wall-clock latency to the call's own end-to-end
Reply-return time that is not counted against, or exempted from, ANY of this draft's own named caps
(`exec_wall_ms`, the zip-bomb threshold, or anything in §6) — an otherwise fully-successful, well-within-
budget extraction can still take meaningfully longer, from the caller's own perspective, than
`exec_wall_ms` alone would suggest, purely because of cleanup on the way out. This draft names no
caller-side/orchestrator-level timeout on the whole round trip that this composition could interact badly
with, but it also never rules one out — an ambiguity left unexamined rather than a proven defect, the same
class of gap round 7's own finding 25 found in the (now-eliminated) pool's acquire-wait-vs.-deadline
composition, not yet re-asked of this revision's own new wait/retry loop.

**Fix direction**: one sentence in §6, next to finding 30's own OS-call-cost disclosure, stating that the
retry-loop-plus-`revoke_path()` sequence is (a) not bounded by `exec_wall_ms`/`ctx.deadline`, so it cannot
itself cause a deadline failure on the way out, and (b) therefore adds unsized, uncapped latency to the
call's own Reply-return path on the failure branch specifically — the same "mechanism resolved, cost not
sized" honesty this section already extends to everything else.

### Finding 36 (Medium, new — I8: the retry loop's own backoff is never stated to be non-blocking, and a blocking backoff on the calling thread composes with ADR-160's `ThreadPool` worker budget in a way this draft's own finding-30 disclosure does not cover)

Nothing in finding 31's fix specifies whether the "short backoff between attempts (tens of milliseconds,
not seconds)" is an actual blocking sleep on the thread executing `Tool<>::invoke()` or some asynchronous
mechanism — the natural, default reading (and the only one consistent with `run()`/cleanup already being a
synchronous, RAII-scoped call, finding 35 above) is a blocking sleep on that same thread. For a
sequentially-dispatched call that thread is whatever called `run_rounds()`; for an ADR-160 parallel-batch
call it is one of `rt::ThreadPool`'s own worker threads (`native_jail_backend.cpp`'s own file-top comment,
re-cited at line 47 of this draft, and §6's own citation of "`rt::ThreadPool`'s own worker-count ceiling"
as the concurrency bound for scratch-directory creation). A transient `ERROR_SHARING_VIOLATION` from an
AV/indexing scanner sweeping a batch of just-created scratch directories is plausibly a CORRELATED failure
across concurrently-dispatched calls of the SAME batch, not an independent one — several calls hitting it
at once is a realistic scenario, not a contrived one, given the scanner's own sweep is likely triggered by
the SAME batch of file-creation events. If so, several `ThreadPool` worker threads are each independently
blocked in their own bounded retry-plus-backoff loop at once, each holding its own worker slot for longer
than the call's own extraction work alone would need — real, bounded per call (3 attempts, tens of
milliseconds each), but never weighed against concurrent-batch throughput the way round 4's own finding 16
required this draft to weigh a different, structurally similar aggregate-concurrency shift (the XLSX
interpreter-startup cost) explicitly rather than leave it implicit. §6's own finding-30 fix discloses the
OS-call cost of the cycle but says nothing about thread-occupancy during backoff specifically, and neither
does finding 31/32's own text.

**Fix direction**: state explicitly, next to finding 30's or finding 16's own disclosure, that retry backoff
blocks the calling thread (if that is in fact the intended design — this round did not find anything ruling
out an async alternative, only that nothing states one), and that under ADR-160 concurrent dispatch this
composes with `ThreadPool`'s own worker-count ceiling the same way ordinary call duration already does — a
real, bounded, but previously unexamined contributor to worst-case batch latency under correlated transient
failures.

### Finding 37 (Low, new — documentation clarity: the "Correction (Revision 9)" paragraph narrows the REASONING of "Why not `revoke_path()` either," but the paragraph it corrects keeps its own bolded, unqualified conclusion sentence standing, now literally false, with no strikethrough or inline qualifier)

Investigation 4's own question, checked directly: the "Correction (Revision 9...)" paragraph (§4b, immediately
after "Why not `revoke_path()` either") is honestly and clearly flagged as a correction, explicitly says "not
a reversal... the one case that reasoning did not yet cover," and the actual authoritative design (the later
"Fix for finding 31" subsection) is fully self-contained and correct read on its own — so a reader who reads
either the whole section top-to-bottom, or ONLY the "Fix for finding 31" subsection, gets the right answer
either way; this is not the reader-confusion risk the investigation asked about in the strong form. There is
one smaller, real gap, though: the paragraph being corrected ends on a bolded, standalone sentence — "**`revoke_path()` is not built for v1**" — stated as this draft's own convention for a section's takeaway, not
hedged to the success-path case the surrounding prose was reasoning about. That sentence is now simply false
(Revision 9 does build it) and is left completely unedited, textually adjacent to a correction that discusses
the REASONING above it without ever pointing at, qualifying, or striking the specific clause that itself needs
qualifying. A reader who trusts this draft's own bolded-conclusion convention (reasonable, since every other
section uses bold this same way) and stops at that sentence, rather than continuing into the next paragraph,
comes away with a materially wrong summary of the current design. This is the same class of gap finding 29
(round 7) found in a different corner of this draft's revision-history discipline — a real conclusion changing
without the surrounding ledger text being updated to match — recurring here in miniature, inside a single
subsection rather than across §8.

**Fix direction**: qualify the original bolded sentence in place — e.g. "**`revoke_path()` is not built as the
PRIMARY retirement mechanism for v1**" — or add a one-clause forward-pointer ("see the Correction immediately
below") directly onto it, rather than relying on the reader to keep going past a sentence this draft's own
convention marks as a stopping point.

### Investigated, no real defect found

- **Investigation (does `EffectContext::report_progress` fire too late to be seen, if cleanup runs after the
  call's own `Reply` has already been constructed and returned)**: no defect found. Checked directly against
  `rt/agent_session.hpp:1739-1784` (the sequential dispatch path) and the mirrored parallel-batch path
  (`:1739-1747`'s `make_call_ctx` closure): `report_progress` is rebound to this call's own closure
  immediately BEFORE `invoke_tool(...)` runs and reset to a no-op closure only immediately AFTER
  `invoke_tool` returns (`:1773`/`:1781`) — strictly before `emit_run_event(tool_call_finished, ...)`
  (`:1782`) and before `out[i]` is populated with the `DispatchedCall` the caller ultimately sees (`:1784`).
  Because finding 15/31's own RAII scope-guard design runs cleanup (including finding 32's own
  `report_progress` calls) synchronously inside `Tool<>::invoke()`'s own call stack, before it returns, the
  channel is still live and bound to this exact call's `call_id` for the entire cleanup sequence, retries
  included — there is no window where cleanup's own `report_progress` calls fire after the bracket has
  already closed. `EffectContext::report_progress`'s own real contract (`core/effect_context.hpp`) is
  genuinely call-scoped, informational, and zero-cost-if-unattached (`agent_session.hpp:1661`:
  `if (!run_event_producer_.valid() && !run_event_tap_attached_) return;`) — no default persistence backs
  it, exactly as this draft's own "composition with finding 31" paragraph already, honestly, discloses ("a
  host that wires none gets neither"); this is a real, already-disclosed residual, not a new one this round
  found.
- **Investigation (is the "Correction (Revision 9)" narrative, taken as a WHOLE, internally contradictory
  about what the current design actually is)**: no defect found at the level asked — see finding 37 above
  for the one real, narrower editorial gap this investigation did surface.
- **Investigation (does `grant_path()`'s `(OI)(CI)` inheritance flag change anything about round 8's own
  "Investigated, no real defect found" conclusion that concurrent `grant_path()`/`revoke_path()` calls never
  race, since finding 9's per-call-unique naming rules out two calls targeting the SAME directory)**: no
  defect found. Finding 33 above is a correctness/completeness gap in what `revoke_path()` achieves, not a
  concurrency defect — two calls still never target the same directory (finding 9, unchanged), so the
  race-freedom argument round 8 already verified for both `grant_path()` and `revoke_path()` stands
  unaffected by finding 33.

## Prove pass 3 — `revoke_path()` proven under real open-handle contention (closing Prove pass 2's last disclosed residual, finding 34)

Prove pass 2 built and executed real, ACE-enumeration-based positive-control tests for Claim 1 (deleting a
`grant_path()`-granted directory destroys its ACE as a structural NTFS side effect — the premise behind
Revision 8's pool-elimination collapse) and Claim 2 (`revoke_path()` on a directory plus its known children
strips each of their independently-materialized inherited ACEs — closing round 9's finding 33). It named one
honest residual it left unproven: whether `revoke_path()` actually succeeds in the specific open-handle/
sharing-violation scenario that would cause `remove_all()` to fail in the first place — the exact condition
finding 31's cleanup-failure fallback exists to handle. Finding 34's reasoning (that `WRITE_DAC`/
`READ_CONTROL` access, which `SetNamedSecurityInfoW`'s ACL modification needs, is not arbitrated by NTFS's
sharing-violation check the way `DELETE`/data-stream opens are) was real, documented Win32 semantics — but
argued, never executed.

**This pass built and ran that test.** A third claim was added to `tests/test_native_jail_grant_path_ace_lifecycle_windows.cpp`:

- **Claim 3**: grant a directory, create a child file inside it, then open a REAL `HANDLE` on that exact
  child via `CreateFileW` with `dwShareMode = FILE_SHARE_READ` — deliberately excluding `FILE_SHARE_DELETE`.
  **Sanity check, run inside the test itself before anything else is asserted**: with that handle still
  open, `std::filesystem::remove_all()` on the directory is called and verified to genuinely FAIL
  (`ec` set, the directory and the held-open child both still exist afterward) — reproducing the real
  failure mode finding 31's fallback exists for, not assuming it. Only then, while the handle is STILL
  open, `revoke_path()` is called on both the directory and the held-open child (per finding 33's
  fixed-enumeration fallback), and both calls succeed; a fresh `GetNamedSecurityInfoW` query on each
  confirms the profile's SID's ACE is genuinely gone from both, despite the handle never having been
  closed. The handle is then closed and a final `remove_all()` is confirmed to succeed — proving the
  earlier failure really was caused by the held handle, not something else going on with the directory.

**Result: the claim holds.** `revoke_path()` succeeded in exactly the live-contention window it needed to
work in, real evidence rather than an argument from documented semantics. Build: full rebuild via CMake/
Ninja, 0 errors. Test run: all 37 checks pass (`test_native_jail_grant_path_ace_lifecycle_windows: all
checks passed`, exit 0), including the sanity check confirming `remove_all()` genuinely failed first. Full
`native_jail|extract_pdf|pdf_text` suite: 9/9 tests pass, no regressions (`test_native_jail_grant_path_ace_lifecycle_windows`
now included in that count).

**What this closes**: finding 34 (round 9) is resolved — real evidence, not just documented-semantics
reasoning, now backs the claim. Combined with Prove pass 2's Claims 1 and 2, all three load-bearing safety
claims behind Revision 8's pool-elimination collapse and Revision 9/10's `revoke_path()` safety-net fix are
now proven by real, executed, adversarially-informed tests rather than argued from code-reading alone —
closing out the "prove" phase of design → red-team → prove → judge for this specific mechanism. This does
not mean the whole draft is implementation-ready in every other respect (§8's own remaining "still genuinely
open" bullets — resource-cap sizing, the correlated-retry-under-batching cost, finding 37's small textual
fix, etc. — are unaffected by this pass and remain open); it means the one class of claim this entire ten-
revision, nine-round, three-prove-pass arc kept returning to — "does this OS-level authority mechanism
actually behave the way the design says it does" — now has real, run evidence behind it, not just
increasingly careful reading of Win32 documentation.
