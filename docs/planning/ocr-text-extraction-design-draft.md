# Design draft — a first-party OCR tool, Track C of the native-C++ first-party tool effort

**Status: Revision 1 — no red-team round yet.** This is a first design pass only, following Track A
(`read_content`, `include/agentengine/tools/read_content.hpp`) and Track B (the four `extract_pdf_*`
tools, PDFium-backed, `docs/planning/pdf-text-extraction-design-draft.md` Revision 7 after six real
red-team rounds). Unlike that draft's own final state, this one has NOT been adversarially reviewed —
say so plainly rather than borrowing Track B's revision count. No code, no vendoring, no ADR yet.
Continues `009-Plugin-and-Extension-System.md` §7's "OCR" catalog row: "Tesseract-class... Reading
text from scanned/image-only pages no text layer covers — not a `ChatClient` call, but imperfect;
deterministic-tier alongside text-layer extraction, not attributed like tier 3 below." Also responds
to `docs/planning/v1-office-user-toolkit.md` §5, which already names Tesseract OCR as a real, unmet
office-user need ("no pure-Python equivalent of comparable quality").

## 1. What already exists, reused rather than re-invented

Everything below was verified against the real, currently-shipped code, not assumed from Track B's
own (partly superseded) draft text:

- **Sourcing.** `extract_pdf_text.hpp` did NOT adopt its own draft's proposed shared `read_source::
  fetch_source_bytes` helper (that was left "not decided" and shipped as option (a), fully
  standalone). The real, shipped pattern is: `url`/`path` `Args` (mirroring `ReadContentArgs`'s
  exactly-one-of-two contract), a small standalone `fetch_via_url`/`fetch_via_path` pair reusing only
  `read_content.hpp`'s free `parse_read_content_url`/`net_out_target_string` helpers for URL parsing,
  with its own capability checks (`find_net_out`/`find_fs_read`) and its own error codes. This draft
  reuses that exact, already-proven pattern for the OCR tool rather than re-opening the shared-helper
  question a second time — two real precedents (`ReadContent`, `ExtractPdfText`) choosing standalone
  over shared is itself now evidence for what this codebase's own judgment favors here.
- **Isolation.** One-shot `NativeJailBackend::create()`/`exec()` (Windows) / `LinuxNativeJailBackend`
  (Linux), spawning a dedicated per-call worker binary — never linking the OCR library into the host
  process. `NativeJailBackend::grant_ro_path_once()` for a fixed scratch directory (Windows-only
  concern; Linux's private mount namespace per `exec()` call has no equivalent cross-session exposure,
  per Track B's own round-6 red-team finding, still true here). The Linux toolchain-mount residual
  (`/bin`/`/lib`/`/lib64`/`/usr`, because `LinuxNativeJailBackend::exec()` hardcodes `/bin/sh -c`) is
  identical and inherited, not re-derived.
- **Worker output framing.** `pdf_worker_main.cpp`'s fixed 16-byte binary record header (magic/kind/
  index/payload_len) plus a self-queried output-byte ceiling (`F_GETPIPE_SZ` on Linux, a conservative
  fixed constant under Windows' 1 MiB `CreatePipe` request) is a real, working, already-battle-tested
  design for "a worker that must never block on a pipe `exec()` only drains after exit." An OCR worker
  reuses this shape directly (its own magic/kind constants, not literally the same header, since the
  payload semantics differ) rather than re-deriving a new wire format.
- **Companion-skill mechanism.** `builtin_skills_detail::parse_builtin()` + `InlineSkillSource` (as of
  commit `b23eaba`, both first-party skill sources now literally construct `InlineSkillSource`) is the
  proven, real mechanism for shipping a compiled-in `SKILL.md`. Reused directly; see §5 for whether
  this tool joins an existing skill or gets its own.
- **Vendoring pattern to match or explicitly fail to match.** PDFium is vendored as a pinned,
  SHA256-checksummed, prebuilt release fetched via `FetchContent_Declare`/`FetchContent_MakeAvailable`
  from `bblanchon/pdfium-binaries`'s GitHub releases (`CMakeLists.txt` ~line 917-982) — chosen
  specifically because building PDFium from source needs Chromium's own `gn`/`ninja`/`depot_tools`
  toolchain and a multi-GB checkout, "genuinely infeasible... as part of this project's own build."
  §7 below checks whether an equivalent exists for Tesseract/Leptonica, and finds the honest answer is
  no — a materially different, and in one way easier, vendoring shape than PDFium's.

## 2. License check (ADR-106-style: checked directly, not assumed)

Verified directly against each project's own license file, the same discipline `decisions/ADR-106-
document-extraction-catalog-candidate-license-substitution.md` §2 used for poppler/mupdf/libxml2:

- **Tesseract** (`tesseract-ocr/tesseract`): **Apache License, Version 2.0** — verified against the
  repository's own `LICENSE` file (`raw.githubusercontent.com/tesseract-ocr/tesseract/main/LICENSE`),
  header text "Apache License / Version 2.0, January 2004". Permissive, no copyleft obligation.
- **Leptonica** (`DanBloomberg/leptonica`) — Tesseract's real image-processing dependency (pixel
  buffers, image I/O, morphology; Tesseract's own build requires it, it is not optional): **BSD
  2-Clause ("Simplified BSD") License** — verified against the repository's own `leptonica-
  license.txt`. Permissive, no copyleft obligation.
- **Trained language data** (`tesseract-ocr/tessdata_fast` — the smaller, faster-inference model tier;
  `tessdata`/`tessdata_best` are the same project's slower/higher-accuracy tiers, same licensing
  posture): **Apache-2.0**, verified against that repository's own README, which states directly "All
  data in the repository are licensed under the Apache-2.0 License, see file LICENSE."

**Consequence, stated the same way ADR-106 stated its own finding:** unlike poppler (GPL) / mupdf
(AGPL/commercial), there is **no license conflict here at all** — Tesseract, Leptonica, and the
`tessdata_fast` language-data package are all permissively licensed (Apache-2.0 / BSD-2-Clause),
composing cleanly with this project's own locked MIT license the same way wasmtime (Apache-2.0),
mbedTLS (Apache-2.0), and PDFium (BSD-3-Clause) already do. **This candidate does not need an ADR-106-
shaped substitution decision** — no standalone licensing ADR is required before an implementation ADR,
because there is no conflict to resolve. (A short ADR recording this finding may still be worth
writing purely for the paper trail future contributors would otherwise have to re-derive — that is a
process call for the project owner, not something this design draft needs to force.)

## 3. Vendoring feasibility — the real open question here, and a materially different shape than Track B's

This is genuinely less certain than the license question, and the honest finding is: **no direct
equivalent of `bblanchon/pdfium-binaries` exists for Tesseract+Leptonica** — no actively-maintained,
purpose-built, cross-platform (Windows AND Linux), pinned-release, checksummed prebuilt-binary
distribution comparable to what PDFium's ecosystem offers. What actually exists, checked directly:

- **vcpkg** ships a real `tesseract` port. `vcpkg install tesseract:x64-windows-static` produces a
  genuinely self-contained static build with no runtime DLLs — a real, usable path for Windows.
  **Linux is worse via vcpkg**: the `x64-linux-static` triplet is not supported for this port (only
  the dynamic `x64-linux` triplet is) — so vcpkg alone does not give the same clean static story on
  both platforms the way PDFium's single prebuilt-binaries project does.
- **conda-forge** carries a `tesseract` package for multiple platforms, but consuming a conda package
  through this project's existing `FetchContent_Declare(URL ... URL_HASH ...)` pattern is not the same
  shape as a plain release tarball — it would mean either standing up conda tooling as a build-time
  dependency (a new, heavier moving part this project has not needed for any other vendored library)
  or hand-extracting conda package contents outside conda's own resolver, which is fragile and not
  really "using conda-forge" as designed. Not ruled out, but not a drop-in fit either.
- **Unofficial prebuilt-binary GitHub projects exist** (`ollydev/libTesseract` — a single statically-
  linked shared library across platforms via GitHub Releases, structurally the closest analog to
  `bblanchon/pdfium-binaries`'s shape; `DanielMYT/tesseract-static` — Linux x86_64/aarch64 only) but
  both are small, single-or-few-maintainer projects, not an actively-maintained, widely-depended-on
  project the way `bblanchon/pdfium-binaries` is (that project is itself a well-known, heavily-used
  mirror specifically because Google's own PDFium ships no prebuilt binaries at all). Pinning a
  foundational, security-relevant dependency (untrusted-image decoding, matching 009 §7's own "hostile
  input" framing for this catalog row) to a much smaller, less-audited third-party binary mirror is a
  real, different trust posture than PDFium's case, not a like-for-like substitute — this draft does
  **not** recommend it without the project owner's own visibility into that trade, the same posture
  ADR-106 itself took ("awaiting the project owner's own review... even though this session judges the
  technical substitution itself to be low-risk").
- **No official Windows binary distribution** exists from the Tesseract project itself. The
  community-standard fallback (UB-Mannheim's installer, `github.com/UB-Mannheim/tesseract`,
  Apache-2.0, widely trusted and used by most Windows Tesseract consumers) ships as an **installer
  executable**, not a plain extractable archive — a structurally worse fit for this project's
  `FetchContent_Declare(URL ...)`-a-tarball pattern than PDFium's `.tgz` releases, and would need its
  own extraction/silent-install handling this project has not built before.

**The one genuine silver lining, and a real structural difference from Track B**: Tesseract and
Leptonica are ordinary CMake-buildable C/C++ projects (Leptonica ships a standard CMake build;
Tesseract's own build system is CMake-based too) — **not** Chromium's `gn`/`ninja`/`depot_tools`
multi-GB toolchain that made building PDFium from source "genuinely infeasible" for this project.
Where PDFium's problem was "infeasible to build, easy to get prebuilt," Tesseract's problem may be the
**inverse**: no equally clean prebuilt story, but a real, buildable-from-source option this project's
own existing CMake `FetchContent` machinery could plausibly drive directly (fetch Leptonica + Tesseract
source releases, build both as ordinary CMake subprojects, same category of work as the WASM Component
Model's own from-source pieces elsewhere in this build) — this has NOT been prototyped or timed here,
and is the single most important next-step question (see §8).

**This draft's recommendation**: do not commit to a vendoring mechanism yet. A real, short, timeboxed
spike — attempt an actual from-source CMake build of Leptonica + Tesseract on both Windows and Linux,
and separately attempt the vcpkg-static path on Windows — is genuinely necessary before an
implementation ADR could respond to review with real numbers, the same way ADR-106 could name PDFium
confidently only because `bblanchon/pdfium-binaries`' existence was a known, checkable fact rather than
a guess.

## 4. The tool's own shape — scoped to images, not PDF pages, and say why

**Args/Reply:**

```
Args:  { url?: string, path?: string, language?: string }
Reply: { preview: string, truncated: bool, total_bytes: uint64, mean_confidence: float,
         truncated_output: bool, blob?: BlobRef }
