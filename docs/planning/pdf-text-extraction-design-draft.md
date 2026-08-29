# Design draft — a first-party PDF text-extraction tool, and its companion skill

**Status: draft, no red-team round yet, no code.** Continues 009 §7's "Document extraction" catalog
row after ADR-106 settled the license question (PDFium, BSD-3-Clause, not poppler/mupdf). This draft
covers the PDF-text-layer slice only — DOCX/PPTX/XLSX stays explicitly out of scope, per ADR-106 §3.

Scope was widened mid-draft, at the project owner's own direction: a first-party *tool* this size
should ship with a first-party *skill* alongside it, not the tool alone. §8f's five generic skills
(`include/agentengine/core/builtin_skills.hpp`) are the established, real, tested precedent for
exactly that pairing — `reading-large-content` already teaches the preview-then-page pattern this new
tool's own result shape reuses. This draft designs both halves together.

## 1. What already exists, reused rather than re-invented

- **The result shape**: `ReadContent`'s bounded-preview + `BlobRef`-promotion pattern
  (`read_content_detail::build_reply`, `include/agentengine/tools/read_content.hpp`, extended this
  branch with a `path` source) is source-agnostic over already-obtained bytes. A PDF-extraction tool
  should produce a `std::string` of extracted text and hand it to the SAME threshold/blob-promotion
  discipline — 006 §7's token-budget rule does not care whether the bytes came from a URL fetch, a
  sandbox file read, or a PDF decode.
- **The sourcing**: `ReadContentArgs`'s `url`-xor-`path` shape, and its two dynamic capability checks
  (`find_net_out`/`find_fs_read`), is exactly the "any granted source" 009 §7 asks for. Re-deriving a
  third sourcing mechanism for this tool would be pure duplication.
- **The skill-shipping mechanism**: `builtin_skills.hpp`'s `inline constexpr` `SKILL.md`-formatted
  string constants, parsed at construction via `parse_builtin()`/`make_builtin_skills_source()`,
  already ship five skills this exact way — compiled in, not loose files a deployment could omit.

## 2. Open design question: compose with `ReadContent`, or a standalone tool?

Two credible shapes, not yet decided (this is exactly the kind of choice this project's own README
says an ADR should record once red-teamed — flagged here, not resolved):

**(a) `ExtractPdfText` is a standalone `Tool<>`** with its own `Args{url|path}` (mirroring
`ReadContentArgs` exactly) that independently fetches bytes (via the same dynamic-capability-check
logic `ReadContent` already has) and runs PDFium over them. Simple, no coupling, but duplicates the
fetch-and-check logic between two tools — a maintenance hazard if `find_net_out`/`find_fs_read` gain
a third case later and only one of the two tools gets updated.

**(b) `ExtractPdfText` composes `ReadContent`'s fetch logic.** Requires factoring `ReadContent::
invoke_via`/`invoke_from_sandbox`'s "get me the raw bytes for this Args, after the capability check"
half out from the threshold/blob-promotion half (`build_reply`, already factored out this branch) —
a `fetch_source_bytes(Args, ctx[, proxy]) -> result<std::pair<std::string bytes, std::string
media_type>>` helper both tools call, each then doing its own thing with the bytes (`ReadContent`:
`build_reply` directly; `ExtractPdfText`: PDFium decode, THEN `build_reply` on the extracted text).
Avoids duplication, but couples the two tools' `Args` shapes together (both would need `url`/`path`,
in lockstep) — reasonable since 009 §7 already frames "any granted source" as the shape every
content-touching tool in this catalog should share, but worth a red-team pass on whether that
coupling is load-bearing or accidental.

**Leaning (b)** for the reuse win, but this is exactly the kind of question a red-team round should
attack before it's locked in, not something this draft decides unilaterally.

## 3. The tool: `ExtractPdfText`

```
Args:  { url?: string, path?: string }          -- identical shape to ReadContentArgs
Reply: { preview: string, truncated: bool, total_bytes: uint64, page_count: uint32,
         truncated_pages: bool, blob?: BlobRef }
