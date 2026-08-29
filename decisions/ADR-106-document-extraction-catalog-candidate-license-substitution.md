# ADR-106 — 009 §7's "Document extraction" candidate names poppler/mupdf; does a locked MIT license let either ship?

**Status:** Proposed — design phase only. No red-team round, no implementation, no vendoring has
happened yet. This ADR exists to record a licensing finding and the resulting substitution decision
*before* any code or CMake `FetchContent` wiring is written, per this project's own "design → red-team
→ prove → judge" discipline (`decisions/README.md`) — writing the vendoring first and discovering the
license conflict after would be exactly the ad-hoc change that discipline exists to prevent. Full
red-team (hostile-PDF-parsing-in-process is genuinely security-relevant — untrusted file, in-process
decoder, matches 009 §7's own "Parsing hostile files in-process" framing for this catalog row) and a
real implementation pass are both explicitly deferred to a follow-up ADR, the same design-acceptance-
then-implementation split this project already uses (ADR-099 → ADR-102).

**Relates to:** `009-Plugin-and-Extension-System.md` §7 (the generic tool catalog table this ADR
narrows one row of), `OpenQuestions.md` OQ-17 (the catalog itself), `LICENSE` (MIT, locked per
CLAUDE.md's "Locked decisions" section, resolved 2026-08-04), `docs/research/2026-08-24-dify-ai-
feature-comparison.md` (names the built-in-tools gap this whole first-party-tools effort responds to),
`include/agentengine/tools/read_content.hpp` (this catalog's first shipped candidate, Track A/B,
`e6d7b2a`/this branch's own prior commit — the `read_content`-class shape a document-extraction tool
should plausibly reuse: bounded preview + `BlobRef` promotion, dynamic capability check).

## 1. The question

**Stated so it has a wrong answer:** 009 §7's own table names the "Document extraction (text layer)"
candidate as "poppler / mupdf-class PDF, libxml2, and a DOCX/PPTX/XLSX structural parser." Can this
project vendor poppler or mupdf as written, or does the project's own locked MIT-license decision rule
both out, requiring a different library choice before any implementation ADR can proceed?

## 2. What checking the actual licenses found

Not assumed from memory — checked directly against each project's own license file/statement:

- **poppler**: GPL v2 (or later, at the distributor's option) for the bulk of the codebase (it is a
  fork of `xpdf`, itself GPL). Some individual files carry LGPL/BSD headers, but the library as a
  whole — and specifically `libpoppler` as most consumers link it — is GPL-licensed. Static-linking a
  GPL library into an MIT-licensed binary places the combined work under GPL obligations for the whole
  binary; there is no LGPL-style "link dynamically and you're fine" escape for a GPL dependency.
- **mupdf**: dual-licensed — AGPL v3, or a paid commercial license from Artifex Software. AGPL is
  stricter than GPL (it extends copyleft to network use, not just distribution). Shipping AgentEngine
  — a library other projects embed and often expose over a network boundary (011/012/021/039's own
  inbound-protocol RFCs) — under an AGPL dependency would be a materially worse outcome than GPL, not
  a comparable one.
- **libxml2**: MIT License (verified against its own `Copyright` file text — "Permission is hereby
  granted, free of charge..."). No conflict. Unaffected by this ADR; still the right choice for the
  XML half of this catalog row once that half is scoped.

**Consequence:** shipping poppler or mupdf as written in 009 §7 would either put the whole compiled
`agentengine` binary under GPL/AGPL terms (if statically linked, this project's `FetchContent`-based
vendoring pattern for every other dependency — wasmtime, mbedTLS, CPython — is exactly that: build-
owned, statically-composed, not a system package a consumer supplies at link time) or require every
downstream consumer to separately acquire a commercial mupdf license just to use a document-extraction
tool that's supposed to be a zero-friction built-in. Both outcomes contradict the "licence is MIT,
decided" locked decision (CLAUDE.md) and the whole point of a first-party catalog (009 §7: "so
applications stop reinventing the same dozen capability-crossing operations" — not so they inherit a
copyleft obligation for using one).

## 3. The substitution decision

**PDFium** (BSD-3-Clause, Google — the PDF rendering/parsing engine `Chromium`/`Chrome` itself ships)
replaces poppler/mupdf for the PDF half of this catalog row. Checked: BSD-3-Clause is a permissive
license with no copyleft/relink obligation, compatible with static-linking into an MIT-licensed
binary the same way this project already statically links wasmtime (Apache-2.0) and mbedTLS
(Apache-2.0) — both permissive, both already proven to compose cleanly with this project's MIT
license and its existing `FetchContent_Declare`/`FetchContent_MakeAvailable` vendoring pattern
(`CMakeLists.txt`'s `ae_vendored_wasmtime`/`ae_vendored_mbedtls`/`ae_vendored_python` precedent).
Chosen over other permissive options (e.g., writing a bespoke minimal PDF text-layer parser) because
PDFium is a real, actively-maintained, security-fuzzed (Chromium's own OSS-Fuzz corpus) decoder for a
format this catalog row explicitly frames as hostile input — reuse of a hardened parser is exactly
009 §7's own stated program ("wrap high-value open-source native libraries... so that capability-heavy
functionality is available without host trust"), not a corner cut.

**Not decided by this ADR, left explicitly open for the follow-up implementation ADR:**
- The DOCX/PPTX/XLSX structural parser third of this catalog row. 009 §7's own text names "Docling/
  MarkItDown-class" as the shape wanted (one structural representation — layout, reading order, table
  cells — across formats), but both of those are Python frameworks, not embeddable C++ libraries, and
  this pass did not do the research needed to find a permissively-licensed C++ equivalent. Rather than
  either block PDFium's substitution on that unresolved research or silently drop it, this ADR scopes
  itself to PDF-only and defers DOCX/PPTX/XLSX to a separate follow-up (implementation should not
  quietly narrow scope without saying so, per this project's own disclosure discipline).
- Vendoring mechanics (which PDFium build/branch to pin, whether to use Google's own `pdfium` repo
  directly or a friendlier prebuilt-binaries mirror, WASI-SDK buildability for a future WASM Component
  Model port per 009 §7's own plugin-ABI framing) — a real implementation ADR's job, not a license
  ruling's.
- Whether this ships as a native `Tool<>` (matching `read_content.hpp`'s current precedent — "no
  production plugin loader exists yet") or waits for the WASM plugin loader.
- The actual red-team pass this project's own README requires before any security-relevant design is
  implemented (hostile-PDF-parsing-in-process is squarely in that category).

## 4. Decision

For any future first-party "Document extraction" tool, **PDFium, not poppler/mupdf**, is the PDF
text-layer engine this project vendors. This binds the choice of library; it does not itself
authorize implementation. A follow-up ADR must still run this project's design → red-team → prove →
judge loop — including a real adversarial pass on hostile-PDF handling — before any such tool ships,
matching the split this project already used for the identity-native sandbox/worktree design
(ADR-099 accepted the design; ADR-102 did the implementation and its own red-team rounds separately).

**Awaiting the project owner's own review** — a license/legal-adjacent decision, even though this
session judges the technical substitution itself to be low-risk (BSD-3 is uncontroversially compatible
with MIT), per this project's own norm that consequential decisions get owner visibility before being
treated as settled.
