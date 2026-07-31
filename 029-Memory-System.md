# 029 — Memory System

**Status:** Draft · **Depends on:** 003, 005, 007, 009, 025 · **Gate:** §9

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
  and budgeted exactly like a `ChatClient` call, because it is one (§1).
- Either mechanism is exposed to the model the way MAF's `TextSearchProvider` precedent already
  established for `ContextProvider` in general (005 §5, `docs/research/2026-maf-provider-concepts.md`
  §1): a short, budgeted set of high-ranked items is injected into `ContextContribution.messages`
  by default, and a `recall(query)` tool is contributed via `ContextContribution.tools` for on-demand
  lookups beyond what was pre-injected. `recall` works identically whether the ranking underneath it
  is keyword arithmetic or vector similarity — swapping the plugin never changes the agent-facing
  shape, only the ranking quality and the determinism guarantee (§9 G6).

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
- **G5 (cross-principal isolation)** — N principals with concurrent sessions; no memory item is
  retrievable outside its owning principal, proven against a deliberately adversarial shared-index
  configuration, not merely asserted.
- **G6 (upgrade path)** — the same agent, unchanged, runs against the default structured provider
  and against a vector-based `ae:memory` plugin; both satisfy G2–G5 unchanged. G1's determinism is
  explicitly **waived**, not silently dropped, for the vector case, and the waiver itself is recorded
  in the run trace — the same "declared fallback, never a silent one" discipline as 004 §2.

## 10. Open questions

- **Q1** — Decay function and its parameters (half-life vs. step). Needs an evidence-based default
  from a real usage corpus, not a guess baked into the spec.
- **Q2** — Whether procedural memory ("learned instructions") may modify an agent's effective
  `instructions` directly, or only ever arrive as a `ContextContribution.instructions` append. The
  former is more powerful and a materially larger I3 risk surface — an accumulating, self-modifying
  instruction set is a plausible route to instructions the operator never reviewed.
- **Q3** — Consolidation cadence: per-turn (cheap per step, many small merges) versus periodic batch
  (cheaper overall, a staleness window in between). 005 §4 doesn't have to answer this for history
  because history's compaction is triggered by context-window pressure; memory has no equivalent
  forcing function.
- **Q4** — Whether the agent-writable portion of `/memory` should default to read-only with writes
  only through `agent.notes` (§4), or genuinely read-write. A read-write mount lets an agent
  overwrite or corrupt structured records the host's own extraction depends on; a read-only default
  with a separate writable `notes/` subtree avoids that at the cost of a slightly less "ordinary"
  filesystem.
- **Q5** — Whether `agent.memory` (026 §5) should expose read access to the *default-ranked* view
  (what `on_context` would inject) as a callable, so an agent inside CodeAct can ask "what does
  memory say about X" without waiting for the next turn's automatic injection.
