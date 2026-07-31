# v1 target persona: office user — builtin commands and package list

**Status:** Planning input, not a spec. This is a concrete, curated cut that answers 010 §10 Q1
("whether to ship a curated first-party interpreter image") for one persona; it does not amend 010,
009, or 029 — it is the kind of evidence those RFCs' promotion gates ask for before a default is
locked. Update or replace freely as the persona is validated.

## Why office user, first

The largest addressable population doing bounded, well-understood tasks: documents, spreadsheets,
presentations, PDFs, email, reports. The primitives this project already has fit this persona with
almost no exotic capabilities — `FsRead`/`FsWrite` on the worktree (025), moderate CPU/memory, no
GPU, no long-lived background jobs. It's a good first cut precisely because it stress-tests the
"ordinary environment" claim (026 §1) against ordinary work, not against an adversarial or
specialist workload.

**Scope discipline**, per CONVENTIONS' dependency tiers: everything below is either

1. a `ShellRunner` **builtin** (010 §2 — engine-native, no process, no binary),
2. a **pinned, pure-Python package** in the `preinstalled` policy image (010 §5) — no native
   extension, so it imposes no additional platform-build burden beyond the interpreter itself,
3. a **pinned, native/binary-wheel package** — needs a compiled extension per platform, or
4. a candidate **`ae:tool` plugin** (009 §7) — a separate process or component `ShellRunner`/CodeAct
   dispatch to, never bundled into the Python process itself.

Every entry is justified by a task this persona actually does. "Generally useful" is not a
justification — CONVENTIONS explicitly rejects growing the core for hypothetical need.

## 1. `ShellRunner` builtins — extending 010 §2's fixed set

010 §2 already names `cd`, `pwd`, `ls`, `cat`, `echo`, `export`, `mkdir`, `rm`, `mv`, `cp`. For an
office user, whose recurring problem is "which of these forty files has the client's name in it,"
a small, justified extension:

| Builtin | Why this persona needs it | Backed by |
|---|---|---|
| `find` | Locate a file by name/pattern across a worktree with many documents | Worktree tree walk (025 §2) |
| `grep` | Search file *contents* — the actual "which doc mentions Acme Corp" case | Read + match over worktree blobs |
| `wc` | Quick line/word count on an export before deciding whether to open it in Python | Worktree read |
| `head` / `tail` | Peek at a large CSV/export without loading it fully | Worktree read, bounded |
| `diff` | Compare two versions of a document/export (`report-v1.csv` vs `report-v2.csv`) | Worktree read, standard diff algorithm |
| `touch` | Create a placeholder file | Worktree write |
| `sort` / `uniq` | Trivial one-line dedup/ordering of an exported list | Worktree read/write |

All of these stay builtins, not Tools: they operate only on the worktree through capabilities
already granted, need no external binary, and are cheap enough to implement directly (010 §2's
"fixed builtin set" already commits to this shape). **Not** builtins, deliberately: anything
format-aware (opening a `.docx`, converting a file) — that needs a real library or a Tool, listed
below, because a shell builtin that starts parsing Office XML is scope creep on the parser-safety
concern already flagged in 010 Q7.

## 2. Python — stdlib only (already present, no install)

`csv` · `json` · `sqlite3` · `zipfile` · `tarfile` · `email` (parse/compose `.eml`) · `smtplib` /
`imaplib` (send/read mail — requires a granted `NetOut` capability, same as any network effect) ·
`datetime` · `decimal` · `statistics` · `pathlib` · `difflib` · `textwrap` · `re` · `hashlib` ·
`uuid` · `configparser`.

## 3. Python — pure-Python packages (no native extension to build or pin per platform)

| Package | Task it serves | License |
|---|---|---|
| `openpyxl` | Read/write `.xlsx` — the single most common office format | MIT |
| `python-docx` | Read/write `.docx` | MIT |
| `python-pptx` | Read/write `.pptx` | MIT |
| `pypdf` | Read, merge, split, extract text from PDFs — pure Python, lighter than the native alternatives in §4 | BSD-3 |
| `docxtpl` | Jinja2-templated `.docx` generation — mail merge, templated reports | LGPL-3.0-or-later ⚠ |
| `Jinja2` | General templating for generated reports/emails | BSD-3 |
| `markdown` | Markdown → HTML for quick formatted output | BSD-3 |
| `python-dateutil` | Robust date parsing/arithmetic — fiscal periods, recurrence rules | Apache-2.0 / BSD dual |
| `PyYAML` | Read/write YAML config or data files | MIT |
| `tabulate` | Pretty tables from Python data — pairs well with CodeAct's "last expression printed" model (010 §1) | MIT |

