# Design draft — a first-party PDF text-extraction tool, and its companion skill

**Status: Revision 6 — closes round 5's two findings on Revision 5's two fixes.** Round 5 confirmed
Revision 5's REASONING was sound on both counts (the right function to target for dedup; the right
architecture for the output-ceiling fix) but found each fix unreachable/incomplete as literally
specified: `grant_ro_deduped()` has internal linkage, uncallable from outside `native_jail_backend.
cpp`; Linux's `F_SETPIPE_SZ` pipe-enlargement is unchecked best-effort, so "confirmed identical on
Linux" was false. **Five real, independent red-team rounds so far. Round 4 was the first to confirm
most of a predecessor's claims under direct verification; round 5 confirmed even more — the WINDOWS
half of Revision 5's design (the `create()`/`exec()` pivot, `wait_or_kill()`/`wall_ms`, the output-
ceiling mechanism's interaction with `exec()`'s real exit handling, `drain_pipe_bounded`'s record-
reassembly) is now independently verified sound; what remained were two narrow, correctly-diagnosed-
but-incompletely-executed fixes, not new structural problems.** No code yet; not yet re-attacked.
Prior revisions' findings are recorded in git history (`git log -p -- docs/planning/pdf-text-
extraction-design-draft.md`), not reproduced here. Continues 009 §7's "Document extraction" catalog
row after ADR-106 settled the license question (PDFium, BSD-3-Clause, not poppler/mupdf). Covers the
PDF-text-layer slice only — DOCX/PPTX/XLSX stays explicitly out of scope, per ADR-106 §3.

Scope was widened before round 1, at the project owner's own direction: a first-party *tool* this size
should ship with a first-party *skill* alongside it, not the tool alone. §8f's five generic skills
(`include/agentengine/core/builtin_skills.hpp`) are the established, real, tested precedent for that
pairing.

## 1. What already exists, reused rather than re-invented

- **The result shape**: `ReadContent`'s bounded-preview + `BlobRef`-promotion pattern
  (`read_content_detail::build_reply`, `include/agentengine/tools/read_content.hpp`) is source-
  agnostic over already-obtained bytes. A PDF-extraction tool's extracted text goes through the same
  discipline — 006 §7's token-budget rule does not care where the bytes came from.
- **The sourcing**: `ReadContentArgs`'s `url`-xor-`path` shape and its two dynamic capability checks
  (`find_net_out`/`find_fs_read`) is the "any granted source" 009 §7 asks for.
- **The skill-shipping mechanism**: `builtin_skills.hpp`'s compiled-in `SKILL.md` constants.
- **Real process-isolation precedent, verified directly against the actual functions (not a
  restated claim)**: `NativeJailBackend::create()`/`exec()` (`src/backends/native_jail/native_jail_
  backend.cpp`) — the SIMPLE, one-shot conformer, deliberately NOT `create_python_worker()`'s
  persistent-worker sibling (Revision 3's citation, which round 3 confirmed is Windows-only; `create()`/
  `exec()` exists on both Windows and, per `linux_native_jail_backend.cpp`, Linux). `exec()`'s
  wall-clock enforcement is `JobObjectLimits::wait_or_kill()` — the SAME function `job_object_limits.
  hpp`'s own 11-run measured finding is about (`wall_ms` reliable, `cpu_ms` not), not the unmeasured
  `session_watchdog_loop()` poll a persistent-worker design would need. `exec()` already captures
  output via `drain_pipe_bounded()` AFTER the process exits or is killed — a real, already-working,
  already-bounded (`output_bytes`) mechanism for getting back whatever a killed child had already
  written, needing no new IPC framing.

## 2. Composition question — still open, narrowed by round 1

Round 1 (finding 7) named a real, cited anti-coupling precedent this draft had not engaged:
`read_content_detail::kWorkMount` is deliberately NOT shared with `read_sandbox_file.hpp`'s own
`kSandboxWorkMount`, specifically to avoid two independent symbols colliding if both headers are ever
included together — this project's own recent judgment favors small, deliberate duplication over
sharing when the shared thing might need to diverge.

Applying that lesson here changes what "compose" should mean. The SOURCING half (get bytes for a
`url`/`path` Args, after a capability check) is still worth sharing — round 1 finding 8 named a real
hazard in doing that naively, though: `ReadContent`'s current error codes (`read_content.malformed_
url`, `read_content.no_sandbox`, `read_content.ambiguous_source`, `read_content.http_error`) are
tool-name-prefixed literals, and a shared helper built by lifting `ReadContent::invoke_via`/
`invoke_from_sandbox` verbatim would leak THOSE literal strings into `extract_pdf_text` failures.

**Revised design**: a shared `read_source::fetch_source_bytes(Args, ctx[, proxy]) -> result<std::pair
<std::string, std::string>>` (bytes, media_type) in its own new header (`tools/read_source.hpp` —
NOT inside `read_content.hpp`, so it doesn't inherit that file's tool-specific error-code namespace),
with its OWN generic error codes (`read_source.malformed_url`, `read_source.no_sandbox`, `read_source.
ambiguous_source`, `read_source.capability_not_held`, `read_source.http_error`) — a real, disclosed
implementation-ADR task: `ReadContent` itself gets migrated to call this shared helper and its own
existing tests get updated to the new error-code strings, rather than the helper silently growing a
second, `ReadContent`-flavored set of codes nobody the wiser about. `Args` for both tools must
structurally provide `.url`/`.path` (a concept, not inheritance — `ReadContentArgs` and
`ExtractPdfTextArgs` stay independent types, matching round-1 finding 9's point that no shared base
type was designed). The url-xor-path ambiguity check moves INTO `fetch_source_bytes` itself — the one
place it needs to live, not duplicated per tool (round 1 finding 9).

**Still not decided; the implementation ADR's job**: whether this is worth it at all versus option
(a) (fully standalone, some duplication) given the real migration cost of changing `ReadContent`'s
already-shipped, already-tested error-code contract. Flagged, not resolved, same as Revision 1 — but
now with the actual shape of the cost named instead of hand-waved.

## 3. The tool: `ExtractPdfText`

```
Args:  { url?: string, path?: string }
Reply: { preview: string, truncated: bool, total_bytes: uint64,
         total_page_count: uint32, pages_processed: uint32, truncated_pages: bool,
         blob?: BlobRef }
