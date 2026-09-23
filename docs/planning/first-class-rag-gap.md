# First-class RAG: remaining gaps and backlog

**Status:** Scoping note, not a design. Written 2026-09-23, right after ADR-180 was Judged, so the
backlog is kept in one place instead of scattered across chat history and ADR residual lists. Each
item below still needs its own `design → red-team → prove → judge` pass and ADR before it is built.

**Goal.** RAG should be a first-class feature an app developer can turn on, not a set of C++
building blocks they have to assemble themselves.

## What exists today

| Piece | Where | ADR |
|---|---|---|
| Folder ingestion, chunking, content-addressed dedup | `core/corpus_source.hpp` (`DiskCorpusSource`, `RecursiveChunker`) | 063, 180 |
| Embedder (OpenAI-compatible only) | `protocol/openai/embedder.hpp` | 063 |
| Dense index: CPU brute force, persistent snapshot | `core/vector_index.hpp` | 063, 180 |
| Dense index: GPU (Vulkan), exact brute force | `src/backends/vulkan_vector_index/` | 180 |
| Dense index: Qdrant | `protocol/qdrant/vector_index.hpp` | 180 |
| Keyword index (BM25), RRF hybrid fusion | `core/sparse_index.hpp`, `core/hybrid_rag_context_provider.hpp` | 180 |
| Context providers: auto-inject + `recall` tool, tainted rendering, text citation label | `core/vector_rag_context_provider.hpp`, `core/hybrid_rag_context_provider.hpp` | 063, 064, 180 |

## Backlog (in recommended order)

### 1. Declarative (YAML/JSON) surface for RAG — invariant I6

`core/agent_yaml_compiler.hpp` has no key for memory, RAG, or a knowledge source. RAG is reachable
only from C++, which breaks I6 ("declarative and native surfaces are equivalent") and is the single
biggest reason RAG is not first-class yet. Design questions:

- Which fields YAML may set: source, chunking, embedder, index kind, hybrid on/off, `k`.
- **Authority must not come from YAML (I2).** A YAML string naming a folder path must not be able to
  grant read access to it. The likely shape: the host registers a named data source (with its
  capability) in code, and YAML refers to it by name only. See also the use case below, item A6.

### 2. Index maintenance: `remove()` and stale-chunk cleanup

When a source file is edited or deleted, its old chunks stay searchable and citable (named residual
in `corpus_source.hpp`'s top comment and ADR-180 §7). Persistence and Qdrant make this worse: the
stale chunks now survive restarts. Needs:

- `remove(ids)` on the `VectorIndex`, `RemoteVectorIndex`, and `SparseIndex` concepts.
- Re-mount drops a changed or deleted file's old chunks (mark-and-sweep keyed off
  `CorpusChunkRecord::source_path`, or a per-file chunk-id list in a manifest).
- A batched existence check for Qdrant. Today `contains()` is one network call per chunk on re-mount.

### 3. Structured citations

`core/content.hpp` already defines `Citation`, and RFC 029 §5b says retrieval attaches one "by
construction". Today only the `⟦rag:path:lines⟧` text label is emitted. Return real `Citation`
annotations (RFC 003 §1) so an app can show sources and check them mechanically.

### 4. Qdrant collection name derived from `corpus_scope`

ADR-180 §7's most serious open residual: `QdrantVectorIndex::collection_name` is a free-form string,
so two corpora can share a collection and see each other's chunks. Derive it the way
`rag_corpus_mount()` already derives `Mount`, so the isolation holds by construction.

### 5. Retrieval quality

- A reranker concept plus one conformer (RFC 029 cites reranking as the largest single gain).
- Metadata filtering (path prefix, tag, file type).
- A small recall@k evaluation set. It would finally test the BM25 tokenizer (ASCII only, no
  stemming) and RRF `k_rrf = 60` against real data instead of assuming them.

### 6. Breadth (later)

- A local/offline embedder (also matters a lot for the use case below, item A6).
- Non-text formats: wire the existing PDF / Office / OCR extraction drafts
  (`pdf-text-extraction-design-draft.md`, `office-document-extraction-design-draft.md`,
  `ocr-text-extraction-design-draft.md`) into corpus ingestion.
- OpenTelemetry GenAI spans for embed / search / rerank.
- Contextual retrieval at ingestion time (RFC 029 §5a, optional).
- An ANN index (hnswlib) for corpora where exact brute force is too slow (ADR-180 §7).

## Use case: a large host folder as a RAG source (Windows desktop app)

**Scenario (project owner, 2026-09-23).** An agent app on Windows lets the user point at one of
their real folders (e.g. `D:\Documents\Contracts`) as a knowledge source. That folder:

- is the user's real folder, **not** in the sandbox or a worktree, and must never be copied or moved
  there;
- may be very large (tens of GB, hundreds of thousands of files);
- takes a long time to index the first time, and the app must stay usable while it does.

Today there is no defined behavior for any of the "slow" part. What the code does now, and what is
missing:

### A1. The source stays in place — mostly true already, but the text is copied

`DiskCorpusSource` reads files in place from an arbitrary root path, so the folder itself is not
moved. But every chunk's **text** is copied into the corpus worktree's object store
(`put_blob()`), plus one `CorpusChunkRecord` per chunk. For a huge folder that is a second full copy
of all its text. Decision needed:

- **Copy (today):** citations and replay (I5) stay stable even after the user edits the file.
- **Reference:** store only `{path, file hash, line range}` and re-read the file at query time,
  dropping the chunk if the hash no longer matches. No duplication, but a retrieved chunk can vanish
  or go stale between indexing and use.

### A2. Long indexing has no progress, no checkpoint, and no cancel

