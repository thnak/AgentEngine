# ADR-180 — Hybrid retrieval, pluggable/persistent vector storage, and a generic GPU search backend

**Status:** Judged (2026-09-23, project owner sign-off — see §9 for what was accepted, including the
named residuals the decision carries). Originally Proposed (2026-09-22, design pass drafted
collaboratively with the project owner in a live session). **§8 steps 1-6 are REAL, compiled, and passing, AND have now been through one independent
red-team pass with every finding fixed and proven** (2026-09-22, same session — see §4 below for the
full findings and §5-6 for the fixes). **§8 step 7 (`QdrantVectorIndex`) is now REAL for its
offline-provable half, AND has been through its own dedicated red-team pass** (2026-09-22, same
session — see §5-6 for the full account, including a real heap-buffer-overflow this pass's OWN offline
test caught via AddressSanitizer, and §4b for a second pass finding one genuine Critical confidentiality
residual — fixed with defense-in-depth plus an honest operational-fix disclosure, not fully closed by
code alone; see §7). **The live half now passes against a real Qdrant instance (2026-09-23)**: 12/12
checks against Qdrant 1.19.1 in a local Docker container with API-key auth enabled, built under WSL2 g++
with `AGENTENGINE_WITH_HTTPS=ON`. A deliberate wrong-key re-run fails as it should (exit 1). See §3
claim 6. The Docker daemon hang that blocked this run on 2026-09-22 had cleared by the next day.
**§8 step 8 (`VulkanCosineIndex`) is now REAL and built** — a `worktree`-isolated agent built it against
real Vulkan hardware (a discrete AMD Radeon RX 5300M), found and fixed two genuine performance bugs
along the way, and it has merged cleanly to `main` (2026-09-22, commit `f085bbd`). Correctness holds
(GPU/CPU cosine scores agree within ~1e-7). **Claim 7's performance half did NOT hold**: after both
fixes, GPU search measured ~1.3x SLOWER than CPU brute-force at both n=1000 and n=5000, not faster —
see §3 claim 7 and §5-6 for the full numbers. **It has now been through TWO independent, dedicated red-team
passes** (2026-09-22, same session — pass 3: a second `worktree`-isolated agent, no prior context, §4c,
one Critical and two Real gaps, all fixed; pass 4: a further independent agent, no prior context beyond
§4c's own account, explicitly run to answer this ADR's own previously-open "is a second pass owed"
question, §4d, ONE MORE Critical (`vkBindBufferMemory`'s `VkResult` discarded in both buffer-creation
lambdas — the same gap-class as pass 3's Critical, missed at a call site outside pass 3's narrower
`search()`-hot-path brief) plus two more Real gaps and one Minor — see §5-6 for every fix, all fixed and
proven the same session against the real GPU this backend was already built against). Every piece of
this ADR's scope has now had at least one red-team pass, and the two attack surfaces this ADR itself
flagged as needing the most scrutiny (a network-writable remote index; raw GPU buffer handling) have
each now had two independent passes (steps 1-6: one; `QdrantVectorIndex`: two, §4b; `VulkanCosineIndex`:
two, §4c/§4d, then a third, §4e, below) — parity across this ADR's contested surfaces, not just steps 1-6's own single pass.
**A THIRD `VulkanCosineIndex` pass has now run: red-team pass 5 (2026-09-23, a further independent
agent with no prior context, §4e). It resolves the previously open "is a third pass owed" question:
yes, it was owed.** It found:
- **one more Critical**: a scores-buffer double-destroy on an allocation-failure path that corrupted the
  process heap (`0xC0000374`) on the next call. This was NOT a discarded `VkResult`; every `VkResult`
  there was checked. It sat on a failure branch no pass had ever executed.
- **five Real gaps**:
  - the device's `maxStorageBufferRange` was never enforced, which is invalid usage at a realistic
    21,846 × 1536 corpus on a spec-floor device;
  - finite out-of-range inputs broke §3 claim 7(b) by a full 1.0;
  - NaN input led to `std::sort` UB;
  - a moved-from instance crashed on any call;
  - no failure branch had ever run.
- **two Minor issues**.

All eight are fixed with executed proof. Pass 5 was systematic by construction, not by eye:
- an exhaustive 47-entry-point Vulkan-call table;
- the Khronos validation and profiles layers;
- a permanent fault-injection test that executes 21 of 21 failure-capable entry points under the
  validation layer;
- UBSan, for the first time.