```

- `url`/`path`: exactly one of the two, identical contract to `ReadContentArgs`/`ExtractPdfTextArgs`.
  Accepts an ordinary raster image (PNG/JPEG/TIFF/BMP — whatever Leptonica's own `pixRead`/`pixReadMem`
  format support covers; the exact supported-format list is real implementation-ADR work, not decided
  here).
- `language`: optional, defaults to `"eng"`. Only a language this build actually vendored trained data
  for is valid — see the deliberate v1 scope-narrowing below.
- `mean_confidence`: Tesseract's own `TessBaseAPI::MeanTextConf()` (0-100, its own OCR-confidence
  estimate) — a real, cheap, already-computed signal worth surfacing so a caller (or the model reading
  the reply) can treat low-confidence OCR output with appropriate suspicion (I3-adjacent honesty: OCR
  output is imperfect by construction, 009 §7's own framing, and this field is the tool being upfront
  about that rather than presenting recognized text as ground truth).
- `preview`/`truncated`/`total_bytes`/`blob`: identical bounded-preview + `BlobRef`-promotion shape
  `read_content_detail::build_reply`/`extract_pdf_text_detail::build_reply` both already use — no new
  pattern invented for this tool.
- `truncated_output`: distinct signal from `truncated` (which is the byte-threshold preview split).
  This one means the WORKER's own output ceiling or a real recognition-time cap stopped OCR before the
  whole image was processed (see §6 item 3) — matching `ExtractPdfTextReply`'s own real precedent of
  keeping "the preview was cut for display" and "the underlying extraction itself didn't finish"
  as two separate booleans (`truncated` vs. `truncated_pages`), not one overloaded flag.

**Deliberately, explicitly out of scope for this revision (named, not silently dropped, per ADR-106's
own disclosure discipline):**

- **OCR of a PDF's own scanned pages.** 009 §7's OCR row explicitly frames this as the motivating case
  ("scanned/image-only pages no text layer covers"), but doing it for real requires RASTERIZING a PDF
  page to a pixel buffer first (PDFium's `FPDF_RenderPageBitmap`), a capability `pdf_worker_main.cpp`
  does not have today — that worker only ever calls `FPDFText_*`/`FPDFPage_*`/`FPDFImageObj_*`
  (metadata-only) APIs, never anything that rasterizes a page. Bolting page rendering onto the OCR
  tool's own worker would blur two genuinely separate capabilities (image decode+recognize vs. PDF
  page rendering) into one binary for no real reuse benefit. **Recommendation**: this tool stays
  image-only for v1; "render a PDF page to an image" is a separate, not-yet-designed capability (maybe
  its own future catalog candidate, maybe a mode added to the existing PDF worker) that a LATER
  revision of this draft — or a new one — should design once it's actually needed, not assumed away
  quietly here.
- **Multiple languages / multi-language vendoring.** Each language needs its own `.traineddata` file
  (real, non-trivial vendored bytes each — `tessdata_fast`'s English model alone is several MB); this
  draft assumes English-only (`eng.traineddata`) for v1, matching `tessdata_fast`'s own per-language
  file granularity, and defers a multi-language vendoring/selection story entirely.
- **Layout-aware output** (bounding boxes, per-word/line geometry, table/form structure). 009 §7's own
  table already names a SEPARATE candidate row for this — "Layout-aware document extraction (VLM
  tier)... recovering table/form structure that tiers 1-2 can't... the attributed, budgeted,
  determinism-waived tier." This tool's own OCR row is explicitly "deterministic-tier," so staying at
  plain recognized text (no geometry) keeps this candidate inside its own row's stated scope rather
  than quietly absorbing the VLM-tier row's job.

## 5. Isolation posture and resource caps (I8)

Reuses Track B's real, shipped mechanism almost exactly, with the one deviation named and justified:

- One-shot `NativeJailBackend::create()`/`exec()` per call (Windows) / `LinuxNativeJailBackend`
  (Linux) — not a persistent worker, matching Track B's own Revision 4 finding that a persistent-
  worker shape is not uniformly available on both platforms in this codebase today.
- A NEW fixed, read-only grant for wherever the vendored `tessdata` directory lives (via the same
  `NativeJailBackend::grant_ro_path_once()` dedup mechanism the PDF worker's own `worker_dir` grant
  already uses) — Tesseract's engine initialization (`TessBaseAPI::Init()`) needs to read the
  `.traineddata` file(s) off disk at worker startup; this is a real, additional grant beyond what
  Track B needed, not a copy-paste of an existing one.
- Scratch-file sourcing: identical shape to `extract_pdf_text.hpp`'s own `fetch_via_url`/
  `fetch_via_path`/`scratch_dir()`/`unique_scratch_filename()` — the same already-accepted residual
  (`FileSystemAdapter::read_file()` has no size limit; not a new defect this tool introduces) applies
  identically, inherited rather than re-solved.
- **Resource caps — deliberately NOT copied verbatim from Track B's own numbers, and named as
  unmeasured, matching that draft's own honesty about `wall_ms`/`memory_bytes` needing real
  measurement before being trusted at a specific value:**
  - `memory_bytes`: Tesseract's own working set (Leptonica's decoded pixel buffer + the LSTM
    recognition model's own loaded weights, which for `tessdata_fast`'s English model is itself
    several MB, plus per-page recognition scratch memory) is a real, likely-larger footprint than
    PDFium's pure-parse 256 MiB (`kWorkerMemoryBytes`, `extract_pdf_text.hpp`). This draft's
    placeholder is 512 MiB — explicitly a guess pending real measurement, not a benchmarked value.
  - `wall_ms`: OCR recognition (segmentation + LSTM inference per line/word) is real, non-trivial
    per-page compute, plausibly seconds rather than PDFium's fast parse-only extraction. This draft's
    placeholder is 30000 ms — again explicitly unmeasured.
  - **Model-load cost is a NEW, real, per-call cost Track B never had.** Every one-shot worker
    invocation means every OCR call re-reads and re-initializes the `.traineddata` model from disk —
    Tesseract's own `TessBaseAPI::Init()` is not cheap. This is a heavier version of the same
    "spawn-rate cost, not yet solved" gap Track B's own §4 item 3 already named honestly and left
    open; for OCR specifically it is worse because the per-call fixed cost (model load) is larger
    relative to the per-call useful work (recognizing one image) than PDFium's own per-call fixed
    cost (opening a PDF) was. Not solved here — named, the same way Track B named its own unsolved
    spawn-rate gap rather than silently accepting it without disclosure.
  - Output-byte ceiling: same shape as `pdf_worker_main.cpp`'s own self-queried `F_GETPIPE_SZ`
    (Linux) / fixed-margin-under-1-MiB (Windows) discipline, reused directly — no new design needed
    here, the mechanism is source-agnostic to what's being written.

## 6. Companion skill — a NEW skill, not an extension of `extracting-document-text`

`extract_pdf_images.hpp`/`extract_pdf_metadata.hpp`/`extract_pdf_toc.hpp` were each folded into the
SAME `extracting-document-text` skill's `allowed-tools` (their own top comments say so explicitly) —
but that skill's own name, description, and body are all specifically about PDFs ("When and how to use
extract_pdf_text/extract_pdf_toc/extract_pdf_images/extract_pdf_metadata instead of reading a PDF's
raw bytes"). Given §4's scope decision (image-only for v1, PDF-page OCR explicitly deferred), this
tool's actual input is a different content type than that skill teaches — folding it in would mean a
"document text" skill silently starting to talk about scanned photographs, a real content mismatch,
not just a naming inconvenience.

**Recommendation: a new skill**, tentatively named `extracting-image-text`, describing when to reach
for OCR (a scanned document, a photographed receipt/form/sign — anything where the content is pixels,
not encoded text) versus `read_content` (which returns raw bytes a model cannot itself decode into
text from an image) or `extract_pdf_text` (which only works when a PDF already has an embedded text
layer). Should explicitly note, honestly, that `mean_confidence` exists and low values mean the
recognized text may be wrong — this tool's own §4 disclosure, surfaced to the model too, not just to
this draft. When/if PDF-page rasterization+OCR composition (§4's deferred item) ships, this skill is
the natural place to extend, not `extracting-document-text` — but that's a future revision's call, not
locked in here.

## 7. What this draft does NOT decide

- §3: the actual vendoring mechanism (from-source CMake build vs. vcpkg-static vs. an unofficial
  prebuilt mirror) — genuinely open, needs the spike named there before an implementation ADR could
  make this call with real evidence instead of a guess.
- §4: exact supported image format list (Leptonica's own real format-support matrix needs checking
  against what this project would actually want to accept); the PDF-page-rasterization capability
  named as deferred; multi-language support.
- §5: real `memory_bytes`/`wall_ms` values (placeholders only, explicitly unmeasured); the model-load
  spawn-rate cost (named, not solved); the exact `tessdata` grant path/layout.
- §6: the skill's own final prose — a name and a rough shape only, not review-ready text, matching
  Track B's own draft's treatment of its skill section before that project's later revisions refined
  it.
- Whether this ships as a native `Tool<>` (matching Track A/B's own current precedent — "no production
  plugin loader exists yet") or waits for the WASM plugin loader — same open question Track B's own
  ADR-106 left open, unchanged here.
- **Any red-team round at all.** This is Revision 1. Track B's own draft was not implementation-ready
  until Revision 7, after six real adversarial passes found real defects (a genuinely unreachable
  internal API, a false cross-platform pipe-buffer-parity claim, an unsequenceable host/worker data-
  flow design) that this first pass has had no chance to surface yet. Treat every design choice above
  as a real, considered starting position, not a settled one.
