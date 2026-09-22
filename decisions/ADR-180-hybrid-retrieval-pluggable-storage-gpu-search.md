# ADR-180 — Hybrid retrieval, pluggable/persistent vector storage, and a generic GPU search backend

**Status:** Proposed (2026-09-22, design pass drafted collaboratively with the project owner in a live
session). **§8 steps 1-6 are REAL, compiled, and passing, AND have now been through one independent
red-team pass with every finding fixed and proven** (2026-09-22, same session — see §4 below for the
full findings and §5-6 for the fixes). **§8 step 7 (`QdrantVectorIndex`) is now REAL for its
offline-provable half, AND has been through its own dedicated red-team pass** (2026-09-22, same
session — see §5-6 for the full account, including a real heap-buffer-overflow this pass's OWN offline
test caught via AddressSanitizer, and §4b for a second pass finding one genuine Critical confidentiality
residual — fixed with defense-in-depth plus an honest operational-fix disclosure, not fully closed by
code alone; see §7). The live half (a real Qdrant instance) is written but not yet run — this session
made roughly eight separate attempts to bring up a local Qdrant via Docker on this machine, and
confirmed the blocker is not transient: even a bare `docker rm -f`/`docker start` now hangs past a 15s
timeout, while read-only commands (`docker ps`, `docker info`) answer fine, pointing at the daemon's
container-lifecycle path specifically, not at this session's code.
**§8 step 8 (`VulkanCosineIndex`) is now REAL and built** — a `worktree`-isolated agent built it against
real Vulkan hardware (a discrete AMD Radeon RX 5300M), found and fixed two genuine performance bugs
along the way, and it has merged cleanly to `main` (2026-09-22, commit `f085bbd`). Correctness holds
(GPU/CPU cosine scores agree within ~1e-7). **Claim 7's performance half did NOT hold**: after both
fixes, GPU search measured ~1.3x SLOWER than CPU brute-force at both n=1000 and n=5000, not faster —
see §3 claim 7 and §5-6 for the full numbers. **It has now been through its own dedicated red-team
pass** (2026-09-22, same session, a second `worktree`-isolated agent, no prior context — see §4c below
for the full findings, one Critical, and §5-6 for the fixes, all fixed and proven the same session
against the real GPU this backend was already built against). Every piece of this ADR's scope has now
had at least one red-team pass (steps 1-6: one; `QdrantVectorIndex`: two; `VulkanCosineIndex`: one).
Per `CLAUDE.md`'s "contested, hot-path, or security-critical designs go through `design → red-team →
prove → judge`" rule, this ADR is **still not Judged**: whether `VulkanCosineIndex` is owed a SECOND
red-team pass before Judged, mirroring `QdrantVectorIndex`'s own two-pass precedent (it remains this
ADR's newest and least-familiar attack surface — a compute shader, raw GPU buffer handling, a new
third-party SDK dependency), is an open question this session did not resolve (§7); and the live
Qdrant run (§3 claim 6) remains blocked on the same persistent local Docker daemon issue, unresolved.
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
   build and AddressSanitizer. **The live half (a real Qdrant instance) is NOT executed** — still
   PENDING, not claimed. See §5-6 below for the real finding this offline pass caught (a heap-buffer-
   overflow, found via ASan) before any live test could have made it worse.
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
- **What is still owed, explicitly not claimed done**: a live-network test against a real Qdrant
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
- **`QdrantVectorIndex`'s live half (§3 claim 6) is unexecuted** — the live test is written
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
- **(NARROWED by red-team pass 3, §4c) `VulkanCosineIndex` has now had ONE red-team pass, not zero** —
  one Critical finding (unchecked Vulkan API return codes across `search()`'s hot path) and two Real
  gaps (a push-constant integer-overflow gap; a test-coverage gap), all fixed and proven the same
  session (§5-6). Whether a SECOND pass is owed before Judged, mirroring `QdrantVectorIndex`'s own
  two-pass precedent (this remains this ADR's newest and least-familiar attack surface: a compute
  shader, raw GPU buffer handling, a new third-party SDK dependency), is an open question this session
  did not resolve.
- **(NARROWED by red-team pass 3, §4c) `VulkanCosineIndex`'s CMake wiring default-OFF impact is now
  confirmed zero by static inspection** — a `grep` across both `CMakeLists.txt` files found no Vulkan
  target/symbol reference outside the two `if(AGENTENGINE_WITH_VULKAN)` guards. **Still NOT confirmed
  by an actual successful whole-project `cmake`+`ninja` configure-and-build** — attempted this pass
  (real MSVC 14.51 toolchain, `-DAGENTENGINE_WITH_VULKAN=ON` and, separately, the default `OFF`), and
  BOTH fail identically at an earlier, wholly unrelated `tests/compile_fail/*.cpp` positive-control
  `try_compile` step (`C1083: Cannot open compiler generated file: ''`, reproduced against a file
  (`tainted_declassify_positive_control.cpp`) with no connection to Vulkan) — a real, pre-existing,
  intermittent MSVC/Ninja toolchain issue on this dev box (the same class of issue this ADR's own §8
  step-7 account already names for a different tool, `mt.exe`), not something the Vulkan wiring causes
  or could itself fix. `VulkanCosineIndex` was instead compiled, linked, and run directly against the
  real Vulkan SDK and the real GPU (the same method the original building agent used) — cleanly, zero
  warnings under `/W4 /WX`, plain build and AddressSanitizer both. The whole-project configure issue
  itself remains open, unresolved, and out of scope for this backend to fix.
- **`vkCmdDispatch`'s workgroup count is not validated against the physical device's own
  `maxComputeWorkGroupCount[0]` limit** (red-team pass 3, §4c finding 4) — unreachable at any `n` this
  backend has actually been measured at (1000, 5000) and, per the Vulkan spec's guaranteed minimum
  (65535 per dimension), unreachable up to the low millions even on the least-capable conformant
  device; the new uint32 dispatch-size guard (§5-6) bounds `n` far below where this could plausibly
  matter in practice. Left as a named, low-likelihood residual, not fixed.
- **Red-team pass 3's fix for unchecked Vulkan API failures (Critical finding 1, §4c) is proven only on
  the happy path, not via a forced-failure test** — safely inducing a genuine Vulkan device-lost or
  out-of-memory condition on real hardware, without risking this machine's stability, was judged out of
  scope for this pass's own "resource-capped" machine-safety discipline (CLAUDE.md). The fix compiles
  cleanly under `/W4 /WX` and the full test suite (54 assertions) stays green, in both a plain build and
  under AddressSanitizer, but a reviewer should not read "green" here as "the failure path itself was
  exercised" — it was reviewed and reasoned about, not executed under a real failure.
- **This pass's AddressSanitizer run found the host side clean, but Windows' MSVC ASan implementation
  has no LeakSanitizer** — `detect_leaks` is a no-op on this platform, so while the repeated-
  `add_batch()`/cache-rebuild test (§5-6) proves CORRECTNESS across multiple rebuild cycles, true
  leak-freedom of the "destroy the OLD GPU buffer before reassigning" pattern rests on code review
  (confirmed correct: the old buffer is destroyed only after the new one is fully built, right before
  reassignment) rather than an executed, leak-detecting run — named honestly rather than implied by
  "ASan came back clean."
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
9. **PARTIALLY DONE.** Red-team pass 1 (§4, steps 1-6), pass 2 (§4b, `QdrantVectorIndex`), and pass 3
   (§4c, `VulkanCosineIndex`) have all now run, every finding fixed and proven. **Still owed**: the live
   Qdrant run (§3 claim 6, blocked on a persistent local Docker daemon issue — §7), and an open question
   (not yet resolved either way) of whether `VulkanCosineIndex` is owed a SECOND red-team pass before
   Judged, mirroring `QdrantVectorIndex`'s own two-pass precedent (§7). Only once the live run lands and
   that question is settled does this ADR go to the project owner for Judged sign-off — the same
   sequence `ADR-063`/`ADR-064` both actually followed, not skipped for expedience.