```

- Capability model: identical to `ReadContent` — `Capabilities<>` empty static ceiling, dynamic
  `find_net_out`/`find_fs_read` check before any fetch, same reasoning (the target isn't known until
  call time). No NEW capability kind — parsing is not a distinct authority from reading; the source
  capability that already gates "can this call see these bytes at all" is the right (and only) gate.
- `EffectClass<effect_class::pure>` — no side effects, matches `ReadContent`/`ReadSandboxFile`.
- Extracted TEXT goes through `build_reply`, unmodified from how `ReadContent` already uses it —
  `total_bytes`/`preview`/`truncated`/`blob` all mean the same thing they mean today, just measured
  against extracted text instead of raw fetched bytes.
- `page_count`/`truncated_pages`: PDFium extracts page-by-page; a resource cap (§4) may stop before
  every page is processed on a very large document. `truncated_pages` must be a DISTINCT flag from
  `truncated` (the preview-vs-full-text flag `build_reply` already sets) — "the whole document was
  processed but the resulting text was long" and "processing itself was cut short" are different
  facts a caller needs to tell apart, the same "don't conflate two different truncation reasons"
  discipline `ReadContentReply`'s own fields already draw between `truncated` (this reply) and a
  denied/failed call (an `error`, not a reply at all).

## 4. Resource caps (I8) — not yet sized, must be measured, not guessed

PDFium is a hardened, fuzzed decoder, but "hardened" bounds crash/memory-corruption risk, not
resource exhaustion — a legitimately-parseable, non-malicious-looking PDF can still have thousands of
pages, deeply nested content streams, or huge embedded images, and a genuinely malicious one can be
built specifically to maximize CPU/memory for its file size (the PDF format's own compression/
content-stream-recursion features make this a real, well-known category, independent of decoder
bugs). Three caps this design names but does NOT size (that's the follow-up ADR's measured-evidence
job, per this project's own "measured numbers, not argument" ADR discipline):

1. **Input byte cap** — reject before ANY PDFium call if the source bytes exceed a configured
   ceiling. Cheapest possible check, closes the trivial case for free.
2. **Wall-clock cap on the decode itself** — PDFium's own C API is synchronous; this needs either a
   real timeout mechanism (a worker thread + cancellation, matching how `MediatedPythonRunner`/
   `MediatedShellRunner` already bound their own execution) or a page-count/page-complexity heuristic
   cap enforced BETWEEN pages (cheaper, but only catches slowness that's spread across pages, not one
   pathological page). Needs a real measured worst-case before either is sized.
3. **Page-count cap** — `truncated_pages` (§3) is what a caller sees when this cap stops extraction
   early; needs to be a real, tested, fail-**visible** (not silent) behavior, not merely "stop the
   loop and hope the partial result reads as complete."

None of these are optional per CLAUDE.md's own "Machine safety" section ("hostile tests are
resource-capped... must not be able to take the machine with it") — this section exists so the
follow-up implementation ADR does not have to rediscover that requirement from scratch, and so its
red-team round has three concrete, named targets to attack rather than a blank page.

## 5. The companion skill: `extracting-document-text`

Sixth entry in `builtin_skills.hpp`'s pattern (§8f), added the same way as the existing five: an
`inline constexpr std::string_view` `SKILL.md`-formatted constant, parsed via `parse_builtin()`,
listed in `make_builtin_skills_source()`'s `entries[]` array. Draft frontmatter/content shape (not
final — this is what the follow-up implementation ADR's actual constant should resemble, written now
so the tool and its documentation are designed together, not the tool first and the skill bolted on
after):

```
---
name: extracting-document-text
description: When and how to use extract_pdf_text instead of reading a PDF's raw bytes -- it returns
  decoded text, not a file to parse yourself, and follows the same preview-then-page discipline as
  any other large tool result. Use this before calling extract_pdf_text, and alongside
  reading-large-content for the paging half.
allowed-tools: extract_pdf_text
metadata:
  version: "1"
---
```

Body content should cover, drawing directly on `reading-large-content`'s already-established voice
rather than restating it: (a) `extract_pdf_text` returns DECODED TEXT, not the PDF's raw bytes —
never fetch a PDF via `read_content`/`extract_pdf_text`'s `path`/`url` source and then try to parse it
yourself in the code interpreter; (b) `page_count`/`truncated_pages` tell you when a huge document was
only partially processed — treat that the same as any other "this is a reference to more, not the
whole thing" signal `reading-large-content` already teaches; (c) prefer requesting a narrower `path`/
`url` source (a specific file) over expecting the tool to filter pages itself — it extracts, it does
not search.

## 6. What this draft does NOT decide

- §2's (a)-vs-(b) composition question.
- Exact cap values (§4) — needs real measurement against representative and adversarial PDFs.
- PDFium vendoring specifics (which pin, `FetchContent` source, WASI-SDK buildability for a future
  WASM port) — ADR-106 §3 already named this as implementation-ADR territory, unchanged here.
- The skill's final prose (§5's draft is directional, not to be shipped verbatim without review the
  same weight the existing five skills got — each of those "is written against the actual spec
  section it teaches... a stale or wrong claim here is a real bug against those sections, not just
  prose," per `builtin_skills.hpp`'s own file-top comment; this draft's skill text hasn't earned that
  yet).
- The actual red-team round. This draft is the "design" step of "design → red-team → prove → judge" —
  §4's resource-cap section exists specifically to give that round real, concrete surface area to
  attack rather than starting from nothing.