⚠ `docxtpl` is LGPL — fine to *depend on* (dynamic/import-time linking, not a derivative-work
question the way static linking would be), but it is the one entry here that isn't permissive, and
024/OQ-11's licensing governance should have a documented position on LGPL Python dependencies
before this list is locked, not discover the question later.

## 4. Python — native/binary-wheel packages

These need a compiled extension per platform, available in the interpreter's real ecosystem
(010 §2) — the same PyPI packages a desktop Python install would use.

| Package | Task it serves | License |
|---|---|---|
| `pandas` (+ `numpy`) | Tabular data manipulation, CSV/Excel ingestion at any real scale | BSD-3 |
| `matplotlib` | Static charts as artifacts (bar/line/pie) embedded in a generated report | PSF-style / BSD-compatible |
| `Pillow` | Resize, convert, watermark, and embed images — logo placement, photo compression for reports | MIT-CMU ("Pillow license") |
| `pdfplumber` | Layout-aware table/text extraction from PDFs (scanned or structured), heavier than `pypdf` | MIT |
| `beautifulsoup4` | Parse/clean HTML from pasted or scraped content before it enters a document | MIT |

## 5. Not Python packages — candidate `ae:tool` plugins (009 §7)

The highest-leverage items for this persona are format **conversion** and **OCR**, and neither
belongs inside the Python process — they are external binaries or heavy native libraries, exactly
what 009's plugin track exists for.

| Candidate | Task it serves | License | Note |
|---|---|---|---|
| **Pandoc** | Universal document conversion: `.docx` ⇄ Markdown ⇄ HTML ⇄ PDF and more | GPL-2.0-or-later ⚠ | Arguably the single highest-leverage tool for this persona. Invoked as a separate process/component, not linked — the usual GPL linking concern doesn't apply, but 024/OQ-11 should record that position explicitly rather than assume it. |
| **LibreOffice** (headless, `--convert-to`) | High-fidelity conversion of legacy formats (`.doc`, `.ppt`, `.xls`) and rendering `.docx`/`.pptx` to PDF/PNG for preview | MPL-2.0 | Heavy — better suited to the `remote` profile's cluster-managed lifecycle than a per-turn `native-jail` cost. |
| **Tesseract OCR** | Text extraction from scanned documents and images — invoices, signed forms | Apache-2.0 | Common real office need; no pure-Python equivalent of comparable quality. |
| **poppler** (already 009 §7) | PDF rendering/text extraction, lighter than LibreOffice for PDF-only work | GPL-2.0-or-later ⚠ | Same invocation-not-linking note as Pandoc. |
| **libarchive / 7-Zip** (already 009 §7) | Opening `.zip`/`.7z` email attachments | BSD-2 / LGPL-2.1 | Already listed in 009 §7 for the general case; office attachments are the concrete motivating case. |

**Deliberately not on this list, and not `mupdf`:** `mupdf`-class PDF libraries are dual-licensed
**AGPL-3.0 or commercial**. AGPL's network-use clause is a materially different risk than GPL's for
a hosted engine, and it should be a deliberate legal decision if it's ever adopted, not a transitive
dependency someone picks up because "it's the PDF library 009 §7 already mentions." `poppler`
(GPL-2.0, invoked as a separate process) is the safer default for this persona's PDF needs; 009 §7's
existing table should be read with this distinction in mind.

## 6. Explicitly deferred — not this persona, not v1

- OCR/document-AI beyond Tesseract (layout-aware ML models) — a GPU/ML dependency that belongs to a
  later, different persona (analyst/developer), not this one.
- Automated mailbox sync beyond stdlib `smtplib`/`imaplib` — a connector/integration concern (a
  `Tool`, likely MCP-sourced, 011), not a core package.
- Rich/animated presentation generation beyond `python-pptx` templates — low return for this
  persona's actual tasks.
- Interactive charting (Plotly/Bokeh) — pulls in a JS-rendering dependency for interactivity this
  persona mostly doesn't need; `matplotlib`'s static output is enough for a generated report.

## 7. What this list is not

This is the **default** curated set for the office-user profile under the `preinstalled` policy
(010 §5) — it is not a ceiling. An operator can still select `allowlist` or `open` package policy
to add more, per agent, per deployment. It also does not resolve 010 §10 Q1 (who owns the pinned
image's CVE cadence) or the licensing-governance question 024/OQ-11 already tracks — it gives both
questions something concrete to be asked about instead of an abstract "the interpreter has
packages."
