# ADR-063 — Shape of a retrieval-augmented (RAG) `ContextProvider` and its storage seam

**Status:** Judged (2026-08-19, project owner sign-off). Designed and independently red-teamed (§4, `general-purpose`
agent pass with no prior context on this document, found 4 critical + 6 real-gap findings). Two
implementation passes the same day (§5): first a foundation layer (`provenance_marker.hpp`,
`embedder.hpp`, `vector_index.hpp`, `corpus_scope.hpp`, `corpus_chunk.hpp` + tests), then a
three-way parallel pass (`corpus_source.hpp`, `protocol/openai/embedder.hpp`,
`vector_rag_context_provider.hpp` + tests, each built by an independent agent against the shared
foundation) that closes every one of §2's designed pieces with REAL, compiled, passing code — the
full local test suite is 193/193 green (MSVC 19.4x, `ctest -LE live-network`), including 4 new
dedicated test files. §3 claims 5 and 6 now have a real end-to-end CORRECT verdict (§6) — the FULL
`VectorRagContextProvider`-level proof, not just the underlying-mechanism-level proof recorded
earlier. Claim 4 has since been executed for real against live `openrouter.ai` with a real
credential (all 5 assertion groups pass) and is also CORRECT. **Claim 3 has since been executed for
real and is CORRECT** (a scripted embedder returning two different vectors for the same query text
produces two different `ContextContribution`s across two `on_context()` calls). **Claim 1 has since
been benchmarked for real** (`tests/test_vector_index_benchmark.cpp`, Release/clang numbers: ~248
µs/~1.45 ms/~7.6 ms per `search()` call at n=200/1000/5000, dim 1536) — verdict is INCONCLUSIVE, not
CORRECT, since RFC 023 names no dedicated budget row to compare against yet, but the numbers already
exceed the closest existing analog at realistic corpus sizes, a real finding worth acting on. Only
claim 7 (Vulkan, deliberately deferred per §2.3B — not a gap) remains outside a CORRECT/INCONCLUSIVE
verdict.
**A second, independent red-team pass against the NEW (implementation) code ran 2026-08-19**
(`general-purpose` agent, again no prior context on this document — matching §4's own methodology),
specifically hunting for what the design-level §4 pass could not have found. It reported **1
critical + 2 real-gap + 2 minor findings**, none overlapping §4/§7's own already-known list — see
§5's new "Red-team pass 2" subsection for the full findings and §7 for each one's fix status. **All
5 were fixed the same pass**, each with a new, real, passing test proving the fix (full suite still
193/193 green, `ctest -LE live-network`, zero regressions). The most severe: `rag_corpus_key()`
(`corpus_scope.hpp`) could derive an IDENTICAL key for two structurally different mounts when a
`tenant_id`/`principal.id`/`corpus_name` field itself contained `':'` (now fixed via a
length-prefixed, uniquely-decodable encoding); and `last_user_text()` (both
`vector_rag_context_provider.hpp` and, inherited, `memory_provider.hpp`) read `history.back()`
unconditionally, which is reachable as the ASSISTANT's own message during a tool-call round —
letting model output become the embedder's real network payload and steer retrieval, a direct I3
violation the moment this provider is wired into a live `AgentSession` (now fixed: walks backward to
the last genuine `role::user` message).
**Judged (2026-08-19)** — this design is now a settled decision, accepted by the project owner on the
evidence above. The `recall(query)` sync-invoke/async-embedder gap (§7), the one item this paragraph
previously named as blocking Judged, is CLOSED — see
`decisions/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md` (Design B implemented and proven,
2026-08-19, also Judged the same day). Claim 3 is CORRECT with real executed evidence; claim 1 stays
honestly INCONCLUSIVE (real measured numbers exist, but RFC 023 names no dedicated budget row to
compare against yet) — accepted as a residual, not a blocker, the same posture ADR-005 §7 already set
precedent for (a Judged ADR with one named INCONCLUSIVE claim, not every claim required to resolve
CORRECT before sign-off). What remains open, not gating this sign-off: whether the ANN/GPU seam
backend gate (§2.3B) should move up the roadmap given claim 1's real finding, `GraphRagContextProvider`
(§2.1b, named/cost-scoped, not designed), and the other named residuals in §7 below.

**Relates to:** `005-Sessions-State-and-Memory.md` §5 (`ContextProvider`), `029-Memory-System.md`
(the sibling "kind" this generalizes — 029 is keyword/salience/recency, deterministic, no network
call; this ADR's design explicitly does NOT inherit that no-network guarantee, see §3.2).
`CONVENTIONS.md`'s three-tier dependency posture (§3.4 below is bound by it directly).
`core/worktree.hpp`, `core/memory.hpp`, `core/skill_source.hpp` (the precedent this design reuses
rather than reinvents). `docs/research/2026-08-05-rag-chunking-and-ingestion.md` (pre-existing, dated
research this draft initially missed — covers chunking strategy evidence and GraphRAG cost/pipeline
data directly relevant to §2.4 and §3; see §2.4b below). `docs/research/2026-08-19-embedding-
provider-landscape.md` (OpenRouter/Anthropic embeddings facts backing §2.5, split out after an
earlier draft cited them inline without a dated source file, violating CLAUDE.md's own citation
rule).

## 1. The question

**Stated so it has a wrong answer:** can AgentEngine add embedding-based retrieval (RAG) as an
ordinary `ContextProvider` conformer, using only infrastructure this project already has a proven
pattern for (content-addressed worktree storage, declared-backend policy tags, capability-gated
effects) — or does RAG's own nature (external embedding calls, potentially large vector indices,
folder-scale corpora) require a genuinely new subsystem outside the `ContextProvider`/`WorktreeObjectStore`
model this codebase is built on?

**This document's answer: no new subsystem is required.** Every piece reuses an existing, proven
shape (`ChatClient`-shaped declared backend, `WorktreeObjectStore` content-addressing,
`SkillSource`-shaped folder mounting, the three-tier dependency policy). The design below is that
composition, made explicit enough to attack.

## 2. Competing designs, steelmanned

### 2.1 Where does RAG retrieval logic live?

- **(A, chosen) A new `ContextProvider` conformer** (`VectorRagContextProvider<EmbedderT, IndexT>`
  — named for the specific kind it is, not "the" RAG provider; see §2.1b),
  composed into `AgentSession` via `ComposedContextProvider<...>` alongside `HistoryProvider`/
  `SkillsProvider`/`MemoryProvider`. *Steelman for the alternative:* a bespoke pre-turn hook outside
  `ContextProvider` could avoid `ContextContribution`'s narrow 3-field surface (§5 of the "AI
  context" discussion) and let a provider do more — e.g. mutate `SessionContext` directly. Rejected:
  that reopens I2/I3 surface area (a provider able to touch things `ContextContribution` deliberately
  excludes) for a feature that fits the existing 3 fields (`instructions`/`messages`/`tools`)
  without needing more.
- **(B, chosen) Retrieval exposed as injected messages by default, with an optional contributed
  `recall(query)` tool** (mirroring `MemoryProvider`'s exact shape) for on-demand lookups beyond
  what was pre-injected, rather than ONLY a tool (Agentic-RAG-only) or ONLY injection
  (Naive-RAG-only). *Steelman for tool-only:* matches "Agentic RAG" literature more closely (the
  agent decides if/when to retrieve). Rejected as the sole mode: it silently changes turn cost/shape
  (a turn that never calls `recall` never benefits) and diverges from `MemoryProvider`'s already-
  established default-injection convention for no forced reason — both modes cost nothing extra to
  support together.

### 2.1b One `ContextProvider` class per RAG kind — never one mega-parametrized provider

Decided in conversation (2026-08-19): **each RAG "kind" (flat/vector, graph, ...) is its own,
separately-named `ContextProvider` conformer class — not one `RagContextProvider` widened with more
template parameters or a runtime mode flag to cover every style.** This matches the precedent already
set by every other `ContextProvider` kind in this codebase: `HistoryProvider<Window<N>>` and
`HistoryProvider<Summarize<N, SummarizerT>>` are two *specializations* of one template (same
family, same single concern — how much history to keep), but `MemoryProvider` and `SkillsProvider`
are fully separate, unrelated classes, composed together only via `ComposedContextProvider<...>` when
an agent wants both. RAG kinds are the `MemoryProvider`/`SkillsProvider` shape, not the
`HistoryProvider<Strategy>` shape — a flat-vector retriever and a graph retriever do not share one
underlying concern with two dials, they are genuinely different mechanisms with different backends,
different storage shapes, and different failure modes (§2.3, §2.4b).

**Concrete naming this implies:**
- **`VectorRagContextProvider<EmbedderT, IndexT>`** — the flat/vector-similarity kind (Naive/Advanced
  RAG per the earlier survey), the only kind actually designed by this ADR (§2.2-§2.5).
- **`GraphRagContextProvider<...>`** — named, cost-scoped (§2.4b), **not designed** — its own
  extraction/summarization backends and graph storage shape are separate follow-on work, per §7.
- **No `AgenticRagContextProvider`.** Explicitly rejected as its own class: "Agentic RAG" from the
  earlier survey is a *usage pattern*, not a distinct retrieval mechanism — it means the agent's own
  tool loop decides when/whether to call retrieval, which `MemoryProvider`'s existing contributed
  `recall(query)` tool (§2.1's design B) already gives for free, and `VectorRagContextProvider`
  inherits the identical pattern by contributing its own `recall`-shaped tool alongside default
  injection. Naming a 3rd provider class for this would duplicate a capability the composition model
  already has.
- **Self-RAG / Corrective RAG — genuinely open, not decided here.** Both are post-retrieval
  verification/self-critique loops, not new retrieval mechanisms — they could be (a) built INTO a
  specific provider's own `on_context()` (a verification step before returning results), or (b) a
  decorator wrapping any other `ContextProvider`, mirroring how `MiddlewareModelCallGateway<Inner,
  Ms...>` wraps any `ModelCallGatewayLike` inner rather than being baked into `ModelCallGateway`
  itself. (b) is the more consistent choice given this codebase's own middleware precedent, but is
  named here as an open question, not a decision.

### 2.2 Embedder shape

- **(A, chosen) `Embedder` concept shaped like `ChatClient`** (`capabilities()` +
  `embed_batch(texts, EffectContext&) -> task<result<vector<vector<float>>>>`), **batch-only** (no
  separate single-item `embed()`), **no Recording/Replay wrapper**. Decided in conversation
  (2026-08-19): the user explicitly chose NOT to require the `RecordingChatClient`/`ReplayChatClient`-
  style determinism wrapper that `ChatClient` backends get. *Steelman for wrapping:* 029 §9 G1's
  "no network call, byte-identical replay" guarantee is real and tested for Memory; an unwrapped
  `Embedder` means `VectorRagContextProvider` cannot make the same claim. **This is a named, accepted
  tradeoff, not an oversight** — recorded here so it is visible to a future reader, not silently lost.
- **(B, chosen) Batch is mandatory, not an optimization added later.** *Steelman for single-item
  first, batch later:* smaller initial surface. Rejected: `on_turn_end()`-style extraction (multiple
  memory items in one turn) and bulk corpus ingestion (§2.4) both need batching from day one to be
  affordable at all; adding it later would mean a second code path, which this project's own
  one-path discipline (`Tool`, `invoke_tool()`) argues against.

### 2.3 Index / storage backend

- **(A, chosen) `VectorIndex` concept; a `BruteForceCosineIndex` default living in `core/`
  (std-only, zero third-party dependency, matches CONVENTIONS.md tier 1 directly); ANN (hnswlib) and
  GPU-accelerated (Vulkan compute) implementations as opt-in seam backends** (`src/backends/*`,
  CONVENTIONS.md tier 2, one dependency each, behind a CMake option, never linked unless selected).
  *Steelman for ANN-by-default:* real deployments may exceed brute-force's comfortable range.
  Rejected as the DEFAULT: no bench exists yet proving brute-force is insufficient for this
  project's actual corpus sizes (per-session/per-tenant memory, not web-scale) — CLAUDE.md's own
  "a hot path without a bench... is not done" bar applies directly, and reaching for ANN/GPU first
  would violate it.
- **(B, deferred, not rejected) Vulkan compute-shader acceleration**, gated behind: (i) a CPU
  brute-force baseline existing and benched, (ii) the baseline's bench showing it is the actual
  bottleneck, (iii) an explicit resolution of §3.5's determinism concern before merge. Not designed
  further here — recorded only as a real, later option, not dismissed.

### 2.4 Folder-source-mount retrieval semantics

- **(A, chosen) Copy-at-mount**, mirroring `DiskSkillSource` exactly: walk the folder once, chunk
  each file, `put_blob()` the chunk TEXT into the content-addressed store (natural dedup), embed each
  chunk, and separately record `{source_path, line_range, source_file_hash}` as **citation metadata
  only** — never re-read at query time.
- **(B, rejected as the default) Live-pointer**: index stores only `{source_path, byte_offset,
  length, vector}`, re-reading the original file from disk on every retrieval via an explicitly
  granted `cap::FsRead` (same `granted`-parameter pattern `memory.hpp` already uses). *Steelman:*
  always fresh, smaller durable footprint (no duplicated text). **Rejected as the default** for two
  concrete failure modes, not merely "adds complexity":
  1. Breaks 029 §9 G1-style determinism a second, independent way (disk content can legitimately
     differ between index time and any later query time) — stacking two separate, unbounded
     nondeterminism sources (embedder API + live re-read) onto one provider is a materially bigger
     claim than accepting one.
  2. **Silent wrong-content risk, not just staleness**: if the source file is edited after mount,
     `byte_offset`/`length` computed against the OLD file can address into the NEW file's unrelated
     content — a `MemoryItem`-shaped item silently becoming a different, wrong item. Copy-at-mount
     cannot exhibit this failure mode; a frozen chunk is never re-interpreted against different bytes.
  - Re-freshness is instead handled by an **explicit, capability-gated re-mount** operation (not an
    implicit per-query read): walk the folder again, compare `source_file_hash` per file, and rely on
    content-addressing to make re-mounting an *unchanged* file a no-op (§3.3) rather than full
    re-indexing.

### 2.4b Chunking strategy — not designed here, but not unscoped either

Left "deliberately out of scope" in an earlier draft of this discussion; re-grounding against the
repo found `docs/research/2026-08-05-rag-chunking-and-ingestion.md` already exists and answers most
of it, corroborated across multiple primary sources (Anthropic's own contextual-retrieval post,
Chroma's chunking-strategy research, a cross-checked NVIDIA benchmark):

- **Default**: recursive/structure-aware splitting (paragraph → line → sentence fallback, or
  format-aware for code/markdown/HTML), token-bounded at roughly 400-512 tokens with 10-20% overlap
  — corroborated as a reasonable starting point across every source that research doc cites, not
  claimed optimal.
- **Must be swappable, not hardcoded** — that research found up to a 9-point recall swing between
  strategies on identical corpora, and two independent benchmarks disagree on which strategy wins.
  `CorpusSource`/chunking should therefore take the chunking strategy as a declared, replaceable
  policy (matching this project's own CRTP-policy idiom elsewhere), not a fixed algorithm.
- **Semantic/contextual chunking (Anthropic's Contextual Retrieval) is an optional upgrade behind a
  real eval**, never a default — it is itself a model call (nonzero cost, another spot needing the
  same I5 tradeoff-acknowledgment §2.2 already names for `Embedder`), and Anthropic's own source
  states: under ~200,000 tokens, skip retrieval and put the whole corpus in context instead.
- **GraphRAG cost data directly answers the "GraphRAG is expensive" concern raised earlier in this
  design conversation**: the same research doc's §4 cites Microsoft's reference GraphRAG pipeline at
  **$20-40 per million document tokens** to index (community summarization is the expensive stage),
  versus a lighter LightRAG variant that skips precomputed community summaries at **~$0.50 per
  million tokens** for comparable-or-better accuracy on holistic queries — a 50-6000x difference for
  dropping one LLM-heavy stage. Both sources place GraphRAG's useful range at roughly
  1,000-1,000,000 corpus tokens. If a Graph-shaped `ContextProvider` is ever built on top of this
  ADR's storage design, ITS OWN entity/relationship extraction and (optional) community
  summarization are separate, real model calls — same declared-backend/batch/no-wrap shape as
  `Embedder` §2.2, not solved by this ADR, only correctly cost-scoped by it now instead of asserted
  from memory.

### 2.5 Concrete `Embedder` conformers — who actually implements the shape

Named because §2.2 specified `Embedder`'s SHAPE but not who implements it — caught in review after
the first draft, not designed here yet as real code:

- **(chosen direction) One `OpenAIEmbedder` conformer, host-configurable — not a separate class per
  vendor.** Re-grounded against current code (2026-08-19, not assumed): `OpenAIChatClient`
  (`protocol/openai/chat_client.hpp`) is **already** the one and only client this codebase uses
  against OpenRouter — `http_referer_`/`x_title_` exist specifically for OpenRouter's
  `HTTP-Referer`/`X-Title` app-attribution headers (no JSON body field, confirmed against real
  OpenRouter docs per that file's own comment), and `tests/test_openrouter_live_e2e.cpp` proves
  `OpenAIChatClient` live against real `api.openrouter.ai`. There is no separate `OpenRouterChatClient`
  today, and none is needed — OpenRouter is modeled as "an OpenAI-compatible host," not a distinct
  vendor. **The identical relationship holds for embeddings** — see
  `docs/research/2026-08-19-embedding-provider-landscape.md` §1 for the sourced claim (OpenRouter runs
  a real `/api/v1/embeddings` endpoint, OpenAI-request-shape-compatible). So `OpenAIEmbedder` — same
  constructor shape as `OpenAIChatClient` (host/port/model/`SecretRef`/`Store const&`, credential
  resolved via `SecretStore::resolve()` inside `embed_batch()`, never at construction, 004 §1/018 §4),
  POSTing `{input: [...], model}` to `/embeddings` — covers BOTH `api.openai.com` and
  `api.openrouter.ai` by construction argument alone, exactly mirroring the chat-side precedent
  instead of inventing a parallel one.
- **No `AnthropicEmbedder`.** See `docs/research/2026-08-19-embedding-provider-landscape.md` §2:
  Anthropic runs no first-party embeddings endpoint at all — Claude is text-generation-only, and
  Anthropic's own published guidance points to a third-party provider (Voyage AI) for embeddings.
  This is a real, permanent asymmetry with `ChatClient`'s two-conformer (OpenAI + Anthropic) baseline,
  not a gap to backfill later — there is no Anthropic wire format for this to implement. A
  `VoyageEmbedder` is a plausible THIRD conformer if a real need for it shows up, but is not designed
  here.
- `EmbedderCapabilities.max_batch_size` must be populated per real, confirmed provider limits
  (declared, never probed — same rule `ChatClientCapabilities` already follows, 004 §2), not
  guessed; OpenRouter's own limit may differ from OpenAI's directly since it is a proxy in front of
  several underlying embedding backends.

### 2.6 Design response to §4's two unresolved-by-design Critical findings

Designed here (2026-08-19), after the red-team pass — closes findings 3 and 4 at the design level;
implementation and proof are still pending (§5-6).

#### 2.6a Multi-tenant / multi-principal corpus scoping (closes finding 3)

**A corpus's scope is a host-declared policy, never inferred from or overridable by a request** —
same I2/I3 posture as everything else in this codebase. Three scopes, named explicitly rather than
left implicit:

```cpp
enum class corpus_scope { per_principal, per_tenant, global_shared };
```

- **`per_principal`** — mirrors `memory_ref_name`/`memory_mount_id` EXACTLY (`memory.hpp:79`,
  `:122`): `"rag:vector:" + tenant_id + ":" + principal.id + ":" + corpus_name` for both the ref
  name and the mount id. Same reasoning `memory.hpp:102-124`'s own comment states for why `mount_id`
  is derived this way — a private, per-user corpus (e.g. "my own uploaded notes").
- **`per_tenant`** — `"rag:vector:" + tenant_id + ":" + corpus_name`, no `principal.id` component:
  shared across every principal WITHIN one tenant (e.g. a company's internal knowledge base), but
  the tenant boundary is still load-bearing — two tenants naming the same `corpus_name` never
  collide, by construction of the key.
- **`global_shared`** — `"rag:vector:global:" + corpus_name`, no tenant/principal component at all.
  **Must be an explicit host/operator configuration on the `CorpusSource`/mount descriptor at setup
  time** (a deployment-level decision, like `Capabilities<...>` on a `Tool`), never something a
  running session, a request, or model output can select into — the exact same "declared, not
  request-derived" rule this ADR's `Embedder`/`VectorIndex` seams already follow via CRTP policy
  tags, applied here to trust scope instead of backend selection.

Capability gating follows the scope: `per_principal`/`per_tenant` corpora are read/written through
`cap::FsRead`/`cap::FsWrite` bound to that specific derived mount — same `granted`-parameter pattern
`write_memory_item`/`read_memory_item` already use — so a principal without a grant naming THAT
mount cannot reach another principal's or another tenant's corpus, structurally, not by convention.

#### 2.6b Citation/provenance forgery defense (closes finding 4)

**Generalize, don't duplicate**, `memory_provider.hpp`'s existing fix rather than writing a second
one: `neutralize_forged_memory_labels()` (`memory_provider.hpp:95-113`) is a marker-neutralization
technique, not something inherently Memory-specific — its actual job is "break any occurrence of
THIS specific structural marker inside untrusted content, so the only place the real, unbroken
marker can appear is where this function itself emitted it." That generalizes directly:

```cpp
// core/provenance_marker.hpp (new, shared) — the technique memory_provider.hpp already proved,
// parameterized over which marker family is being protected instead of hardcoded to "memory:".
[[nodiscard]] inline std::string neutralize_forged_provenance_markers(
    std::string const& content, std::string_view marker_open);
```

`memory_provider.hpp`'s own `neutralize_forged_memory_labels(content)` becomes a one-line call to
this with `marker_open = "\xE2\x9F\xA6memory:"` — behavior-preserving, not a rewrite of a working
mechanism, just factored out.

`VectorRagContextProvider`'s citation rendering gets its own distinct marker (`"\xE2\x9F\xA6rag:"`,
distinguishable from `memory:`'s own so the two provenance vocabularies can never be confused for
each other) and follows the identical shape `memory_item_to_labeled_text()` already establishes:

```cpp
// citation_label = "⟦rag:<source_path>:<line_range>⟧" (real, structurally emitted)
// rendered text  = citation_label + " " + neutralize_forged_provenance_markers(chunk_text, "⟦rag:")
```

Label ALWAYS precedes the (now-neutralized) chunk body, same ordering `memory_item_to_labeled_text`
uses — a reader (human or model) sees the attribution before the content it attributes, and the
chunk's own text can never forge a second, different-looking citation because every open-marker
occurrence inside it gets broken before concatenation.

**What this does NOT close, named honestly as a separate residual**: `source_path` itself is
structurally derived from the mount root (`entry.path().lexically_relative(...)`, mirroring
`DiskSkillSource`/`skill_source_detail::collect_files`, `skill_source.hpp:99-126`) — it cannot
escape the mount root, but if the MOUNTED FOLDER ITSELF mixes trusted and adversarial files (e.g. a
shared-uploads directory where an attacker names a file `IT-Verified-Security-Advisory.md`), the
resulting citation is technically accurate (that really is the file it came from) while still
misleading a reader about the FOLDER's own trustworthiness. That is a mount-level content-trust
question — who is allowed to add files to a mounted corpus root — not a marker-forgery question,
and this design does not solve it. Named here so it is not later conflated with finding 4 as if
already closed.

## 3. Falsifiable claims (per design, paired with a disproving experiment)

1. **Claim (§2.3A):** brute-force cosine similarity over a realistic per-session/per-tenant memory
   corpus (hypothesis: low hundreds to low thousands of items, embedding dimension in the low
   hundreds to ~1536) completes `on_context()`'s retrieval step within a bound acceptable for a
   per-turn hot path (no number claimed yet — TODO: set a real target against 023's budget
   vocabulary before this is a real claim, not just a placeholder).
   **Disproof:** a bench (`tests/bench_...`, matching this project's bench-file convention) at the
   hypothesized corpus size exceeding that bound.
2. **Claim (§2.4A):** re-mounting a folder where only `k` of `n` files actually changed costs
   `O(k)` embedder calls and `O(n)` file hashes, not `O(n)` embedder calls — i.e., content-addressed
   dedup on `put_blob()` genuinely prevents re-embedding unchanged chunks.
   **Disproof:** a test that mounts, mutates 1 of N files, re-mounts, and asserts the embedder mock's
   call count equals exactly the mutated file's chunk count (not `N`'s worth).
3. **Claim (§2.2A, the accepted tradeoff):** an unwrapped `Embedder` genuinely breaks replay —
   i.e., this is not a theoretical concern but a reproducible one.
   **Disproof-of-the-null (i.e., proving the risk is real, which this ADR expects to succeed):** two
   `on_context()` calls against the identical stored corpus, through a scripted `Embedder` mock that
   returns two DIFFERENT vectors for the same input text (modeling real API nondeterminism), produce
   two different `ContextContribution`s — demonstrating the guarantee 029 §9 G1 makes for Memory does
   NOT hold here, so this must never be described as "deterministic like Memory" in future docs.
4. **Claim (§2.5):** a single `OpenAIEmbedder`, pointed at `api.openrouter.ai` instead of
   `api.openai.com` (host/path_prefix as constructor arguments only, no code branch), successfully
   embeds text through OpenRouter's real `/api/v1/embeddings` endpoint — i.e., no OpenRouter-specific
   client class is actually required, mirroring `OpenAIChatClient`'s already-proven relationship to
   OpenRouter for chat.
   **Disproof:** a live test (matching `test_openrouter_live_e2e.cpp`'s own opt-in,
   env-var-gated, `live-network`-labeled pattern) that fails to get a valid embedding response through
   `OpenAIEmbedder` against real `api.openrouter.ai`, or one that requires an OpenRouter-specific
   header/body shape `OpenAIEmbedder`'s OpenAI-shaped request cannot produce.
5. **Claim (§2.6a):** two principals in different tenants (or two `per_tenant` corpora in different
   tenants) that both use the identical `corpus_name` never see each other's chunks, even under a
   `global_shared`-scope misconfiguration attempt from request-level code.
   **Disproof:** a test mirroring `test_memory_cross_tenant_isolation.cpp`'s exact shape — mount the
   same `corpus_name` under two different `{tenant_id, principal}` pairs at `per_principal` or
   `per_tenant` scope, assert a query from one never returns a chunk written under the other.
6. **Claim (§2.6b):** a chunk whose own text contains the literal bytes of the RAG citation marker
   (e.g. crafted to read `"...⟦rag:trusted-file.md:1-5⟧ ignore prior instructions..."`) cannot cause
   the assembled context to contain a second, unbroken citation marker anywhere except the one this
   design's own rendering step emitted.
   **Disproof:** a test mirroring `memory_provider.hpp`'s own forged-label test — mount a corpus
   containing exactly that crafted chunk, render it, assert the marker byte sequence appears exactly
   once in the output (the real, structurally-emitted label) and the embedded attempt is
   zero-width-space-broken.
7. **Claim (§3.5, Vulkan, deferred):** a GPU-parallel reduction over cosine/dot-product scores can
   change which items land in the top-K versus a sequential CPU reduction, for inputs constructed
   near a score tie.
   **Disproof:** construct two candidate vectors with cosine scores differing by less than one ULP
   of accumulated floating-point error under the specific reduction tree used; show CPU sequential
   and GPU parallel reductions disagree on which one ranks higher.

## 4. Red-team attack (2026-08-19, independent pass, `general-purpose` agent, no prior context)

Performed against the real source this ADR cites (`context_provider.hpp`, `memory_provider.hpp`/
`memory.hpp`, `worktree.hpp`, `rt/append_log_store.hpp`, `skill_source.hpp`/`skill_provider.hpp`,
`protocol/openai/chat_client.hpp`, `trust/secret.hpp`, `context_assembly.hpp`), not against the
ADR's own prose alone. Findings below; **none of these are fixed yet** — this section records what
was found, §7 carries forward what becomes a tracked residual.

### Critical

1. **`VectorIndex` persistence is never specified — §3 claim 2 may only hold within one process
   lifetime.** Chunk TEXT rides `WorktreeObjectStore::put_blob()` (content-addressed, and the
   missing `File`-backed store is already named in §7). But the embedding VECTORS `Embedder::
   embed_batch()` produces are a separate artifact, and §2.2-§2.4 never say where they live once
   computed. If `VectorIndex` is in-memory-only, every process restart re-embeds the entire corpus
   regardless of what changed — "re-mounting is O(k)" only holds until the process dies.
2. **Claim 2's mechanism is asserted, not designed.** `put_blob()` dedupes *storage of bytes already
   seen* (`worktree.hpp:144-149`) — it says nothing about whether the embedder was already CALLED for
   that digest. The design never names a manifest/lookup that lets re-mount answer "does this digest
   already have a stored vector" before calling `embed_batch()` again, nor where a per-file
   `source_file_hash` from a PRIOR mount is read back from for the staleness comparison. Without that,
   claim 2's own disproof test (mutate 1 of N files, assert embedder-call-count == exactly the mutated
   file's chunk count) is not obviously satisfiable by the design as written.
3. **[Design response: §2.6a] Multi-tenancy is never mentioned — in the one codebase with a proven scar for exactly this bug.**
   `memory.hpp:64-124` documents a REAL, found-and-fixed cross-tenant leak: through Milestone 4,
   `memory_mount_id` derived from `principal.id` alone, so two principals in different tenants with
   the same id collided on one memory worktree (fixed Milestone 5 Phase I1, `test_memory_cross_
   tenant_isolation.cpp`). This ADR claims to reuse "the identical content-addressed blob/tree/ref
   model," yet never says whether a RAG corpus `Mount`/ref is derived from `Principal`/`tenant_id`
   the way `memory_mount_id` structurally is, or whether corpora are deliberately host-configured
   shared resources (plausible for "a folder of docs," but never stated). Either answer could be
   right; the ADR gives none.
4. **[Design response: §2.6b] Citation metadata has no defense against the forgery class this codebase already found and
   fixed for Memory.** `memory_provider.hpp:86-113` (gap-audit finding 17) exists specifically
   because a `ModelInferred`/tool-derived item's own CONTENT can contain marker bytes that impersonate
   a higher-trust provenance label once rendered into context — `neutralize_forged_memory_labels()`
   is the fix. ADR-063's `source_path`/`line_range` citation is exactly analogous provenance
   information rendered next to untrusted retrieved text, and corpus content (an ingested folder,
   possibly populated by someone else, or scraped) is at least as plausible an adversarial surface as
   a memory item. No rendering format or neutralization step is proposed anywhere in §2.4, not even
   as a residual.

### Real gap

5. **The named concurrent-writer residual (§7) is understated — for a shared corpus it is a
   lost-update bug, not staleness.** `commit_ref()` (`worktree.hpp`) has no compare-and-set (contrast
   `merge_branch_into_parent`, which explicitly re-reads and rejects a stale parent,
   `worktree.hpp:646-653`); `memory.hpp:288-297` accepts this only because memory has exactly one
   writer per principal by construction. If a RAG corpus is genuinely shared (finding 3), two
   concurrent re-mounts (or a re-mount racing an initial mount) can both read the same base tree,
   build divergent trees, and the second `commit_ref` silently discards the first writer's entire
   batch of new/changed chunks.
6. **`assemble_context()`'s drop policy (oldest-first by array index) likely drops the BEST RAG
   result under budget pressure, not the worst.** `context_assembly.hpp:169-175` drops
   `msgs[drop_from]` starting at index 0, matching `Window<N>`'s "keep the last N verbatim" direction.
   `MemoryProvider::on_context` already emits messages best-match-first (descending relevance,
   `memory_provider.hpp:262-271`), so this mismatch already exists there, just masked by a small
   default (3 items, short text). `VectorRagContextProvider` will typically inject more, larger
   chunks, making budget-triggered drops far more likely to actually fire — and the current rule
   drops the MOST relevant chunk first, keeps the least relevant. Not addressed anywhere in this ADR
   or in `assemble_context()` itself.
7. **No tie-break defined for `BruteForceCosineIndex`'s top-K on equal/near-equal scores.** Real
   corpora have duplicated boilerplate/license headers/repeated snippets. `rank_memory_items`
   (`memory_provider.hpp:219-226`) is explicit and deliberate here (score desc, then `write_seq` desc,
   "a genuine total order"); §2.3 proposes no equivalent. Compounded by unspecified chunk-insertion
   order if `DiskCorpusSource` mirrors `DiskSkillSource::load_skills`'s own unsorted
   `recursive_directory_iterator` (`skill_source.hpp:152`) — retrieval over a FIXED, already-embedded
   corpus could return different top-K sets across runs/platforms, independent of and not covered by
   the `Embedder`-nondeterminism tradeoff already named in claim 3.
8. **`embed_batch()` partial-failure semantics are undefined.** One `task<result<vector<vector<
   float>>>>` for the whole batch has no way to express "3 of 10 succeeded." Whether one bad chunk
   aborts the whole re-mount (matching `DiskSkillSource`'s explicit all-or-nothing stance,
   `skill_source.hpp:132-134`) or is silently skipped (leaving the index silently incomplete) is
   undecided.
9. **No sub-batching story when ingestion exceeds `EmbedderCapabilities.max_batch_size`.** Mount-time
   ingestion of a large folder will routinely exceed any single provider batch cap; nothing says
   whether the mount-logic or each `Embedder` conformer owns splitting the request.
10. **Unmount lifecycle is entirely absent**, despite "folder-source-mount" being this ADR's own
    framing for §2.4 — citation metadata, persisted vector state, and any in-flight `recall()` grant
    on a later-unmounted corpus are not addressed even as a residual.

### Minor

11. Two external protocol claims in an earlier draft bypassed this project's own citation rule
    (CLAUDE.md: external claims need `docs/research/<date>-<topic>.md`) — **fixed** in this pass:
    split into `docs/research/2026-08-19-embedding-provider-landscape.md`, §2.5 now cites it instead
    of asserting inline.
12. Claim 1 (§3) is honestly self-flagged as incomplete but is worth restating plainly: it is not
    yet a falsifiable claim per `decisions/README.md`'s own bar — no latency bound is named, so
    nothing a bench could currently disprove.
13. `DiskCorpusSource`'s inherited symlink/traversal posture is unexamined — `DiskSkillSource::
    collect_files` (`skill_source.hpp:99-126`) has no explicit symlink policy under
    `recursive_directory_iterator`. Not a risk this ADR introduces, but inherited without comment,
    and folder corpora are plausibly pointed at less-curated locations than a skill bundle directory.

### What held up under the pass

`Embedder` credential resolution is genuinely sound and factually accurate against real code:
`SecretStore::resolve()` requires a granted `cap::Secret` checked via `EffectContext::capabilities`
at the point of use (`secret.hpp:255-265`), exactly matching `OpenAIChatClient::chat()`
(`chat_client.hpp:940`) — "never at construction" is correct. The copy-at-mount-vs-live-pointer
argument (§2.4) is well-reasoned, and its "silent wrong-content" failure mode for live-pointer is
realistic given how `mount_read`/blob addressing actually works. Every concrete factual claim about
`OpenAIChatClient` (`http_referer_`/`x_title_`, no existing `OpenRouterChatClient`,
`test_openrouter_live_e2e.cpp`) checks out verbatim. The one-`ContextProvider`-class-per-kind
decision (§2.1b) is consistent with the real `MemoryProvider`/`SkillsProvider` precedent, not merely
asserted.

## 5. Executed evidence — real code, two passes (2026-08-19)

**First pass — foundation layer. Implemented, compiled (MSVC 19.4x, clean, no warnings), and
passing:**

- `core/provenance_marker.hpp` — `neutralize_forged_provenance_markers(content, marker_open)`, the
  generalized §2.6b mechanism. `memory_provider.hpp::neutralize_forged_memory_labels()` is now a
  one-line call to it (behavior-preserving refactor, not a rewrite).
  `tests/test_provenance_marker.cpp`: proves the mechanism against the ADR's own `⟦rag:` marker
  family (a hostile chunk embedding a forged `⟦rag:trusted-file.md:1-5⟧` marker is neutralized to
  zero unbroken occurrences; the real, structurally-prepended label survives as the only unbroken
  occurrence in the assembled text); proves the `rag:` and `memory:` marker families never collide
  with each other; re-runs the pre-existing forged-memory-label scenario directly against the
  refactored function as a regression check. All existing memory tests
  (`test_memory_provider`, `test_memory_no_authority_laundering`, `test_memory_cross_tenant_
  isolation`, `test_memory_worktree`) re-run after the refactor — still green, no regression.
- `core/embedder.hpp` — `EmbedderCapabilities` + the `Embedder` concept (§2.2A/B), compiles; no
  conformer exists yet (no `OpenAIEmbedder`), so this is the shape only, not yet exercised by a real
  backend.
- `core/vector_index.hpp` — `VectorIndex` concept + `BruteForceCosineIndex` (§2.3A), including the
  score-desc/id-asc tie-break that closes §4 finding 7. `tests/test_vector_index.cpp`: cosine
  ranking correctness (identical/orthogonal/opposite-direction vectors rank as expected), `k`
  truncation, contract rejection on length-mismatched and duplicate-id batches, and the tie-break
  itself (three byte-identical vectors resolve to a deterministic id-ascending order, stable across
  repeated `search()` calls on the same index).
  **A real bug was found and fixed during this pass, not merely asserted correct**: the first
  `add_batch()` implementation checked a candidate id only against already-stored entries
  (`entries_.contains(id)`), which never catches two duplicate ids arriving in the SAME call (none
  of a fresh batch's own ids are in `entries_` yet) — the duplicate-id contract test caught this
  immediately (`FAIL` on first run); fixed with a local `unordered_set` tracking ids seen within the
  current call, re-verified green.
- `core/corpus_scope.hpp` — `corpus_scope {per_principal, per_tenant, global_shared}` +
  `rag_corpus_key()`/`rag_corpus_ref_name()`/`rag_corpus_mount_id()`/`rag_corpus_mount()` (§2.6a).
  `tests/test_corpus_scope.cpp`, mirroring `test_memory_cross_tenant_isolation.cpp`'s shape: proves
  `per_principal` isolates both across tenants (the same-id-different-tenant case Memory's own Phase
  I1 fixed) and across principals within a tenant; proves `per_tenant` deliberately SHARES a key
  across principals within one tenant while still isolating across tenants; proves `global_shared`
  deliberately collapses tenant/principal entirely; proves the three scopes never collide with each
  other for the same principal/corpus_name; proves `rag_corpus_mount()`'s field wiring.

**Second pass (2026-08-19, same day) — three independent agents, one shared foundation, built in
parallel, then integrated and built centrally (naming-lint-clean, `tools/naming_lint.py` OK; full
project rebuild + `ctest -LE live-network`: 193/193 green):**

- `core/corpus_chunk.hpp` — `CorpusChunkRecord{id, source_path, line_start, line_end,
  source_file_hash}` + `write_corpus_chunk_record()`/`read_corpus_chunk_record()` (O(1) point lookup
  by id, path `"chunks/<id>.json"`), the shared citation-metadata contract `CorpusSource` (writer)
  and `VectorRagContextProvider` (reader) both build against — written first, by the coordinator, so
  the three parallel agents below had a fixed, already-compiling interface to target instead of
  guessing at each other's shape. `tests/test_corpus_chunk.cpp`: round-trip through a real
  capability-gated mount, missing-id rejection, malformed-JSON rejection.
- `core/vector_index.hpp` gained `VectorIndex::contains(id) -> bool` (and
  `BruteForceCosineIndex::contains()`), added by the coordinator before dispatch — closes part of §4
  finding 2 ("a manifest/lookup that lets re-mount answer 'does this digest already have a stored
  vector'") that the concept as originally shipped had no way to satisfy; without it, `add_batch()`'s
  own duplicate-id rejection would have made a normal re-mount fail outright the moment it encountered
  an unchanged, already-indexed chunk.
- `core/corpus_source.hpp` — `ChunkingPolicy` concept + `RecursiveChunker` (§2.4b's default:
  paragraph→line→sentence→hard-word-split recursion, ~450 whitespace-delimited "tokens"/chunk, ~15%
  atom-granular overlap, both overridable — the "word, not a real subword tokenizer" approximation is
  stated honestly in-file, not hidden) and `DiskCorpusSource::mount()` (§2.4A copy-at-mount + the
  re-mount dedup mechanism §4 finding 2 named as missing: per-file whole-file-hash comparison against
  a caller-threaded `previous_file_hashes` map, `index.contains()`-gated chunk-level dedup, one
  sub-batched `embed_batch()` call for all new chunks per pass with nothing committed until every
  sub-batch succeeds — closing findings 8 (partial-failure: whole-mount-aborts, structurally enforced
  by commit ordering, not just asserted) and 9 (sub-batching) for this call site).
  `tests/test_corpus_source.cpp`, against real temp files and a scripted mock `Embedder`: **directly
  proves §3 claim 2's own disproof test** — mutate 1 of 3 files, re-mount, the mock embedder's
  texts-embedded counter rises by exactly 1, not 3; also proves two byte-identical files across
  different source paths dedup to one shared index entry (content-addressed dedup, not an error), and
  that `RecursiveChunker` actually splits with real, verified atom-granular overlap at every boundary.
- `protocol/openai/embedder.hpp` — `OpenAIEmbedder<Store>` (§2.5), mirroring `OpenAIChatClient`'s
  constructor/credential-resolution shape exactly (`SecretStore::resolve()` inside `embed_batch()`,
  never at construction), reusing `chat_client.hpp`'s own `decoded_response_body()`/
  `map_http_status_error()` rather than duplicating the chunked-transfer decode logic a second time.
  POSTs `{model, input:[...]}` to `<path_prefix>/embeddings`; parses `{"data":[{"index":N,
  "embedding":[...]}]}` placing each vector by its own `index` field (never assumed array order — a
  wrong assumption here would silently mis-align a chunk's text with a different chunk's vector).
  Rejects a batch exceeding a declared, nonzero `max_batch_size` before touching the network or a
  credential (closes finding 9 for this conformer's own call site — fails closed rather than
  guessing how to sub-batch, leaving that to the caller). No `HTTP-Referer`/`X-Title` headers —
  neither OpenRouter's chat nor embeddings docs state the embeddings endpoint reads them; adding them
  would have been an unsourced assertion. `EmbedderCapabilities` is a plain constructor argument,
  never hardcoded (OpenAI's real, sourced limits — 2048 items/request, 1536/3072 dims — are cited in
  `docs/research/2026-08-19-embedding-provider-landscape.md` §3, appended this pass; OpenRouter's own
  batch limit is explicitly recorded as unconfirmed rather than guessed at ~96 from unsourced
  community chatter).
  `tests/test_openai_embedder.cpp` (offline, no network): request-body shape, response parsing
  (in-order, out-of-order-by-index, incomplete, malformed item/vector, top-level error envelope,
  missing `data`), and `embed_batch()`'s two pre-network gates (empty batch touches nothing;
  declared-limit overflow rejected before credential resolution, proven with an unpopulated
  `SecretStore` so a wrong gate-ordering would surface a different error code).
  `tests/test_openai_embedder_openrouter_live_e2e.cpp` — **§3 claim 4's own disproof test**, real,
  env-var-gated (`AGENTENGINE_OPENROUTER_API_KEY`), mirroring `test_openrouter_live_e2e.cpp`'s exact
  pattern (SKIP not FAIL when unset, a positive control with a wrong key, an I2 ungranted-capability
  control). **Since executed against real `openrouter.ai` (2026-08-19), with a real credential — all
  5 assertion groups (OR-EMB-1 through OR-EMB-5) PASS**: `embed_batch()` succeeds over real DNS/TLS
  against the vendored CA bundle for both a single text and a real 3-item batch, with each vector
  placed by the response's own `index` field (1536-dim vectors observed for
  `openai/text-embedding-3-small`); a syntactically-valid-but-wrong key is rejected by the real
  endpoint and classified `policy` (proving the successes above are not an unauthenticated endpoint
  answering anyone); an ungranted `cap::Secret` is denied at the point of use before any network
  call, even with a reachable, correctly-credentialed endpoint sitting right there (I2, the strongest
  form this assertion can take); and a second call with the same text returns a vector of the same
  length (a shape guarantee only — no byte-equality/determinism claim is made, consistent with §3
  claim 3). **Claim 4 is now CORRECT, not PENDING.**
  **A real bug was found and fixed in this pass**: the offline test's `EB-1`/`EB-2`/`EB-3` blocks
  originally wrote `auto result = run_task_sync<result<...>>(...)` — the local variable name `result`
  self-shadows the `ae::result<T>` alias template used inside its own initializer (the declarator's
  point-of-declaration is before the initializer, so `result` inside `run_task_sync<result<...>>`
  resolved to the not-yet-initialized local, not the type alias). MSVC rejected it
  (`C3536`/`C2275`/`C2678`) on the first build; fixed by renaming the local to `outcome` (matching the
  live-e2e file's own already-correct choice) in all three occurrences, rebuilt clean.
- `core/vector_rag_context_provider.hpp` — `VectorRagContextProvider<EmbedderT, IndexT, OS, RS>`
  (§2.1/§2.1b), the RAG-flavored `ContextProvider` conformer mirroring `MemoryProvider`'s SHAPE (not
  its inheritance — a genuinely different class per §2.1b): `on_context()` embeds the query via
  `Embedder::embed_batch()`, ranks via `IndexT::search()`, resolves each `ScoredId` back to its
  `CorpusChunkRecord` + blob text, and renders `citation_label + " " +
  neutralize_forged_provenance_markers(chunk_text, "⟦rag:")` exactly per §2.6b, label-before-body,
  every injected message tainted/external/`role::system` (029 §6's rule, unmodified for RAG).
  `on_turn_end()` is a genuine no-op (a RAG corpus is populated by mount-time ingestion, never written
  from a turn — unlike `MemoryProvider`'s own non-trivial extraction hook).
  `tests/test_vector_rag_context_provider.cpp` **closes the "full claim" gap for §3 claims 5 and 6
  end-to-end, not just at the mechanism level**: two separate provider instances for two different
  `{tenant_id, principal}` pairs, IDENTICAL `corpus_name`, DIFFERENT chunks under the SAME
  `source_path`/`line_range` (so citation metadata alone can't be what isolation relies on) — a query
  against tenant A's provider never returns tenant B's content and vice versa, proven at both the
  index-scoping layer and the `CorpusChunkRecord` capability-gated-read layer independently (claim 5);
  a chunk whose own text embeds a forged `⟦rag:trusted-file.md:1-5⟧` marker is rendered with the REAL
  marker appearing exactly once and the forged bytes appearing zero times (claim 6).
  **A real, previously-unnamed gap surfaced while building this file** (not in §4's original red-team
  pass — recorded honestly here, not folded into an existing finding it isn't): `ToolDescriptor::
  invoke` (`tool_pipeline.hpp`) is synchronous-only, but `recall(query)` needs `Embedder::
  embed_batch()`, a genuine `ae::rt::task<T>` coroutine, and this codebase had no sound way to drive
  `ae::rt::task<T>` to completion from a synchronous call site outside a test-only helper that
  explicitly disclaims production use. **CLOSED (2026-08-19) by `decisions/ADR-064-recall-tool-sync-
  invoke-vs-async-embedder.md`**: `rt::drive_leaf_task()` + the required `Embedder::synchronous_leaf`
  trait now let `recall`'s `invoke` genuinely drive `embedder_.embed_batch()` for any conformer that
  declares itself leaf (all 4 real conformers in the tree do, including `OpenAIEmbedder`) — a
  conformer declaring `false` still gets the original fail-closed
  `vector_rag_context_provider.recall_tool_requires_synchronous_leaf_embedder` error, unchanged in
  spirit. See ADR-064 §5/§6 for the full executed evidence.

**Still not implemented — genuinely out of scope for this pass, not silently dropped**: any
`VectorIndex` persistence across a process restart (finding 1 — `BruteForceCosineIndex` is
in-memory-only regardless of how cheap `DiskCorpusSource`'s dedup makes a re-mount within one process
lifetime), stale-chunk garbage collection when a file's content changes (a new chunk's old sibling ids
become orphaned, unreachable-by-any-current-file, but still live and retrievable in the index — named
honestly in `corpus_source.hpp`'s own top comment), the concurrent-writer lost-update on
`commit_ref()`'s missing compare-and-set (finding 5, inherited unchanged), `assemble_context()`'s
oldest-first drop-order under budget pressure (finding 6, inherited unchanged — `VectorRagContextProvider`
uses the SAME best-match-first message order `MemoryProvider` already uses, so it inherits the
identical known issue rather than introducing a new one), corpus unmount lifecycle (finding 10), a
real bench for claim 1, `GraphRagContextProvider`, and Self-RAG/Corrective-RAG's shape (both still
open per §2.1b). `WorktreeObjectStore::get_blob()` carries no capability parameter of its own
(confidentiality across corpora rests entirely on which ids a caller's OWN `IndexT` was ever populated
with, since a `search()` against one tenant's index structurally cannot return another tenant's chunk
id) — this is `corpus_chunk.hpp`'s own designed mechanism, not a gap this pass introduced, but worth
restating plainly here since it is load-bearing for claim 5's real proof.

### Red-team pass 2 (2026-08-19) — findings against the real implementation, and their fixes

A second `general-purpose` agent, again with no prior context on this document (matching §4's own
methodology), read every file from both implementation passes above plus `memory_provider.hpp` (the
structural precedent) and traced real call paths via CodeGraph rather than grepping blind. It
explicitly did not re-report anything already named in §4/§7. Findings, most severe first, all fixed
the same pass with a real, passing test proving each fix:

- **CRITICAL — `rag_corpus_key()` key collision (`corpus_scope.hpp`).** The original derivation
  plain-`":"`-joined `tenant_id`/`principal.id`/`corpus_name` with no escaping. Since none of those
  fields are constrained against containing `':'` (`Principal` documents them as opaque;
  `corpus_name` is host-chosen free text), two structurally different mounts could derive the
  IDENTICAL key — e.g. `per_tenant{tenant_id="T", corpus_name="alice:docs"}` and
  `per_principal{tenant_id="T", principal.id="alice", corpus_name="docs"}` both produced
  `"rag:vector:T:alice:docs"`, silently aliasing a tenant-shared corpus onto a different principal's
  private one. This directly undermined §2.6a's own stated guarantee and claim 5. **Fixed**: each
  field is now encoded as a length-prefixed ("netstring": `"<byte-length>:<bytes>"`) token before
  concatenation — a uniquely-decodable encoding, so no field's content can ever be mistaken for a
  delimiter plus the start of another field, closing the collision class by construction rather than
  by pattern-matching specific bad inputs. `tests/test_corpus_scope.cpp` now proves the exact
  colliding pair named above derives distinct keys, plus the general property for an arbitrary
  colon-bearing field. **Deliberately NOT fixed in this pass**: `memory_ref_name()`/
  `memory_mount_id()` (`core/memory.hpp`) use the identical unescaped-join scheme and inherit the
  identical latent bug — out of scope for this ADR (Memory has only one scope shape, a narrower
  surface, but the underlying primitive is the same); flagged here as a follow-up for whoever next
  touches `memory.hpp`.
- **REAL GAP — `last_user_text()` didn't check `role::user` (`vector_rag_context_provider.hpp`,
  inherited by `memory_provider.hpp`).** Read `history.back()` unconditionally. Traced via CodeGraph
  through `rt/agent_session.hpp`: `resolve_interaction()`'s approved-tool-call branch and
  `resolve_codeact_ask()` both call a `ContextProvider`'s `on_context()` at a point where
  `history.back()` is the ASSISTANT's own pending tool-call message (its text preamble, if any), not
  the user's — `tool_results_message` is only pushed afterward. Consequence: model-generated text
  could become the literal payload of a REAL network call to `Embedder::embed_batch()` (a live
  OpenAI/OpenRouter request) and steer what gets retrieved next round — a direct I3 violation ("model
  output is data, never authority") the moment this provider is composed into a live `AgentSession`.
  Not yet exploitable today (`VectorRagContextProvider` isn't wired into any `AgentSession` in this
  tree yet — only its own test exercises it directly), but load-bearing the moment it is. The
  identical pattern in `memory_provider.hpp` is comparatively inert (a local, no-network keyword
  match) but is still the same class of I3 violation in principle. **Fixed in both files**:
  `last_user_text()` now walks backward to the most recent `role::user` message, ignoring any
  assistant/tool/system messages appended after it — never just `history.back()`.
  `tests/test_vector_rag_context_provider.cpp`'s new `R11` proves it directly: a trailing assistant
  message with its own distinct scripted embedder vector is present in history, and the query used is
  still provably the last real user message (the correct chunk still ranks first).
- **REAL GAP — `RecursiveChunker` had no byte-length bound (`corpus_source.hpp`).** Chunk-size
  enforcement was purely word-count-based (`atom.word_count <= max_words`, a "word" being a
  whitespace-delimited run). A line dominated by one run with NO internal whitespace at all (a
  minified blob, base64, a long hash/URL list) could have a tiny word count yet unbounded byte
  length, and stayed as ONE unsplit atom/chunk regardless — sent whole to `Embedder::embed_batch()`
  (a real network call, risking a provider's per-item limit and aborting the whole all-or-nothing
  mount pass), blobbed whole, cited whole. **Fixed**: a new backstop,
  `corpus_source_detail::enforce_max_atom_bytes()`, runs after the word-based recursive split and
  hard-splits any atom exceeding `max_atom_bytes` (new `RecursiveChunker` constructor parameter,
  default 16 KiB) into fixed-size byte pieces, each tagged with a sentinel word-count so
  `merge_atoms_into_chunks` can never re-merge two already-maximal pieces into one oversized output
  chunk. `tests/test_corpus_source.cpp`'s new test (e) proves a pathological no-whitespace line is
  split, every resulting chunk stays within the cap, the split is lossless, and `max_atom_bytes=0`
  genuinely disables the backstop (confirming it — not some unrelated effect — is what changed
  behavior).
- **MINOR — `VectorIndex`/`BruteForceCosineIndex` never validated vector dimensionality
  (`vector_index.hpp`).** `add_batch()` only checked `ids.size() == vectors.size()`, never that every
  vector shared one width; `search()`'s `cosine_similarity()` silently used `std::min(a.size(),
  b.size())`, so a dimension mismatch (e.g. an `Embedder` model/config change between mounts) would
  compare a truncated, semantically meaningless prefix and return an ordinary-looking but wrong
  score — a "silently wrong answer" this codebase's reject-not-coerce convention otherwise exists to
  prevent. **Fixed**: `add_batch()` establishes the index's dimensionality from its first stored
  vector and rejects any subsequent (or same-call) vector of a different width; `search()` rejects a
  mismatched-width query the same way. `tests/test_vector_index.cpp` proves both gates, including the
  within-one-call disagreement case.
- **MINOR — `corpus_chunk_record_from_json()` silently defaulted `line_start`/`line_end` to 0 on a
  missing/non-numeric field (`corpus_chunk.hpp`).** Inconsistent with the same function's hard-reject
  handling of `id`/`source_path`/`source_file_hash`. **Fixed**: both fields now go through the same
  `require_*`-shaped reject-not-coerce helper as the string fields. `tests/test_corpus_chunk.cpp`
  proves both a missing and a wrong-JSON-type case are rejected, not coerced.
- **Precision addendum to an already-named §7 residual, not a new finding**: the commit phase in
  `DiskCorpusSource::mount()` originally called `index.add_batch()` BEFORE
  `put_blob()`/`write_corpus_chunk_record()` within a sub-batch. Since a future re-mount's only
  "already handled" signal is `index.contains()`, a later blob/record write failing after an earlier
  `add_batch()` succeeded could leave a PERMANENT ghost entry (indexed, but no backing blob/record,
  un-healable by any future re-mount) rather than merely "not rolled back this once," as the file's
  top comment previously stated. **Fixed**: reordered so blob + record commit FIRST, `add_batch()`
  LAST, per sub-batch — a failure partway through blob/record writes now leaves those ids still
  `index.contains()`-false, so the next mount simply retries them (idempotent). This does not make
  the phase transactional (an earlier sub-batch's writes still aren't rolled back if a later sub-batch
  fails, per the file's own top comment, unchanged); it converts one specific failure mode from
  "permanent ghost" to "harmless retry."

Everything found in this pass and listed above is now fixed and tested; nothing from this pass
remains open. The full local suite is still 193/193 green after these fixes
(`ctest -LE live-network`), zero regressions.

### Closing claims 1 and 3 (2026-08-19)

Two small, targeted additions, neither requiring new production code beyond what already existed:

- **`tests/test_vector_index_benchmark.cpp`** (new) — closes claim 1's own disproof
  (`BruteForceCosineIndex::search()` latency at the hypothesized corpus size). Mirrors
  `test_capability_token_benchmark.cpp`'s own bench-file shape (single-threaded, bounded iteration
  count, warm-up before timing, machine-safety comment per CLAUDE.md). Measured at dim=1536
  (text-embedding-3-small's real dimension) across n=200/1000/5000. First measured under this
  session's Debug/MSVC build and found to be meaningless (`~5 ms/call` even at n=200 — an `/Od`
  artifact); rebuilt and re-measured under the repo's existing `build-clang-release` (Release, clang)
  configuration for real numbers, reported in §6.
- **`tests/test_vector_rag_context_provider.cpp`** (new block, added to the existing file) — closes
  claim 3's own disproof exactly as §3 specifies: a new `AlternatingEmbedder` mock alternates between
  two distinct vectors by CALL COUNT (not by input text), so the identical query text legitimately
  embeds differently across two calls, modeling real provider nondeterminism. Two `on_context()`
  calls against the identical stored corpus and identical history retrieve different chunks, proven
  by an unambiguous proxy (each chunk's own stored vector is an exact match for one of the two
  scripted embedder outputs). Kept deliberately distinct from `R10` (embedder FAILURE propagation) —
  neither test can be cited as proving the other's property.

Both proven against the existing 193/193-green suite; full suite re-run after these additions still
green, zero regressions (see below).

## 6. Per-claim verdicts — partial, several now real

Per `decisions/README.md`'s CORRECT / WRONG / INCONCLUSIVE bar. Only claims with real, executed
evidence get a verdict; the rest stay PENDING, not guessed at.

- **Claim 2 (§2.4A, re-mount is O(k) not O(n)) — CORRECT.** `tests/test_corpus_source.cpp`'s own
  disproof test executed exactly as §3 specifies: 1 of 3 files mutated, re-mounted, the mock
  embedder's call count rose by exactly the mutated file's own chunk count (1), not the whole corpus's
  (3+). The mechanism (`index.contains()` + per-file `source_file_hash` comparison) is real, not
  asserted.
- **Claim 5 (§2.6a, multi-tenant isolation) — CORRECT, end-to-end, superseding the earlier
  mechanism-only verdict.** `tests/test_vector_rag_context_provider.cpp`'s `C5-R1`–`C5-R5` run the
  ADR's own disproof scenario for real: two providers, two tenants, identical `corpus_name`, and a
  query against one never returns the other's content — proven at two independent layers (index
  scoping, capability-gated record reads), not merely the key-derivation level `test_corpus_scope.cpp`
  proved earlier (that result still stands as a narrower, independently-useful proof of the
  underlying mechanism, not superseded, just no longer the ONLY evidence for claim 5).
- **Claim 6 (§2.6b, citation-marker forgery) — CORRECT, end-to-end, superseding the earlier
  mechanism-only verdict.** `tests/test_vector_rag_context_provider.cpp`'s `C6-R1`–`C6-R3` run the
  ADR's own disproof scenario for real through `VectorRagContextProvider::on_context()`'s actual
  rendering path: a chunk with an embedded forged `⟦rag:...⟧` marker renders with the real marker
  exactly once, the forged bytes zero times.
- **§4 finding 7 (top-K tie-break) — CORRECT, closed at the implementation level** (unchanged from the
  prior pass): `BruteForceCosineIndex`'s deterministic score-desc/id-asc tie-break.
- **Claim 4 (§2.5, OpenAIEmbedder reaches real OpenRouter) — CORRECT, executed for real.** The
  OFFLINE-provable half of this claim (request/response shape, the two synchronous pre-network gates)
  is real and green (`test_openai_embedder.cpp`). **The live disproof test
  (`test_openai_embedder_openrouter_live_e2e.cpp`) has now actually been RUN (2026-08-19) against real
  `openrouter.ai` with a real credential, and all 5 assertion groups pass**: real DNS/TLS against the
  vendored CA bundle, a real single-text and a real 3-item-batch embed, index-based response placement
  (not array-order assumption), a wrong-key positive control correctly rejected and classified
  `policy`, an I2 ungranted-capability control denied before any network call, and a same-input
  same-length shape guarantee across two calls. Full transcript in §5's Second-pass evidence.
- **Claim 3 (§2.2A, unwrapped Embedder breaks replay) — CORRECT, executed
  (2026-08-19).** `tests/test_vector_rag_context_provider.cpp`'s new claim-3 block runs exactly the
  scenario §3 names: a scripted `AlternatingEmbedder` returns two DIFFERENT vectors for the SAME
  query text on two successive calls; two `on_context()` calls against the IDENTICAL stored corpus
  and IDENTICAL history produce two DIFFERENT `ContextContribution`s (chunk A ranks first on call 1,
  chunk B on call 2), solely because the embedding itself changed. This is distinct from `R10` (which
  proves an embedder FAILURE propagates, not a determinism break) — do not conflate the two, they
  were kept as separate tests specifically so neither could be cited as proving the other.
- **Claim 1 (§2.3A, brute-force latency) — evidence now exists; verdict is INCONCLUSIVE, not
  CORRECT, pending a real budget target.** `tests/test_vector_index_benchmark.cpp` (new, 2026-08-19)
  measures `BruteForceCosineIndex::search(k=5)` at dim=1536 (text-embedding-3-small's real
  dimension) across the hypothesized corpus-size range. Numbers under a Debug/MSVC build are not
  representative (`/Od`, ~5 ms/call even at n=200 — an artifact of no optimization, not of the
  algorithm); rebuilt and measured under `build-clang-release` (Release, clang) for real numbers:
  **n=200 → ~248 µs/call, n=1000 → ~1.45 ms/call, n=5000 → ~7.6 ms/call** (single-threaded, one
  reference machine, not yet a 023 §7 G1-baselined reference machine — a caveat worth restating, not
  a reason to withhold the number). RFC `023-Performance-Targets-and-Budgets.md` names no dedicated
  budget row for vector-index search specifically; the closest existing analog, "Context assembly:
  assemble a 50-message context, p99 ≤ 500 µs (Goal)," is already exceeded at n=1000 and clearly
  exceeded at n=5000 — both comfortably inside claim 1's own hypothesized "low hundreds to low
  thousands" range. This is a real, honest finding, not a fabricated pass/fail: `BruteForceCosineIndex`
  itself already says "correct by construction, not tuned" (`vector_index.hpp`'s own comment) and
  §2.3B already names ANN/GPU as opt-in seam backends "gated on a real bench against this default
  proving it is the actual bottleneck" — this bench is that proof, earlier than "if it turns out to
  be a bottleneck" implied. **Recommended follow-up, out of this ADR's own scope to do unilaterally**:
  a dedicated budget row for RAG/vector-index retrieval in RFC 023 §3, and revisiting whether the
  ANN backend gate should move up the roadmap rather than stay speculative.
- **Claim 7 (Vulkan) is explicitly deferred per §2.3B** — not a gap, a deliberate scope decision.

## 7. The decision — not yet made; this is the proposal awaiting judgment

**What this document commits to, pending the real loop:** if/when this is built, it should be built
as §2 describes — a `ContextProvider` conformer, a `ChatClient`-shaped batch-only `Embedder` with the
determinism tradeoff explicitly documented at the call site (not just here), a `core/`-resident
brute-force default index with ANN/GPU as later opt-in seam backends gated on a real bench, and
copy-at-mount folder retrieval with citation metadata rather than live re-reads.

**Residual risks, named now so they are not rediscovered later:**
- **(from §4 red-team, still unresolved) Vector-index persistence is undesigned** — a real gap, not a
  detail: `BruteForceCosineIndex` is in-memory-only, so `DiskCorpusSource`'s re-mount dedup (below)
  only makes a re-mount cheap WITHIN one process lifetime; a fresh process starts with an empty index
  regardless of what the caller's `previous_file_hashes` map claims.
- **(from §4 red-team, IMPLEMENTED 2026-08-19, §3 claim 2 CORRECT) The re-mount dedup mechanism
  (§2.4A) is now real, not asserted** — `DiskCorpusSource::mount()` (`core/corpus_source.hpp`)
  compares each file's whole-content hash against a caller-threaded `previous_file_hashes` map
  (unchanged files skip chunking/embedding entirely) and checks `VectorIndex::contains()` per chunk
  before embedding (content-addressed dedup across files, not an error). **Still open**: no stale-chunk
  garbage collection — when a file's content changes, its OLD chunk ids become orphaned, still fully
  live and retrievable in the index, never removed (named honestly in `corpus_source.hpp`'s own
  top comment; needs either a real `VectorIndex::remove()`, not in the concept today, or a
  mark-and-sweep pass keyed off `CorpusChunkRecord::source_path`).
- **(from §4 red-team, DESIGNED §2.6a AND IMPLEMENTED 2026-08-19, §3 claim 5 CORRECT end-to-end)
  Multi-tenant scoping** — `corpus_scope {per_principal, per_tenant, global_shared}`, host-declared,
  key derivation ORIGINALLY mirrored `memory_ref_name`/`memory_mount_id` exactly (`core/
  corpus_scope.hpp`), proven both at the key-derivation level (`test_corpus_scope.cpp`) AND
  end-to-end through a real `VectorRagContextProvider` (`test_vector_rag_context_provider.cpp`'s
  `C5-R1`–`C5-R5`). **Red-team pass 2 (2026-08-19) found that "mirrors memory exactly" scheme
  collision-prone**: a `tenant_id`/`principal.id`/`corpus_name` containing `':'` could alias two
  structurally different mounts onto the identical key. **Fixed same pass** — see the "Red-team pass
  2" subsection in §5 for the exact colliding pair and the length-prefixed-encoding fix; claim 5's
  CORRECT verdict above already reflects the fixed code. `memory_ref_name`/`memory_mount_id`
  themselves still carry the identical unfixed primitive — a follow-up for whoever next touches
  `memory.hpp`, out of this ADR's scope.
- **(from §4 red-team, DESIGNED §2.6b AND IMPLEMENTED 2026-08-19, §3 claim 6 CORRECT end-to-end)
  Citation forgery defense** — generalizes `neutralize_forged_memory_labels()` into a shared
  `neutralize_forged_provenance_markers()`, a distinct `rag:` marker family, same
  label-before-content ordering, now rendered for real by `VectorRagContextProvider::on_context()`
  and proven end-to-end (`C6-R1`–`C6-R3`). **Still open, NOT closed by this**: mount-level content
  trust (who may add files to a mounted corpus root) — named explicitly in §2.6b as a separate
  question, unaddressed by any code written so far.
- **(NEW, found during implementation 2026-08-19, not in the original §4 red-team pass — CLOSED
  2026-08-19)** `recall(query)`'s sync-invoke/async-embedder mismatch. `ToolDescriptor::invoke`
  (`core/tool_pipeline.hpp`) is synchronous-only; `VectorRagContextProvider`'s `recall(query)` needed
  `Embedder::embed_batch()`, a genuine `ae::rt::task<T>` coroutine with no sound synchronous
  "drive to completion" path anywhere in this codebase outside a test-only helper that explicitly
  disclaims production use. **Resolved by `decisions/ADR-064-recall-tool-sync-invoke-vs-async-
  embedder.md`, Design B**: a narrow, opt-in `rt::drive_leaf_task<T>()` plus a required
  `Embedder::synchronous_leaf` declaration a conformer must actively assert before `recall`'s `invoke`
  will drive it — NOT a project-wide `ToolDescriptor::invoke` signature change. `recall` is now
  genuinely invocable end-to-end for any `synchronous_leaf = true` conformer (all 4 real conformers in
  the tree, including `OpenAIEmbedder`); a conformer declaring `false` still fails closed with the
  original error, unchanged. Real, executed evidence in ADR-064 §5/§6, including a new
  `tests/test_rt_drive_leaf_task.cpp` and real end-to-end recall coverage.
- **(from §4 red-team) The concurrent-writer race, now more precisely understood as a lost-update on
  `commit_ref()`'s missing compare-and-set** (not mere staleness) for any corpus with more than one
  writer — worse than Memory's own accepted single-writer assumption if RAG corpora turn out to be
  shared (see the multi-tenancy point above; these two residuals interact). Still inherited, not
  solved, by `DiskCorpusSource::mount()`.
- **(from §4 red-team) `assemble_context()`'s oldest-first drop policy likely discards the BEST RAG
  result under budget pressure**, not the worst — a real interaction bug between this ADR's design
  and existing, unmodified `context_assembly.hpp` logic, not fixable by `VectorRagContextProvider`
  alone.
- **(from §4 red-team, IMPLEMENTED 2026-08-19) Tie-break for equal/near-equal top-K scores is now
  closed** — `BruteForceCosineIndex::search()` (core/vector_index.hpp) sorts score desc, then id
  asc, a genuine deterministic total order (see §5/§6). **Still open**: retrieval order may depend on
  unspecified filesystem directory-iteration order at MOUNT time (which ids get assigned to which
  chunks in the first place) — `DiskCorpusSource::mount()` inherits `recursive_directory_iterator`'s
  own unspecified order (mirroring `DiskSkillSource`'s identical, unexamined posture), a separate,
  still-unresolved determinism gap independent of the index's own tie-break.
- **(from §4 red-team, PARTIALLY CLOSED 2026-08-19) `embed_batch()` partial-failure and
  `max_batch_size` overflow are now designed and implemented for `DiskCorpusSource`'s own call site**
  (whole-mount-aborts, structurally enforced by commit ordering; sub-batching respects the declared
  cap) **and independently for `OpenAIEmbedder`'s own call site** (fails closed on a batch exceeding
  a declared limit, before any network attempt). Red-team pass 2 (2026-08-19) sharpened one detail:
  the commit phase originally called `index.add_batch()` BEFORE `put_blob()`/
  `write_corpus_chunk_record()`, so a blob/record failure after `add_batch()` succeeded left a
  PERMANENT ghost index entry (un-healable by any future re-mount, since `index.contains()` would
  already read true). **Fixed** — commit order reversed (blob + record first, `add_batch()` last),
  converting that one failure mode to a harmless retry on the next mount. **Corpus-unmount remains
  entirely undesigned** — no code written so far touches it.
- The concurrent-writer race already accepted as a residual for `memory.hpp` (§4) would apply
  equally, likely more severely, to a shared/multi-writer RAG or graph index — not solved by this
  design, only inherited.
- **Corrected 2026-08-19 (an earlier draft of this ADR overstated this gap):** `rt::AppendLogStore`
  already has a real disk-backed conformer, `FileAppendLogStore` (`rt/append_log_store.hpp:143-244`,
  one file per log id, length-prefixed records, documented torn-trailing-record recovery on crash) —
  mirroring `session_store.hpp`'s own `FileSessionStore`. **What is actually still missing is a
  disk-backed `WorktreeObjectStore`** (blob/tree persistence) — `core/worktree.hpp` today has only
  `InMemoryWorktreeObjectStore`, no `File`-prefixed sibling, unlike both other store concepts in this
  codebase. This design's "re-mount doesn't rebuild" property (§2.4, §3 claim 2) is contingent on
  THIS ONE missing piece, not on both stores — a narrower, more precisely-scoped prerequisite than
  originally stated. It is ordinary implementation work under CONVENTIONS.md/`decisions/README.md`'s
  own "ordinary implementation choices do not need an ADR" rule (mirrors an existing, twice-proven
  pattern almost directly), not something this ADR itself needs to design.
- Chunking strategy now has a research-backed default (§2.4b: recursive/structure-aware,
  ~400-512 tokens, 10-20% overlap, swappable) — still not implemented, and semantic/contextual
  chunking's "optional, eval-gated" status still needs a real eval on this project's own corpora
  before anyone turns it on.
- `GraphRagContextProvider` (§2.1b) is now named and cost-scoped (§2.4b: real per-mode pricing
  cited) but not designed — its own extraction/summarization backends would need the same
  declared-backend treatment §2.2 gives `Embedder`, as a separate follow-on ADR, not silently folded
  into this one.
- Self-RAG/Corrective-RAG's shape (§2.1b: baked into one provider's `on_context()` vs. a
  `MiddlewareModelCallGateway`-style decorator wrapping any `ContextProvider`) is an open question,
  not decided — leaning decorator for consistency with this codebase's own middleware precedent, but
  not designed here.
- **(IMPLEMENTED 2026-08-19)** `OpenAIEmbedder` (`protocol/openai/embedder.hpp`) is real, covers both
  OpenAI and OpenRouter by construction argument alone (no code branch), and its offline-provable
  surface is tested green. **(CLOSED 2026-08-19)** The live-test-style proof §3 claim 4 describes has
  now actually been RUN against real `openrouter.ai` with a real credential — all 5 assertion groups
  pass (see §5's Second-pass evidence for detail). Claim 4 is CORRECT, not PENDING.

**Judged (2026-08-19, project owner sign-off).** Real implementation of §2's chosen shapes is done
(§5/§6). A red-team pass against the NEW code ran (2026-08-19, §5's "Red-team pass 2" subsection) and
its 5 findings are fixed and tested. `recall(query)`'s sync-invoke/async-embedder gap now has an
actual, reviewed, tested fix (`decisions/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md`,
Design B, implemented, proven, and independently red-teamed a second time, also Judged 2026-08-19).
Claims 2, 3, 4, 5, 6 and finding 7 are closed with real executed evidence; claim 1 has real measured
numbers but stays INCONCLUSIVE, accepted as a residual rather than a blocker (no dedicated RFC 023
budget row exists yet to compare against — a separate, cross-cutting spec change, not this ADR's own
call to make unilaterally); claim 7 (Vulkan) is deliberately deferred, not a gap. Named residual, not
gating this sign-off: since claim 1 turned up a real, measured finding (the closest existing 023
budget analog is already exceeded at realistic corpus sizes), whether the ANN/GPU seam backend gate
(§2.3B) should move up the roadmap rather than stay speculative remains an open follow-on question.
Per `decisions/README.md`, this file would be superseded (not silently edited into a stale status),
not this "Judged" status quietly reverted, if a future pass finds this shape does not hold up.
