# 029 — Memory System

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 005, 007, 009, 025 · **Gate:** §9

## Goal

A memory system built from primitives this engine already owns — the worktree's content-addressed
store, `ContextProvider`'s attributed injection seam, and the capability/taint model — rather than a
wrapped vector database or a ported memory framework. Deterministic and replayable by default;
attributed always; semantic/vector retrieval an optional upgrade, never the only way memory works.

## 1. Why not wrap an existing memory framework

Mainstream memory frameworks (Mem0, Zep, MemGPT/Letta-style tiered memory, LangMem) converge on a
shape: an LLM extracts "facts" from conversation in the background, an embedding model turns them
into vectors, and a vector index serves similarity search at recall time. That shape carries three
costs this project's invariants make expensive to inherit wholesale, not because the shape is wrong
for those frameworks' goals, but because it is wrong for *these* invariants specifically:

- **Retrieval becomes a nondeterministic external effect.** An embedding call is a model call
  (004) — it must be attributed, budgeted, and recorded for replay (**I5**) like any other, but
  memory frameworks generally treat it as free background plumbing. Silently exempting the single
  most-invoked path (every turn wants context) from the recording discipline the rest of this engine
  enforces is not a small gap.
- **Extraction is an unattributed background job.** A "fact" appearing in memory with no visible
  boundary between "the user said this" and "the model guessed this" is exactly the confusion **I3**
  exists to prevent — model output treated as authority because it arrived through a system-labeled
  channel instead of a chat bubble.
- **It is a second storage engine.** AgentEngine already refuses to add a storage engine of its own
  for sessions (005 §2) or files (025 §2); a vector database bolted on beside them for memory would
  be the exception the rest of the design goes out of its way not to need.

The resolution is not "build our own vector database" — that would just be a fourth storage
technology instead of a third. It is: **memory is what falls out of using the worktree and
`ContextProvider` for a new purpose**, with semantic retrieval available as an explicit, attributed,
optional upgrade for operators who want to pay for it.

## 2. Model: memory is a worktree, scoped to the principal

- Every **principal** (007) owns a **memory worktree** — the identical content-addressed
  blob/tree/ref model as a session worktree (025 §2), scoped one level up:
  `principal:p-7` instead of `session:s-42`. **No new storage primitive.**
- It is **mounted at `/memory`**, capability-gated exactly like any worktree mount (025 §5):
  `FsRead<mount>` / `FsWrite<mount>` scoped to the mount, granted per agent or session like any
  other capability. Memory access is never ambient — an agent with no `/memory` mount has no memory,
  the same way an agent with no `NetOut` capability has no network.