`DiskCorpusSource::mount()` is one all-or-nothing coroutine:

- It reads **every changed file's full content into memory**, chunks everything, then embeds
  **everything** before committing **anything**. Memory grows with the size of the folder, and
  nothing is searchable until the whole pass ends.
- One embedding failure at file 90,000 of 100,000 aborts the whole pass. The paid embedding calls for
  the first 89,999 files are thrown away (they are not committed), and the next attempt redoes them.
- There is no progress report while it runs (only counters in the final `DiskCorpusMountResult`), no
  cancel or pause, and no embedding-cost budget (I8).

Needs: commit in bounded batches with a durable checkpoint so indexing is resumable; progress events
(files scanned / total, chunks embedded, bytes, rate, errors); cancel and pause; a cost budget; and
running in the background.

**Background-task support exists, but does not fit this job as-is.** Milestone 7 Phase B built
006 §6b: the `Backgroundable` tool policy, `AgentSession::start_background_task()`, `StandingEffect`
handles with list and cancel (`core/standing_effect.hpp`, `rt/standing_effect_registry.hpp`, extracted
by ADR-097). It does **not** report progress: `background_task()` deliberately resets
`report_progress` to a no-op (ADR-060 §4). (The older `backgroundable-standingeffect-gap.md` still
says "never built"; that note is stale.) The full audit, and the fix, is
**`decisions/ADR-181-durable-cancellable-background-jobs.md`**. Gaps for a long indexing job, found
by reading that code:

- **Session-scoped.** A background task belongs to one `AgentSession`, and its completion is silently
  dropped if that session is gone. A corpus index is shared by many sessions and should outlive any
  one of them. There is no host-level background job that is not tied to a session.
- **Not persisted.** `StandingEffect` is in-memory only, so an app restart forgets the job. Resuming
  would rely entirely on the indexing checkpoint above.
- **Cancel does not stop the work.** `StandingEffectRegistry::cancel()` erases the handle; the running
  work continues and its result is dropped. Indexing needs cooperative cancellation (a token the
  ingestion loop checks between batches) so cancel actually stops the embedding spend.

These are pre-existing limits of the background-task feature itself. **They should be fixed there,
before any RAG indexing is built on top of it** (project-owner direction, 2026-09-23).

Note the tension with the current all-or-nothing rule (chosen to mirror `DiskSkillSource`): for a
large corpus, "partially indexed and honest about it" is better than "nothing until done". This
needs to be an explicit decision, not a silent change.

### A3. Searching while indexing is not defined

Once commits are incremental (A2), a query can hit a partly built index. Define what happens:
search what is there, and **tell the agent** that coverage is incomplete (e.g. "index 40% complete,
results may be missing") in the injected context and in the `recall` tool result. Results must not
look complete when they are not.

### A4. Re-indexing re-reads the whole folder

Every `mount()` opens, fully reads, and SHA-256 hashes every file, even unchanged ones. The
`previous_file_hashes` map that makes re-mount cheaper is returned to the caller, and the engine does
not persist it, so a restarted app starts from scratch unless the host saved it. Needs:

- A persisted manifest (per-file hash, size, mtime, chunk ids).
- A cheap pre-check (size + mtime) before hashing.
- Deletion detection, which needs `remove()` (backlog item 2).
- Optional live updates on Windows: `ReadDirectoryChangesW`, or the NTFS USN change journal for fast
  catch-up after the app was closed.

### A5. No file selection rules

The walk reads **every** regular file: binaries, huge files, no size cap, no include/exclude globs,
no ignore file. Windows-specific traps to handle deliberately:

- **OneDrive / cloud placeholder files.** Reading a cloud-only file triggers a download. Indexing a
  OneDrive folder could silently download the user's whole cloud drive.
- **Junctions and symlinks.** Policy is inherited from `DiskSkillSource` unexamined (ADR-063 §4
  finding 13). A junction can loop or leave the chosen root.
- Locked files (open in Office), long paths (`MAX_PATH`), and non-UTF-8 text encodings.
- Non-text formats need backlog item 6's extractors.

### A6. Authority and privacy

- **Authority.** The root is a raw path passed to the `DiskCorpusSource` constructor, with no
  capability. That is fine while trusted host code picks it. Once YAML can declare a source (backlog
  item 1), the folder grant must come from the host (user picks the folder in the app → host grants a
  scoped read capability), never from a YAML path string or anything model-derived (I2, I3).
- **Privacy.** A user's real folder can hold secrets. Indexing sends **all** its text to the
  embedding provider, and retrieved chunks go to the chat model. The app needs a clear disclosure, and
  a local embedder (backlog item 6) matters more here than anywhere else.

### A7. Where the index lives, and how it scales

- A desktop app needs a defined location for the persistent index and manifest (e.g. under
  `%LOCALAPPDATA%`), separate from the user's folder.
- The persistent brute-force snapshot rewrites one whole blob; at hundreds of thousands of chunks
  that is slow and large. Exact brute-force search at ~1M chunks also misses latency targets even on
  the GPU. At that scale an ANN index (backlog item 6) or Qdrant is the realistic option.

## Suggested order

1. **Backlog items 1 + 2** (declarative surface, index maintenance) as one ADR: "RAG as a declared,
   maintained knowledge source". Items 3 and 4 are small enough to ride along.
2. **Use case A2 + A3 + A4** (resumable background indexing with progress, search-while-indexing,
   persisted manifest) as the next ADR. This is what makes the large-host-folder case usable. It
   depends on item 2, and on the background-task limits in A2 being fixed first.
3. A1, A5, A6, A7 decisions fold into those two ADRs where they belong. Item 5 and the rest of item 6
   follow as their own ADRs.