```

`page_count` (Revision 1) is split into two fields per round-1 finding 5 (its single-field meaning was
undefined and couldn't express both facts §3 already claimed mattered): `total_page_count` is the
document's real page count (PDFium can report this cheaply, independent of how much text extraction
actually completed); `pages_processed` is how many pages actually got extracted before any cap (§4)
stopped the run. `truncated_pages` is `pages_processed < total_page_count`, kept as its own bool for
the same reason `ReadContentReply::truncated` is its own bool rather than requiring a caller to
compare two counts.

### 3a. Isolation posture (Revision 4 — one-shot `exec()`, not a persistent worker; no `grant_path()`
on caller-supplied paths)

**Two structural changes from Revision 3, both direct responses to round 3's two MUST-FIXes, not
restatements:**

**(i) One-shot, per-call `NativeJailBackend::create()`/`exec()`, not a persistent per-session worker.**
Round 3 confirmed the persistent-worker shape (`create_python_worker()`/`exec_session()`/duplex-pipe
protocol) is Windows-only in this codebase today — `LinuxNativeJailBackend` implements only the
simpler `create()`/`exec()` shape. Using THAT shape instead is a real, verified Linux-parity claim,
not an asserted one: both platforms implement it. It also resolves round 3's `wall_ms`-measurement
mismatch directly — `exec()` uses `JobObjectLimits::wait_or_kill()` on Windows, the exact function
`job_object_limits.hpp`'s 11-run measurement is about, not the never-measured `session_watchdog_loop()`
poll a persistent worker would need. Linux's own poll-based wall-clock enforcement in `cgroup_limits.
hpp` still has no equivalent measured data — this revision does NOT claim Linux timing is proven,
only that the MECHANISM cited for Windows is now the one that's actually measured, and Linux's own
measurement is named as required implementation-ADR work (§6), not silently assumed.

Consequence for §3's `total_page_count`/`pages_processed` design: with no persistent worker, "per-
session reuse" and its own spawn-rate-DoS mitigation (Revision 3's rationale) no longer apply. Each
`extract_pdf_text` call is its own `create()` + `exec()` + teardown, matching `NativeJailBackend::
create()`'s own per-call `SandboxHandle` lifetime. Spawn-rate cost returns as a real, named, NOT-yet-
capped concern (§4 item 3) — Revision 3's per-session-reuse answer to it is gone along with the
mechanism it relied on; this revision does not claim to have solved it, only names it honestly.

**(ii) `AppContainerProfile::grant_path()` is NOT used for the caller-supplied source path, at all.**
Round 3's most serious finding: granting an arbitrary, caller-supplied `path` via `grant_path()` is
additive, non-revocable, and lands on `shared_profile()`'s single process-wide AppContainer SID
(confirmed: `create()` itself already calls `(*profile)->grant_path(host_path, mount.read_write)`
per `MountSpec`, on that SAME shared profile) — a permanent, cross-session-reachable grant, a real I2
violation for anything but the small, fixed, deployment-level paths (`python_home` etc.) this
mechanism's own file-top comment scopes it to ("Callers of this class must not treat a `grant_path`
as the whole filesystem boundary... interpreter-level `open()` mediation... is what 008 §1b names as
primary"). This design does not fight that scope — it stays inside it:

- The HOST still performs the capability check and the read for the `path` source, via §2's shared
  `fetch_source_bytes` — UNCHANGED from Revision 2/3's original design, not routed through the worker
  at all. This reopens round-1 MUST-FIX 3 / round-2 MUST-FIX 2's original concern (`FileSystemAdapter::
  read_file()` has no size limit) — but this revision does NOT claim that's newly solved. **It is the
  SAME, already-shipped, already-accepted residual `ReadContent`'s own `path` source already carries
  today** (this branch's own Phase 1 commit) — `ExtractPdfText` inheriting an existing, disclosed gap
  is not a new defect this design introduces, and is explicitly NOT this design's problem to solve; a
  real size-limited `FileSystemAdapter` read primitive (named since Revision 2) would fix both tools
  at once and remains a worthwhile, separately-scoped follow-up.
- The already-fetched, already-capability-checked bytes are written by the HOST to a per-call file
  inside a FIXED scratch directory (e.g. under `%TEMP%`). **Revision 5 correction**: round 4 verified
  directly against the code that `create()`'s own `SandboxSpec.mounts` loop calls the RAW `grant_path()`
  (`native_jail_backend.cpp:249`), never `grant_ro_deduped()` — that wrapper is called only at a
  separate, dedicated call site for `create_python_worker()`'s own fixed paths (lines 634-644),
  entirely OUTSIDE the `spec.mounts` mechanism. Routing the scratch directory through `SandboxSpec.
  mounts` (Revision 4's design) would therefore call the UNDEDUPED `grant_path()` on every single
  `extract_pdf_text` call — reproducing, on a new path, the exact unbounded-DACL-growth defect this
  same file's own `grant_ro_deduped()` comment (lines 160-168) already documents finding and fixing
  once for `python_home`/`extra_sys_path`/`exe_dir`. **Revision 6 correction**: round 5 verified
  `grant_ro_deduped()` and `shared_profile()` both sit inside `native_jail_backend.cpp`'s own anonymous
  namespace (lines 27-210) — INTERNAL LINKAGE. Neither is declared in `native_jail_backend.hpp`, and
  the only real call site (`create_python_worker()`) works exclusively because that's a same-TU member
  function. "Call the existing free function" from a separate `tools/` translation unit does not
  compile — Revision 5's fix was correctly TARGETED (the right function, the right precedent) but not
  actually reachable as written. **Fix**: `NativeJailBackend` gains one new, small, PUBLIC method —
  `grant_ro_path_once(std::wstring const& path) -> result<void>` — declared in `native_jail_backend.
  hpp`, implemented in the .cpp by forwarding to the SAME existing dedup machinery (`grant_ro_deduped`
  becomes this method's private implementation, called with `shared_profile()`'s result, both already
  correct — only their VISIBILITY changes, not their logic). `ExtractPdfText`'s implementation calls
  this new public method once, before its first use, exactly the same call shape Revision 5 already
  described, now through a real, compilable API rather than an unreachable internal one. This is
  genuine new API surface (unlike Revision 5's framing, "a real, disclosed implementation-ADR task"
  undersold how much: a header change and a new public entry point, not merely wiring up an existing
  one) but a small, mechanical one — no change to `grant_ro_deduped`'s own already-correct dedup
  behavior, no change to `create()`/`create_python_worker()`'s existing call sites. Same materially
  smaller, correctly-precedented exposure as before: one fixed, low-sensitivity scratch directory,
  granted exactly once, not one growing grant per call and not every real file any session ever names.
  **Windows-only residual, disclosed precisely this time** (round 4 found Revision 4's own disclosure
  was written as platform-general and wrong): on Windows, the scratch directory, once granted, is
  still reachable by any OTHER session's `native_jail` process sharing the same AppContainer SID for
  the life of the host — the same structural exposure `python_home`'s own existing grant already
  accepts. Verified this revision: **Linux has no equivalent exposure at all** —
  `LinuxNativeJailBackend::create()` never calls anything AppContainer-like; each `exec()` call's
  bind-mounts happen inside a freshly `clone(CLONE_NEWNS)`'d PRIVATE mount namespace, invisible to
  every other process including a sibling `extract_pdf_text` call's own worker, and torn down with the
  child. A real per-session-scoped or auto-expiring grant mechanism does not exist in
  `AppContainerProfile` today and remains out of this design's scope to build — the Windows residual
  is accepted, not solved, matching `python_home`'s own already-accepted posture, one real ACE for the
  process lifetime, not a growing one.
- The worker itself is a small, PDFium-linked CLI-style program invoked via `exec()`'s `ExecRequest::
  source` command line (worker binary path + the scratch-file path), reading that one file itself
  (now safely bounded by ITS OWN `JobObjectLimits`/cgroup memory cap, since decode-time memory blow-up
  — as opposed to the pre-existing host-read-size gap above — IS this design's own new risk to bound,
  and now genuinely is, via the real, measured `wait_or_kill()`/Job Object mechanism).
- **Output — revised again this revision after round 5 found the Windows/Linux pipe-buffer parity
  claim false.** Round 4 traced `exec()`'s real architecture on both platforms and found it drains
  stdout/stderr ONLY AFTER the process exits or is killed — Revision 5's fix (worker self-limits its
  own output well under the pipe buffer, stops and exits cleanly instead of blocking) is architecturally
  sound and round-5-verified correct against `exec()`'s real exit-code/outcome handling on the platform
  it was actually checked against. **What round 5 found wrong**: Revision 5 claimed the 1 MiB pipe
  buffer was "confirmed identical on Linux" — false. `linux_native_jail_backend.cpp` sets it via
  `fcntl(..., F_SETPIPE_SZ, 1024*1024)` and DISCARDS the return value (the code's own comment: "best-
  effort... failure here... just leaves the OS default in place," typically 64 KiB). A real, plausible
  host (a hardened sysctl profile, a container base image capping `/proc/sys/fs/pipe-max-size`) gets a
  pipe buffer over 12x smaller than the 768 KiB ceiling assumed safe — reproducing the exact deadlock
  this fix exists to prevent, on Linux specifically. **Fix**: the ceiling is no longer a single fixed
  constant. The host, after creating the stdout pipe and attempting `F_SETPIPE_SZ` (Linux) — Windows'
  `CreatePipe` `nSize` is a real floor, not a hint, so no equivalent check is needed there — reads back
  the ACTUAL negotiated size (`fcntl(fd, F_GETPIPE_SZ)`) and passes a safe fraction of THAT real number
  to the worker (one integer, via `argv`/an environment variable on the worker's command line) as its
  output-byte ceiling, rather than the worker assuming a fixed value. Windows keeps the illustrative
  768-under-1-MiB margin (a real floor, verified sound); Linux computes its own real margin from
  whatever `F_GETPIPE_SZ` actually reports, safe even when the enlargement silently failed and the OS
  default held. A NEW, named resource cap (§4 item 5), distinct from `ctx.tool_result_byte_threshold`
  (which governs what the REPLY hands back to the model, already handled by `build_reply` on whatever
  the worker returns). Once the worker's own cumulative stdout output would cross ITS ceiling, it stops
  emitting pages and exits cleanly (not killed) — the SAME `truncated_pages`/`pages_processed`
  reporting path §3 already has, now covering "stopped because of the pipe-safe output ceiling" as a
  third, equally legitimate reason alongside "stopped because of `wall_ms`" and "stopped because of
  `memory_bytes`". Text: one page per line prefixed with a fixed delimiter (e.g. `\x01PAGE\x01<n>\x01`)
  the host parses after `drain_pipe_bounded()` returns; `pages_processed` counts only COMPLETE
  delimited records (a record
  cut off by an actual kill is discarded, not counted — `drain_pipe_bounded`'s own read loop correctly
  reassembles a record split across multiple `ReadFile` calls, round 4 verified, so this is a real,
  not merely asserted, distinction).

**Not decided by this revision**: the exact scratch-directory location and per-call filename scheme
(collision-safety across concurrent calls, cleanup-on-crash); the worker binary itself; the exact
page-delimiter wire format and the worker's own output-ceiling exact value (768 KiB is illustrative,
needs real measurement against the 1 MiB pipe buffer's actual safe margin — does the delimiter/framing
overhead, or OS-level pipe buffering behavior, eat into that margin in a way that needs a bigger gap);
whether the scratch-directory `grant_ro_deduped()` call needs its own new host-side entry point (this
revision assumes the free function is reachable from wherever `ExtractPdfText`'s implementation would
live, not yet checked against the real namespace/linkage boundaries between `tools/` and
`backends/native_jail/`).

## 4. Resource caps (I8) — grounded in `exec()`'s real, measured mechanism; spawn-rate now a named gap

1. **Worker `ResourceLimits::memory_bytes`, enforced by `JobObjectLimits`/cgroups v2, bounds the
   DECODE, not the pre-existing host-read gap (§3a(ii) is explicit these are different, separately-
   owned risks).** A real kernel signal either way — `JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` via completion
   port on Windows, `memory.events`' `oom_kill` on Linux cgroups v2 — not a heuristic.
2. **`wall_ms`, via `wait_or_kill()`, is the measured-reliable timing cap; `cpu_ms` is NOT**, per
   `job_object_limits.hpp`'s own 11-run finding (3/11 fired, 1.38x-8.22x over budget when it did) —
   and per §3a(i), this design now actually uses the function that finding is about, on Windows.
   Linux's own poll-based wall-clock enforcement (`cgroup_limits.hpp`) has no equivalent measured
   data — NOT claimed proven here, named as required real measurement before this design's Linux side
   can make the same reliability claim its Windows side now can. Also closes round-1 SHOULD-FIX 6 (a
   single pathological page, or an expensive open/xref-parse step, is bounded by the same process-wide
   `wall_ms`/memory ceiling regardless of where the cost concentrates).
3. **Spawn-rate/call-rate cost — reopened by §3a(i)'s per-call `create()`/`exec()`, not yet capped, and
   more expensive per call than "just process-spawn overhead" implies.** Round 4 traced this
   precisely: every call also does a fresh Job Object or cgroup creation (`instance->job.create()`) and
   — once §3a(ii)'s dedup fix lands — the scratch-directory grant itself becomes a one-time cost, not a
   per-call one, but `create()`'s own bookkeeping (a fresh `SandboxHandle`/`Instance`) still runs every
   call. This revision does not invent a cap for it — named honestly as unresolved, a real
   implementation-ADR task (a per-session or per-run call-rate/concurrency limit on `extract_pdf_text`
   specifically, or a project-wide mechanism if one already exists elsewhere in the tool pipeline — not
   checked here).
4. **`pages_processed` via the delimited-stdout-record scheme (§3a(ii))** — a killed/capped worker's
   drained output may end mid-record; only complete, fully-delimited records count, per §3a(ii)'s own
   text, and `drain_pipe_bounded()`'s own multi-read reassembly (round-4-verified) means this is a real
   distinction, not merely asserted. Needs a real, tested positive control (a forced-kill mid-
   extraction, checked against an independently-known expected page count) before being trusted.
5. **Worker's own output-byte ceiling (new, closes round-4 MUST-FIX 2)** — a self-imposed cap, well
   under `exec()`'s fixed 1 MiB stdout pipe buffer (both platforms, round-4-verified), so a legitimate,
   non-adversarial large document's extracted text can never block the worker's own `write()` call
   waiting for a reader that `exec()`'s drain-after-exit architecture won't provide until the process
   already exited. Distinct from `ctx.tool_result_byte_threshold` (§2's `build_reply`, governs what the
   REPLY hands the model) — this cap governs what the WORKER is willing to write to the pipe at all,
   upstream of that. Exact value not sized here (§6).

CLAUDE.md's "Machine safety" section applies to all five items; item 3 is the one this revision leaves
genuinely open rather than closed.

## 5. The companion skill: `extracting-document-text`

Sixth entry in `builtin_skills.hpp`'s pattern (§8f), same mechanism as the existing five.

```
---
name: extracting-document-text
description: When and how to use extract_pdf_text instead of reading a PDF's raw bytes -- it returns
  decoded text, not a file to parse yourself. A large result still arrives as a bounded preview plus a
  pageable blob reference (see reading-large-content); a document too big or complex to fully process
  in one call instead reports how far it got, with no automatic continuation. Use this before calling
  extract_pdf_text.