- Because it is a worktree, memory inherits, at no additional implementation cost: content-addressed
  dedup, diffability ("what did this session add?"), fork/branch sharing between concurrent agents
  (025 §3's sharing modes apply unchanged, §8 below), rewind, and redaction (005 §6, 025 §6).

## 3. `MemoryItem` — structured and provenanced, never an opaque blob of text

```cpp
enum class MemoryKind   { Episodic, Semantic, Procedural };
enum class MemorySource { UserStated, ModelInferred, ToolDerived, AgentAuthored };

struct MemoryOrigin {
    MemorySource source;
    std::string  run_id;
    std::string  turn_id;
    Principal    principal;              // 007
};

struct MemoryItem {
    std::string   id;                    // content digest (025 §2) — identity, not assigned
    MemoryKind    kind;
    std::string   content;
    std::vector<std::string> tags;       // free-form, indexed (§5)
    float         salience;              // 0..1, decays (§7)
    MemoryOrigin  origin;
    std::optional<Timestamp> expires_at;
};
```

Stored as ordinary blobs under a path derived from `{kind, id}`, so "what memory exists" is
answerable with an ordinary tree listing (025 §7), not a query language only the host understands.

**`MemorySource` is a trust signal, not decoration (I3).** It is a memory-specific refinement of
003 §2's `origin` model, not a duplicate of it: `UserStated` items came from something the user
actually said; `ModelInferred` items are the host's own summarization of a turn (§4) and are model
output; `ToolDerived` came from a tool result; `AgentAuthored` came from the agent's own writes
(026 §5 `agent.notes`). A `ModelInferred` item must never be rendered or used as if it had the
standing of a `UserStated` one — see §6.

## 4. Writing memory: extraction is an attributed effect, not a hidden job

- `ContextProvider.on_turn_end` (005 §5) is where memory is written. A memory-writing provider's
  `on_turn_end` may call a declared `ChatClient` (004) to extract candidate `MemoryItem`s from the
  turn — an ordinary, budgeted, `EffectContext`-carrying model call, recorded exactly like any other
  (004 §6). There is no background thread the trace does not know about.
- **Extraction is opt-in policy, not implicit behaviour.** An agent with no memory-writing
  `ContextProvider` configured writes nothing. Whether extraction happens, on what cadence, and
  under what budget is an operator decision, not a default the engine assumes.
- The agent's own writes land in the same worktree with `origin.source = AgentAuthored`: writing
  `/memory/notes/project-x.md` through ordinary file operations (026 — the same "ordinary
  environment" philosophy as everything else the agent touches) *is* a memory write, not a separate
  mechanism from what the host's own extraction does. `agent.notes` (026 §5) is this path named for
  the agent-facing surface.

## 5. Reading memory: structured retrieval by default, vectors as an upgrade

- **Default retrieval has no embedding-model dependency.** Ranking is
  `salience × recency × tag/keyword overlap with the current turn` — arithmetic over stored,
  structured fields, computed host-side with no external call.
- This is the deliberate opposite bet from every vector-first framework: less semantically clever,
  and **entirely deterministic and replayable (I5)** — retrieval is a pure function of
  `{memory worktree tree digest, current turn}`, satisfying 005 §3's requirement that every context
  contributor be assembled deterministically, with no exception carved out for memory.
- **Semantic (vector) retrieval is an optional `ae:memory` plugin** (009 §2) — the embedding
  pipeline and the vector index both live inside it, never in the core. An operator who wants it
  gets it by granting the plugin's capabilities; the embedding call it makes is an effect, recorded
  and budgeted exactly like a `ChatClient` call, because it is one (§1). The plugin's default
  configuration is one embedding index per principal, mirroring the one-worktree-per-principal
  model above (§2); a "shared index" — one embedding index deliberately configured to serve
  multiple principals, e.g. for cross-tenant retrieval efficiency — is a non-default, adversarial
  configuration this RFC does not recommend and §9 G5 exists specifically to prove safe.
- Either mechanism is exposed to the model the way MAF's `TextSearchProvider` precedent already
  established for `ContextProvider` in general (005 §5, `docs/research/2026-maf-provider-concepts.md`
  §1): a short, budgeted set of high-ranked items is injected into `ContextContribution.messages`
  by default, and a `recall(query)` tool is contributed via `ContextContribution.tools` for on-demand
  lookups beyond what was pre-injected. `recall` works identically whether the ranking underneath it
  is keyword arithmetic or vector similarity — swapping the plugin never changes the agent-facing
  shape, only the ranking quality and the determinism guarantee (§9 G6).

## 5a. Knowledge corpora — shared and read-only, a different object from memory

Everything above is about **memory**: a principal's own facts, private, writable, sourced from
extraction or the agent's own notes. A **knowledge corpus** — product docs, a wiki, a codebase, a
support KB — is a structurally different object and does not fit `/memory`'s model:

- **Operator-populated, not extracted.** No `ContextProvider.on_turn_end` writes it; it is loaded
  the same way a skill is (009 §4's discover/verify/approve pipeline, or a plain operator sync job),
  never a byproduct of a conversation.
- **Shared by design, not per-principal.** It is mounted read-only at `/knowledge/<corpus>` (025
  §3) — the identical pattern `/skills/<name>` already uses for a read-only, operator-loaded,
  multi-principal-visible mount — rather than scoped one-per-principal like `/memory` (§2).
  Cross-principal visibility here is the intended default, not the adversarial misconfiguration §9
  G5 tests against for memory specifically.
- **Retrieval reuses §5's exact mechanism, pointed at a different mount.** Default keyword/tag
  ranking or an `ae:memory`-shaped vector upgrade, served the same way (`ContextContribution`
  message injection or an on-demand `recall`/`search` tool) — no new provider kind, no new plugin
  world. The only things that differ from §5 are the mount and the sharing default.
- **No `MemoryOrigin` (§3).** A knowledge-corpus item has no principal, no run, no turn to attribute
  to — it is provenanced as `origin: knowledge-corpus, corpus: <id>` instead, and is tainted external
  content exactly like retrieved memory (§6) since it still arrived from outside the current turn.

This closes the gap between 009 §8's read-only shared mount pattern (skills) and §2's private
per-principal pattern (memory): a knowledge corpus is the shared-read-only case applied to
retrieval instead of instructions, using primitives both sections already define.

## 5b. Ingestion — chunking is a pluggable choice, not a fixed answer

Populating `/knowledge/<corpus>` (§5a) from a source document is a pipeline — **discover → extract
→ chunk → (optional) contextualize → (optional) embed → write** — and every stage reuses a
mechanism this spec already has; none is new plumbing.

- **Extract**: 009 §7's document-extraction plugin family turns a source file into text or structured
  fields. Which tier and which downstream path depends on the document's *shape*, not just its file
  format — detailed in §5d.
- **Chunk**: a new `ae:codec` plugin candidate (009 §2's world already covers "content
  transformation: parse, extract, transcode, tokenize" — chunking is exactly this; added to 009 §7's
  table). **No single strategy is fixed by this spec.** The evidence doesn't converge on one winner:
  chunking-strategy choice swings recall by up to 9 points on identical data, and two independently
  run benchmarks *disagree* on whether recursive or semantic chunking wins
  (`docs/research/2026-08-05-rag-chunking-and-ingestion.md` §2). The engine ships one default —
  recursive/structure-aware, token-bounded, ~400-512 tokens at 10-20% overlap, the one configuration
  corroborated as a reasonable *starting point* across every source checked — and treats fixed-size,
  sliding-window, semantic/embedding-boundary, and contextual (below) chunking as **swappable
  plugins implementing the same shape**, exactly the discipline §5 already applies to retrieval
  ranking (keyword-arithmetic default, vector as an upgrade, identical agent-facing shape either
  way). This is deliberate: a developer picks the strategy that fits their corpus rather than the
  engine forcing one answer the cited benchmarks show can lose by a double-digit recall margin on
  the wrong corpus.
- **Determinism split, mirroring §5's rule.** Structural chunking (recursive/fixed/sliding-window/
  structure-aware) is a pure function of `{document content, chunking config}` — no network or model
  call, replayable, satisfies §9 G1 unmodified. **Contextual or semantic chunking calls a
  `ChatClient` or embedding model** (to generate chunk-context text or find semantic breakpoints) and
  must be attributed, budgeted, and recorded exactly like §5's embedding call; like vector retrieval,
  its determinism guarantee is explicitly **waived**, not silently dropped, per §9 G6's pattern.
- **Contextualize (optional)**: Anthropic's Contextual Retrieval method — prepending 50-100 tokens of
  document-situating context to each chunk before embedding/indexing, measured at up to 49% fewer
  retrieval failures combined with a lexical (BM25-class) index, 67% with reranking added
  (`docs/research/2026-08-05-rag-chunking-and-ingestion.md` §2). This is a corpus-ingestion-time
  model call, same attribution rule as above — not free (Anthropic's own figure: roughly $1 per
  million document tokens, even with prompt caching) — and is explicitly **not recommended below the
  corpus size where retrieval is needed at all**: under roughly 200k tokens, §5a's whole retrieval
  mechanism is unnecessary overhead and the corpus belongs directly in context, the same threshold
  Anthropic states for its own method.
- **Embed (optional)**: §5's existing `ae:memory` vector upgrade, unchanged.
- **Write**: each chunk becomes a corpus item carrying `{source document id, char offset span, chunk
  index}`. The span is what lets a retrieved chunk attach a `Citation` annotation (003 §1) pointing
  back to its exact location in the source document — closing a real gap MAF's own design exposes:
  checked directly against MAF source, only one vendor-specific mode (Azure AI Search's agentic
  Knowledge Base retrieval) produces structured, machine-checkable citations; everywhere else in MAF
  (`TextSearchProvider`, Mem0, Foundry memory) citation is an optional `SourceName`/`SourceLink`
  string folded into prose with a plain-text "please cite" instruction to the model — not
  machine-checkable. §5/§5a's retrieval path attaches the `Citation` annotation by construction
  whenever span metadata exists, using a mechanism 003 already defines rather than inventing one.

## 5c. Graph-structured retrieval — a different corpus shape, the same pipeline

A knowledge graph over a corpus (GraphRAG-style: entities, relationships, community summaries,
grounded in `docs/research/2026-08-05-rag-chunking-and-ingestion.md` §4) is not a new mechanism once
checked against what §5b already specifies — its indexing pipeline maps onto §5b's stage split
exactly, one stage at a time:

- **Extract** entities/relationships/claims per chunk — an LLM call, attributed exactly like §5b's
  contextualize stage.
- **Cluster** into communities (Leiden or equivalent) — a pure deterministic algorithm over the
  extracted graph, no model call, satisfying §9 G1/G7's default-path guarantee, the same as
  structural chunking.
- **Summarize** each community — an LLM call, attributed, waiving determinism exactly as §5b's
  contextualize stage does.
- **Store**: entities, relationships, claims, and community reports as structured `Data` content
  items (003 §1) in the corpus worktree — no graph database is needed at GraphRAG's own stated
  working scale (roughly 1k-1M corpus tokens, per the cited source); a heavier graph-store-backed
  `ae:memory` plugin is the same opt-in-for-scale upgrade §5 already allows for a vector index at
  larger scale, never a default.
- **Retrieve**: local search (entity-centric traversal, naturally an on-demand `recall`/`graph_search`
  tool), global search (community-summary rollup for holistic corpus-wide questions, naturally
  pre-injected), and DRIFT search (a hybrid) are three more instances of §5's existing "inject vs.
  on-demand tool" choice — not a new seam.

**No new promotion gate is needed.** G7's split (deterministic default path; attributed,
budgeted, waived-determinism model-call path) already covers "extraction and summarization are
model calls, clustering is not" — this is an instance of that gate, not a new one.

**Cost is a real, spec-relevant axis here, not just accuracy.** The cited source measured roughly
$20-40 per million document tokens to index with full community summarization, against roughly
$0.50 per million tokens for a variant that skips precomputed summaries and does dual-level
retrieval over the graph directly, at comparable accuracy on the same query class. This is §5b's
"no default answer picks a winner" principle again, on a cost axis instead of only a recall axis:
whether to run the summarization stage at all is exactly the kind of per-corpus, developer-made
choice the swappable-plugin discipline exists for, not something this spec should fix.

## 5d. Extraction forks on document shape, not file format

`docs/research/2026-08-05-rag-chunking-and-ingestion.md` §5 establishes **three extraction tiers**
(text-layer, OCR, VLM/neural layout-aware) and five recurring **document shapes**, each of which
routes to a different downstream path — the file format (`.pdf`, `.docx`, `.xlsx`) picks the parser;
the *shape* picks what happens to the output:

| Shape | Examples | Tier | Downstream path |
|---|---|---|---|
| Prose | Papers, DOCX, wiki pages | 1 (text-layer), preserving reading order/headings | §5b's chunk→embed pipeline; extracted headings become natural chunk boundaries |
| Slides | PPT | 1, per-slide | One slide (title + body + speaker notes) is a natural chunk unit on its own — no further splitting needed; embedded charts/images need tier 2 or 3 to become retrievable text at all |
| Grid | Excel, CSV-shaped sheets | 1, structure-preserving | Forks on intent: content meant for semantic search still goes through §5b as Markdown/CSV-shaped chunks; content meant to be *queried* (sums, filters, lookups) belongs in 009 §7's existing SQLite/DuckDB candidate instead — a query tool, not a `recall` call, because it isn't a retrieval problem |
| Form | Invoices, receipts | 3 (VLM-based field extraction, no per-vendor template) | Output is structured `Data` content (003 §1) matching a declared schema, via 003 §5's structured-output shaping — not a chunk. Bypasses embedding retrieval entirely: a single invoice isn't searched semantically, its fields are looked up, landing in the same structured store as query-intent grid data above |
| Image | Drawings, blueprints, scanned pages with no text layer | 2 for embedded text, 3 for genuinely visual content (symbols, dimensions, diagram structure) | Whatever tier 2/3 recovers feeds §5b like any other extracted text; tier 3 output here is the least deterministic of the group and always the attributed path |

**Tier 3 carries a real risk beyond accuracy, worth stating as a rule, not just a caveat**: a VLM
extractor can hallucinate content that never appeared in the source document — that content is model
output, not document content, the moment it's invented rather than read. It must be provenanced
distinctly from tier 1/2 output (§3's `MemoryOrigin`/§5a's knowledge-corpus provenance extended with
a tier marker), never silently merged as if all three tiers were equally literal transcriptions —
the same authority-laundering hazard §6 already names for `ModelInferred` memory, one stage earlier
in the pipeline.

No new promotion gate: tier 1/2 extraction satisfies G1/G7's deterministic default path; tier 3
satisfies G7's attributed/waived path exactly as chunking and graph extraction already do.

## 6. Serving memory to the model — provenance stays visible

- Retrieved memory is **tainted external content** (003 §2, already 005 §5's rule): it was written
  by a process on an earlier turn, not asserted live by the current user, and is delimited with
  provenance markers like any other retrieved content (017).
- **`ModelInferred` items are rendered with visibly lower confidence than `UserStated` ones** when
  both are injected in the same turn — the prompt-level counterpart to not letting a guess pose as a
  fact.
- **No `MemoryItem`, regardless of `MemorySource`, may satisfy a policy predicate that requires a
  user assertion** (007 §4's `PolicyDriven` approval, e.g. "auto-approve because the user said X") —
  I3 confines model-derived content to data, never authority, and memory is model-derived content the
  moment it passes through extraction (§4), even though it is *stored* like first-class engine state.

## 7. Consolidation is compaction, not a second mechanism

Deduplicating near-identical items, merging decayed items into a summary, and forgetting expired
ones follow **005 §4's compaction contract verbatim**: never silent, audited with its own span,
pre-consolidation state retained and addressable for time-travel, and a consolidation that drops a
still-referenced item is a defect. Memory does not get a separate lifecycle policy because history
already solved this problem — reusing it is the point.

**Salience decays** on an explicit, operator-declared schedule (§10 Q1) — never abrupt deletion.
Items decayed below a threshold are consolidation candidates, never silently gone.

## 8. Cross-agent and cross-session sharing

- Multiple agents belonging to one principal share the memory worktree the way sub-worktrees share
  a session's (025 §3): `shared` for collaborating agents, `branch` for concurrent ones needing
  isolation until merge, with the same three-way merge and conflict-surfacing rules (025 §4) —
  reused, not reinvented.
- **Cross-principal memory leakage through a shared index remains the release-blocking defect
  class** 005 §5 already names. A memory worktree belongs to exactly one principal, mirroring 005
  §1's rule that a session belongs to exactly one principal.

## 9. Promotion gate

- **G1 (determinism)** — default retrieval given a fixed memory-worktree tree digest and a fixed
  turn produces byte-identical `ContextContribution` output across repeated runs; no network call
  occurs.
- **G2 (attribution)** — every `MemoryItem` in the store traces to a `MemoryOrigin`; a store scan
  finds none without one.
- **G3 (no authority laundering)** — hostile-corpus check: a `ModelInferred` item crafted to read as
  authoritative cannot satisfy an approval predicate or a capability decision that requires
  `UserStated` provenance. Positive control included.
- **G4 (redaction)** — deleting a principal's data removes it from the memory worktree, its
  checkpoints, and unreferenced objects, matching 025 §6 / 005 §6.
- **G5 (cross-principal isolation)** — scoped to `MemoryItem`s (§2's principal-owned worktree), not
  §5a's knowledge corpora, whose cross-principal visibility is the intended design, not a
  misconfiguration this gate tests against. N principals with concurrent sessions; no memory item is
  retrievable outside its owning principal, proven against a deliberately adversarial shared-index
  configuration — an `ae:memory` plugin instance deliberately configured to share one embedding
  index across principals (§5) — not merely asserted.
- **G6 (upgrade path)** — the same agent, unchanged, runs against the default structured provider
  and against a vector-based `ae:memory` plugin; both satisfy G2–G5 unchanged. G1's determinism is
  explicitly **waived**, not silently dropped, for the vector case, and the waiver itself is recorded
  in the run trace — the same "declared fallback, never a silent one" discipline as 004 §2.
- **G7 (ingestion determinism and chunking swap)** — structural chunking (§5b's default) given a
  fixed source document and a fixed chunking config produces byte-identical corpus items across
  repeated runs, no network call. Swapping to a contextual/semantic chunking plugin changes only
  chunk boundaries/content and requires G6's identical declared-waiver discipline; the corpus item
  schema, `Citation` wiring, and downstream retrieval path (§5/§5a) are unchanged by the swap.

## 10. Open questions

- ~~**Q1** — Decay function and its parameters (half-life vs. step). Needs an evidence-based default
  from a real usage corpus, not a guess baked into the spec.~~ **Resolved, shape only — exponential
  half-life; the parameter value stays genuinely evidence-gated (2026-08-04):** the shape is
  answerable by reasoning even without a corpus: `salience`'s continuous `0..1` field (§3) already
  implies smooth decay, not discrete tiers, which is what a step function would fit better —
  exponential half-life avoids the sharp-cliff discontinuity (fully usable one turn, gone the next)
  this project's other design choices consistently avoid (008's graceful profile downgrade, §7's
  consolidation-not-deletion). The specific half-life *value* can't be picked from reasoning alone
  and stays exactly what this question already says it needs — an evidence-based default measured
  through 022's evaluation framework once an implementation exists, not guessed into the spec now.
  §7's "explicit, operator-declared schedule" is the shipping mechanism regardless: an operator can
  override the shape entirely, and the engine-shipped default value is a tunable starting point,
  never a claim of correctness.
- ~~**Q2** — Whether procedural memory ("learned instructions") may modify an agent's effective
  `instructions` directly, or only ever arrive as a `ContextContribution.instructions` append.~~
  **Resolved, No — append only, never direct modification (2026-08-04):** this is §6's already-
  established principle (memory is model-derived content the moment it passes through extraction,
  confined to data, never authority) applied to a bigger version of the same hazard. `instructions`
  is part of 002 §1's compiled, operator-reviewed agent metadata table, built once at startup and
  read-only by design — a procedural-memory item directly modifying it would be model-derived
  content reaching something 002 treats as trusted policy, exactly the authority-laundering §6
  already forbids for approval predicates, except persistent across every future turn rather than
  one decision — worse, not equivalent. `ContextContribution.instructions` (005 §5, already the
  designed per-turn, attributed, tainted injection path) is the only route: a learned instruction is
  injected alongside the static instructions each turn, visibly sourced (§6), never silently becoming
  part of the agent's reviewed baseline.
- ~~**Q3** — Consolidation cadence: per-turn (cheap per step, many small merges) versus periodic
  batch (cheaper overall, a staleness window in between).~~ **Resolved, operator-configurable,
  defaulting to periodic batch (2026-08-04):** this dissolves the per-turn-vs-batch framing as a
  false binary once *when* consolidation runs is recognized as a genuine 020 §1 configuration knob —
  it doesn't change what the agent does given the same input, only an operational cost/staleness
  tradeoff, the same category §4 already puts extraction cadence in ("whether extraction happens, on
  what cadence... is an operator decision"). Consolidation gets the identical treatment rather than a
  different, hardcoded answer for a sibling mechanism. Default: periodic batch, the safer
  absent-evidence choice (§7's own framing: "cheaper overall"), matching how other periodic-
  maintenance obligations in this project already default to scheduled rather than continuous (the
  interpreter image's cadence, 010 §10 Q1; the safety corpus's cadence, OQ-10).
- ~~**Q4** — Whether the agent-writable portion of `/memory` should default to read-only with writes
  only through `agent.notes` (§4), or genuinely read-write.~~ **Resolved, read-only outside
  `notes/`, confirming what §4's own example already does (2026-08-04):** §4 already shows the write
  path as `/memory/notes/project-x.md`, and `agent.notes` (026 §5) is already scoped narrowly to
  exactly that subtree — the choice was already made; only §10 hadn't marked it. Stated explicitly:
  an agent's write access to `/memory` is scoped to `notes/`, never generalized read-write access to
  the structured records under `{kind, id}` (§3) that the host's own extraction/consolidation logic
  depends on being well-formed — a genuine, not speculative, corruption hazard. The cost is real but
  small: `/memory` still reads as an ordinary, browsable directory (026 §1); only the write boundary
  is scoped, the same shape ordinary filesystems already have (a user's home directory has read-only
  system paths alongside a writable one) — not an unfamiliar pattern.
- ~~**Q5** — Whether `agent.memory` (026 §5) should expose read access to the *default-ranked* view
  as a callable, so an agent inside CodeAct can ask "what does memory say about X" without waiting
  for the next turn's automatic injection.~~ **Resolved, No — already served by `recall`, applying
  026 §5a's admission test (2026-08-04):** the "default-ranked view" §5 above describes is
  explicitly "a short, budgeted set of high-ranked items" — not bulk, which is the only bar (besides
  run-intrinsic, which this isn't) 026 §5a's admission test allows a module to clear. A short,
  budgeted read is served equally well by `recall` called with no query — the same already-designed
  `ContextContribution.tools`-contributed tool (§5 above), reachable via `agent.tools.recall(...)`
  like any other tool — which is exactly 026 §5a's disqualifying condition for a *new* top-level
  module. `agent.memory` (026 §5's table) stays what its capability already says it is — ordinary
  `FsRead<mount>` access to the memory worktree's files — not a second, ranked-query surface
  duplicating `recall`.