It also resolved two long-carried environment residuals: the "C1083" whole-project configure blocker is
Windows `MAX_PATH`, and with a short build root the real `cmake`+Ninja+`ctest` flow passes. Per
`CLAUDE.md`'s "contested, hot-path, or security-critical designs go through `design → red-team → prove →
judge`" rule, this ADR is **still not Judged**. **Pass 5's recommendation: the Vulkan surface is ready
for the Judge step**, with two named conditions (§8 item 9):
- ~~a ruling on device-lost's `failure_class`~~ — **RULED 2026-09-23 by the project owner: stays
  `resource`** (§7);
- ~~tracking `BruteForceCosineIndex`'s identical NaN-sort defect as an ADR-063 follow-on~~ — **CLOSED
  2026-09-23**: fixed directly rather than deferred (§7), so this condition no longer stands.

A fourth eyes-only red-team pass is not recommended. The live Qdrant run (§3 claim 6) **passed
2026-09-23** (§3 claim 6).
What "real" means for steps 1-6 below: every new type compiles under clang++
(`-std=c++23`) against this tree, every claim below that is marked CORRECT has a real, executed, passing
check (a permanent test registered in `tests/CMakeLists.txt`), and the existing ADR-063 test suite
(`test_vector_rag_context_provider`, `test_corpus_source`) was re-run after every change — both the
original implementation pass and every red-team fix — and stayed 100% green throughout: zero regression
to already-Judged behavior, not merely asserted.

**Relates to:** `decisions/ADR-063-retrieval-augmented-context-provider-shape.md` (the design this
amends/extends — `VectorRagContextProvider`, `Embedder`, `VectorIndex`, `corpus_source.hpp`,
`corpus_scope.hpp`, the citation-forgery defense), `decisions/ADR-064-recall-tool-sync-invoke-vs-async-
embedder.md` (the `synchronous_leaf`/`rt::drive_leaf_task()` mechanism this design reuses a second time,
for a second type parameter), `CONVENTIONS.md` §"Dependency posture — the three tiers", `023-Performance-
Targets-and-Budgets.md` §3 (the `RAG retrieval` budget row ADR-063 added, which this design's GPU claim
must be measured against).

## 0. What ADR-063 already left open, that this document closes or designs

Re-grounded against the real, current, Judged ADR-063 text (not assumed from memory) before drafting:

1. **§2.3B named Vulkan as deferred, not rejected**, behind three gates: (i) a CPU baseline exists and is
   benched — **done**, `BruteForceCosineIndex` + `tests/test_vector_index_benchmark.cpp`; (ii) that bench
   shows brute-force is the actual bottleneck — **done**, ADR-063 §6's own verdict on claim 1 is a real,
   measured Goal-tier MISS beyond ~1000 chunks against `023-Performance-Targets-and-Budgets.md`'s
   `≤ 500 µs` row; (iii) an explicit resolution of §3.5's determinism concern before merge — **not done**,
   this is §2.6 below.
2. **§4 finding 1 / §7's first residual: vector-index persistence is undesigned.** `BruteForceCosineIndex`
   is in-memory-only; a process restart re-embeds the entire corpus regardless of `previous_file_hashes`.
   Still true today (re-confirmed by reading `vector_index.hpp` fresh for this draft). §2.4 below closes it.
3. **No hybrid (dense+sparse) design anywhere.** The only "hybrid" in the existing RFC/ADR corpus is
   GraphRAG's DRIFT search (`029-Memory-System.md` §219-222) — vector-then-graph, unrelated to
   dense+keyword fusion. §2.2/§2.3 below design it from scratch.
4. **No external/network-backed `VectorIndex` conformer, and the `VectorIndex` concept as written cannot
   admit one** — `add_batch`/`search`/`contains` are plain synchronous `result<T>`, no `task<T>`, no
   `EffectContext`. §2.1/§2.5 below.
5. **§2.3 already named ANN (hnswlib) as a future opt-in seam backend, alongside Vulkan.** Per this
   session's own scoping decision, **hnswlib stays exactly where ADR-063 left it — named, not designed,
   not built here.** This document's GPU work does not touch it.

## 1. The question

**Stated so it has a wrong answer:** can hybrid (dense+sparse) retrieval, a persistent/pluggable vector
storage seam, and GPU-accelerated search all be added to `VectorRagContextProvider`'s existing shape
**without breaking or regressing the already-Judged, already-tested (193/193 green) ADR-063 surface** — or
does admitting a network-backed index (the "custom vector storage" ask) force a change to the `VectorIndex`
concept itself, and therefore to every existing conformer and call site?

**This document's answer: no breaking change is required.** A network-backed index needs an `async`,
`EffectContext`-threaded shape that the existing synchronous `VectorIndex` concept structurally cannot
provide — but rather than widen `VectorIndex` itself (risking every already-proven call site:
`BruteForceCosineIndex`, `vector_rag_context_provider.hpp`'s `on_context()`/`recall`, `corpus_source.hpp`),
this design adds a **second, new concept** (`RemoteVectorIndex`) side-by-side with the untouched original,
and threads a `constexpr` dispatch through the (also untouched-in-shape) provider logic — the identical
"declared trait, driven differently depending on its value" move `ADR-064` already proved sound for
`Embedder::synchronous_leaf`. `VectorIndex`, `BruteForceCosineIndex`, and every existing test remain
byte-for-byte unmodified.

## 2. Competing designs, steelmanned

### 2.1 How does a network-backed index fit the existing seam?

- **(A, chosen) A new `RemoteVectorIndex` concept, additive, never replacing `VectorIndex`:**
  ```cpp
  template <class T>
  concept RemoteVectorIndex =
      requires(T idx, std::vector<std::string> ids, std::vector<std::vector<float>> vecs,
               std::span<float const> query, std::size_t k, std::string const& id, EffectContext& ctx) {
          { idx.add_batch(ids, vecs, ctx) } -> std::same_as<task<result<void>>>;
          { idx.search(query, k, ctx) }     -> std::same_as<task<result<std::vector<ScoredId>>>>;
          { idx.contains(id, ctx) }         -> std::same_as<task<result<bool>>>;
          { T::synchronous_leaf }           -> std::convertible_to<bool>;
      };
  // Umbrella, used everywhere a provider currently spells `IndexT` alone:
  template <class T>
  concept AnyVectorIndex = VectorIndex<T> || RemoteVectorIndex<T>;
  ```
  Shape mirrors `Embedder` exactly on purpose (batch-only, `EffectContext`-threaded, declared
  `synchronous_leaf`) — a reader who already understands `Embedder`/ADR-064 recognizes this immediately.
  `contains()` becomes a real network round-trip for a remote conformer (unlike the O(1) local hash lookup
  today) — named explicitly in §7 as a re-mount-dedup cost this design accepts, not hides.
  *Steelman for widening `VectorIndex` itself instead:* one concept, not two; every provider's template
  constraint stays a single name. **Rejected**: `BruteForceCosineIndex::search()` etc. would have to become
  coroutines (even though they never suspend), every existing call site in `vector_rag_context_provider.hpp`
  and `corpus_source.hpp` would need `co_await` added, and `tests/test_vector_rag_context_provider.cpp`'s
  193 green assertions would all need re-verifying against the new shape — real risk to Judged, proven code
  for a feature (remote indices) most deployments won't use. CLAUDE.md's actions-with-care guidance applies
  to code changes too, not just shell commands: don't take a hard-to-reverse-feeling risk against tested
  code when an additive alternative exists at no real cost.
- **(B, chosen) Providers dispatch via `if constexpr`, not runtime polymorphism.** `VectorRagContextProvider<
  EmbedderT, IndexT>`'s `on_context()` and `make_recall_tool_descriptor()`'s `invoke` closure gain one new
  branch each: `if constexpr (RemoteVectorIndex<IndexT>) { co_await index_->search(...) /* or, in `recall`,
  the existing `synchronous_leaf` ? `drive_leaf_task` : fail-closed pattern, now checking IndexT::
  synchronous_leaf too */ } else { /* existing VectorIndex<IndexT> path, unchanged */ }`. Matches how
  `Embedder::synchronous_leaf` is already checked with `if constexpr` in `make_recall_tool_descriptor()`
  today (`vector_rag_context_provider.hpp:314`) — same idiom, one more axis.

### 2.2 Sparse (keyword) index

- **(A, chosen) New `SparseIndex` concept, core-resident, std-only (Tier 1), synchronous** — mirrors
  `VectorIndex`'s exact shape, substituting text for vectors:
  ```cpp
  template <class T>
  concept SparseIndex = requires(T idx, std::vector<std::string> ids,
                                   std::vector<std::string> texts, std::string const& query,
                                   std::size_t k, std::string const& id) {
      { idx.add_batch(ids, texts) } -> std::same_as<result<void>>;
      { idx.search(query, k) }      -> std::same_as<result<std::vector<ScoredId>>>;
      { idx.contains(id) }          -> std::same_as<bool>;
  };
  ```
  No `RemoteSparseIndex` counterpart in this pass — no vendor need was named (unlike Qdrant for dense);
  the concept is the extension seam if one shows up later, exactly the answer already given for
  `VectorIndex` itself.
- **(A, chosen) `BM25Index` default** — standard Okapi BM25 (`k1 = 1.2`, `b = 0.75`, both constructor
  parameters, not hardcoded — matching `RecursiveChunker`'s own "declared policy, not a fixed algorithm"
  precedent, ADR-063 §2.4b), whitespace/punctuation tokenizer + lowercasing (ASCII-only stemming-free v1,
  named as a real quality residual in §7, not silently claimed equivalent to a real IR library's
  tokenizer). Inverted index: `unordered_map<term, vector<{id, term_freq}>>` plus per-doc length and corpus
  average length, both required by the BM25 formula. Same `std::shared_mutex` reader/writer discipline
  `BruteForceCosineIndex` already uses (ADR-064 §6's fix, directly reused, not reinvented).
  *Steelman for a real IR library (Tantivy-style) instead:* better tokenization, stemming, phrase queries.
  **Rejected as the default** (session decision): BM25's core scoring is simple enough to implement
  correctly at Tier 1 with no new dependency, matching `VectorIndex`'s own std-only bar; a Tier-2
  library-backed `SparseIndex` conformer is a plausible, separate future seam backend (same "named, not
  built" posture ADR-063 gave hnswlib), not designed here.

### 2.3 Hybrid fusion

- **(A, chosen) Reciprocal Rank Fusion (RRF)**, `score(id) = Σ 1/(k_rrf + rank_i(id))` over each result list
  the item appears in (`k_rrf = 60`, the standard literature default, a constructor parameter not a magic
  number). *Why RRF over weighted linear combination:* cosine similarity (roughly `[-1, 1]`, in practice
  `[0, 1]` for normalized text embeddings) and BM25 scores (unbounded, corpus-dependent magnitude) are not
  on comparable scales — a weighted sum would need per-corpus score normalization to mean anything, a real
  tuning burden and a second place (after chunking strategy, ADR-063 §2.4b) this design would otherwise be
  asserting a good default for without evidence. RRF only needs each list's *rank*, sidestepping scale
  entirely — the same reason it's the field's de facto default for exactly this kind of fusion.
  Items appearing in only one list still participate (their absent list simply contributes 0 to the sum),
  so `HybridRagContextProvider` never requires an item to be found by both retrieval methods.
- **(A, chosen) `HybridRagContextProvider<DenseIndexT, SparseIndexT>`, a new, separate `ContextProvider`
  class** — matches ADR-063 §2.1b's own precedent exactly ("each RAG kind is its own class... not one
  mega-parametrized provider"). Owns one `EmbedderT` + `DenseIndexT` and one `SparseIndexT`, runs both
  searches (dense via the §2.1 dispatch, sparse directly — `SparseIndex` has no async variant),
  RRF-fuses the two `ScoredId` lists into one ranked list of chunk ids, then reuses
  `VectorRagContextProvider`'s existing `render_scored_chunk()`/citation-rendering path **verbatim by
  extraction**, not by reimplementation (§2.3a below) — the identical discipline ADR-063 §2.6b already used
  when it generalized `neutralize_forged_memory_labels()` into `neutralize_forged_provenance_markers()`
  rather than writing a second copy.
  Contributes both a default-injection path (fused top-K, mirroring `VectorRagContextProvider`) and its
  own `recall(query)` tool (mirroring the same tool shape, `synchronous_leaf` gated on **both** `EmbedderT`
  and, when `DenseIndexT` is a `RemoteVectorIndex`, `DenseIndexT::synchronous_leaf` too — a call is only
  synchronously drivable if every leaf it touches is).
- **(A, chosen — real implementation-shape consequence, named explicitly) `render_scored_chunk()` moves to
  a free function** taking `(ScoredId, OS&, RS&, Mount const&)` rather than staying a private method on
  `VectorRagContextProvider`, so `HybridRagContextProvider` calls the exact same code, not a fork of it.
  This is a real, small refactor of already-Judged `vector_rag_context_provider.hpp` — behavior-preserving
  (same signature minus `this`), the same kind of "factor out, don't duplicate" move §2.6b's own citation
  work already made once. Flagged here so it isn't silently missed as "just new code."

#### 2.3a Corpus ingestion for two indices from one chunk set

`DiskCorpusSource::mount()` today is `mount<EmbedderT, IndexT, OS, RS, ...>(..., EmbedderT& embedder,
IndexT& index, ...)` — chunks once, embeds once, writes to one index. A hybrid corpus must **chunk once**
(chunking must not run twice — ADR-063 §3 claim 2's re-mount-dedup property depends on one deterministic
chunk set per file) and then **ingest that same chunk set into both indices**: embed + `dense_index.add_batch()`,
and separately `sparse_index.add_batch(ids, chunk_texts)`. Chosen shape: `mount()` gains an optional second
index parameter (`mount<EmbedderT, DenseIndexT, SparseIndexT, ...>`, `SparseIndexT = void` default meaning
"dense-only, unchanged behavior" — existing callers passing only `EmbedderT, IndexT` are unaffected, matching
this ADR's "additive, don't break ADR-063 callers" posture applied to `corpus_source.hpp` too). Both indices'
`contains(id)` gate their respective ingestion step independently — a chunk already in the dense index but
not yet in a *newly added* sparse index (e.g. hybrid search enabled on a pre-existing dense-only corpus)
still needs sparse-side backfill, and the design must not assume the two indices are always populated in
lockstep. Named explicitly as real, not-yet-written logic — §8's implementation plan, step 3.

### 2.4 Local persistence (closing ADR-063 §7's named residual)

- **(A, chosen) A refinement concept, `PersistentVectorIndex`, not a requirement on `VectorIndex` itself:**
  ```cpp
  template <class T>
  concept PersistentVectorIndex = VectorIndex<T> &&
      requires(T const& idx, T& mutable_idx, WorktreeObjectStore auto& store) {
          { idx.snapshot(store) } -> std::same_as<result<Digest>>;   // writes a blob, returns its digest
          { mutable_idx.restore(store, std::declval<Digest const&>()) } -> std::same_as<result<void>>;
      };
  ```
  `BruteForceCosineIndex` gains `snapshot()`/`restore()`: serialize `{id, vector}` pairs to a compact
  binary blob (length-prefixed id strings + raw `float` arrays, no JSON — this is a hot path artifact, not
  a human-inspected record, unlike `CorpusChunkRecord`), `put_blob()` it into the **same**
  `WorktreeObjectStore` the corpus mount already uses (content-addressed — an unchanged index re-snapshots
  to the identical digest, a free dedup this design gets for reusing existing infra rather than inventing a
  new store). A `DiskCorpusSource`-level convention (`index-snapshot` ref, alongside the existing chunk
  ref) records the latest snapshot digest per corpus mount.
  *Why a refinement concept, not a requirement:* `VulkanCosineIndex` (§2.6) and any future in-memory-only
  conformer remain valid `VectorIndex`s without being forced to implement persistence they may not need
  (e.g. a purely ephemeral per-session scratch index); callers who *do* want restart-survival name
  `PersistentVectorIndex` in their own constraint.
  **`RemoteVectorIndex` conformers (Qdrant, §2.5) do not implement `PersistentVectorIndex` at all** — named
  explicitly, not an oversight: the persistence problem this concept solves (local process memory is not
  durable) does not apply to them; the vectors' durable home *is* the remote service by construction. A
  `RemoteVectorIndex`'s `contains()` is itself the re-mount-dedup check, already durable, no snapshot needed.
- **(B, rejected) Persist inside `VectorIndex` itself, no separate concept.** Rejected for the identical
  reason §2.1's Design A was chosen over widening `VectorIndex`: forces every current and future local
  conformer (including a possible future in-memory-scratch use case) to carry persistence machinery it may
  not want, and forces `BruteForceCosineIndex`'s already-tested shape to change unconditionally rather than
  by opt-in refinement.

### 2.5 External vector storage — `QdrantVectorIndex`

- **(A, chosen) A `RemoteVectorIndex` conformer targeting Qdrant's REST/JSON API**, `protocol/qdrant/
  vector_index.hpp` — same **directory tier** as `protocol/openai/embedder.hpp` (Tier 1-adjacent
  "protocol" code: JSON over HTTPS, zero new third-party dependency, reuses `sandbox::
  perform_provider_https_exchange` and `json::Value` exactly as `OpenAIEmbedder` does). Constructor shape
  mirrors `OpenAIEmbedder`'s deliberately: `(host, port, collection_name, SecretRef api_key_ref,
  EmbedderCapabilities-equivalent, Store const&, path_prefix, resolver, ca_bundle_pem_override, transport)`
  — credential resolved via `SecretStore::resolve()` **inside** each `add_batch`/`search`/`contains` call,
  never at construction (the same 004 §1 / 018 §4 rule `OpenAIEmbedder::embed_batch()` already follows,
  re-affirmed here rather than assumed). `add_batch()` → Qdrant `PUT /collections/{name}/points`;
  `search()` → `POST /collections/{name}/points/search`; `contains()` → `GET /collections/{name}/points/{id}`
  (a real network round-trip, named cost, §7). Documented, like `OpenAIEmbedder`'s OpenAI/OpenRouter
  relationship (ADR-063 §2.5), as **"any Qdrant-API-compatible host"** — no separate class per self-hosted
  vs. Qdrant Cloud, construction arguments only.
  *Steelman for pgvector instead:* if a deployment already runs Postgres, one fewer service to operate.
  **Rejected as the reference conformer** (session decision): no Postgres wire-protocol client exists
  anywhere in this codebase today; adding one is a genuinely new Tier-2 dependency with no existing
  precedent to model it on, versus Qdrant's plain-JSON-over-HTTPS shape needing *zero* new dependency and
  directly reusing the exact machinery `OpenAIEmbedder` already proved live (ADR-063 claim 4). A
  `PgVectorIndex` remains a plausible future `RemoteVectorIndex` conformer — the concept is the seam — just
  not designed here.
- **I2/I4 posture, stated explicitly (a network-reaching `VectorIndex` is new attack surface this ADR must
  own, not wave at):** identical to `Embedder`'s — the capability is the `SecretRef` grant itself (no
  separate `cap::NetEgress`-style token exists in this codebase for provider calls; `OpenAIEmbedder` and
  `OpenAIChatClient` both gate purely through secret resolution + `EffectContext`), and every call is
  attributable through the same `EffectContext` provenance chain (I4) every other declared backend uses.
  `QdrantVectorIndex` introduces no new capability primitive — it is a second conformer of an existing
  policy, not a new policy.

### 2.6 GPU — `VulkanCosineIndex`, a generic (single, cross-vendor) backend

- **(A, chosen) Exact brute-force cosine similarity, GPU-parallel across *candidates*, sequential *within*
  each candidate's dot product** — a compute shader where each invocation owns exactly one stored vector
  and accumulates its dot product with the query in ascending-dimension order, the identical operation
  order `BruteForceCosineIndex::cosine_similarity()` already uses. This directly resolves §3.5/§2.3B gate
  (iii)'s concern ("a GPU-parallel reduction... can change which items land in the top-K versus sequential
  CPU reduction") **for the reduction-order half of that concern**: there is no cross-dimension tree
  reduction in this design at all, by construction — parallelism is entirely across the `n` candidates, not
  within any one candidate's `d`-dimension sum, so the *order* of floating-point operations per candidate is
  identical to the CPU path.
  **What this does NOT eliminate, named as an honest, accepted residual (the same posture ADR-063 §2.2A
  already established for `Embedder`'s determinism, not a new kind of dishonesty):** `BruteForceCosineIndex`
  accumulates in `double` (`vector_index.hpp:157`, `double dot = 0.0`); a Vulkan compute shader realistically
  accumulates in `float` for portability — `VK_KHR_shader_float64` is **not** a baseline-guaranteed feature
  across the Vulkan-conformant device range this design intends to run on (mobile/integrated GPUs commonly
  lack it), and requiring it would contradict the "generic, cross-vendor" goal this section exists to serve.
  So GPU and CPU results can diverge by a few ULPs from accumulated single- vs. double-precision rounding
  alone, independent of operation order — bounded, small, and **only capable of reordering genuinely
  near-tied scores**, not of producing a wrong-by-a-wide-margin ranking. `VulkanCosineIndex` satisfies the
  plain (unchanged) `VectorIndex` concept — no `EffectContext`, no capability gating, no async: GPU dispatch
  is local compute, not a network effect, matching the reasoning that let `RecursiveChunker` and
  `BruteForceCosineIndex` itself stay synchronous.
- **(B, rejected) Match CPU's `double` accumulation on GPU via an extension check + fallback.** Steelman:
  bit-for-bit parity, zero residual. **Rejected**: makes the "generic" backend's actual numeric behavior
  device-dependent (bit-exact on a `float64`-capable GPU, residual-bearing on one without) — a strictly
  *worse*, harder-to-reason-about property than "always has the same small, named, bounded residual
  everywhere," and reintroduces exactly the kind of silent, environment-dependent behavior difference
  CLAUDE.md's I5 ("nondeterminism crosses a recorded seam") exists to prevent papering over.
  Deterministic top-K tie-break (score desc, then id asc) is unchanged and still applied on the CPU side
  after the GPU scores are read back — `VulkanCosineIndex::search()` dispatches the shader, reads back one
  `float` score per candidate, and reuses `BruteForceCosineIndex`'s existing sort/tie-break/truncate-to-k
  logic verbatim (factored into a shared free function, not duplicated — same discipline as §2.3's
  `render_scored_chunk()` extraction) — so the *only* place GPU and CPU can disagree is a near-tied score
  itself, never the tie-break rule applied after.
- **Dependency shape (Tier 2, `src/backends/vulkan_vector_index/`, one CMake-gated option
  `AGENTENGINE_WITH_VULKAN`):** the Vulkan **loader** (`libvulkan`/`vulkan-1.dll`, dynamically loaded,
  present on essentially any machine with a GPU driver — not a heavy SDK dependency to vendor) plus
  **pre-compiled SPIR-V** for the one cosine-similarity shader, checked into the repo as a build artifact
  (compiled once via `glslc`/`glslangValidator` at shader-authoring time, not at every build) — avoiding a
  shader-compiler dependency at build time entirely, matching "one dependency per backend" as tightly as
  this feature allows. No `VK_KHR_shader_float64`, no vendor extension requirement — anything implementing
  Vulkan 1.1 core runs it, which is the entire point of choosing Vulkan over CUDA/Metal/ROCm here (session
  requirement: "a generic one, so if anybody needs another backend they have to do it themself" — this ADR
  ships exactly **one** GPU conformer; a `CudaVectorIndex` or `MetalVectorIndex` is a new `VectorIndex`
  conformer someone else writes against the same, already-generic seam, not a gap in this design).
- **Falls back cleanly when unavailable:** `VulkanCosineIndex` construction fails closed (`result<...>`
  error, `vulkan_vector_index.device_not_found`/`.feature_unsupported`) rather than crashing, when no
  Vulkan-capable device is present — a caller composing it is expected to have a CPU (`BruteForceCosineIndex`)
  fallback path, exactly the same posture `docker_execution_surface.hpp`/`containerd_execution_surface.hpp`
  already take toward "the backend this host doesn't have."

## 3. Falsifiable claims (per design, paired with a disproving experiment)

1. **Claim (§2.1):** every existing `VectorRagContextProvider<EmbedderT, IndexT>` call site, with `IndexT =
   BruteForceCosineIndex` (a plain `VectorIndex`, not a `RemoteVectorIndex`), behaves byte-identically to
   pre-ADR-180 code — zero regression from adding the `AnyVectorIndex`/`if constexpr` dispatch.
   **Disproof:** `tests/test_vector_rag_context_provider.cpp`'s existing suite (all pre-existing cases,
   unmodified) fails, or any new assertion is needed to make it pass again.
2. **Claim (§2.3):** RRF fusion over a dense list and a sparse list, where a specific chunk is ranked #1
   dense / unranked sparse and a different chunk is ranked #1 sparse / unranked dense, produces a fused
   ranking that is neither list alone — i.e., fusion is real, not one input silently dominating.
   **Disproof:** construct exactly that scenario against `HybridRagContextProvider`; assert the fused top-1
   differs from both pure-dense-top-1 and pure-sparse-top-1, or equals one of them only when RRF's own
   arithmetic genuinely produces that result (compute the expected RRF scores by hand in the test and
   assert against them, not just "some plausible order").
3. **Claim (§2.2):** `BM25Index` reproduces the standard Okapi BM25 formula, not an approximation —
   hand-computed expected scores for a small (3-5 document), fixed corpus with known term frequencies match
   the index's output to floating-point tolerance.
   **Disproof:** a test with hand-derived expected scores (worked out independently of the implementation)
   that the real `BM25Index::search()` output fails to match.
4. **Claim (§2.4):** `snapshot()` → drop the in-memory index → construct a fresh instance → `restore()` from
   the snapshot digest → `search()` returns the identical top-K (same ids, same scores, same order) as the
   pre-snapshot index for an identical query.
   **Disproof:** any divergence in ids, scores, or order between pre-snapshot and post-restore search
   results for the same query against the same underlying data.
5. **Claim (§2.4, re-mount cost):** re-mounting a corpus after a process restart, with a valid prior
   snapshot present, costs `O(1)` embedder calls (zero — every chunk already present) rather than `O(n)`,
   restoring ADR-063 §3 claim 2's property *across* process lifetimes, not just within one.
   **Disproof:** a test that snapshots, restarts (simulated: fresh process-equivalent objects), restores,
   re-mounts an unchanged folder, and asserts the embedder mock's call count is exactly zero.
6. **Claim (§2.5):** `QdrantVectorIndex` reaches a real Qdrant instance and round-trips `add_batch` →
   `search` → `contains` correctly — mirroring ADR-063 claim 4's live-test shape exactly.
   **Disproof:** a live test (env-var-gated, `live-network`-labeled, matching `test_openrouter_live_e2e.cpp`'s
   established pattern) against a real (can be a local Docker) Qdrant instance that fails to add points,
   fails to retrieve the added point via `search`, or `contains()` disagrees with what was actually added.
   **STATUS (2026-09-22): the offline-provable HALF is CORRECT, executed** — mirroring `OpenAIEmbedder`'s
   own claim-4 split (ADR-063 §6), `tests/test_qdrant_vector_index.cpp` proves the request/response
   shape, point-id derivation and its documented (128-bit) collision surface, the `payload.chunk_id`
   round-trip that recovers the real chunk digest from a search result, and the Qdrant-specific error
   envelope (`status.error`, not OpenAI's `error.message`) — 27 assertions, green under both a plain
   build and AddressSanitizer. See §5-6 below for the real finding this offline pass caught (a heap-buffer-
   overflow, found via ASan) before any live test could have made it worse.
   **STATUS (2026-09-23): the live HALF is CORRECT, executed.** Setup and results:
   - **Target:** `qdrant/qdrant:latest` (reports version 1.19.1, commit `6ab21ca`) in local Docker
     Desktop, started with a random per-run `QDRANT__SERVICE__API_KEY`. The key was generated into a
     scratch file and never committed.
   - **Collection:** `ae_qdrant_live_e2e_test`, created with `{"size":3,"distance":"Cosine"}`. An
     unauthenticated `GET /collections` returned 401, confirming auth was actually on.
   - **Build:** WSL2 Ubuntu, g++ 15.2, Ninja, Debug, `-DAGENTENGINE_WITH_HTTPS:BOOL=ON`, with
     `AGENTENGINE_QDRANT_TRANSPORT=plaintext_http`. This goes through the real
     `ProviderHttpClient` transport, not the Windows-side stub used for the offline k=0 case.
   - **`test_qdrant_vector_index_live_e2e`: ALL PASS, 12/12, exit 0.** It covers:
     - QD-1: a real upsert of 2 points.
     - QD-2: search returns both. The exact-match point ranks first with score 1.000000, and its id
       is the real chunk digest recovered from `payload.chunk_id`, not Qdrant's internal UUID.
     - QD-3: `contains()` is true for an added digest and false, not an error, on Qdrant's real 404.
     - QD-4: live `k=0` returns empty.
     - QD-5 positive control: a wrong key is rejected with `http_401`, classified `policy`.
     - QD-6 (I2): without a `cap::Secret` grant, the call is denied before reaching the network,
       classified `policy`.
   - **Independent confirmation:** `POST .../points/count {"exact":true}` returned `count: 2`, so
     Qdrant really stored the points; the test did not merely report success.
   - **Negative run (the test can fail):** the same binary with
     `AGENTENGINE_QDRANT_API_KEY=deliberately-wrong-key` fails QD-1/2/3 and exits 1, while QD-5 still
     passes.
   - **Offline suite:** `test_qdrant_vector_index` also passes under this Linux build, 36 `ok` lines,
     exit 0.
7. **Claim (§2.6):** for a fixed, realistic corpus (dim 1536, n ∈ {1000, 5000} — precisely the sizes ADR-063
   §6 already measured as CPU Goal-tier misses), `VulkanCosineIndex::search(k=5)` both (a) stays within the
   RFC 023 §3 `≤ 500 µs` Goal budget where `BruteForceCosineIndex` did not, and (b) produces a top-K set
   that differs from the CPU top-K, if at all, only in candidates whose CPU-computed cosine scores are
   within a stated epsilon of each other (bounding §2.6's named float32/float64 residual empirically, not
   just asserting it's small).
   **Disproof:** a bench (mirroring `test_vector_index_benchmark.cpp`'s own harness) showing no latency
   improvement over brute-force at n=1000/5000, or a top-K divergence between GPU and CPU involving a pair
   of candidates whose CPU scores differ by more than the stated epsilon.
   **STATUS (2026-09-22): part (b) CORRECT, part (a) DISPROVEN.** Run for real against a discrete AMD
   Radeon RX 5300M: GPU/CPU cosine scores agree within ~1e-7, well inside the stated 1e-3 epsilon — no
   top-K divergence beyond that bound was observed at either n. But GPU search was measured ~1.3x
   SLOWER than CPU brute-force at both sizes (n=1000: 2.9ms GPU vs. 1.5ms CPU; n=5000: 10.9ms GPU vs.
   7.8ms CPU) — neither figure gets within two orders of magnitude of the 500µs Goal budget, and CPU
   brute-force is still the faster of the two on this hardware, the opposite of what this claim needed.
   Two real perf bugs (a whole-buffer re-upload per `search()` call, then host-visible-vs-device-local
   GPU memory for repeated shader reads) were found and fixed along the way, closing most but not all of
   the original gap; what remains is very likely fixed per-`search()`-call `vkQueueSubmit`+
   `vkWaitForFences` CPU↔GPU synchronization overhead that doesn't amortize at these corpus sizes — named
   as an open residual (§7), not chased further this pass. See §5-6 for the full account. (Part (b)'s
   correctness was RE-CONFIRMED by red-team pass 3, §4c, on a rebuilt binary that ALSO fixes several
   unrelated fail-closed gaps — see §5-6.)
   **(Red-team pass 5, §4e finding 3) Part (b) was FALSE for finite inputs outside float32's squared
   range, and is now restored.** Components above ~1.8e19 or below ~1e-19 overflowed or underflowed in
   the shader's float32 squares. An exact match then scored 0 or NaN on the GPU against 1.0 on the CPU: a
   divergence of 1.0, not a near-tie. Every row and query is now rescaled by an exact power of two before
   upload, and the GPU now matches the CPU within 1e-5 at magnitudes 1e20, 1, 1e-25 and 0. The rescale is
   exact, so the measured in-range max divergence is bit-identical to before (1.04308e-07 / 1.37836e-07).
   Non-finite inputs are outside the claim's domain: `VulkanCosineIndex` now rejects them as a contract
   violation.
8. **Claim (§2.3a, carried forward from ADR-063 claim 6):** the existing citation-forgery defense
   (`neutralize_forged_provenance_markers`) applies identically to chunks surfaced through
   `HybridRagContextProvider` as through `VectorRagContextProvider` — because §2.3 reuses
   `render_scored_chunk()` verbatim rather than reimplementing rendering, this should hold by construction,
   but must be proven, not assumed, since "reuses the function" is a claim about the diff, and diffs can be
   wrong.
   **Disproof:** mirror ADR-063's own `C6-R1`–`C6-R3` scenario through `HybridRagContextProvider::
   on_context()` instead of `VectorRagContextProvider`'s; any forged marker surviving unbroken is a failure.

## 4. Red-team pass 1 (2026-09-22, `general-purpose` agent, no prior context on this document)

Run against the real §8 steps 1-6 code (`vector_index.hpp`'s persistence additions,
`remote_vector_index.hpp`, `sparse_index.hpp`, `vector_rag_context_provider.hpp`'s refactor,
`hybrid_rag_context_provider.hpp`, `corpus_source.hpp`'s `mount_hybrid()`) plus the tests that already
existed for them — matching `ADR-063` §4's own methodology (a fresh agent, no context, attacking the real
cited source, not the ADR prose alone). **Steps 7-8 do not exist yet and were explicitly out of scope for
this pass** — a second pass covering those, and re-attacking steps 1-6 as they interact with real
`QdrantVectorIndex`/`VulkanCosineIndex` conformers, is still owed before Judged (mirroring `ADR-063`'s own
two-pass structure).

### Critical

1. **`BruteForceCosineIndex::restore()` allocated from an unvalidated, corruption/tamper-reachable
   `count`/`dimension` header before any bounds check against the blob's actual remaining bytes** —
   `vector_index.hpp`'s `restore()`. A blob claiming `count = UINT64_MAX` (only 20 bytes actually present)
   reached `reserve(UINT64_MAX)` directly, throwing `std::length_error`/`std::bad_alloc` with **no
   try/catch anywhere in the call chain** (this function is plain `result<T>`-returning, per
   CONVENTIONS.md's "no exceptions for control flow" model) — crashing the process instead of `restore()`
   returning the typed `error` its own contract already promised ("reject-not-coerce on any structural
   inconsistency"). The existing persistence tests only covered "digest not found" and "wrong magic," never
   a header that lies about how much data follows.

### Real gaps

2. **`VectorIndex` and `RemoteVectorIndex` were not structurally disjoint.** Concepts only check "does this
   call expression compile with this return type" — a type offering BOTH a synchronous 2-arg overload set
   (satisfying `VectorIndex`) and an async 3-arg overload set (satisfying `RemoteVectorIndex`) — a plausible
   thing to write, e.g. a caching wrapper with a local fast-path and a network fallback — satisfied BOTH
   concepts at once with zero compiler diagnostic. Every dispatch site (`if constexpr
   (RemoteVectorIndex<IndexT>) {...} else {...}`) checks `RemoteVectorIndex` first, so such a dual-conformer
   would be silently and unconditionally routed onto the network/task path, invisibly to its own author.
   Doesn't fire against any conformer in the tree today (only `BruteForceCosineIndex`/`BM25Index` exist),
   but was the load-bearing risk for the very next conformer this ADR names (`QdrantVectorIndex`, §8 step 7).
3. **The entire `RemoteVectorIndex` dispatch path (in `VectorRagContextProvider`, `HybridRagContextProvider`,
   `DiskCorpusSource::mount_hybrid()`) was uninstantiated by any committed test.** Only a session-local,
   not-yet-registered mock smoke check had ever exercised it. Because C++ templates are only checked at
   instantiation, a bug in any of those `if constexpr` branches (wrong argument order, a missing
   `co_await`, a mismatched `synchronous_leaf` check) would not have been caught by `ctest` at all until
   `QdrantVectorIndex` was built against the seam for the first time.
4. **`mount_hybrid()`'s dense-leg `add_batch()` failure aborted the sparse-leg commit too, even for chunks
   that only needed the sparse leg** — pure statement-ordering coupling (the two commit blocks ran
   sequentially, second one unreached if the first errored), not a real dependency between the two legs.
   Nothing was permanently lost (a retry re-derives `needs_*` and picks up whatever didn't land), but it
   understated the independence `mount_hybrid()`'s own design (§2.3a) already claims for the two legs.
5. **No cross-tenant isolation test existed for `HybridRagContextProvider`**, even though ADR-063's own
   claim 5 proved this end-to-end for `VectorRagContextProvider` and the hybrid provider reuses the
   identical `Mount`/capability-gated plumbing. The mechanism itself held up on inspection, but the
   guarantee had never been checked for the fused (dense+sparse) case specifically — precisely the class of
   bug ADR-063's own first red-team pass found FIRST, for the sibling provider.

### Minor

6. **RRF's `rrf_k` was an unvalidated constructor `double`** — `rrf_k + rank + 1 <= 0` (e.g. `rrf_k <= -1`
   for rank 0) divides by zero/a negative number, producing `+inf`/an inverted score that would silently
   dominate the ranking. Host-configured only, not reachable from user/model input — not an I2/I3 issue.
7. **`mount_hybrid()`'s within-pass cross-file dedup counters (`chunks_dense_deduped`/
   `chunks_sparse_deduped`) silently undercounted** a chunk that duplicated an earlier PENDING (not yet
   committed) chunk from the same pass — cosmetic, progress/logging counters only, no correctness impact
   (`seen_ids_this_pass` itself still correctly prevented any double-commit).
8. **`restore()` silently discarded whatever an index already held if called on a non-empty index**
   (intended usage was always "fresh, empty index, once, at process start" — documented in prose, never
   enforced). Also named, but explicitly NOT closed by the fix below: a concurrent `add_batch()` racing
   between `restore()`'s parse and its final swap could still be silently overwritten — that race remains
   an open, named residual (see §7).

### What held up

`last_user_text()`'s I3 defense and `neutralize_forged_provenance_markers()`'s citation-forgery defense are
genuinely shared, not duplicated, between the two providers (confirmed both by reading the code and by
diffing against the pre-ADR-180 version — the extraction is byte-for-byte behavior-preserving, and `mount()`
in `corpus_source.hpp` is untouched). `BM25Index`'s formula, tie-break, contract rejection, and its
`unsigned char`-cast-before-`isalnum`/`tolower` are all correct. `BruteForceCosineIndex::snapshot()`
correctly takes the same lock `add_batch()` does, so a snapshot cannot observe a torn write mid-write. RRF
fusion and the citation-forgery defense are both genuinely tested end-to-end for the hybrid provider, not
merely plausibility-checked. The reviewing agent's own verdict: "closer to ADR-063's own first, rougher
red-team pass than its polished second one" — appropriate, since this WAS steps 1-6's first pass, not a
second pass after an earlier round of fixes the way ADR-063 §4/§5's "red-team pass 2" was.

**All 8 findings above were fixed the same session, each with a new, real, passing test proving the fix**
(the full existing suite re-run and green throughout, zero regressions) — see §5-6 below for the fix
details and §3's claim list for which claims each fix closes or strengthens.

## 4b. Red-team pass 2 (2026-09-22, `general-purpose` agent, no prior context) — `QdrantVectorIndex` specifically

Run against `protocol/qdrant/vector_index.hpp` and its offline test once that file existed (pass 1 above
predates it). Six findings, one Critical.

### Critical

1. **`parse_search_response()` trusted `payload.chunk_id` with zero verification against the point's own
   id — breaking, for this one conformer, the exact cross-tenant confidentiality invariant
   `vector_rag_context_provider.hpp`'s own top comment states as this codebase's whole model**
   ("confidentiality across corpora relies entirely on which ids a caller's OWN `IndexT` was ever
   populated with... `WorktreeObjectStore::get_blob()` carries NO capability check of its own"). For
   `BruteForceCosineIndex` that holds structurally (only an in-process reference can populate it). For
   `QdrantVectorIndex`, "the index" is a network-writable Qdrant collection gated by nothing but
   possession of the configured `api-key` — so if two corpora ever share one `collection_name` and key (a
   real, plausible self-hosted-deployment mistake, not enforced against by anything in this ADR), anyone
   able to write to that collection can upsert a point whose `payload.chunk_id` names a digest they do not
   own, and the downstream `render_scored_chunk()`/`get_blob()` path (itself carrying no capability check)
   will fetch and inject that digest's real content into whichever corpus's search happened to return it.

### Real gaps

2. **No batch-size ceiling on `add_batch()`** — `RemoteVectorIndex` declares no `capabilities()`/
   batch-limit concept at all (unlike `Embedder`), and this conformer didn't self-impose one either; an
   enormous batch would build a multi-gigabyte JSON body in memory before any network attempt.
3. **`k == 0` behaved differently between local and remote conformers of the same provider** —
   `BruteForceCosineIndex::search(query, 0)` succeeds empty; Qdrant's own `limit` is documented
   `required, >=1`, so the identical provider configuration (`max_injected_ = 0`) would silently succeed
   locally and likely 400 against Qdrant.

### Minor

4. `contains()`'s 404-means-false logic is correctly scoped to the single-point endpoint, but whether a
   *missing collection* (vs. a missing point) also 404s the same way is unconfirmed — could silently mask
   a misconfigured `collection_name` as "corpus never ingested" rather than a real, actionable error.
5. No finiteness (NaN/Infinity) check on outbound vector/query components before `json::dump()` — shared
   `core/json_value.hpp` infrastructure, not unique to this file, noted because these are the first
   vector-carrying call sites in this ADR family to hit it unguarded.
6. The length-mismatch error message omitted the actual sizes, inconsistent with the sibling
   too-short-id message a few lines above it.

### What held up

Credential resolution is correctly gated (resolved only inside each call, never at construction); no
chunk text, host, or collection selection is ever influenced by model/chunk content; `map_http_status_
error()`'s status classification is sound for every status the research doc names; `contains()`'s 404
handling is correctly scoped; the length-mismatch defense from pass 1 (§5-6 below) holds independent of
`add_batch()`'s own guard; the class is thread-safety-neutral (`const`-qualified, no mutable state,
matching `OpenAIEmbedder`'s identical posture); `json::parse()`'s existing `ParseBudget` (max depth 64,
max nodes 100,000) already bounds adversarial response sizes before `parse_search_response()` ever runs.

**All 6 findings fixed the same session** (2, 3, 6 as real code changes with new passing tests; 1 as a
real defense-in-depth cross-check — explicitly NOT a full fix, see below — plus a prominent, explicit
top-of-file disclosure of the real, operational fix a deployer must apply; 4 and 5 as named, honest
residuals, not silently left undocumented) — see §5-6 below.

## 4c. Red-team pass 3 (2026-09-22, `general-purpose` agent, no prior context) — `VulkanCosineIndex` specifically

Run against `src/backends/vulkan_vector_index/vulkan_cosine_index.{hpp,cpp}`, `cosine_similarity.comp`
(+ its checked-in `.spv`), `tests/test_vulkan_cosine_index.cpp`, and the `AGENTENGINE_WITH_VULKAN` CMake
wiring in both `CMakeLists.txt` files — §8 step 8's own account (above) was the only prior context this
pass started from, per this repo's own established "fresh agent, no context, attack the real cited
source" methodology (ADR-063 §4, ADR-180 §4/§4b). A real Vulkan SDK (1.4.350.0) and a real discrete GPU
(AMD Radeon RX 5300M) were both available and used for every claim below — no finding here is asserted
from code reading alone without an executed, passing (or, before the fix, would-have-failed) check
against real hardware, matching this session's own "every claim must be real, executed, and tested"
discipline. One Critical, two Real gaps, one Minor.

### Critical

1. **Nearly every Vulkan API call inside `search()`'s hot path was fire-and-forget — no `VkResult` check,
   no mapped-pointer check.** `vkMapMemory` (×3: the vectors staging upload, the query buffer upload, the
   scores readback), `vkAllocateCommandBuffers` (×2), `vkBeginCommandBuffer` (×2), `vkEndCommandBuffer`
   (×2), `vkCreateFence` (×2), `vkQueueSubmit` (×2), `vkWaitForFences` (×2), and `vkResetCommandBuffer`
   (×1) all ignored their return value. A genuine, in-spec failure at any of these points (host or device
   out-of-memory, a lost device) would NOT have produced the typed `agentengine::result` error this
   codebase's fail-closed/"reject-not-coerce" posture requires — it would have either crashed outright
   (`std::memcpy(nullptr, ...)` after a silently-failed `vkMapMemory` left `mapped` as `nullptr`) or
   invoked undefined behavior (`vkWaitForFences` waiting on a `VK_NULL_HANDLE` fence that a
   silently-failed `vkCreateFence` never actually created — the Vulkan spec requires every fence handle
   passed to `vkWaitForFences` to be valid). This is the exact class of gap CLAUDE.md's/this repo's own
   "fail-closed behavior" review discipline names directly, and the same posture
   `BruteForceCosineIndex::restore()`'s own red-team-pass-1 fix (§4 finding 1, §5-6) already established
   for this ADR family: reject, don't crash, on a structural/environmental failure the caller's contract
   never promised couldn't happen.

### Real gaps

2. **`search()`'s push constants (`cosine_similarity.comp`'s `{uint32 count; uint32 dimension;}`) were
   populated via `static_cast<std::uint32_t>(n)`/`(dim)` with no prior check that `n`/`dim` (both
   `std::size_t`, 64-bit) actually fit in 32 bits.** A candidate count or dimensionality that fits in
   `size_t` but exceeds `UINT32_MAX` would silently WRAP at that cast, dispatching the shader against a
   wrong, wrapped `count`/`dimension` that disagrees with what the host actually allocated
   (`n * dim * sizeof(float)` bytes, computed in 64-bit `VkDeviceSize`, which does not wrap at the same
   32-bit boundary) — a structural integer-overflow gap of exactly the kind this pass's own brief asked
   about directly (only astronomically impractical to reach in practice, since holding that many real
   vectors in memory is itself infeasible, but a real, named structural fact about the code, not merely a
   probabilistic hand-wave — the same framing `QdrantVectorIndex`'s own point-id collision residual, §7,
   already uses for an analogous "practically unreachable but structurally real" gap). No test exercised
   this at all.
3. **Test coverage gap: `VulkanCosineIndex`'s own test file never exercised several `VectorIndex` contract
   behaviors `BruteForceCosineIndex`'s own test suite already covers** — an empty (never populated)
   index's `search()`, `search(k=0)`, `search(k > corpus size)`, a duplicate id rejected ACROSS two
   separate `add_batch()` calls (only a duplicate WITHIN one call was tested), and more than one
   cache-rebuild cycle on the same instance (every existing test populated an instance exactly once
   before searching it — the persistent-buffer-cache's "destroy the OLD buffer, then reassign"
   invalidation path, §5-6's own step-8 account, had only ever been exercised on its FIRST build, never
   its second or third). Code reading confirms the underlying implementation already mirrors
   `BruteForceCosineIndex`'s identical logic for the first four cases (this was a coverage gap, not a
   behavior divergence); the repeated-cache-rebuild case had never been proven correct at all before this
   pass.

### Minor

4. **`vkCmdDispatch`'s `group_count` (`(n + 63) / 64`) is never checked against the physical device's own
   `VkPhysicalDeviceLimits::maxComputeWorkGroupCount[0]`.** Every Vulkan-conformant device guarantees at
   least 65535 workgroups per dimension, so this is unreachable at the `n` sizes this backend is actually
   exercised at (1000, 5000) and remains unreachable up to `n` in the low millions even on a device
   offering only that guaranteed minimum — but nothing in this file asserts that bound, and no validation
   layer is enabled (`vulkan_cosine_index.cpp`'s own instance-creation comment: "no extensions, no
   validation layers -- pure headless compute") to catch a violation if one somehow occurred. Named as an
   honest, low-likelihood residual (§7), not fixed — the new uint32 dispatch-size guard (finding 2's fix)
   already bounds how large `n` can get before that check fires first; fixing this specific limit would
   need querying and threading `VkPhysicalDeviceLimits` through `create()`, a real but not urgent
   follow-on.

### What held up

The persistent GPU vectors-buffer cache's OLD-buffer-destroyed-only-after-the-NEW-buffer-is-fully-built
ordering (`vulkan_cosine_index.cpp`'s own rebuild block) is correct — confirmed both by code reading and
by this pass's own new repeated-`add_batch()`-then-`search()` test (§5-6) exercising it across THREE
consecutive rebuild cycles on one instance, run clean under both a plain build and AddressSanitizer.
`Impl`'s destructor teardown order (every Vulkan handle destroyed in reverse-creation order, device
before instance, every handle either real or a documented-no-op `VK_NULL_HANDLE`) is correct — no
double-free or use-after-free found. `search()`'s `unique_lock` (not `shared_lock`, unlike
`BruteForceCosineIndex`) genuinely serializes every concurrent caller through the ENTIRE
submit-and-wait, and each `VulkanCosineIndex` instance owns its own independent `VkInstance`/`VkDevice`/
`VkQueue` — so the "Vulkan queues are not implicitly thread-safe" concern this pass's own brief named does
not have a live race to exploit here, either within one instance (excluded by the lock) or across
instances (no shared state to race on). The compute shader's tail-invocation guard
(`if (i >= pc.count) return;`) correctly excludes the last workgroup's excess lanes from both the
vectors-buffer read and the scores-buffer write — confirmed both by code reading and empirically (GPU and
CPU cosine scores agree to ~1e-7 at n=1000 and n=5000, neither divisible by the shader's
`local_size_x = 64`, so the tail path is genuinely exercised at both sizes, not merely at a conveniently
round `n`). `AGENTENGINE_WITH_VULKAN=OFF` (the default) was confirmed zero-impact by two independent
methods: a `grep` across both `CMakeLists.txt` files found no Vulkan target/symbol reference outside the
two `if(AGENTENGINE_WITH_VULKAN)` guards, and a real `cmake`+Ninja configure of the WHOLE project (MSVC
14.51, real toolchain) was attempted with the option BOTH off and on — both fail identically, at an
EARLIER, wholly unrelated `tests/compile_fail/*.cpp` positive-control `try_compile` step
(`C1083: Cannot open compiler generated file: ''`, an intermittent MSVC/Ninja issue unrelated to any file
this pass touched — reproduced against `tainted_declassify_positive_control.cpp`, a file with no
connection to Vulkan), confirming this is the SAME pre-existing, already-disclosed (§7, §8 step 8's own
account) environment/toolchain blocker on this dev box, not something the Vulkan wiring introduces. Since
the whole-project configure could not be completed for that unrelated reason, `VulkanCosineIndex` itself
(both the library translation unit and the test binary) was instead compiled, linked, and run directly
against the real Vulkan SDK and the real GPU — the same fallback method the original building agent used —
cleanly, with zero warnings under `/W4 /WX` (CONVENTIONS.md's own mandated flags), both in a plain build
and under AddressSanitizer.

**All 4 findings fixed the same session** (1 and 2 as real code changes with new passing tests; 3 as 12
new proving assertions closing the coverage gaps named; 4 as a named, honest residual, not fixed) — see
§5-6 below for the fix details.

## 4d. Red-team pass 4 (2026-09-22, `general-purpose` agent, no prior context) — `VulkanCosineIndex`,
second pass

Run against the SAME files §4c reviewed (`src/backends/vulkan_vector_index/vulkan_cosine_index.{hpp,cpp}`,
`cosine_similarity.comp` + its checked-in `.spv`, `tests/test_vulkan_cosine_index.cpp`, and the
`AGENTENGINE_WITH_VULKAN` CMake wiring), post-§4c-fix — a genuinely SECOND, independent pass, run because
this ADR's own Status line named it an open question whether `VulkanCosineIndex` was owed one, mirroring
`QdrantVectorIndex`'s own two-pass precedent (§4b). This pass started from §4c's own account only (the
same "fresh agent, no prior context, attack the real cited source" discipline as every prior pass), was
explicitly briefed NOT to re-report anything §4c already found and fixed, and re-verified §4c's own fixes
directly rather than trusting the prior pass's account. A real Vulkan SDK (1.4.350.0, confirmed via
`VULKAN_SDK`) and a real discrete GPU were both available and used for every claim below; the
whole-project `cmake`+Ninja configure was re-attempted (not merely cited from §4c) and a real
AddressSanitizer run was executed against this pass's own rebuilt binary, including its own new tests —
no finding here is asserted from code reading alone. One Critical, two Real gaps, one Minor.

### Critical

1. **`vkBindBufferMemory`'s `VkResult` was discarded in BOTH of `search()`'s buffer-creation lambdas
   (`create_host_visible_buffer()` and `create_device_local_buffer()`)** — meaning EVERY buffer this
   class ever creates (the persistent, cached vectors device-local buffer; its host-visible staging
   buffer; the query buffer; the scores buffer) went through an unchecked bind, despite §4c's own
   account claiming "every Vulkan call in search()'s hot path" was checked. A real, in-spec bind failure
   (`VK_ERROR_OUT_OF_HOST_MEMORY`/`VK_ERROR_OUT_OF_DEVICE_MEMORY`) would have silently handed back a
   `(VkBuffer, VkDeviceMemory)` pair that LOOKS fully constructed but has no memory actually bound to
   it — using such a buffer in `vkMapMemory`, `vkCmdCopyBuffer`, or a shader binding is undefined
   behavior per the Vulkan spec (a buffer must have bound memory before any use), the identical class of
   gap §4c's own Critical finding fixed for this file's OTHER calls, just missed at this one, twice-
   duplicated call site. This is exactly the kind of thing a first pass whose brief was "is every Vulkan
   call checked" can still miss: §4c's own fix list enumerates every checked call by NAME
   (`vkMapMemory`, `vkAllocateCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`,
   `vkCreateFence`, `vkQueueSubmit`, `vkWaitForFences`, `vkResetCommandBuffer`) and `vkBindBufferMemory`
   is not on it — a real, not merely theoretical, gap in that pass's own coverage.

### Real gaps

2. **`create()`'s device-enumeration step had the same unchecked-`VkResult` gap §4c's Critical fixed
   elsewhere, in the one call site §4c's own brief (scoped to `search()`'s hot path) didn't reach.**
   Both `vkEnumeratePhysicalDevices` calls (the count query and the fill query) discarded their return
   value. A real failure at the count-query call leaves `device_count` in an implementation-defined
   state per spec (not guaranteed to stay 0), which the old code would have sized `devices` off of
   directly; separately, and independent of whether the return value is checked, the fill call can
   legitimately report FEWER devices than the count query did (a device removed between the two calls,
   a real if rare case the Vulkan spec explicitly allows via `VK_INCOMPLETE`), and the old code iterated
   the WHOLE (larger, stale-sized) `devices` vector regardless, including value-initialized
   (`VK_NULL_HANDLE`) tail entries the second call never actually wrote — calling
   `vkGetPhysicalDeviceQueueFamilyProperties` on such a handle is invalid API usage. Construction-time
   only, and requires either an early host/device OOM or a mid-enumeration device removal to reach, so
   materially less likely than finding 1 above — a Real gap, not Critical.
3. **A zero-dimensional vector (`add_batch({"a"}, {{}})`) was a second, distinct, and REACHABLE way to
   set `impl_->dimension == 0` on a non-empty index — contradicting `search()`'s own comment, which
   claimed that state was "possible only if this index has never been `add_batch()`'d with a real
   vector, already excluded above by the `order.empty()` early return."** That reasoning was simply
   wrong: `add_batch()`'s existing dimension logic (`expected_dim = ... vectors.front().size()`) already
   accepted a 0-sized vector as establishing a 0-dimensional index, identically to how
   `BruteForceCosineIndex::add_batch()` (`core/vector_index.hpp`) already does — but where
   `BruteForceCosineIndex::search()` handles that state silently (a 0/0-guarded cosine similarity
   returning 0.0f for everything, no error at all, an existing, already-Judged ADR-063 quirk out of
   scope for this pass to touch), `VulkanCosineIndex::search()` instead classified it as
   `failure_class::fatal` ("internal invariant was violated" — `error.hpp`: "unrecoverable; the run
   ends") — the wrong severity for reachable, caller-supplied degenerate input, not an actual internal
   bug, and a behavior DIVERGENCE from the sibling conformer this file's own top comment claims to
   "mirror... exactly." No memory-safety consequence (the dispatch-size guard and the `dim == 0` check
   both correctly prevent an invalid zero-byte GPU buffer from ever being created), but a real
   classification/documentation defect a caller's error-handling logic could act on incorrectly (e.g.
   treating a normal, correctable input mistake as grounds to abort the whole run, per `error.hpp`'s own
   stated `fatal` semantics).

### Minor

4. **A fully-empty `add_batch({}, {})` call (a legitimate no-op, distinct from finding 3's zero-
   DIMENSIONAL-vector case) still unconditionally sets `impl_->cache_dirty = true`**, forcing the next
   `search()` call to rebuild the GPU vectors buffer even though nothing about the corpus changed. Not a
   correctness bug (proven by this pass's own new test: a no-op `add_batch()` sandwiched between real
   ones still produces a correct subsequent `search()`), purely a performance cost, and one this pass did
   not benchmark — named honestly rather than fixed, matching this file's own established "a hot path
   without a bench... is not done" bar cutting against optimizing a path with no evidence it is ever hit
   in practice by a real caller (nothing in this ADR family constructs an intentionally-empty
   `add_batch()` call site).

### What held up

Every §4c fix was re-verified directly, not merely trusted from that pass's own account. The `CleanupStack`
introduced by §4c's Critical fix was traced through EVERY early-return path in the vectors-buffer rebuild
block by hand (`vkAllocateCommandBuffers`/`vkBeginCommandBuffer`/`vkEndCommandBuffer`/`vkCreateFence`/
`vkQueueSubmit`/`vkWaitForFences` failures) — `dismiss()` is reached only after the copy fully succeeds,
and every earlier return correctly unwinds whatever was pushed so far in reverse order, confirmed
consistent for all six failure points, not merely the ones §4c's own account narrated. Every `vkMapMemory`
check (×3: vectors staging upload, query upload, scores readback) genuinely guards every subsequent use of
its `mapped` pointer — no branch dereferences it before the check, none after an early return past it.
`detail::check_gpu_dispatch_size_plausible()` has exactly ONE call site (`search()`, before any GPU
resource is touched) and push constants are constructed at exactly one place in the whole file — no second
call site could bypass it even if one existed today. `add_batch()` itself makes no Vulkan calls at all
(pure host-side bookkeeping), so it carries none of this file's `VkResult`-gap class by construction, and a
rebuild failure inside `search()` correctly leaves the OLD, still-valid vectors buffer in place (re-traced
by hand: the old buffer is destroyed only after the new one is fully built AND uploaded, and only on that
success path — re-confirmed, not merely re-asserted from §4c). `create()`'s own teardown-on-partial-failure
was traced through every one of its now-eleven checked failure points: every early `return
std::unexpected(...)` lets the local `std::unique_ptr<Impl>` go out of scope, invoking the
already-independently-confirmed-correct reverse-order destructor (every handle either real or a documented
`VK_NULL_HANDLE` no-op) — no leaked instance/device/pipeline on any later step's failure. Thread-safety was
independently re-verified, not trusted from §4c's claim: every method touching shared `Impl` state takes
the mutex (`add_batch()`/`search()`: `unique_lock`; `contains()`/`size()`: `shared_lock`) — there is no
getter, stats accessor, or other method that reads/writes the GPU-buffer-cache fields without it. The
CMake wiring's default-OFF impact is unaffected by this pass's changes (only the `.cpp`'s own call sites
changed; no new target/symbol was added outside the existing `if(AGENTENGINE_WITH_VULKAN)` guards). The
whole-project `cmake`+Ninja configure (`-DAGENTENGINE_WITH_VULKAN=ON`) was RE-ATTEMPTED this pass, against
an even newer preview MSVC toolset (VS 2026 "18", MSVC 14.51.362xx) than §4c used — and reproduced the
IDENTICAL `C1083: Cannot open compiler generated file: ''` failure at the IDENTICAL unrelated file
(`tests/compile_fail/tainted_declassify_positive_control.cpp`), confirming this is a persistent,
still-unresolved, pre-existing environment/toolchain blocker, not something that has resolved itself or
that this backend's changes affect. `VulkanCosineIndex` (library + test binary) was again compiled, linked,
and run directly against the real Vulkan SDK/GPU, cleanly, zero warnings under `/W4 /WX`, in both a plain
build and under AddressSanitizer (this pass's rebuilt binary, 63 total assertions — 48 from §4c's own file
plus 15 new ones this pass added — 0 failures, both configurations). **New this pass**: an UndefinedBehavior
Sanitizer attempt via `clang-cl` (22.1.5) was made — not attempted by §4c — and was blocked by a DIFFERENT,
unrelated toolchain-interop gap: `clang-cl` failed to recognize C++20/23 STL surface (`std::span`,
`std::expected`, `unordered_map::contains()`, CTAD for `std::unique_lock`) when compiling against this
preview MSVC 14.51 STL, even with an explicit `/std:c++23`/`-std=c++23`, diagnosed down to a language-mode/
feature-macro mismatch between this specific `clang-cl` release and this specific preview MSVC STL — a
real, distinct, environment-specific gap, not caused by or fixable from this backend's own code, named as
a residual (§7) rather than chased further, matching the exact same "don't spend the session fixing an
unrelated toolchain issue" discipline §4c's own C1083 finding already established.

**All 4 findings fixed the same session** (1 and 2 as real code changes — new checked `VkResult`s with
correct cleanup on failure, no new test possible for either without an actual forced Vulkan/host OOM,
matching §4c's own identical "not provable via forced-failure test" residual for its own Critical fix; 3
as a real code change — `add_batch()` now rejects a zero-dimensional vector as an ordinary `contract`
violation, with `search()`'s own comment corrected to match the now-TRUE invariant — plus 15 new proving
assertions (zero-dimension rejection, both before and after a real dimensionality is established; a
genuine empty-batch no-op is NOT confused with it; an n=1/dim=1 corpus with intervening no-op
`add_batch()` calls; `k` exactly equal to corpus size); 4 as a named, honest residual, not fixed) — see
§5-6 below for the fix details.

## 4e. Red-team pass 5 (2026-09-23, general-purpose agent, no prior context) — VulkanCosineIndex, third pass

Run against the same files §4c/§4d reviewed (`src/backends/vulkan_vector_index/vulkan_cosine_index.{hpp,cpp}`,
`cosine_similarity.comp` + its checked-in `.spv`, `tests/test_vulkan_cosine_index.cpp`, and the
`AGENTENGINE_WITH_VULKAN` CMake wiring), post-§4d-fix, at `main`'s tip (`11f774f`). The project owner asked
for this pass specifically because §4d found a new Critical that §4c missed. It was the same bug class (a
discarded `VkResult`), at a call site outside §4c's narrower brief. So this pass's brief was to be
**systematic rather than sampled**. It started from §4c/§4d's own accounts only, re-reported nothing either
of them already found and fixed, and did not relitigate their named residuals. A real Vulkan SDK
(1.4.350.0) and the real discrete GPU (AMD Radeon RX 5300M, AMD proprietary driver, confirmed via
`vulkaninfo`) were used for every claim below. Every finding has an executed pre-fix reproduction and an
executed post-fix proof. None rests on code reading alone. **One Critical, five Real gaps, two Minor.**

Method, stated so the next reader can see coverage was complete rather than sampled. The prior passes
searched by eye. This pass used four mechanical instruments none of them had:

1. **An exhaustive Vulkan-entry-point inventory** (the table below): all 47 distinct `vk*` entry points in
   the file, each classified as `VkResult`-returning or void. Each `VkResult`-returning call was checked for
   whether its failure is handled. Each void call was checked for the precondition it relies on.
2. **The Khronos validation layer** (`VK_LAYER_KHRONOS_validation`, SDK 1.4.350.0) over the full suite,
   with synchronization validation and best-practices enabled.
3. **The Khronos profiles layer** (`VK_LAYER_KHRONOS_profiles`), emulating the SDK's own
   `VP_ANDROID_vulkan_profile_2021` baseline profile. This tests against a spec-floor device, not just the
   one GPU on this desk.
4. **A test-only fault-injection seam.** It made every checked-`VkResult` failure branch actually execute
   for the first time. §4c and §4d both recorded, as an accepted residual, that none of those branches had
   ever run.

That fourth instrument is what found the Critical. It is not a discarded `VkResult`. Every `VkResult` on
that path WAS checked. The bug was the cleanup ordering after a correctly detected failure, so a brief
framed as "is every `VkResult` checked" could not have found it.

### Critical

1. **The scores-buffer regrow destroyed the OLD buffer before creating the new one. So a failed creation
   left destroyed handles cached, and they were then destroyed a second time.** In `search()`, when `n`
   outgrew `scores_buffer_capacity`, the block called `vkDestroyBuffer`/`vkFreeMemory` on
   `impl_->scores_buffer`/`scores_memory` FIRST, then called `create_host_visible_buffer()`. If that
   creation failed (any of its three checked calls: `vkCreateBuffer`, `vkAllocateMemory`,
   `vkBindBufferMemory`), `search()` correctly returned a typed error. But `impl_` still held the
   just-destroyed handles, and the old, now-too-small capacity. The next `search()` on the same instance
   re-entered the block, because `n` still exceeded that capacity, and destroyed/freed those handles
   AGAIN. `~Impl()` would have done so once more. The result is a double-destroy/double-free of driver
   objects, which is undefined behavior per the Vulkan spec.
   **Executed on the real GPU.** A scratch build of the fixed `.cpp` had ONLY this block reverted to its
   pre-fix ordering, and was driven by this pass's fault-injection test:
   - With no layer loaded, the process died with **`0xC0000374` (STATUS_HEAP_CORRUPTION)**. The AMD driver
     corrupted the process heap on the second free.
   - Under the validation layer it reported **18 validation errors**, led by
     `VUID-vkDestroyBuffer-buffer-parameter` ("Invalid VkBuffer Object").

   The fixed build shows 0 validation errors and no crash. This is the "vectors-buffer rebuild ordering"
   lesson §4c's "What held up" praised in one block, never applied to its sibling block a few dozen lines further
   down. Reachable only after an allocation failure, like every failure-path finding in §4c/§4d. But
   unlike theirs, this one turns a correctly reported, recoverable out-of-memory into heap corruption on
   the NEXT call.

### Real gaps

2. **The device's own `maxStorageBufferRange` was never enforced, and the realistic corpus size where it
   bites is 21,846 vectors.** The whole flattened vectors buffer is bound as ONE storage-buffer descriptor
   with `VK_WHOLE_SIZE`. The spec requires that effective range to be `<=
   VkPhysicalDeviceLimits::maxStorageBufferRange` (`VUID-VkWriteDescriptorSet-descriptorType-00333`).
   §4c's uint32 guard checks only that `n`/`dim` fit the push constants, which is necessary but far from
   sufficient.
   - The spec's required minimum for that limit is 2^27 bytes (128 MiB). That is exactly the value the SDK's
     `VP_ANDROID_vulkan_profile_2021` baseline profile declares (`Config/VK_LAYER_KHRONOS_profiles/`, read
     from the SDK, not from memory).
   - At dim=1536, a 21,846-vector corpus crosses it. That is a realistic RAG corpus, not an adversarial one.
   - **Reproduced** under the profiles layer emulating that baseline, plus the validation layer. `search()`
     at n=21,846 produced `VUID-VkWriteDescriptorSet-descriptorType-00333` ("effective range [134221824] is
     greater than maxStorageBufferRange (134217728)").

   It was not reachable on the reference AMD device, whose own limit is 4 GiB−1 (`vulkaninfo`). That is why
   no prior pass saw it, and why this is rated a Real gap rather than Critical. The same missing bound also
   left the shader's own 32-bit index arithmetic unguarded: `uint base = i * pc.dimension` wraps once
   `n*dim >= 2^32`. Enforcing `n*dim*4 <= maxStorageBufferRange`, itself a uint32, caps `n*dim` below 2^30,
   so it closes that too. Fixed alongside it: §4c's named-but-unfixed `maxComputeWorkGroupCount[0]`
   residual (finding 4). It needed the same `VkPhysicalDeviceLimits` query this fix introduces, so it was
   closed here rather than left open.

3. **Finite inputs outside float32's squared range made GPU scores diverge from CPU scores by up to the
   full `[-1, 1]` range. That falsifies §3 claim 7(b) as stated, for finite input.** The shader squares RAW
   components in float32. `BruteForceCosineIndex` accumulates in double. Any component with `|x| > ~1.8e19`
   overflows to `+inf` when squared, and any with `|x| < ~1e-19` underflows to 0.
   **Reproduced** with stored vectors `{1e20, 1e20}` (exact match), `{1e-25, 1e-25}` (exact match), and
   `{1, 0.2}`, against query `{1, 1}`:
   - CPU scored the two exact matches `1, 1` and the third `0.832`.
   - GPU scored them `0, 0, 0.832`, so both exact matches ranked **last**.
   - A `{1e20, 1e20}` query gave GPU **NaN** scores.

   Claim 7(b) bounds GPU/CPU divergence to "near-tied scores" (epsilon 1e-3). This was a divergence of 1.0,
   from ordinary finite floats. Not realistic embedding magnitudes, but reachable through the public API,
   and outside the residual §2.6 claims to bound.

4. **NaN/Inf components were accepted, and the resulting NaN scores reached `std::sort` under a
   comparator that is not a strict weak ordering over NaN. That is undefined behavior, not just a wrong
   answer.** The comparator `a.score != b.score ? a.score > b.score : a.id < b.id` makes NaN "equivalent"
   to every score, so equivalence is not transitive. That violates `std::sort`'s precondition.
   **Reproduced**: 200 vectors, 29 of them with a NaN component. The GPU `search()` output had **11
   descending-order violations among the FINITE scores**, so the non-NaN results themselves came back
   mis-ranked. `BruteForceCosineIndex` produced the identical 11 violations. It shares the comparator and
   also accepts NaN, which is **a latent defect in the already-Judged ADR-063 CPU conformer, named in §7,
   not fixed here** (out of this backend's scope). `QdrantVectorIndex`'s §4b Minor 5 had already noticed
   the non-finite-input gap on the remote side.

5. **A moved-from `VulkanCosineIndex` crashed on any method call.** Move operations are `= default` on a
   `unique_ptr<Impl>`, so a moved-from instance holds a null pimpl. Every method then dereferenced it
   (`impl_->mutex`). The natural `VulkanCosineIndex idx = std::move(*created);` idiom, which this test file
   itself used 6 times before this pass, leaves exactly such an object inside `created`.
   **Reproduced**: `size()` on a moved-from instance gave **`0xC0000005` (access violation)**. That falls
   short of the standard library's "valid but unspecified" moved-from convention, and of this codebase's
   "typed error, never a crash" posture.
   Copy is correctly `= delete`d, so there is no double-own. What held up there is recorded below.

6. **Test-methodology gap: no checked-`VkResult` failure branch in this file had EVER executed.** §4c and
   §4d both accepted this as a residual, since a real OOM or device-lost cannot be induced safely.
   Finding 1 is the direct proof of what that cost. A cleanup-ordering bug sat on exactly such a branch
   through two prior passes, each of which reviewed that code by hand.

### Minor

7. **Every runtime Vulkan failure collapsed into one `failure_class::resource` error, with no trace of
   WHICH `VkResult` occurred.** A caller could not tell a possibly transient OOM from `VK_ERROR_DEVICE_LOST`.
   After device loss, this instance's `VkDevice` is permanently unusable and every later call fails too.
   Checked against `error.hpp`'s definitions:
   - `contract` is used for caller input: correct.
   - `resource` is used for limits and allocation failure: correct.
   - `fatal` survives only on the genuinely unreachable `dim == 0` invariant: correct since §4d.

   So no class is outright wrong, but device-lost is poorly served by `resource` ("budget, quota, or limit
   exceeded").

8. **The header's top comment still described the ORIGINAL design** ("the stored vectors are re-uploaded
   to a GPU buffer on every `search()` call"). §5-6's step-8 account replaced that with a persistent cache
   before any red-team pass ran. The header was stale documentation that three prior reviews read past.

### Vulkan-call coverage table

Every distinct `vk*` entry point in `vulkan_cosine_index.cpp`: 47 in total, 22 of them `VkResult`-returning.
"Fault point" means this pass's injection seam makes that call's failure branch execute. The branch is
proven by `tests/test_vulkan_cosine_index_fault_injection.cpp`: the typed error, the same-instance recovery,
and zero validation errors.

| Entry point | Sites | Returns `VkResult`? | Failure handling / void-call precondition | Failure branch executed? |
|---|---|---|---|---|
| `vkCreateInstance` | 1 | yes | checked (original) | yes, `create_instance` |
| `vkEnumeratePhysicalDevices` | 2 | yes | checked, `VK_INCOMPLETE` accepted on fill, `resize` to written count (§4d) | yes, `enumerate_physical_devices` #1 (count) and #2 (fill) |
| `vkGetPhysicalDeviceQueueFamilyProperties` | 2 | void | handles come only from a successful enumeration, list resized to the count actually written (§4d) | n/a |
| `vkGetPhysicalDeviceProperties` | 1 | void | NEW (pass 5): same handle guarantee; feeds finding 2's limits | n/a |
| `vkCreateDevice` | 1 | yes | checked | yes, `create_device` |
| `vkGetDeviceQueue` | 1 | void | family index + queue 0 are exactly what `VkDeviceQueueCreateInfo` requested | n/a |
| `vkCreateCommandPool` | 1 | yes | checked | yes, `create_command_pool` |
| `vkCreateDescriptorSetLayout` | 1 | yes | checked; bindings 0/1/2 match the shader (see "What held up") | yes, `create_descriptor_set_layout` |
| `vkCreateDescriptorPool` | 1 | yes | checked | yes, `create_descriptor_pool` |
| `vkCreatePipelineLayout` | 1 | yes | checked; 8-byte push range matches the shader | yes, `create_pipeline_layout` |
| `vkCreateShaderModule` | 1 | yes | checked; `pCode` 4-byte aligned (`alignas(4)` in the generated header) | yes, `create_shader_module` |
| `vkCreateComputePipelines` | 1 | yes | checked | yes, `create_compute_pipeline` |
| `vkGetPhysicalDeviceMemoryProperties` | 1 | void | valid physical device; `1u << i` safe (`memoryTypeCount <= 32`) | n/a |
| `vkCreateBuffer` | 2 (4 buffers) | yes | checked | yes, `create_buffer` #1-#4 |
| `vkGetBufferMemoryRequirements` | 2 | void | only after a successful `vkCreateBuffer` | n/a |
| `vkAllocateMemory` | 2 | yes | checked; buffer destroyed on failure | yes, `allocate_memory` #1-#4 |
| `vkBindBufferMemory` | 2 | yes | checked, memory + buffer released on failure (§4d) | yes, `bind_buffer_memory` #1-#4 (first execution ever of §4d's own Critical fix) |
| `vkMapMemory` | 3 | yes | checked + null-pointer checked (§4c) | yes, `map_memory` #1 (staging), #2 (query), #3 (scores) |
| `vkUnmapMemory` | 3 | void | only after a successful map | n/a |
| `vkAllocateCommandBuffers` | 2 | yes | checked; cached handle nulled on failure | yes, `allocate_command_buffers` #1 (copy), #2 (dispatch) |
| `vkBeginCommandBuffer` | 2 | yes | checked | yes, `begin_command_buffer` |
| `vkCmdCopyBuffer`, `vkCmdPipelineBarrier` (×2), `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, `vkCmdDispatch` | 7 | void | recorded only between a successful begin and end; push size 8 == declared range; dispatch count now `<= maxComputeWorkGroupCount[0]` (pass 5, finding 2) | n/a |
| `vkEndCommandBuffer` | 2 | yes | checked | yes, `end_command_buffer` |
| `vkCreateFence` | 2 | yes | checked | yes, `create_fence` |
| `vkQueueSubmit` | 2 | yes | checked; `VkResult` now in the message (finding 7) | yes, `queue_submit` |
| `vkWaitForFences` | 2 | yes | checked; `VK_ERROR_DEVICE_LOST` now gets its own code (finding 7) | yes, `wait_for_fences` (as `DEVICE_LOST`, after the real wait completed) |
| `vkResetCommandBuffer` | 1 | yes | checked (§4c) | yes, `reset_command_buffer` |
| `vkAllocateDescriptorSets` | 1 | yes | checked; cached handle now nulled on failure (pass 5) | yes, `allocate_descriptor_sets` |
| `vkUpdateDescriptorSets` | 1 | void | set allocated; all three buffers non-null; **effective range `<= maxStorageBufferRange` was NOT guaranteed (finding 2, fixed)** | n/a |
| `vkDestroyBuffer` / `vkFreeMemory` | lambda error paths, `CleanupStack`, rebuild swap, scores regrow, `~Impl` | void | **"valid handle" violated on the scores-regrow failure path (finding 1, Critical, fixed)**; every other site validated clean across all 59 injected-failure teardowns | n/a (every site exercised via the fault points above) |
| `vkDestroyFence` | per-call + `CleanupStack` | void | valid, created handle only | n/a |
| `vkFreeCommandBuffers` | 3 | void | not pending: after a successful wait, or never submitted. See §7 for the genuine-device-lost case | n/a |
| `vkFreeDescriptorSets` | 1 (`~Impl`) | yes | result ignored: correct, since the spec's only return code for it is `VK_SUCCESS` | n/a (no failure mode exists) |
| `vkDestroyPipeline`, `vkDestroyShaderModule`, `vkDestroyPipelineLayout`, `vkDestroyDescriptorPool`, `vkDestroyDescriptorSetLayout`, `vkDestroyCommandPool`, `vkDestroyDevice`, `vkDestroyInstance` | 1 each (`~Impl`) | void | reverse creation order, every one NULL-guarded; validated clean after every one of the 9 `create()`-step failures | n/a |

Result: 21 of the 22 `VkResult`-returning entry points now have an executed failure branch. The 22nd,
`vkFreeDescriptorSets`, has no failure code in the spec.

### What held up

- **The checked-in `.spv` is exactly its source.** `glslc` (SDK 1.4.350.0) recompiled `cosine_similarity.comp`
  **byte-identical** to the checked-in 3100-byte `.spv` (`cmp`), and `spirv-dis` diffs are empty.
  `spirv-val` passes. It is SPIR-V 1.0, so any Vulkan 1.0+ device accepts it. The disassembly confirms
  what the host declares:
  - three `BufferBlock` storage buffers at set 0, bindings 0/1/2, matching the descriptor-set layout's
    three `STORAGE_BUFFER` bindings exactly;
  - a `PushConstants` block of two `uint`s at offsets 0/4, matching the 8-byte push range at offset 0.
  (`glslangValidator -V` output differs only because it is a different generator, not a staleness
  signal.)
- **Memory-type selection is sound.**
  - The spec guarantees a `HOST_VISIBLE|HOST_COHERENT` type exists, so no flush/invalidate is missing on
    the three host-visible buffers.
  - The device-local path's "any type" fallback is safe, because that buffer is only ever written by
    `vkCmdCopyBuffer`.
  - The `UINT32_MAX` no-match sentinel is checked at both call sites before use as an index.
- **Copy is deleted, so the pimpl is never double-owned.** Moving is sound: a moved-from object was always
  safe to DESTROY, since `unique_ptr` is null. Only calling methods on it crashed (finding 5).
- **Zero-norm handling agrees between GPU and CPU.** The shader's `denom == 0` guard and the CPU's
  `na == 0 || nb == 0` guard both score a zero vector 0, which this pass's own test re-confirms end to end.
- **Clean under the validation layer on every happy path.** Both the pre-fix 63-assertion suite and
  the post-fix 86-assertion suite ran under the validation layer, with synchronization validation and
  best-practices, and each reported **zero** errors or warnings. In the post-fix run, each of the 11
  instances printed the layer's "Khronos Validation Layer Active" banner, listing both enables, as a
  positive control that it actually ran. No finding above is a happy-path API misuse. Every one sits on
  a failure path, an input edge, or a device limit the reference GPU does not hit.
- **The exact power-of-two rescale changed no in-range result at all.** Claim 7(b)'s measured max
  |GPU−CPU| is **bit-identical before and after** this pass's rescale fix: 1.04308e-07 at n=1000 and
  1.37836e-07 at n=5000.

Two environment residuals §4c and §4d carried were also resolved this pass. Neither was a code finding:

- **The "C1083" whole-project configure blocker is Windows `MAX_PATH`, not an intermittent MSVC/Ninja
  bug.** The failing `try_compile` object paths reach about 248 characters from a worktree build
  directory. Moving the build directory shortened the path and moved the failure to a later, longer-named
  `try_compile`. A short root, `subst Q:` onto the scratchpad, made the **whole-project `cmake`+Ninja
  configure succeed with `AGENTENGINE_WITH_VULKAN=ON`**. Both Vulkan test targets, including this pass's
  new one, then built through the REAL CMake targets (with `agentengine_warnings`), and `ctest -R vulkan`
  passed 2/2.
- **UBSan now runs.** §4d's `clang-cl` attempt used LLVM 22.1.5 with `/std:c++23`. The Visual Studio-bundled
  `clang-cl` 22.1.3 (`VC\Tools\Llvm`) with `/std:c++latest /MT -fsanitize=undefined
  -fno-sanitize-recover=all` compiles against the same preview STL. With a signed-overflow canary as a
  positive control (it trapped), both test binaries ran **clean under UBSan**.

**Eight findings: one Critical, five Real gaps, two Minor. All fixed the same session** with executed proof.
See §5-6.

## 5-6. Implementation and proof

**§8 steps 1-6, real, this session (2026-09-22):**
- `core/vector_index.hpp`: `PersistentVectorIndex` concept + `BruteForceCosineIndex::snapshot()`/
  `restore()`, additive (existing `VectorIndex` concept and every other member unchanged). Claims 4/5
  CORRECT — `tests/test_vector_index.cpp`'s new persistence block: round-trip fidelity (identical top-K
  pre-snapshot vs. post-restore), content-addressed re-snapshot dedup (unchanged index → identical
  digest twice), and reject-not-coerce on both a nonexistent digest and a non-snapshot blob (wrong
  magic). Green under the real `InMemoryWorktreeObjectStore` + real `compute_digest()` (linked against
  `src/core/worktree_digest.cpp`), not a stub.
- `core/remote_vector_index.hpp` (new): `RemoteVectorIndex` + `AnyVectorIndex` concepts. No real
  conformer exists yet (that's step 7, not built) — compiled and exercised against two throwaway mock
  conformers (`synchronous_leaf = true` and `= false`) in a session-local smoke check, confirming both
  `VectorRagContextProvider<..., MockRemoteIndex, ...>` instantiations satisfy `ContextProvider`. This
  smoke check is NOT yet a permanent registered test — real coverage of this concept's actual behavior
  waits for a real conformer (`QdrantVectorIndex`, step 7) to test against; a mock-only test would
  mostly test the mock.
- `core/sparse_index.hpp` (new): `SparseIndex` concept + `BM25Index`. Claim 3 CORRECT —
  `tests/test_sparse_index.cpp`: the Okapi BM25 formula matches a hand-computed value (0.957781
  measured vs. 0.957720 hand-computed, within the stated 0.001 tolerance — the small gap is the hand
  computation's own rounding, not a formula discrepancy) on a fixed 3-document corpus; non-negative
  IDF for a universally-common term; the same score-desc/id-asc tie-break `BruteForceCosineIndex`
  uses; contract rejection (length mismatch, duplicate id within/across calls) mirroring
  `BruteForceCosineIndex::add_batch()`'s own checks exactly.
- `vector_rag_context_provider.hpp`: `render_scored_chunk()`, `rendered_chunk_to_message()`, and
  `last_user_text()` extracted to free functions in `vector_rag_detail` (behavior-preserving — same
  bodies, `this`-members become explicit parameters); `IndexT` constrained by `AnyVectorIndex` instead
  of `VectorIndex`; `on_context()` and `recall`'s `invoke` each gained one `if constexpr` branch.
  Claim 1 CORRECT — `tests/test_vector_rag_context_provider.cpp` (unmodified) re-run after every change
  and stayed 100% green throughout, proving the refactor is genuinely behavior-preserving for the
  existing `VectorIndex`/local-index case, not merely claimed to be. The new `RemoteVectorIndex` branch
  is covered only by the same session-local mock smoke check named above (not yet a permanent test).
- `core/hybrid_rag_context_provider.hpp` (new): `HybridRagContextProvider`, RRF fusion. Claims 2 and 8
  CORRECT — `tests/test_hybrid_rag_context_provider.cpp`: a "dense-strong, sparse-silent" chunk and a
  "sparse-strong, dense-silent" chunk (by construction, invisible to the OTHER retrieval method alone)
  both surface in the fused top-K (H1-H8); the citation-forgery defense scenario (ADR-063's own
  disproof shape) re-run through `HybridRagContextProvider::on_context()` shows the real marker exactly
  once, the forged one broken (F1-F3) — proving the reuse of `vector_rag_detail::render_scored_chunk()`
  is real, not merely intended. RRF's own arithmetic was additionally checked directly (a session-local
  smoke check, not a permanent test): a hand-constructed tie case (`RRF(A) == RRF(C)` by construction)
  confirmed the id-ascending tie-break applies to fused scores exactly as claimed.
- `corpus_source.hpp`: `DiskCorpusSource::mount_hybrid()` (new method; existing `mount()` byte-for-byte
  unmodified — `tests/test_corpus_source.cpp`'s pre-existing (a)-(e) blocks all still pass unchanged,
  confirming zero regression). **A real bug was found and fixed during this session's own testing, not
  merely by reasoning about the design**: the first version of `mount_hybrid()`'s backfill scenario
  (§2.3a) silently did nothing, because the unchanged-file fast path (inherited from `mount()`) skips
  chunking entirely before the per-leg `needs_dense`/`needs_sparse` check ever runs — so passing a
  prior mount's own `file_hashes` back in (the natural-looking steady-state call) defeats backfill
  silently. Fixed by documenting the real calling contract explicitly in the method's own comment
  (pass empty `previous_file_hashes` for the one backfill call) and proven via
  `tests/test_corpus_source.cpp`'s new M1-M7 block: a fresh mount, a steady-state re-mount (zero
  embedder calls, zero sparse calls — both legs already caught up), and a backfill pass onto a FRESH
  sparse index that populates it WITHOUT re-embedding. This is exactly the kind of finding `ADR-063`
  §4/§5's own red-team passes exist to catch — found here by executing the design instead, underscoring
  why the red-team pass this ADR still owes (§4) matters before Judged, not just for steps 7-8.

**§8 step 7 (`QdrantVectorIndex`) — real, offline half only, this session (2026-09-22):**
- **Research first, per `CLAUDE.md`'s own citation rule** — `docs/research/2026-09-22-qdrant-rest-api.md`,
  fetched directly from Qdrant's own official API reference and architecture docs (endpoints, request/
  response shapes, the `api-key` auth header — NOT `Authorization: Bearer`, unlike every OpenAI-family
  conformer — the `{"status":{"error":...}}` error envelope shape, and the real protocol constraint
  this ADR's own chunk-id scheme collides with: Qdrant point ids must be a `uint64` or a UUID-formatted
  string, never an arbitrary string like this project's 64-hex-char SHA-256 chunk digests).
- `protocol/qdrant/vector_index.hpp` (new, `#ifdef AGENTENGINE_WITH_HTTPS`-gated, matching
  `protocol/openai/embedder.hpp` exactly): `QdrantVectorIndex<Store>`, `synchronous_leaf = true`
  (identical reasoning to `OpenAIEmbedder`'s own claim — every step is a plain, blocking call, `co_return`
  the only coroutine keyword used). Resolves the chunk-id/point-id mismatch by deterministically
  reformatting a digest's own first 32 hex chars into UUID syntax for every outbound call, and storing
  the ORIGINAL digest as a `chunk_id` payload field, read back via `with_payload: true` on every search
  — so `ScoredId::id` this conformer returns is always the real digest every other piece of this ADR
  family keys on, never Qdrant's own internal id. Zero new third-party dependency, same protocol-tier
  cost `OpenAIEmbedder` already established, reusing `sandbox::perform_provider_https_exchange` verbatim.
  Satisfies `RemoteVectorIndex` (and, proven by a dedicated `static_assert`, does NOT also satisfy plain
  `VectorIndex` — ADR-180 §4 finding R1's disjointness guarantee holds against a real conformer, not
  just a synthetic mock).
- `tests/test_qdrant_vector_index.cpp` (new, registered in `tests/CMakeLists.txt`): mirrors
  `test_openai_embedder.cpp`'s own offline/live split exactly — exercises only the `detail::` functions
  (`chunk_id_to_qdrant_point_id`, `build_upsert_request_body`, `build_search_request_body`,
  `parse_search_response`, `map_http_status_error`) directly, never `perform_provider_https_exchange`,
  so it needs no live network and links without the HTTPS transport implementation at all.
- **A real bug was found by this test, not merely by reasoning about the design**: `build_upsert_request_body()`
  never independently validated `chunk_ids.size() == vectors.size()` — it relied entirely on
  `QdrantVectorIndex::add_batch()`'s own (separate) check, which the OFFLINE TEST bypassed by calling
  the `detail::` function directly with a mismatched pair. AddressSanitizer caught it as a real
  heap-buffer-overflow (`vectors[i]` read past the end of a shorter `vectors`), crashing the process
  outright even in a plain, non-ASan build (exit `-1073740791`, `STATUS_STACK_BUFFER_OVERRUN`) —
  invisible in a normal test run's stdout because the crash preempted any buffered output. **Fixed**:
  the function now validates the length itself (defense-in-depth, matching `BruteForceCosineIndex::
  add_batch()`'s and `corpus_source.hpp`'s own identical "never trust a caller-maintained invariant
  silently" posture), re-verified clean under both a plain build and AddressSanitizer, 27/27 assertions
  green. **Prompted a full ASan re-sweep of every other test file from this session (steps 1-6) as a
  safety check — all came back clean, zero additional findings** — recorded here because the discipline
  itself (re-verify broadly after finding one real memory bug, don't assume it was isolated) is worth
  naming, not just its (negative, reassuring) result.
- **Red-team pass 2 (§4b above) ran against this file specifically, found 6 issues (1 Critical), all
  fixed the same session:**
  1. (Critical) The confidentiality residual — `parse_search_response()` now cross-checks that a
     result's own Qdrant point id agrees with `chunk_id_to_qdrant_point_id(payload.chunk_id)`, rejecting
     an inconsistent pair (`qdrant_vector_index.id_payload_mismatch`) as defense-in-depth. Explicitly
     NOT a full fix — a determined attacker who understands this file's own (public) id-derivation
     scheme can still write both fields self-consistently for a digest they don't own; proven by a
     dedicated test covering BOTH the inconsistent-pair-rejected case and the self-consistent-pair-still-
     passes case (so the check's real limit is demonstrated, not just claimed). The actual fix is
     operational, now stated prominently in this file's own top comment: one Qdrant collection per
     `corpus_scope`, never shared across tenants — the identical "declared, host-configured, never
     shared" discipline ADR-063 §2.6a already requires of `Mount`/`ref_name` derivation, extended here.
  2. An optional `max_batch_size` constructor parameter (default 0 = unlimited), matching
     `EmbedderCapabilities::max_batch_size`'s exact "declared, never probed" convention — `add_batch()`
     now rejects an oversized batch before building any request body.
  3. `search(query, 0, ctx)` now short-circuits to an empty, successful result before resolving any
     credential or attempting any network call — parity with `BruteForceCosineIndex::search(query, 0)`.
     Proven via a dedicated stub transport (`agentengine::sandbox::resolve_host`/
     `perform_provider_https_exchange` stubbed to unconditionally fail if called) linked into the
     offline test binary specifically for this one assertion — since the real mbedtls-backed transport
     isn't buildable in this session's own Windows dev environment (a pre-existing, unrelated toolchain
     issue, `mt.exe` manifest-tool resolution failing `build-https`'s CMake configure), this was the
     only way to prove "the network call never happens" for real rather than merely by code inspection.
  4, 5, 6: named as honest residuals / fixed as minor code changes — see §4b above.
  All new assertions green under both a plain build and AddressSanitizer.
- **What was still owed at the time (since CLOSED 2026-09-23, §3 claim 6: 12/12 live, wrong-key
  negative run fails)**: a live-network test against a real Qdrant
  instance. The test itself is written (`tests/test_qdrant_vector_index_live_e2e.cpp`, registered in
  `tests/CMakeLists.txt`, `live-network`-labeled, mirroring `test_openai_embedder_openrouter_live_
  e2e.cpp`'s exact shape — real add_batch/search/contains round-trip, a `k=0` live counterpart, an
  auth positive control, an I2 capability-denial control) but has **not been run against a real
  instance** — a local Docker Qdrant container was attempted this session and the Docker daemon itself
  became unresponsive (containers stuck in `Created`, never transitioning to `Running`, `docker ps`/
  `docker start` themselves timing out) for reasons unrelated to this code, not yet resolved.

**§8 step 8 (`VulkanCosineIndex`) — real, this session (2026-09-22), built by a `worktree`-isolated
agent then merged to `main` (fast-forward, commit `f085bbd`):**
- `src/backends/vulkan_vector_index/cosine_similarity.comp` (+ pre-compiled `.spv`, checked in):
  exact brute-force cosine similarity, GPU-parallel across candidates, sequential within each
  candidate's own dot product — the specific resolution to §2.6's determinism concern (no cross-
  dimension tree reduction, whose summation order — and therefore rounding — a GPU driver doesn't
  guarantee call-to-call). Float32 accumulation, not float64, for cross-vendor portability — a named,
  accepted residual, not silently narrower than advertised.
- `src/backends/vulkan_vector_index/vulkan_cosine_index.{hpp,cpp}`: `VulkanCosineIndex`, pimpl'd (no
  Vulkan headers leak into the public header, matching this project's existing seam-isolation
  convention for Tier 2 backends). Satisfies the EXISTING, UNMODIFIED `VectorIndex` concept — proven
  via `static_assert`, not merely asserted — so every call site written against `VectorIndex` already
  works with this conformer with no dispatch code of its own. `create()` fails closed (a `result<...>`
  error, never a crash/terminate) when no Vulkan-capable device is present, matching this project's
  "reject-not-coerce" posture for an unavailable Tier 2 backend.
- `CMakeLists.txt`: new `AGENTENGINE_WITH_VULKAN` option (default OFF — zero impact on the existing
  default build), `find_package(Vulkan)`, SPIR-V embedded at CMake configure time via a binary-safe
  `file(READ ... HEX)` (a plain text-mode read would have been silently corrupted by CRLF translation
  — closed by also marking `*.spv binary` in `.gitattributes`, which was NOT already true and is a
  real fix, not a defensive no-op: the checked-in `.spv` was previously subject to `text=auto`
  mangling).
- `tests/test_vulkan_cosine_index.cpp` (registered in `tests/CMakeLists.txt`, gated on
  `AGENTENGINE_WITH_VULKAN`): correctness, `VectorIndex` contract-rejection parity with
  `BruteForceCosineIndex`, and claim 7's epsilon-bounded GPU-vs-CPU top-K comparison at dim=1536,
  n∈{1000, 5000}. The test binary itself fails closed (SKIP, exit 0) at run time if no Vulkan-capable
  device is present, mirroring the live-network tests' own env-gated SKIP-not-FAIL posture for an
  environment precondition outside the code's own control.
- **Proven against real hardware, not just a standalone syntax check**: compiled, linked, and run
  directly against the Vulkan SDK on a real discrete GPU (AMD Radeon RX 5300M) multiple times over the
  course of finding and fixing two genuine performance bugs (not merely estimated):
  1. The first working version re-uploaded the ENTIRE vectors buffer (~30MB at n=5000, dim=1536) on
     every single `search()` call, making GPU search ~10x SLOWER than CPU brute-force outright. Fixed
     by caching the vectors buffer persistently, invalidated only when `add_batch()` actually changes
     the corpus.
  2. Even cached, the buffer used `HOST_VISIBLE`/`HOST_COHERENT` (PCIe-mapped) memory for repeated
     shader reads — slow on a discrete GPU. Fixed via a staging-buffer upload into `DEVICE_LOCAL`
     memory, cutting n=5000 latency a further ~4x (41ms → 10ms).
  3. **Honest final result, claim 7's performance half does NOT hold**: after both fixes, GPU search
     is still ~1.3x slower than CPU brute-force at both n=1000 (2.9ms vs. 1.5ms) and n=5000 (10.9ms
     vs. 7.8ms). Correctness is solid throughout (§3 claim 7 part (b)). See §3 claim 7 and §7 for the
     residual this leaves open.
- **What was owed as of the original build, since closed by red-team pass 3 (§4c above):**
  - No red-team pass on this code yet — **now done, §4c above; all 4 findings fixed, see below.**
  - The building agent validated the compilation unit directly via `clang++` against the Vulkan SDK,
    not through a full `cmake`/`ninja` configure-and-build of the whole project with
    `-DAGENTENGINE_WITH_VULKAN=ON` (attempted, did not complete within that session — root
    `CMakeLists.txt` is large, cause not diagnosed). **Red-team pass 3 diagnosed this**: the
    whole-project configure fails identically, at the identical unrelated step, with
    `AGENTENGINE_WITH_VULKAN` both ON and the default OFF — a pre-existing MSVC/Ninja toolchain issue
    (`C1083: Cannot open compiler generated file: ''`) unconnected to this backend, not something the
    Vulkan wiring itself causes. See §4c's "What held up" for the full comparative test.
  - Default build impact (`AGENTENGINE_WITH_VULKAN=OFF`) was not re-verified after the merge, though
    the change is purely additive inside one `if()` block in both `CMakeLists.txt` files.
    **Re-verified, §4c: a `grep` across both `CMakeLists.txt` files found no Vulkan target/symbol
    reference outside the two `if(AGENTENGINE_WITH_VULKAN)` guards — confirmed zero-impact by static
    inspection (the whole-project configure itself still cannot complete, for the unrelated reason
    above, so this is not YET confirmed by an actual successful build — see §7).**

**Red-team pass 1 fixes (2026-09-22, same session — see §4 for the findings):**
1. (Critical) `vector_index.hpp`: `ByteReader::check_header_plausible()` rejects a `count`/`dimension`
   header that couldn't possibly fit in the blob's remaining bytes, BEFORE either `reserve()` call —
   closes the uncaught-exception DoS. Two new tests (an implausible-count case and an overflow-shaped
   count*dimension case).
2. (Real gap) `remote_vector_index.hpp`: `RemoteVectorIndex<T>` now requires `!VectorIndex<T>` --
   structurally disjoint from `VectorIndex` by construction, not merely by convention. Proven via a
   constructed `DualShapedIndex` type in the new `test_remote_vector_index.cpp` (`static_assert`s that it
   satisfies `VectorIndex` and `AnyVectorIndex` but NOT `RemoteVectorIndex`).
3. (Real gap) New `tests/test_remote_vector_index.cpp` (registered in `tests/CMakeLists.txt`): a real,
   working `MockRemoteIndex` driven through every dispatch branch this ADR added --
   `VectorRagContextProvider::on_context()`'s `co_await` branch, `recall`'s `rt::drive_leaf_task()` branch
   (both embedder AND index legs synchronous), `HybridRagContextProvider::on_context()` with a remote dense
   leg fused against a local sparse leg, and `DiskCorpusSource::mount_hybrid()` with a remote dense leg --
   with real assertions on results, not compile-only checks.
4. (Real gap) `corpus_source.hpp`: `mount_hybrid()`'s dense and sparse commit blocks now run
   independently -- both always attempted, either error reported only after both have run. New regression
   test (`tests/test_corpus_source.cpp`) scripts a dense-leg failure and proves a sparse-only chunk in the
   SAME pass still commits.
5. (Real gap) New cross-tenant isolation test for `HybridRagContextProvider`
   (`tests/test_hybrid_rag_context_provider.cpp`), mirroring ADR-063's own `C5-R1`--`C5-R5` shape -- confirms
   the mechanism held up as expected, closing the coverage gap.
6. (Minor) `hybrid_rag_context_provider.hpp`: `reciprocal_rank_fusion()` skips a non-positive-denominator
   contribution instead of accumulating `+inf`/a negative score. New test proves no `inf`/`nan` for a
   degenerate `rrf_k`, and that a well-formed `rrf_k` is unaffected.
7. (Minor) `corpus_source.hpp`: the within-pass cross-file dedup counters now increment unconditionally on
   the `already_seen_this_pass` path, matching what `DiskCorpusHybridMountResult`'s own field comments claim.
8. (Minor) `vector_index.hpp`: `restore()` now rejects a non-empty target index outright
   (`vector_index.restore_requires_empty_index`) rather than silently discarding it. New test proves the
   rejected call leaves the target's pre-existing state completely untouched. The narrower, still-open
   concurrent race is recorded in §7, not claimed closed.

**Red-team pass 3 fixes (2026-09-22, same session — see §4c for the findings):**
1. (Critical) `vulkan_cosine_index.cpp`: every previously fire-and-forget Vulkan call in `search()`'s
   hot path (`vkMapMemory` ×3, `vkAllocateCommandBuffers` ×2, `vkBeginCommandBuffer` ×2,
   `vkEndCommandBuffer` ×2, `vkCreateFence` ×2, `vkQueueSubmit` ×2, `vkWaitForFences` ×2,
   `vkResetCommandBuffer` ×1) now checks its `VkResult`/mapped-pointer and converts any failure into a
   typed `agentengine::result` error (`vulkan_vector_index.memory_map_failed`,
   `.command_buffer_allocation_failed`, `.command_buffer_begin_failed`, `.command_buffer_end_failed`,
   `.fence_creation_failed`, `.queue_submit_failed`, `.fence_wait_failed`,
   `.command_buffer_reset_failed`) instead of risking a crash or undefined behavior. A new
   `CleanupStack` helper (reverse-order RAII cleanup, `dismiss()`d only once a transient resource is
   handed off to `impl_`) prevents the added early-return paths from themselves leaking the staging
   buffer, copy command, or copy fence the vectors-buffer rebuild allocates partway through. **Not
   provable via a forced-failure test** — safely inducing a genuine Vulkan device-lost or
   out-of-memory condition on real hardware, without risking this machine's stability (VRAM/RAM
   exhaustion), was judged out of scope for this repo's own "resource-capped" machine-safety
   discipline (CLAUDE.md). The fix compiles cleanly under `/W4 /WX` and the full existing + new test
   suite (54 assertions) stays 100% green on the happy path, in both a plain build and under
   AddressSanitizer, against the real GPU — a real, honest residual on the TEST methodology (not the
   fix itself) is recorded in §7.
2. (Real gap) `vulkan_cosine_index.hpp`/`.cpp`: a new, pure, Vulkan-free predicate,
   `detail::check_gpu_dispatch_size_plausible(n, dim)`, rejects a candidate count or dimensionality
   that would silently wrap when narrowed into `cosine_similarity.comp`'s `uint32` push constants (or
   whose product would overflow the `size_t` buffer-size computation), called at the top of `search()`
   before any GPU resource is touched. Proven directly with synthetic `size_t` values (no need to
   actually allocate or hold an adversarial-scale corpus) — mirroring exactly how
   `BruteForceCosineIndex::restore()`'s own red-team-pass-1 fix (`ByteReader::check_header_plausible()`)
   is tested against a crafted header rather than a genuinely oversized blob. Four new assertions:
   accepts a realistic pair, rejects an over-`UINT32_MAX` count, rejects an over-`UINT32_MAX`
   dimension, and does not divide-by-zero for `dim == 0`.
3. (Real gap) `tests/test_vulkan_cosine_index.cpp`: 12 new assertions close the coverage gap named in
   §4c finding 3 — a fresh index's `search()` returns empty (not an error) on a never-populated index;
   `search(k=0)` returns empty; `search(k > corpus size)` returns every entry; an id is rejected by
   `add_batch()` when it was committed by an EARLIER, separate `add_batch()` call (not just a
   duplicate within one call); and a dedicated three-cycle `add_batch()`-then-`search()` growth test
   proves the persistent vectors-buffer cache correctly rebuilds (never returns a stale snapshot) and
   accumulates correctly (every id ever added is present, none dropped or duplicated) across THREE
   consecutive rebuild cycles on the same instance — the cache-invalidation path §5-6's own step-8
   account first built, now actually exercised more than once.
4. (Minor) `vkCmdDispatch`'s missing `maxComputeWorkGroupCount[0]` bound: named, not fixed — see §4c
   and §7.

All new assertions run against the real Vulkan SDK (1.4.350.0) and the real discrete GPU (AMD Radeon
RX 5300M) this backend was already built and measured against, compiled directly via MSVC 14.51
(`cl.exe`, since the whole-project `cmake`/`ninja` configure remains blocked for the unrelated,
pre-existing reason described above); the full existing test file (47 pre-existing assertions) plus
these new ones (54 total) stayed green throughout, in both a plain build and under AddressSanitizer —
zero regression to the already-proven correctness/claim-7 behavior. (Windows' MSVC AddressSanitizer has
no LeakSanitizer — `ASAN_OPTIONS=detect_leaks=...` is a no-op on this platform — so "clean under ASan"
here means no host-side heap corruption/overflow/use-after-free was detected, not that an automated
leak detector ran; see §7 for the honest scope of that claim.)

**Red-team pass 4 fixes (2026-09-22, same session — see §4d for the findings):**
1. (Critical) `vulkan_cosine_index.cpp`: both buffer-creation lambdas
   (`create_host_visible_buffer()`, `create_device_local_buffer()`) now check `vkBindBufferMemory`'s
   `VkResult` and clean up (`vkFreeMemory` then `vkDestroyBuffer`) on failure, returning `std::nullopt`
   like every other failure branch already in those lambdas, instead of handing back an unbound
   buffer/memory pair. Every one of this class's four buffer allocations (vectors device-local,
   vectors staging, query, scores) routes through one of these two lambdas, so this single fix closes
   the gap everywhere it existed. **Not provable via a forced-failure test** — the identical, already-
   accepted residual §4c's own Critical fix recorded (safely inducing a real Vulkan OOM/device-lost
   condition without risking this machine's stability is out of this repo's "resource-capped" scope) —
   proven instead by the full 63-assertion suite staying green, in both a plain build and under
   AddressSanitizer, confirming the fix introduces no new host-side corruption on the happy path.
2. (Real gap) `vulkan_cosine_index.cpp`: `create()`'s physical-device enumeration now checks both
   `vkEnumeratePhysicalDevices` calls' `VkResult` (failing closed with a new
   `vulkan_vector_index.device_enumeration_failed` error) and calls `devices.resize(device_count)`
   after the second call, so the physical-device-selection loop never iterates past however many
   handles the driver actually wrote, closing the latent stale-tail-iteration edge case alongside the
   unchecked-result gap itself. Proven by the happy path: every `VulkanCosineIndex::create()` call in
   the test suite (7 across the full run) still succeeds unchanged; not independently forced-failure-
   tested, for the same reason as finding 1.
3. (Real gap) `vulkan_cosine_index.cpp`: `add_batch()` now rejects a zero-dimensional vector
   (`vulkan_vector_index.add_batch_zero_dimension`, `failure_class::contract`) when it would establish
   the index's dimensionality, BEFORE `search()` could ever reach its `dim == 0` branch — making that
   branch's own "should be unreachable" comment true rather than aspirational, and giving the caller an
   immediate, correctly-classified rejection at add_batch() time instead of a later, wrongly-classified
   `fatal` one at search() time. `search()`'s own comment was corrected to state the (now genuinely
   true) invariant accurately. `tests/test_vulkan_cosine_index.cpp`: 6 new assertions — the rejection
   fires when no dimensionality is established yet; the rejected call leaves the index untouched; the
   SAME zero-sized-vector input, after a real dimensionality is already established, still falls
   through to the pre-existing (unchanged) `add_batch_dimension_mismatch` rejection, not the new check;
   and a genuinely empty `add_batch({}, {})` call is NOT confused with a zero-dimensional vector and
   still succeeds as a no-op, both on a fresh index and after real data exists.
4. (Minor) The unconditional `cache_dirty = true` on every `add_batch()` call, including a no-op empty
   one: named, not fixed — see §4d and §7.
5. (Real gap, test coverage) `tests/test_vulkan_cosine_index.cpp`: 8 new assertions cover the
   adversarial/boundary shapes named in this pass's own brief that §4c's coverage fix didn't reach — an
   n=1, dim=1 corpus (the smallest possible non-empty index), `k` exactly equal to corpus size (distinct
   from both `k == 0` and `k >` corpus size, which §4c's own fix already covered), and two no-op
   `add_batch()` calls (one before any real data, one interleaved after) on the same instance, proving
   they neither corrupt state nor spuriously affect a subsequent `search()`'s correctness.

All new assertions (this pass: 15, bringing the running total to 63 — 48 carried over from §4c's own
file plus these) run against the same real Vulkan SDK (1.4.350.0) and the same real discrete GPU this
backend has been measured against every prior pass, compiled and linked directly via MSVC 14.51
(`cl.exe`/`link.exe`) for the identical, re-confirmed reason the whole-project configure remains blocked
(§4d's own account). Zero failures, in both a plain build and under AddressSanitizer — zero regression to
every previously-proven correctness/claim-7/red-team-pass-3 behavior. The same Windows-ASan-has-no-
LeakSanitizer scope caveat §4c already named applies identically here (host-side-corruption check, not an
automated leak-detector run). A genuine UndefinedBehaviorSanitizer attempt (via `clang-cl` 22.1.5, not
tried by any prior pass) was made and diagnosed as blocked by a distinct, unrelated `clang-cl`/preview-
MSVC-14.51-STL language-mode interop gap (§4d's own account, §7) — not fixed, not this backend's defect,
not chased further.

**Red-team pass 5 fixes (2026-09-23 — see §4e for the findings):**

1. (Critical) `vulkan_cosine_index.cpp`: the scores-buffer regrow is now **build-then-swap**. The new
   buffer is created first, and the old one is destroyed only after its replacement exists: the same
   ordering the vectors-buffer rebuild already used. A failed creation now leaves the old, still-valid
   buffer and its capacity untouched.
   **Proven both ways by executing it.**
   - The new fault-injection test (fix 6) drives `create_buffer`/`allocate_memory`/`bind_buffer_memory`
     failures into exactly this block during a growth-phase `search()`. It then asserts the same instance's
     next `search()` returns the exact expected ranking, under the validation layer, with zero errors.
   - A scratch build with ONLY this block reverted fails that same test: 18 validation errors
     (`VUID-vkDestroyBuffer-buffer-parameter`) under the layer, and `0xC0000374` heap corruption without it.

2. (Real gap) `create()` now records the selected device's `maxStorageBufferRange` and
   `maxComputeWorkGroupCount[0]` (`vkGetPhysicalDeviceProperties`). A new pure predicate,
   `detail::check_gpu_device_limits(n, dim, range, groups)`, rejects a corpus whose flattened vectors
   buffer exceeds that range, or whose dispatch exceeds that workgroup count. It returns
   `vulkan_vector_index.exceeds_device_storage_buffer_range` / `.exceeds_device_workgroup_count`,
   `failure_class::resource`, before any GPU resource is touched. This also closes the shader's own
   uint32 index-wrap (`n*dim < 2^30` once the range holds), and §4c's named workgroup-count residual.
   **Proven:**
   - 5 new synthetic-limit assertions in `test_vulkan_cosine_index.cpp`:
     - n=21,845 × 1536 accepted, n=21,846 rejected, at the 2^27 floor;
     - 65,535 workgroups accepted, 65,536 rejected;
     - `n*dim = 2^30` rejected even against the largest representable range.
   - Re-running the §4e reproduction under the profiles layer emulating `VP_ANDROID_vulkan_profile_2021`
     now returns the typed error, with **no** validation error. Before the fix it gave `VUID-...-00333`.

3. (Real gap) Every stored row, at cache-build time, and every query, at upload time, is now copied into
   the GPU buffer **rescaled by the exact power of two** that brings its largest |component| into
   [0.5, 1): `copy_rescaled_by_power_of_two()`. The host-side `entries` stay untouched.
   - Every squared component is then below 1, so no sum of squares can overflow float32.
   - Cosine similarity is scale-invariant, and power-of-two scaling is exact in IEEE arithmetic, so no
     in-range result changes by even one ULP. Proven: claim 7(b)'s max |GPU−CPU| is bit-identical before
     and after (1.04308e-07 / 1.37836e-07).

   **Proven:** 3 new assertions (plus 2 setup checks) query the §4e reproduction's corpus at three
   magnitudes (1, 1e20, 1e-25). Stored vectors are at 1e20, 1, 1e-25 and 0. The GPU must match `BruteForceCosineIndex` within 1e-5,
   with every score finite and the same ranking.

4. (Real gap) `add_batch()` rejects any non-finite component (`vulkan_vector_index.add_batch_non_finite`,
   `contract`) before any state changes, and `search()` rejects a non-finite query
   (`.search_non_finite`). The sort comparator is also made total over NaN: every NaN orders after every
   non-NaN, and NaNs among themselves order by id. So `std::sort`'s precondition no longer depends on that
   upstream rejection staying correct.
   **Proven:** 5 new assertions (plus 2 setup checks):
   - NaN, +Inf and −Inf each rejected as `contract`;
   - a rejected batch commits NOTHING, not even its finite members;
   - a non-finite query is rejected.

   Re-running the §4e reproduction gives 0 NaN scores and 0 ordering violations on the GPU side. At the
   time this was a deliberate divergence from `BruteForceCosineIndex`, which still accepted non-finite
   input; that divergence is now CLOSED — the CPU conformer rejects it identically (§7, 2026-09-23).

5. (Real gap) A moved-from instance is now safe to call.
   - `add_batch()`/`search()` return `vulkan_vector_index.moved_from` (`contract`).
   - `contains()` returns false and `size()` returns 0.

   **Proven:** 4 new assertions (plus 2 setup checks). The moved-from getters and both typed errors are
   checked, and the object is then used as a move-assignment TARGET and works again afterwards. The §4e reproduction's
   `0xC0000005` becomes `size=0`.

6. (Real gap, test methodology) **A test-only fault-injection seam, and a new test that executes every
   checked-`VkResult` failure branch.**
   - **The seam.** `vulkan_cosine_index.hpp` declares `detail::fault_point` (21 points) and
     `detail::arm_fault_injection(point, nth)` / `disarm_fault_injection()` / `fault_injection_hits()`,
     only under `#ifdef AGENTENGINE_VULKAN_FAULT_INJECTION`. In a production build, `AE_VK_FAULT(...)` is
     a `constexpr` `false` the optimizer removes, and none of the seam's declarations exist. The seam makes
     the Nth call of a named Vulkan entry point report failure:
     - creation-style calls are SKIPPED, so nothing leaks;
     - `vkWaitForFences` is called for real and its result then overridden to `VK_ERROR_DEVICE_LOST`, so
       nothing is torn down while pending.
   - **The build.** `tests/CMakeLists.txt` builds a separate library variant from the same `.cpp` with that
     macro defined (`agentengine_vulkan_vector_index_fault_injection`), plus
     `tests/test_vulkan_cosine_index_fault_injection.cpp`.
   - **The test, `create()`.** For each of the 9 `create()` points it asserts a typed, fail-closed error.
   - **The test, `search()`.** For each of the 12 `search()` points and every N at which the point is
     reached, it runs three phases on one instance:
     - first search (full cache build);
     - growth (rebuild, scores regrow, command-buffer reset);
     - steady state.

     In each phase it asserts that the faulted `search()` returns a typed error and that the SAME instance's
     next `search()` returns the exact expected ranking.
   - **Positive controls.**
     - Every one of the 21 points must actually fire at least once.
     - When the SDK's validation layer is present, the whole run executes under it and must log zero
       errors, with one "Layer Active" banner per `VkInstance` created.
   - **A harness defect found and fixed during this pass, recorded because it is the kind of mistake a
     positive control exists to catch.** The first version used ONE log file. The layer truncates its log
     at every `vkCreateInstance`, so errors from all but the last instance were silently lost. That version
     PASSED against the reverted-Critical build. The per-instance banner count exposed it: 1 banner for
     59 instances. Every `create()` now gets its own log file.
   - **Result: 410 checks, 0 failures, 59 banners for 59 instances, 0 validation errors.** The same result
     holds in a plain build, under AddressSanitizer, and under UBSan.

7. (Minor) `vk_call_error()`: every `vkQueueSubmit`/`vkWaitForFences` failure now carries its numeric
   `VkResult` in the message, and `VK_ERROR_DEVICE_LOST` gets its own stable code,
   `vulkan_vector_index.device_lost`. The fault-injection test exercises that code path. The class is left
   `resource`, not moved to `fatal`: `error.hpp` defines `fatal` as "the run ends", which is too strong for
   a condition a caller with a CPU fallback survives. This is flagged for the Judge step (§7), not decided
   here.

8. (Minor) `vulkan_cosine_index.hpp`'s stale top comment now describes the real persistent-cache design,
   and a new input-domain paragraph documents fixes 3/4.

**Test totals.**

- `test_vulkan_cosine_index`: 86 assertions, up from 63, so 23 new.
- New `test_vulkan_cosine_index_fault_injection`: 410 checks.
- Both pass in every configuration below, all against the real Vulkan SDK (1.4.350.0) and the real
  discrete GPU:
  - a plain MSVC 14.51 build, `/W4 /WX /permissive-`, zero warnings;
  - AddressSanitizer (`/fsanitize=address`, `clang_rt.asan_dynamic` confirmed linked via `dumpbin`);
  - **UBSan, for the first time on this backend** (VS-bundled `clang-cl` 22.1.3, `/std:c++latest /MT
    -fsanitize=undefined -fno-sanitize-recover=all`, with a signed-overflow canary proving it traps);
  - the Khronos validation layer: the main suite with sync validation and best-practices gave 0 errors and
    0 warnings; the fault-injection suite under core validation gave 0 errors, with 59/59 banners;
  - **the real whole-project `cmake`+Ninja build with `-DAGENTENGINE_WITH_VULKAN=ON`**, `ctest -R vulkan`
    2/2 passed. This became possible once §4e diagnosed the C1083 blocker as `MAX_PATH`.
  - a whole-project configure with the option at its default (OFF) also succeeds, and its generated
    `build.ninja` contains no Vulkan reference at all. That confirms the default-OFF zero-impact claim
    empirically for the first time; §4c/§4d could only confirm it by inspection.

The same Windows-ASan-has-no-LeakSanitizer caveat as §4c/§4d applies. Leak-freedom of the failure paths
rests instead on the validation layer, which DOES track every Vulkan object's lifetime per instance and
reported none leaked or double-destroyed across 59 instance teardowns.

## 7. Residuals and open questions (named now, per this repo's own discipline, so none are rediscovered later)

- **Everything ADR-063 §7 already named and left open remains open here too, and several are now sharper:**
  stale-chunk GC (no `VectorIndex::remove()` in the concept today) matters *more* once persistence (§2.4)
  means an orphaned chunk can now survive a process restart instead of vanishing with it; the
  concurrent-writer `commit_ref()` lost-update race applies identically to a hybrid corpus's dual-index
  ingestion (§2.3a).
- **(NARROWED by red-team finding M3's fix, §4/§5-6) `PersistentVectorIndex`'s concurrent-writer race is
  real but smaller than originally feared.** `snapshot()` itself is SAFE against a concurrent `add_batch()`
  — both correctly take `BruteForceCosineIndex`'s own `shared_mutex` (shared vs. unique lock respectively),
  so a snapshot cannot observe a torn write. The actual mechanism is in `restore()`: it fetches and parses
  the blob *outside* any lock, then takes a lock only for the final swap — a concurrent `add_batch()` that
  completes entirely within that unlocked window is not torn/corrupted, but its results ARE unconditionally
  overwritten by `restore()`'s swap and lost with no error. The SEQUENTIAL misuse case (calling `restore()`
  on an already-populated index at all) is now closed — `restore()` rejects a non-empty index outright
  (`vector_index.restore_requires_empty_index`) — but the genuinely-concurrent race (an `add_batch()` landing
  in the specific window between `restore()`'s empty-check and its final swap) is still open, still
  unaddressed, and still real; the intended usage (`restore()` once, at process start, before any concurrent
  writer could plausibly be running yet) makes it unlikely to fire in practice, not impossible by construction.
- **`mount_hybrid()`'s backfill calling contract is a real sharp edge, now documented but not
  structurally prevented** (found by this session's own testing, §5-6 above): passing a prior mount's
  own returned `file_hashes` back in when backfilling a newly-added index leg silently does nothing,
  because the unchanged-file fast path skips chunking before the per-leg dedup check ever runs. A
  caller must know to pass empty `previous_file_hashes` for that one call. Nothing in the type system
  stops a caller from getting this wrong; the fix (2026-09-22) is a comment plus a proving test, not a
  contract the compiler enforces. Whether a future revision should catch this constructively (e.g. a
  distinct `mount_hybrid_backfill()` entry point that ALWAYS re-walks, so this failure mode is
  structurally impossible rather than merely documented) is an open follow-on question, not resolved
  here.
- **`QdrantVectorIndex::contains()` is a real network round-trip per chunk during re-mount dedup** — for a
  large corpus re-mount, this could itself become the new bottleneck ADR-063 §6's bench warned about for
  brute-force search, just moved to a different operation. No batched "which of these N ids exist" Qdrant
  call is designed here; worth revisiting once real re-mount-against-Qdrant numbers exist.
- **`QdrantVectorIndex`'s point-id derivation has a real, named, narrow collision surface**: it reads
  only a chunk digest's OWN first 32 hex characters (128 bits) to build a UUID-shaped Qdrant point id —
  two digests sharing only that prefix (a 1-in-2^128 event for genuinely independent SHA-256 digests,
  not a practical concern, but a real structural fact about the scheme, not merely a probabilistic hand-
  wave) would silently alias onto the SAME Qdrant point, with `add_batch()`'s own "any point with an
  existing id will be overwritten" upsert semantics (research doc §1) meaning the second write wins,
  the first is gone. Proven present (not merely asserted) by `test_qdrant_vector_index.cpp`'s own
  collision test. Not fixed here — a full 64-hex-char (256-bit) digest cannot fit UUID's 128-bit space
  without a real remapping scheme (a lookup table, or accepting a longer non-UUID-shaped point id if
  Qdrant's parser turns out to accept one — unconfirmed, would need its own research pass), left as a
  named, accepted residual for this pass.
- **(§4b finding 1, THE most serious open residual in this whole ADR) `QdrantVectorIndex`'s confidentiality
  guarantee is weaker than every other `VectorIndex`/`RemoteVectorIndex` conformer's, and depends on an
  operational discipline this class cannot enforce by itself.** A local `VectorIndex`'s isolation is
  structural (only an in-process reference can populate it); a Qdrant collection's isolation is "whoever
  holds the configured `api-key`," which is NOT the same guarantee, and nothing stops a deployer from
  pointing two different corpora at the same collection. `parse_search_response()`'s id/payload
  cross-check (§4b/§5-6) catches an inconsistent pair, not a determined, self-consistent forger who
  understands this file's own public id-derivation scheme. **Real fix is operational**: one Qdrant
  collection per `corpus_scope`, stated prominently in the file's own top comment — but this ADR has no
  mechanism forcing a deployer to follow that, unlike `Mount`/`ref_name` derivation which is at least
  computed FOR the caller by `rag_corpus_mount()` rather than handed to them as a free-form string
  constructor parameter the way `QdrantVectorIndex::collection_name` currently is. Whether a future
  revision should take a `corpus_scope`-derived name automatically (closing this gap structurally, the
  same way `rag_corpus_mount()` already does for `Mount`) rather than an arbitrary string is a real,
  open follow-on question this pass did not resolve.
- ~~**`QdrantVectorIndex`'s live half (§3 claim 6) is unexecuted**~~ **CLOSED 2026-09-23**: the Docker
  daemon hang below had cleared by the next day. The live test ran from WSL2 against a real,
  auth-enabled Qdrant 1.19.1 and passed 12/12, and a wrong-key negative run failed as it should
  (§3 claim 6). The original account follows, kept for the record: the live test is written
  (`tests/test_qdrant_vector_index_live_e2e.cpp`) but has not been run against a real instance. This
  session made roughly eight separate attempts (`docker run`, `docker start`, cleanup-and-retry cycles)
  over more than half an hour to bring up a local Qdrant, all either hanging or leaving the container
  stuck in `Created` without ever reaching `Running`; even a bare `docker rm -f` on the stuck containers
  timed out after 15s in the final check. Read-only Docker commands (`docker ps`, `docker info`) respond
  fine, so this is specifically a container-lifecycle (`create`/`start`/`rm`) hang in the local daemon —
  a genuine, persistent environmental blocker on this machine, not caused by this session's code, and
  not something further retries are expected to fix. Both a red-team pass (now done, §4b) and the live
  run were owed; only the live run remains, blocked on this environment issue being resolved outside
  this session (e.g. restarting Docker Desktop, or running the test from WSL2/Linux per this project's
  own existing note that HTTPS-gated builds need that anyway).
- **`VulkanCosineIndex` measured SLOWER than CPU brute-force, not faster (§3 claim 7 part (a),
  DISPROVEN)** — the whole point of shipping a GPU backend (RFC 023 §3's Goal-tier latency budget,
  ADR-063 §6's own CPU bench establishing brute-force misses that budget at these sizes) is not yet
  delivered; on the one reference GPU measured (AMD Radeon RX 5300M), GPU search is currently the wrong
  choice vs. CPU brute-force at both n=1000 and n=5000. Correctness (part (b)) holds, RE-CONFIRMED by
  red-team pass 3 on the rebuilt (fail-closed-hardened) binary. The remaining gap is named, not fixed:
  very likely fixed per-`search()`-call `vkQueueSubmit`+`vkWaitForFences` CPU↔GPU synchronization
  overhead that doesn't amortize at these corpus sizes. Candidate follow-on fixes (not attempted this
  pass): batching multiple queries per submission, a persistent command buffer instead of re-recording
  per call, or accepting that GPU only wins at corpus sizes larger than either size claim 7 tested.
  Whether `VulkanCosineIndex` is worth keeping enabled by default anywhere, given this result, is an
  open question this pass did not resolve — it remains a Tier 2, opt-in-only
  (`AGENTENGINE_WITH_VULKAN` default OFF) backend, which was already the right default regardless of
  this outcome.
- **(RESOLVED by red-team pass 4, §4d) `VulkanCosineIndex` has now had TWO independent red-team
  passes, matching `QdrantVectorIndex`'s own two-pass precedent (§4b) and closing the open question
  this ADR's Status line previously posed.** Pass 3 (§4c) found one Critical (unchecked Vulkan API
  return codes across `search()`'s hot path) and two Real gaps (a push-constant integer-overflow gap;
  a test-coverage gap). Pass 4 (§4d), run independently, with no prior context beyond §4c's own
  account, and explicitly briefed not to re-report anything §4c already covered, found ONE MORE
  Critical (`vkBindBufferMemory`'s `VkResult` discarded in both buffer-creation lambdas — the same
  gap-class as pass 3's own Critical, missed at a call site outside pass 3's `search()`-hot-path-scoped
  brief, affecting every buffer this class ever creates) plus two more Real gaps (an identical
  unchecked-`VkResult` gap in `create()`'s device enumeration; a reachable-but-misclassified
  `failure_class::fatal` error for a zero-dimensional-vector input, plus a factually wrong code comment
  claiming that state was unreachable) and one Minor (an unconditional `cache_dirty = true` on a no-op
  `add_batch()` call, a performance-only, unbenched cost). All fixed and proven the same session (§5-6);
  the Minor is a named, honest, not-fixed residual, matching this ADR's own established posture for
  low-severity findings elsewhere. **This is a genuine "second pass finds what a first, narrower-scoped
  pass missed" result** (mirroring `QdrantVectorIndex`'s own §4b, which likewise found real, new issues
  a first look didn't reach), not a confirmation that nothing was left to find — which is itself the
  argument for treating two independent passes, not one, as this ADR's real bar for "red-teamed" on a
  security/memory-safety-relevant seam backend. Whether a THIRD pass is owed is a judgment call, not
  resolved here: two independent passes each finding genuine, distinct issues on the same ~500-line
  file is a real signal this surface rewards continued scrutiny, but is also the same cadence
  `QdrantVectorIndex` stopped at (two passes, then left to Judged review) — recorded as a live open
  question for whoever runs the Judge step, not decided unilaterally by this pass.
  **(RESOLVED by red-team pass 5, §4e) Yes, a third pass was owed. It found one more Critical, five more
  Real gaps, and two Minor issues.** Its Critical, a scores-buffer double-destroy on a failure path that
  corrupted the heap, sat on a branch no pass had ever executed, in code both prior passes reviewed by
  hand. That is the strongest evidence yet that this surface's remaining risk lived in the
  never-executed failure paths, not in any single call site. Pass 5 therefore changed the method, not just
  the scope:
  - an exhaustive 47-entry-point inventory (§4e's table);
  - the Khronos validation and profiles layers;
  - a fault-injection seam that now executes 21 of the 22 `VkResult`-returning entry points' failure
    branches (the 22nd has no failure code), under the validation layer, in a permanent registered test.

  See §8 item 9 for the Judge-readiness recommendation this leads to.
- **(RE-CONFIRMED by red-team pass 4, §4d) `VulkanCosineIndex`'s CMake wiring default-OFF impact
  remains confirmed zero by static inspection** — unaffected by pass 4's changes (only `.cpp` call
  sites changed; no new target/symbol was added outside the existing `if(AGENTENGINE_WITH_VULKAN)`
  guards). **Still NOT confirmed by an actual successful whole-project `cmake`+`ninja` configure-and-
  build** — RE-ATTEMPTED by pass 4 (`-DAGENTENGINE_WITH_VULKAN=ON`, an even newer preview MSVC toolset
  than pass 3 used — VS 2026 "18", MSVC 14.51.362xx), and reproduced the IDENTICAL `C1083: Cannot open
  compiler generated file: ''` failure at the IDENTICAL unrelated file
  (`tests/compile_fail/tainted_declassify_positive_control.cpp`) pass 3 already found — a real,
  persistent, still-unresolved, pre-existing MSVC/Ninja toolchain issue on this dev box (the same class
  of issue this ADR's own §8 step-7 account already names for a different tool, `mt.exe`), confirmed
  across two independent passes now to be neither transient nor caused by the Vulkan wiring.
  `VulkanCosineIndex` was again compiled, linked, and run directly against the real Vulkan SDK and the
  real GPU (the same method every prior pass used) — cleanly, zero warnings under `/W4 /WX`, plain
  build and AddressSanitizer both. The whole-project configure issue itself remains open, unresolved,
  and out of scope for this backend to fix.
  **(RESOLVED by red-team pass 5, §4e) The "C1083" blocker is Windows `MAX_PATH`, not a toolchain bug.**
  Every failing `try_compile` object path is about 248 characters long when built from a worktree
  directory. A shorter build root moved the failure to a later, longer-named `try_compile`, and a
  `subst`-ed drive root (`Q:\b`) made it disappear. With that root:
  - the whole-project `cmake`+Ninja configure succeeds with `AGENTENGINE_WITH_VULKAN=ON`, and both Vulkan
    test targets build through their REAL CMake targets and pass under `ctest`;
  - it also succeeds at the default OFF, and the generated `build.ninja` contains no Vulkan reference.

  So default-OFF zero impact is now confirmed by an actual configure, not just inspection. The practical
  fix for anyone on this dev box: build from a short root, or enable Windows long paths. No code change is
  needed. This likely also explains the unrelated `mt.exe` HTTPS-configure failure §8 step 7 names; that
  was not verified here.
- **`vkCmdDispatch`'s workgroup count is not validated against the physical device's own
  `maxComputeWorkGroupCount[0]` limit** (red-team pass 3, §4c finding 4; re-examined and NOT relitigated
  by pass 4 per this pass's own explicit brief) — unreachable at any `n` this backend has actually been
  measured at (1000, 5000) and, per the Vulkan spec's guaranteed minimum (65535 per dimension),
  unreachable up to the low millions even on the least-capable conformant device; the new uint32
  dispatch-size guard (§5-6) bounds `n` far below where this could plausibly matter in practice. Left as
  a named, low-likelihood residual, not fixed.
  **(CLOSED by red-team pass 5, §4e finding 2)** `create()` now records `maxComputeWorkGroupCount[0]`,
  and `detail::check_gpu_device_limits()` rejects an over-limit dispatch before any GPU work. Proven at
  the 65,535/65,536 boundary. It was closed alongside the more consequential `maxStorageBufferRange` gap,
  which needed the same limits query.
- **Neither red-team pass 3's nor red-team pass 4's fixes for unchecked Vulkan API failures (§4c
  Critical finding 1; §4d Critical finding 1 and Real gap 2) are proven via a forced-failure test, only
  on the happy path** — safely inducing a genuine Vulkan device-lost or out-of-memory condition on real
  hardware, without risking this machine's stability, was judged out of scope for both passes' own
  "resource-capped" machine-safety discipline (CLAUDE.md). Every such fix compiles cleanly under
  `/W4 /WX` and the full test suite (63 assertions as of pass 4) stays green, in both a plain build and
  under AddressSanitizer, but a reviewer should not read "green" here as "the failure path itself was
  exercised" for ANY of these checks — every one of them was reviewed and reasoned about, not executed
  under a real failure. This residual now spans more call sites than pass 3 alone left open (§4d added
  `vkBindBufferMemory` ×2 and `vkEnumeratePhysicalDevices` ×2 to the same "checked, but only the happy
  path is proven" set), not fewer — an honest widening, not a narrowing, of what "proven" means here.
  **(LARGELY CLOSED by red-team pass 5, §4e finding 6)** A test-only fault-injection seam now makes each
  of the 21 checked entry points' failure branches execute:
  - under the validation layer, with zero errors across 59 instance lifetimes;
  - with same-instance recovery asserted after every injected failure;
  - in a permanent registered test, `test_vulkan_cosine_index_fault_injection`.

  Doing so found a real Critical (§4e finding 1) on exactly such a branch, which justifies the method
  after the fact. **What remains open, named honestly:**
  - **An injected failure is not a driver's real failure.** Creation-style calls are skipped rather than
    failed by the driver, and `vkWaitForFences` reports `VK_ERROR_DEVICE_LOST` only AFTER a real,
    successful wait. So the case where a GENUINELY lost device leaves a command buffer still pending when
    the cleanup path frees it (`vkFreeCommandBuffers`/`vkDestroyFence` after a failed wait) is not
    modeled, and its conformance to the spec's lost-device rules was not verified.
  - **The seam is a separate compile of the same source** (`AGENTENGINE_VULKAN_FAULT_INJECTION`). The
    failure paths proven are the production SOURCE's, not the production BINARY's.
- **A genuine UndefinedBehaviorSanitizer run against `VulkanCosineIndex` was attempted for the first
  time this ADR (red-team pass 4, §4d) and did NOT succeed** — via `clang-cl` (22.1.5, LLVM's own
  MSVC-compatible driver), the only UBSan-capable toolchain available on this dev box (`cl.exe` itself
  has no UBSan). Blocked by a real, reproducible, but unrelated toolchain-interop gap: `clang-cl` failed
  to recognize C++20/23 standard-library surface (`std::span`, `std::expected`,
  `std::unordered_map::contains()`, class-template-argument deduction for `std::unique_lock`) when
  compiling against this dev box's preview MSVC 14.51 STL, even with an explicit `/std:c++23` or
  `-std=c++23` — diagnosed down to a language-mode/feature-detection-macro mismatch between this
  specific `clang-cl` release and this specific preview MSVC STL, not something this backend's own code
  causes or could fix, and not chased further (the same "don't spend the session fixing an unrelated
  toolchain issue" discipline this ADR's own C1083 finding already established, applied to a second,
  distinct toolchain gap). `VulkanCosineIndex`'s UBSan coverage remains genuinely absent, not merely
  unattempted — a real, open gap for whoever next has a working `clang-cl`+preview-MSVC-STL combination,
  or a stable (non-preview) MSVC toolset, on this dev box.
  **(RESOLVED by red-team pass 5, §4e)** Visual Studio's own bundled `clang-cl` (22.1.3, `VC\Tools\Llvm`)
  with `/std:c++latest` (not `/std:c++23`) and `/MT` compiles against the same preview STL. That setup
  ran `test_vulkan_cosine_index` (86/86) and `test_vulkan_cosine_index_fault_injection` (410/410) clean
  under `-fsanitize=undefined -fno-sanitize-recover=all`. A signed-overflow canary built with the same
  flags trapped, as a positive control.
- **This pass's AddressSanitizer run found the host side clean, but Windows' MSVC ASan implementation
  has no LeakSanitizer** — `detect_leaks` is a no-op on this platform, so while the repeated-
  `add_batch()`/cache-rebuild test (§5-6) proves CORRECTNESS across multiple rebuild cycles, true
  leak-freedom of the "destroy the OLD GPU buffer before reassigning" pattern rests on code review
  (confirmed correct: the old buffer is destroyed only after the new one is fully built, right before
  reassignment) rather than an executed, leak-detecting run — named honestly rather than implied by
  "ASan came back clean."
  **(NARROWED by red-team pass 5)** The Khronos validation layer tracks every Vulkan object per device
  and instance, and reports any object still alive at `vkDestroyDevice`/`vkDestroyInstance`. It reported
  zero errors across all 59 instance lifetimes of the fault-injection run, including every failure-path
  teardown. So GPU-object leak-freedom is now executed evidence, not code review. Host-heap leaks remain
  unchecked on Windows.
- **(CLOSED 2026-09-23 — see the fix note at the end of this bullet) (red-team pass 5 §4e finding 4)
  `BruteForceCosineIndex` (the already-Judged ADR-063 CPU conformer) shared the NaN-unsafe comparator
  and accepted NaN/Inf input.** That is the same
  `std::sort` strict-weak-ordering UB `VulkanCosineIndex` was just fixed for. It was reproduced
  identically on the CPU side: 11 ordering violations among finite scores. It was not fixed here: it is
  outside this backend's files, and `core/vector_index.hpp` had uncommitted changes from another session
  in the main checkout at the time. The fix mirrors this pass's two layers (reject non-finite input,
  NaN-total comparator) and belongs in a small ADR-063 follow-on. Until then, the two conformers of the
  same concept DIVERGE on non-finite input, deliberately: GPU rejects, CPU accepts and has UB.
  **Fixed 2026-09-23, directly in `core/vector_index.hpp` rather than as a separate follow-on** (the
  "uncommitted changes from another session" were this ADR's own step-1 work, so there was no conflict to
  avoid). Reproduced first through the PUBLIC API, not just the comparator: `add_batch()` accepted 200
  vectors with every 7th carrying a NaN component, and `search()` returned 29 NaN scores with 17
  descending-order violations among the finite ones. Fix, mirroring `VulkanCosineIndex`'s two layers:
  (a) non-finite input is rejected as `contract` at all THREE entry points — `add_batch()`
  (`vector_index.add_batch_non_finite`, whole batch rejected, nothing committed), `search()`'s query
  (`vector_index.search_non_finite`), and `restore()` (`vector_index.snapshot_non_finite` — a snapshot blob
  is external bytes, so a corrupt or crafted one must not smuggle NaN past `add_batch()`'s check; the
  Vulkan passes had no equivalent path to consider); (b) the sort is factored into
  `vector_index_detail::sort_and_truncate()` with a NaN-total comparator (§8 item 1's originally planned
  shared helper). Finite input cannot produce a NaN score on this path (double accumulation, zero-norm
  already returns 0), so (a) makes NaN scores unreachable and (b) keeps the sort's precondition
  independent of that. `tests/test_vector_index.cpp`: 10 new assertions, including a hand-built AEV1 blob
  carrying a NaN and the comparator driven directly with 29 NaNs among 200 scores; 56/56 green in a
  plain build and under AddressSanitizer. The scratch reproduction now shows NaN rejected at the
  boundary. Every other test depending on `vector_index.hpp` re-run plain + ASan, all green:
  `test_corpus_source`, `test_hybrid_rag_context_provider`, `test_remote_vector_index`,
  `test_sparse_index`, `test_vector_index_benchmark`, `test_vector_rag_context_provider`,
  `test_qdrant_vector_index` (offline, stub transport), and `test_vulkan_cosine_index` on the real GPU
  (86/86). This is a behavior change to an already-Judged ADR-063 type: input that was previously
  accepted (and silently mis-ranked) is now rejected. No in-tree caller passed non-finite vectors.
- **(RULED 2026-09-23) (red-team pass 5 §4e finding 7) `VK_ERROR_DEVICE_LOST` is classed `failure_class::resource`.**
  It now has its own stable code, `vulkan_vector_index.device_lost`, so a caller CAN distinguish it. But
  whether device loss should be `fatal`, `transient`, or a new class is a genuine policy question, not
  something this pass decided. `error.hpp` defines `fatal` as "the run ends", which is too strong for a
  host with a CPU fallback. `resource` means "budget/quota/limit exceeded", which is not quite what
  device loss is either. **Project-owner ruling, 2026-09-23: it stays `resource`.** `fatal` would end the
  whole run for a condition a host with a CPU fallback survives, and the stable
  `vulkan_vector_index.device_lost` code already lets a caller tell it apart from out-of-memory and
  choose to rebuild via `create()` or fall back to `BruteForceCosineIndex`. No code change: this records
  that the existing classification is the decision, not a placeholder.
- **(NEW, red-team pass 5) Only `maxStorageBufferRange` and `maxComputeWorkGroupCount[0]` are
  enforced.** Two other limits are not: `maxMemoryAllocationSize` (Vulkan 1.1 `maintenance3`) and
  `maxBufferSize` (1.3 `maintenance4`). Both are 2 GiB on the reference device, below its 4 GiB−1
  storage range. A 2-4 GiB vectors buffer (roughly 350k-700k vectors at dim=1536) therefore passes the new
  guard. It then relies on `vkCreateBuffer`/`vkAllocateMemory` reporting failure, which they do through
  now-executed, checked branches. The spec says oversized allocations "may fail", so this is not a
  guaranteed failure. Querying those two limits needs `vkGetPhysicalDeviceProperties2`, which this
  backend's Vulkan 1.1 baseline does support. Not done this pass.
- **(NEW, red-team pass 5) The spec-floor reproduction is not itself a permanent test.** The
  `maxStorageBufferRange` finding was reproduced under the profiles layer emulating
  `VP_ANDROID_vulkan_profile_2021`. The permanent regression proof is the pure synthetic-limit predicate
  test, which pins the exact 21,845/21,846 boundary. The emulated-device run needs env-configured layers
  and is recorded in §4e, not registered with `ctest`.
- **(NEW, red-team pass 5, observed, not a defect) `create()` picks the FIRST compute-capable physical
  device, with no preference for a discrete GPU.** This dev box enumerates three devices: the RX 5300M
  twice (two ICD entries) and an integrated AMD GPU. The discrete one happens to come first. On a
  machine where the integrated GPU enumerates first, `VulkanCosineIndex` would silently run there. That
  is a performance residual, not a correctness one; §3 claim 7(b) holds on any conformant device.
- **A no-op `add_batch({}, {})` call still unconditionally marks the GPU vectors-buffer cache dirty**
  (red-team pass 4, §4d finding 4/Minor) — the next `search()` rebuilds the buffer even though nothing
  about the corpus changed. Proven not to be a correctness bug (a new test interleaves a no-op call
  between real ones and confirms subsequent `search()` results are unaffected); purely an unbenched
  performance cost, and one no real call site in this ADR family actually triggers (nothing here
  constructs an intentionally-empty `add_batch()` call), so left as a named, low-priority residual, not
  fixed — the same "a hot path without a bench... is not done" bar cutting against speculatively
  optimizing a path with no evidence a real caller ever hits it.
- **`BM25Index`'s ASCII whitespace/punctuation tokenizer is a real quality gap**, not just a stated
  simplification — no stemming, no Unicode-aware tokenization, no stop-word list. Named so it is not later
  mistaken for a completed, tuned implementation.
- **RRF's `k_rrf = 60` is the literature default, not validated against this project's own corpora** — same
  "corroborated as reasonable, not claimed optimal" posture ADR-063 §2.4b already took for chunking
  parameters.
- **hnswlib (CPU ANN) remains named, not designed, not built** — explicit session scoping decision, carried
  forward unchanged from ADR-063 §2.3.
- **Approximate GPU search remains out of scope** — explicit session scoping decision; `VulkanCosineIndex` is
  exact brute-force only.
- **No CUDA/Metal/ROCm-specific backend is shipped, and none is planned** — by design (§2.6): Vulkan is the
  one, generic, cross-vendor GPU conformer this project ships; a vendor-specific backend is follow-on work
  for whoever needs it, against the same `VectorIndex` seam.
- **A `PgVectorIndex` (or any other `RemoteVectorIndex` conformer) is a plausible future addition**, not
  designed here — the concept is the seam.

## 8. Implementation plan (sequencing, not a commitment to timeline)

1. `core/vector_index.hpp`: add `PersistentVectorIndex` concept; add `snapshot()`/`restore()` to
   `BruteForceCosineIndex`; factor its sort/tie-break/truncate logic into a shared free function usable by
   `VulkanCosineIndex` later. New tests: claim 4 (round-trip fidelity), claim 5 (zero re-embed after restore).
2. `core/remote_vector_index.hpp` (new file): the `RemoteVectorIndex` concept + `AnyVectorIndex` umbrella.
3. `core/sparse_index.hpp` (new): `SparseIndex` concept. `core/bm25_index.hpp` (new): `BM25Index`. New
   tests: claim 3.
4. `vector_rag_context_provider.hpp`: extract `render_scored_chunk()` to a free function (behavior-preserving
   refactor, re-run existing suite to confirm zero change); add the `if constexpr (RemoteVectorIndex<IndexT>)`
   dispatch branch in `on_context()` and `make_recall_tool_descriptor()`. New tests: claim 1 (no-regression).
5. `core/hybrid_rag_context_provider.hpp` (new): `HybridRagContextProvider<DenseIndexT, SparseIndexT>`, RRF
   fusion. New tests: claim 2, claim 8.
6. `corpus_source.hpp`: extend `mount()` for the optional second (`SparseIndexT`) index parameter, per
   §2.3a's ingestion-ordering design. New tests covering the "sparse backfill onto a pre-existing dense-only
   corpus" case named in §2.3a.
7. `protocol/qdrant/vector_index.hpp` (new): `QdrantVectorIndex`, a `RemoteVectorIndex` conformer. New tests:
   claim 6 (live, env-gated, `live-network`-labeled — matches `test_openai_embedder_openrouter_live_e2e.cpp`'s
   pattern), plus an offline request/response-shape test matching `test_openai_embedder.cpp`'s split.
8. **DONE (2026-09-22).** `src/backends/vulkan_vector_index/` (`AGENTENGINE_WITH_VULKAN` CMake option,
   default OFF): `VulkanCosineIndex`, the SPIR-V shader (checked in pre-compiled), fail-closed
   device-selection logic. `tests/test_vulkan_cosine_index.cpp`: claim 7 (bench + epsilon-bounded top-K
   comparison against `BruteForceCosineIndex`) — part (b) CORRECT, part (a) DISPROVEN (GPU measured
   slower than CPU on the one reference GPU tested; see §3 claim 7, §5-6, §7). Built in an isolated
   `worktree` and merged to `main` (fast-forward, commit `f085bbd`).
9. **PARTIALLY DONE.** Red-team passes have run against every piece of this ADR, and every finding is
   fixed and proven or named as a residual:
   - pass 1 (§4, steps 1-6);
   - pass 2 (§4b, `QdrantVectorIndex`);
   - passes 3, 4 and 5 (§4c/§4d/§4e, `VulkanCosineIndex`).

   ~~**Still owed**: the live Qdrant run~~ — **DONE 2026-09-23**, 12/12 against real Qdrant 1.19.1
   (§3 claim 6). The earlier open question, whether `VulkanCosineIndex` needed more red-teaming, is now settled by
   pass 5's outcome and recommendation.
   **Pass 5's recommendation (2026-09-23): the Vulkan surface is ready for the Judge step, with two named
   conditions, and a fourth eyes-only red-team pass is NOT the right next step.** The honest case, both
   sides:
   - **Against.** Every one of passes 3, 4 and 5 found a Critical. That pattern, taken alone, argues for
     more scrutiny.
   - **For.** What pass 5 changed is the method, not just the scope. The Criticals of passes 3 and 4
     (unchecked `VkResult`s) and of pass 5 (wrong cleanup after a checked `VkResult`) are exactly the
     classes that pass 5's permanent, mechanical instruments now catch on every run, rather than a
     reviewer catching them by reading:
     - the exhaustive entry-point table;
     - a fault-injection test executing 21 of 21 failure-capable entry points under the validation layer,
       with same-instance recovery asserted;
     - validation-layer, ASan and UBSan runs, each with a positive control.

     The remaining open items are policy or scope questions, not suspected defects.

   The two conditions for the Judge:
   1. ~~**Rule on `VK_ERROR_DEVICE_LOST`'s `failure_class`** (§7).~~ **RULED 2026-09-23** by the project
      owner: stays `resource` (§7).
   2. ~~**Track `BruteForceCosineIndex`'s identical NaN strict-weak-ordering defect** (§7) as an ADR-063
      follow-on.~~ **CLOSED 2026-09-23** — fixed directly in `core/vector_index.hpp` and proven (§7).
      Both conditions are now closed.

   If more assurance is wanted before Judged, the highest-value next step is also mechanical, not
   another sampled review: run `test_vulkan_cosine_index_fault_injection` on a second vendor's driver
   (e.g. a Linux CI runner with Mesa's lavapipe), since every result above comes from one AMD driver.
   The live Qdrant run has now landed (2026-09-23), and both conditions are closed. What remains is the
   Judge step itself, then the project owner's Judged sign-off. That is the same sequence `ADR-063`/`ADR-064` both actually
   followed, not skipped for expedience. **Done — see §9.**

## 9. The decision — Judged (2026-09-23, project owner sign-off)

The project owner signed off on 2026-09-23, after the live Qdrant run landed (§3 claim 6, commit
`cf6c36c`). As with `ADR-063` and `ADR-064`, the project owner's sign-off is the Judge step. What this
decision settles:

- **Accepted as built:**
  - `SparseIndex` and `BM25Index`.
  - `HybridRagContextProvider`, fusing results with RRF (reciprocal rank fusion).
  - `PersistentVectorIndex`, with snapshot and restore.
  - `RemoteVectorIndex`, with `QdrantVectorIndex` as its first conformer.
  - `VulkanCosineIndex` as the one generic GPU backend, behind `AGENTENGINE_WITH_VULKAN` (default OFF).
    No vendor-specific backend is planned; anyone who needs another one builds it against the same
    `VectorIndex` concept (§2.6).
- **Accepted with an honest negative:**
  - §3 claim 7(a) stays DISPROVEN. On the one reference GPU, `VulkanCosineIndex` is slower than CPU
    brute force. The backend ships for correctness and as the extension point, not as a proven speedup.
  - Claim 7(b), GPU/CPU agreement, is CORRECT.
- **Rulings folded in:**
  - `VK_ERROR_DEVICE_LOST` stays `failure_class::resource`.
  - `BruteForceCosineIndex`'s NaN strict-weak-ordering defect was fixed directly (§7).
- **Carried as named residuals, not blockers.** Everything else §7 lists stays open, notably:
  - `QdrantVectorIndex`'s confidentiality residual (§4b finding 1). It has an operational fix, not
    closure in code.
  - The free-form `collection_name` versus a `corpus_scope`-derived name.
  - Stale-chunk GC.
  - Validation of the tokenizer and of `k_rrf`.
  - A second-vendor Vulkan driver run (for example Mesa lavapipe). This remains the recommended next
    assurance step.

  Each of these needs its own follow-on ADR if it is ever to be closed.
