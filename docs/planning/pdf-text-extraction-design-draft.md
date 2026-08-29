# Design draft — a first-party PDF text-extraction tool, and its companion skill

**Status: Revision 3 — replaces Revision 2's §3a wholesale after round 2 found its central citation
FABRICATED (neither `MediatedPythonRunner` nor `MediatedShellRunner` actually uses
`native_process_spawn`) and its host-memory-exhaustion claim a non-sequitur against its own text. No
code yet; not yet re-attacked.** Continues 009 §7's "Document extraction" catalog row after ADR-106
settled the license question (PDFium, BSD-3-Clause, not poppler/mupdf). Covers the PDF-text-layer
slice only — DOCX/PPTX/XLSX stays explicitly out of scope, per ADR-106 §3.

**Round-2 red-team's verdict on Revision 2**, verbatim summary: not ready, and its findings were more
serious than round 1's — not omissions but affirmative false claims that survived a revision cycle
meant to close exactly that class of defect. Two MUST-FIX: (1) §3a's citation of `native_process_
spawn` as what `MediatedPythonRunner`/`MediatedShellRunner` use is checkably false — `MediatedShellRunner`
spawns no OS process at all (in-process dispatch); `MediatedPythonRunner` uses `NativeJailBackend::
create_python_worker()`, which has its own independent `CreateProcessW`/AppContainer path.
`native_process_spawn` is used ONLY by ADR-071's deliberately-unsandboxed native providers — reusing
it would give the PDFium child NO AppContainer/seccomp confinement, a regression from what round-1
finding 1 actually wanted. (2) §3a's own text has `fetch_source_bytes` reading the whole `path`-source
file into HOST-process memory via `FileSystemAdapter::read_file()` (still size-unlimited, confirmed)
BEFORE any child process exists — so the child's OS resource limit cannot bound that read; §4's
"blast radius bounded by the child's own OS resource limit" claim does not follow from §3a's own
architecture. This revision corrects both by citing and reusing the REAL mechanism (below), not the
false one.

Scope was widened before round 1, at the project owner's own direction: a first-party *tool* this size
should ship with a first-party *skill* alongside it, not the tool alone. §8f's five generic skills
(`include/agentengine/core/builtin_skills.hpp`) are the established, real, tested precedent for that
pairing.

**Round-1 red-team's verdict on Revision 1**, verbatim summary: not ready for an implementation ADR.
Three of four MUST-FIX findings were claims Revision 1 stated as settled or "free"/"matching existing
precedent" that turned out false once checked against the real code; the fourth (process isolation)
wasn't raised as a question at all, despite ADR-106 itself naming "hostile-PDF-parsing-in-process" as
the reason a full red-team pass is required before anything ships.

## 1. What already exists, reused rather than re-invented

- **The result shape**: `ReadContent`'s bounded-preview + `BlobRef`-promotion pattern
  (`read_content_detail::build_reply`, `include/agentengine/tools/read_content.hpp`) is source-
  agnostic over already-obtained bytes. A PDF-extraction tool's extracted text goes through the same
  discipline — 006 §7's token-budget rule does not care where the bytes came from.
- **The sourcing**: `ReadContentArgs`'s `url`-xor-`path` shape and its two dynamic capability checks
  (`find_net_out`/`find_fs_read`) is the "any granted source" 009 §7 asks for.
- **The skill-shipping mechanism**: `builtin_skills.hpp`'s compiled-in `SKILL.md` constants.
- **Real process-isolation precedent** (new this revision, round-1 finding 1/2's fix depends on this):
  `src/backends/native_process/native_process_spawn.{hpp,cpp}` already spawns and forcibly terminates
  real OS processes (`MediatedPythonRunner`/`MediatedShellRunner`'s own execution boundary) — an OS
  process kill is safe regardless of what the killed process was doing internally, unlike terminating
  a thread inside a shared address space.

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

### 3a. Isolation posture (Revision 3, replaces the false Revision-2 citation)

**Verified this revision, by reading the real code, not assumed:** `MediatedPythonRunner`'s real
isolation mechanism is `NativeJailBackend::create_python_worker()` (`src/backends/native_jail/
native_jail_backend.cpp`) — a long-lived worker process, launched with a real `SECURITY_CAPABILITIES`
AppContainer profile on Windows (`grant_path()`-based mount grants, exactly the primitive this design
needs for the `path` source below) and, confirmed via `linux_native_jail_backend.cpp`/`seccomp_
filter.hpp`/`cgroup_limits.hpp`, a real Linux parity path (`clone()` + seccomp + cgroups v2 — NOT the
Windows-only mechanism round 2 assumed was the only option). Resource enforcement is `JobObjectLimits`
(Windows) / cgroups v2 (Linux): `job_object_limits.hpp`'s own MEASURED FINDING (11-run sample,
`tests/test_job_object_limits.cpp`) is load-bearing for this design and must be inherited, not
re-discovered — `wall_ms` fired within a few ms of its deadline in every run and is the mechanism this
whole design's timing guarantee rests on; `cpu_ms`/`JOB_OBJECT_LIMIT_JOB_TIME` fired in only 3/11 runs
(1.38x-8.22x over budget when it did) and must NOT be relied on alone. `memory_bytes` has a real
kernel signal (`JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` via an I/O completion port), not a heuristic.
Communication is a duplex anonymous-pipe pair (`down_read`/`up_write`, handle values passed on the
child's command line, non-inherited otherwise), carrying a JSON-envelope protocol (`call_id`/
`exec_seq`/`kind`/`payload` — `mediated_python_worker_protocol.hpp`) already designed to be extended
with new `kind` values (its own file-top comment: "Slice 2... does not pre-declare typed payload
structs for those five kinds — every field they need is expressible" through the existing envelope).

**Design: a new sibling worker type, `create_pdf_worker()`, on the SAME `Instance`/pipe/AppContainer-
or-seccomp/`JobObjectLimits`-or-cgroup/watchdog machinery `create_python_worker()` already uses** — not
a new isolation mechanism invented from scratch, and explicitly NOT `native_process_spawn` (ADR-071's
deliberately-unsandboxed native providers — the wrong precedent, as round 2 established). This gives
real AppContainer/seccomp confinement (closes round-1 finding 1's actual concern — a PDFium memory-
corruption bug now runs inside a worker with no ambient filesystem/network reach beyond its explicit
grants, not merely a separate address space) and the measured-reliable `wall_ms` cancellation
primitive (closes round-1 finding 2 for real this time, against a mechanism actually verified to
provide it).

**Lifecycle: per-SESSION, not per-call** — one PDF worker created lazily on a session's first
`extract_pdf_text` call and reused for subsequent calls in that session, mirroring `create_python_
worker()`'s own per-session `Instance` lifetime (`exec_session()` is called repeatedly against one
long-lived handle, not re-spawned per call). This is a deliberate design choice, not an oversight:
spawning a fresh AppContainer/seccomp process PER CALL would introduce a real, uncapped spawn-rate
cost (many cheap calls, each paying full process-creation overhead) that Revision 2 left as an open
question and round 2 correctly flagged as a new, unnamed I8 target. Per-session reuse amortizes that
cost the same way the Python worker already does, with no new cap needed for it.

**The host-memory-exhaustion fix (closes round-1 MUST-FIX 3 and round-2 MUST-FIX 2 together, this
time by actually changing where the read happens, not by asserting a boundary that didn't reach it):**
for the `path` source, the HOST does not call `FileSystemAdapter::read_file()` at all. Instead, the
PDF worker's own AppContainer/seccomp profile is granted read access to the specific source path (via
`grant_path()`, the exact mechanism `create_python_worker()` already uses for `python_home`/`extra_
sys_path` grants) and the WORKER reads the file itself, inside its own `JobObjectLimits`/cgroup-capped
process — an oversized file now blows up the WORKER's memory limit (a real, kernel-signaled,
completion-port-observed event, §4), not the host's. For the `url` source, the existing host-side
`HostEgressProxy` fetch (`net_egress_proxy.hpp`, already a real streaming byte-cap independent of
grant `byte_cap`) is unaffected and safe as before — those already-size-bounded bytes are shipped to
the worker over the downstream pipe as an inline payload, not re-read from anywhere.

**Not decided by this revision**: the new `kind` value(s) `mediated_python_worker_protocol.hpp` needs
for "extract this PDF" requests/responses (a real protocol-design task — see §4 item 3 for why this
also needs to be a STREAMING kind, not one request/response pair); whether `create_pdf_worker()`
shares `Instance`'s struct shape with `create_python_worker()`'s `PythonWorkerState` or needs its own
sibling state struct; the worker binary itself (a new, small, PDFium-linked executable this project
builds and ships, analogous to the existing Python worker binary `AE_PYTHON_WORKER_EXE_PATH` points
at) — real, non-trivial implementation-ADR work, named honestly rather than glossed over.

## 4. Resource caps (I8) — grounded in `job_object_limits.hpp`'s real, measured mechanism

§3a's redesign changes where the memory risk from an oversized `path`-source file actually lands (the
worker, not the host) — but the cap that bounds it needs to be the ALREADY-MEASURED-RELIABLE one, not
an assumed one.

1. **Worker `ResourceLimits::memory_bytes`, enforced by `JobObjectLimits`/cgroups v2, is the real cap
   on both sources.** For `path`: the worker's own read of the granted file is bounded by its process's
   memory ceiling — a real kernel signal (`JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` via completion port on
   Windows; `memory.events`' `oom_kill` on Linux cgroups v2), not a heuristic, per `job_object_limits.
   hpp`'s own documented mechanism. For `url`: `net_egress_proxy.hpp`'s existing streaming byte cap
   still applies before the (already-bounded) bytes are shipped to the worker at all — belt-and-
   suspenders, not the only protection either way. A real size-limited read primitive for
   `FileSystemAdapter` (benefiting `ReadContent`'s own `path` source too) remains a worthwhile,
   separately-scoped follow-up, but is no longer this design's blocking gap — the worker's memory cap
   is the actual, measured backstop now, not an aspirational one.
2. **`wall_ms` is the trustworthy timing cap; `cpu_ms` is NOT, per `job_object_limits.hpp`'s own
   11-run measured finding (3/11 fired, 1.38x-8.22x over budget when it did).** This design's wall-
   clock guarantee rests on `wall_ms` alone, inherited directly from `wait_or_kill()`'s own already-
   proven contract — not a new, unverified mechanism this draft would otherwise have had to invent and
   measure itself. Also closes round-1 SHOULD-FIX 6 (a single pathological page, or an expensive
   document-open/xref-parse step before any page is reached, is bounded by the SAME process-wide
   `wall_ms`/memory ceiling — no separate per-page heuristic needed).
3. **`pages_processed` via a STREAMING protocol `kind`, not a single request/response.** For a killed
   worker to yield an accurate partial `pages_processed` (§3), the extraction protocol must emit one
   message per completed page over the upstream pipe as it goes, not buffer the whole result and send
   it once at the end — the host counts completed-page messages received before `wait_or_kill()`
   reports a `wall_clock_timeout`/`memory_limit` kill, and that count IS `pages_processed`. This is a
   real, named protocol-design task (§3a's "not decided" list) — flagged as a requirement here
   precisely because round 2 found Revision 2 silently assumed a single "extracted text as output"
   response could serve this purpose, which it structurally cannot.

Per-session worker reuse (§3a) means spawn cost is NOT a per-call I8 target — closes round-2's
spawn-rate-DoS finding by design rather than by adding a fourth cap. CLAUDE.md's "Machine safety"
section still applies to all three items above; none are optional.

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
  accepting duplication (option (a)) — unchanged from Revision 2; §3a's redesign doesn't bear on this.
- §3a/§4: the new `mediated_python_worker_protocol.hpp` `kind` value(s) for PDF extraction requests
  and STREAMING per-page responses — a real protocol-design task, not yet designed field-by-field.
- §3a: whether `create_pdf_worker()`'s state struct shares shape with `PythonWorkerState` or needs its
  own; the actual new PDFium-linked worker binary this project would build and ship.
- §3a: whether per-session worker reuse (chosen to avoid round-2's spawn-rate-DoS finding) introduces
  its OWN new question — e.g. can two concurrent `extract_pdf_text` calls in the same session share
  one worker safely, or does this need the same kind of single-flight serialization `exec_session()`
  already has for the Python worker (`"exec_session() is already in progress on this handle"`,
  confirmed in the real code) — not yet checked against this design's own needs.
- §4: exact `ResourceLimits::memory_bytes`/`wall_ms` VALUES — needs real measurement against
  representative and adversarial PDFs, on both platforms, the same way `job_object_limits.hpp`'s own
  cited 11-run finding was real measurement, not a guess.
- PDFium vendoring specifics (pin, `FetchContent` source, WASI-SDK buildability) — ADR-106 §3
  territory, unchanged.
- The skill's final prose beyond fixing round 1's factual error — still not to be shipped verbatim
  without the same review weight the existing five skills got.
- **Round 3 of red-team.** This revision replaces Revision 2's fabricated isolation citation with a
  verified real one and redesigns the memory-exhaustion fix to actually change where the read happens
  — but "verified this time" was also Revision 2's own claim about Revision 1, and was wrong. §3a's
  Linux-parity claim, the per-session-reuse concurrency question just named above, and whether
  `create_pdf_worker()` is actually as mechanical an extension of `create_python_worker()` as this
  draft assumes all need independent adversarial verification before an implementation ADR builds
  against this, not self-certification from the same process that produced Revision 2's errors.
