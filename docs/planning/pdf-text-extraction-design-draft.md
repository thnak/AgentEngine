# Design draft — a first-party PDF text-extraction tool, and its companion skill

**Status: Revision 2 — closes round-1 red-team's 4 MUST-FIX findings, addresses its 5 SHOULD-FIX
findings.** No code yet; the central open question (whether the isolation design in §3a is actually
implementable the way this revision assumes) still needs a second red-team round before an
implementation ADR builds against this. Continues 009 §7's "Document extraction" catalog row after
ADR-106 settled the license question (PDFium, BSD-3-Clause, not poppler/mupdf). Covers the PDF-text-
layer slice only — DOCX/PPTX/XLSX stays explicitly out of scope, per ADR-106 §3.

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

### 3a. Isolation posture (closes round-1 MUST-FIX 1 and 2 together)

**The actual PDFium decode runs in a dedicated child process, not in the host `agentengine` process,
and not on a worker thread inside it.** Spawned via the real, already-shipped `native_process_spawn`
machinery `MediatedPythonRunner`/`MediatedShellRunner` already use for exactly this reason — process
isolation for untrusted-input-handling code, with a safe, OS-level kill as the cancellation primitive.
This directly answers round-1 finding 1 (a PDFium memory-corruption bug now corrupts a throwaway
child process's address space, not the address space that holds every session's `CapabilitySet`/
`BoundCapability` state) and finding 2 (round 1 correctly showed Revision 1's "worker thread +
cancellation" claim did NOT actually match its own cited precedent — this revision fixes that by
making the mechanism ACTUALLY match: a real child process, killed by the OS, the same as those two
runners already do, not a thread-termination primitive that risks corrupting PDFium's process-global
`FPDF_InitLibrary` state for concurrent calls).

Consequence for §2's shared helper: `fetch_source_bytes` still runs in the HOST process (it only
touches already-mediated `FileSystemAdapter`/`HostEgressProxy` seams, no untrusted-format parsing) —
only the PDFium call itself crosses the process boundary, host process → child, fetched bytes as
input, extracted text as output.

**Not decided by this revision**: the exact process-boundary mechanics (a new small dedicated helper
binary this project builds and ships alongside `agentengine`, vs. some other shape); how bytes/text
cross the boundary (a pipe, a temp file, shared memory) and what mediation that crossing itself needs;
whether `MediatedPythonRunner`/`MediatedShellRunner`'s spawn machinery is directly reusable as-is or
needs its own extension for a "spawn a helper, feed it bytes, get text back" shape distinct from
"spawn an interactive interpreter/shell." This is real, non-trivial implementation-ADR work, not a
detail — flagged honestly rather than glossed as "just reuse the existing spawn code."

## 4. Resource caps (I8) — revised around the process boundary, still not sized

The child-process boundary (§3a) changes what these caps are FOR: they are no longer the only thing
standing between a hostile PDF and the host process — the process boundary already bounds worst-case
BLAST RADIUS (a runaway/corrupted child can be killed with no risk to the host). They are still needed
to bound COST (a legitimate caller shouldn't be able to tie up unbounded child-process CPU/memory/
wall-clock even without any exploit involved — I8 is about budget enforcement, not just crash safety).

1. **Input byte cap — revised, round-1 MUST-FIX 3.** Round 1 found this was false as stated in
   Revision 1: `FileSystemAdapter::read_file()` (`filesystem_adapter.hpp`) has no size parameter and
   reads a whole file unconditionally, and `cap::FsRead::size_cap_bytes` is explicitly documented in
   `capability.hpp` as unenforced anywhere in this codebase today — so for the `path` source, the
   "reject before any PDFium call" cap cannot fire before the whole file is already loaded into host-
   process memory via `fetch_source_bytes`. This revision does NOT claim that gap is closed — it
   is explicitly NOT closed by this design, and is not this tool's gap to fix (it is `read_source`/
   `FileSystemAdapter`'s). What §3a's process boundary DOES change: the byte cap's role shifts from
   "the only protection" to "avoid needlessly spawning a child process for an obviously-oversized
   input" — a cheap optimization, not the safety property. The url source remains fine as before
   (`net_egress_proxy.hpp` already enforces a real streaming byte ceiling regardless of grant
   `byte_cap`). A real size-limited read primitive for `FileSystemAdapter` is named here as a
   worthwhile, separately-scoped follow-up (it would benefit `ReadContent`'s existing `path` source
   too, not just this tool) — not required to unblock this design, since §3a's isolation already
   bounds the worse outcome (host-process memory exhaustion) that made this a MUST-FIX in Revision 1.
2. **Child-process OS resource limits — revised, replaces Revision 1's worker-thread wall-clock cap
   (round-1 MUST-FIX 2).** The child process gets a real OS-enforced ceiling — a Job Object memory/
   time limit on Windows, an `rlimit`/cgroup equivalent on Linux — sized from real measurement (not
   guessed), covering memory AND wall-clock in one mechanism tied to the process itself rather than a
   heuristic over PDFium's own page-by-page progress. This also directly closes round-1 SHOULD-FIX 6
   (a single pathological page, or an expensive document-open/xref-parse step before any page is even
   reached, is now bounded by the SAME process-wide ceiling — no separate per-page heuristic needed to
   catch a cost concentrated in one place rather than spread across pages).
3. **Page-count cap, now `pages_processed`-driven** — extraction proceeds page-by-page; if the
   process-level ceiling (#2) fires mid-document, `pages_processed` (§3) reports exactly how far it
   got, and `truncated_pages` is `true`. Needs to be a real, tested, fail-**visible** behavior — a
   killed child process must produce a real partial reply with `pages_processed` accurately reflecting
   what was actually returned before the kill, not a bare tool-call error that discards a partial
   result the child had already produced and could have returned.

None of this is optional per CLAUDE.md's "Machine safety" section. §3a/§4 together give a follow-up
red-team round concrete targets: whether the process-spawn-per-call overhead itself is an acceptable
cost, whether the OS resource-limit mechanism is actually correctly applied on both platforms, and
whether a killed child can genuinely still hand back a valid partial `pages_processed` result or
whether killing it necessarily loses everything (a real, unresolved implementation question this
revision does not claim to have answered).

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
  accepting duplication (option (a)).
- §3a: the concrete process-boundary mechanics (new helper binary vs. some other shape; how bytes/
  text cross the boundary; whether existing spawn machinery needs its own extension) — real,
  non-trivial implementation-ADR work.
- §4: exact OS resource-limit values (memory ceiling, wall-clock ceiling) — needs real measurement
  against representative and adversarial PDFs, on both platforms.
- Whether a killed child process can still yield a valid, accurately-reported partial
  `pages_processed` result, or whether that turns out to be unreliable enough that a killed call must
  instead be a hard failure with no partial reply at all (a real open question, not assumed either way).
- PDFium vendoring specifics (pin, `FetchContent` source, WASI-SDK buildability) — ADR-106 §3
  territory, unchanged.
- The skill's final prose beyond fixing round 1's factual error — still not to be shipped verbatim
  without the same review weight the existing five skills got.
- **Round 2 of red-team.** This revision closes round 1's findings; it has not yet been attacked
  itself. §3a's isolation design in particular — brand new this revision, the single biggest structural
  change — is exactly the kind of claim ("this actually matches the cited precedent now") that needs
  independent verification, not self-certification, before an implementation ADR builds against it.