allowed-tools: extract_pdf_text
metadata:
  version: "1"
---
```

**Revised from Revision 1** — round-1 MUST-FIX 4 found the draft skill text made a claim §3's own
tool design cannot support: treating `truncated_pages` as "a reference to more, not the whole thing"
the same way `reading-large-content` teaches `blob` — but unlike `blob` (a real, openable `BlobRef`),
there is no argument on `Args` (a start-page, an offset, a continuation token) that lets a caller
actually fetch the pages `truncated_pages` says were dropped. Revised body content, corrected to match
what the tool can actually do: (a) `extract_pdf_text` returns DECODED TEXT, not raw PDF bytes — never
fetch a PDF via `read_content`'s source and parse it yourself in the code interpreter; (b) the byte-
threshold preview/`blob` behavior IS pageable, exactly as `reading-large-content` already teaches —
that guidance is correct and unchanged; (c) `truncated_pages`/`pages_processed`/`total_page_count`
mean the document itself was too large or complex to fully process in ONE call, and — stated
honestly, unlike Revision 1's skill text — there is no follow-up call in this design that resumes
where it stopped; if that happens, the right move is narrowing the request (a smaller/split source
document) or treating the partial result as genuinely partial, not paging through it as if the rest
were one call away.

## 6. What this draft does NOT decide

- §2: whether sharing `fetch_source_bytes` is worth `ReadContent`'s own error-code migration cost, vs.
  accepting duplication (option (a)) — unchanged since Revision 2; §3a's redesign doesn't bear on this.
- §3a: `NativeJailBackend::grant_ro_path_once()`'s exact signature/error mapping; scratch-directory
  location/filename scheme; the new worker binary; the page-delimiter wire format; the exact
  worker-argv/env-var shape for passing the host-measured pipe-size-derived ceiling down to the
  worker — real implementation-ADR work, not finalized.
- §4 item 3: the call-rate/spawn-rate cap this revision still does not solve, now more precisely
  costed (a fresh Job Object/cgroup and `Instance` per call, not merely process-spawn overhead).
- §4: exact `ResourceLimits::memory_bytes`/`wall_ms` VALUES on Windows, and Linux's own wall-clock
  reliability (unmeasured — `cgroup_limits.hpp` has no equivalent to `job_object_limits.hpp`'s 11-run
  finding) — real measurement work, on both platforms, before either can be trusted at a specific
  number.
- PDFium vendoring specifics (pin, `FetchContent` source, WASI-SDK buildability) — ADR-106 §3
  territory, unchanged.
- The skill's final prose beyond fixing round 1's factual error — still not to be shipped verbatim
  without the same review weight the existing five skills got.
- **Round 6 of red-team.** Round 5 independently verified the WINDOWS half of this design end to end
  (the `create()`/`exec()` pivot, `wait_or_kill()`/`wall_ms`, the output-ceiling mechanism's interaction
  with `exec()`'s real exit handling, `drain_pipe_bounded`'s record-reassembly, and that dropping the
  scratch directory from `SandboxSpec.mounts` doesn't silently break anything else in `create()`) —
  the broadest confirmation any round has given this design so far. What's left, by round 5's own
  precise diagnosis: `grant_ro_path_once()` as a real new public method (not merely re-exposing
  existing internals) and the dynamic Linux pipe-size query. Both need independent verification that
  the NEW code this revision proposes (not previously-existing code this revision merely cited) is
  itself correct — a header change and an `F_GETPIPE_SZ` call are simple in isolation, but "simple in
  isolation" was also true of several claims earlier rounds found broken in practice.
