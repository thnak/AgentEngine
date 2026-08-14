# ADR-047 — Memory ranking becomes a real product of salience, recency, and keyword overlap

**Status:** Judged (2026-08-14, project owner sign-off). Designed, self-red-teamed, implemented, and
proven (real code + new test file, full suite green).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #18 (the finding this
ADR closes: "Default memory ranking is additive, not salience×recency×keyword"). `029-Memory-
System.md` §5 (the normative formula: "Ranking is `salience × recency × tag/keyword overlap with
the current turn`... computed host-side with no external call"). `rt/append_log_store.hpp`
(ADR-037's `AppendLogStore` concept — the real `SeqNo` primitive this ADR's recency signal is built
on).

## 1. The question

**Stated so it has a wrong answer:** does `rank_memory_items()` (`core/memory_provider.hpp`) compute
029 §5's own literal formula, a product of three signals?

**Before this fix: no, and only two of the three signals were used at all.** The formula was
`score = salience + keyword_overlap_score(item, query)` — additive, not multiplicative, and with no
recency term whatsoever. `list_memory_items()`'s own return order (a tree walk, alphabetical by
`<kind>/<id>` path) was used only as a deterministic sort tie-break, documented honestly in the
code's own comment as "recency's proxy," but it never actually participated in ranking — only in
breaking exact score ties, which an additive salience+keyword score rarely produces.

## 2. What re-grounding against current code found

- **The audit's own first-pass alternative (a full commit-history scan per item to derive recency)
  does not work with this codebase's actual worktree model.** `worktree.hpp`'s own `Ref` struct
  carries a name and a current `tree_digest` only — no parent pointer, no history chain. There is no
  "walk backward through this memory worktree's commits" operation to build on; the audit's own gap-
  18 row names this exact objection ("non-monotonic on overwrite and adds a real O(item-count)
  traversal"), and re-reading the actual data model confirms it would in fact be worse than that: not
  merely expensive, but not straightforwardly buildable at all against a Ref with no retained history.
- **A real, cheap, monotonic signal already exists and needed no new machinery**: `rt::
  AppendLogStore::last_seq(id)` (ADR-037), the SAME primitive `commit_ref()`/`mount_write()` already
  use internally for every worktree write in this project. `write_memory_item()` (`memory.hpp`) now
  predicts the SeqNo its own upcoming commit will land at (reading the current tail and adding 1,
  since `mount_write` performs exactly one `commit_ref` internally) and stamps it onto the item as
  `MemoryItem::write_seq` — an O(1) read, no history scan, no second commit to correct it afterward.

## 3. The design

`MemoryItem` gains `write_seq` (`std::uint64_t`, defaults to 0 for records written before this field
existed), serialized in `memory_item_to_json`/`memory_item_from_json`. `write_memory_item()` stamps it
from the memory ref's own append-log tail at write time (§2). `rank_memory_items()`'s formula becomes
a genuine product of three independently-reasoned factors (`memory_detail::memory_rank_score()`):

- **`salience_factor = 0.05 + item.salience`** — a small positive floor, not the raw `[0,1]` value.
- **`recency_factor = 1.0 + (item.write_seq / max_write_seq_in_this_batch)`**, bounded to `[1, 2]` by
  normalizing against the current retrieval batch's own maximum `write_seq` (0 batch max degrades to a
  neutral `1.0` for every item, e.g. an all-legacy batch with no recorded `write_seq` at all) — never
  the raw, unboundedly-growing sequence number used directly.
- **`keyword_factor = hits` when `keyword_overlap_score() > 0`, else a small positive floor (`0.1`)**
  — not the raw hit count used unconditionally as a multiplicative term.

`score = salience_factor * recency_factor * keyword_factor`. Deterministic tie-break: score
descending, then `write_seq` descending (a genuine total order, since no two items ever share a
`write_seq`) — replacing the previous "list-order index" tie-break, which had no real meaning once
`write_seq` gives a genuine recency signal to break ties with instead.

## 4. Self-red-team findings — why none of the three factors uses its raw signal directly

**A literal product over the RAW signals is actively broken, in two ways this codebase genuinely
hits, not hypothetically:**

1. **Keyword overlap is zero on the ordinary turn, not the exceptional one.** `on_context()`'s query
   text is the last user message — rarely a verbatim substring of a short stored fact. A raw
   multiplicative `0` there zeroes out EVERY item's score on most turns, collapsing 029 §5's whole
   "proactively surface salient memory" purpose into "only ever surface items on an exact substring
   hit." Verified directly: `test_memory_provider.cpp`'s own G4-R2 fixture (query "please turn on
   dark mode again" against content "the user asked to enable dark mode") produces ZERO keyword hits
   under the existing substring-direction check even though the items are obviously topically
   related — a literal-product formula would have made this exact, already-tested scenario rank
   entirely on an arbitrary tie-break instead of on salience as intended.
2. **`MemoryProvider::on_turn_end()` never sets `salience`** on an extracted item — it stays at its
   `0.0f` default. A raw multiplicative salience factor makes such an item permanently rank-zero
   forever, regardless of recency or keyword relevance. Nothing in 029 §5 or §7 (salience "decays,"
   trending toward but not necessarily reaching zero) implies a zero-salience item should be
   structurally unsurfaceable — that would be a real functional regression for every freshly-
   extracted memory, not a faithful reading of the RFC.

**An unbounded recency factor would eventually dominate the whole product regardless of relevance.**
`write_seq` grows without bound over a memory worktree's lifetime. Using it directly as a
multiplicative factor means two sufficiently-far-apart writes could differ by orders of magnitude,
letting a merely-more-recent, low-salience, no-keyword-match item beat an overwhelmingly more
salient and relevant older one purely because the store has grown since. Normalizing against the
CURRENT retrieval batch's own maximum bounds the factor to `[1, 2]` regardless of the store's
absolute age — proven directly (`R2`, below) with a 1000:1 `write_seq` gap between two items where
the older, far-more-salient one still wins.

**Neither correction was found by a failing test — both were reasoned through before writing the
formula**, unlike ADR-045's budget/UB findings (which testing caught after the fact). Named here
honestly as the same discipline applied earlier in the design step rather than the proving step.

## 5. What this ADR does not claim

- **The floor/normalization constants (`0.05`, `0.1`, the `[1,2]` recency range) are reasoned
  engineering defaults, not evidence-tuned values** — matching this RFC's own precedent elsewhere
  (029 §7 Q1: the salience decay half-life's *shape* is resolved by reasoning, its exact *parameter*
  stays "genuinely evidence-gated" pending a real usage corpus). Nothing here claims these specific
  numbers are optimal, only that the formula's STRUCTURE (a real product, correctly bounded, never
  degenerate) is sound.
- **Does not touch `keyword_overlap_score()`'s own substring-direction logic** — confirmed (§4.1)
  that it already produces the correct *relative* ordering for both call sites' real test fixtures
  (default injection AND the explicit `recall("CMake")` case), so changing its internal matching
  strategy was out of scope for this ADR; only how its OUTPUT is combined into the product changed.
- **`write_seq`'s prediction (read-then-add-1, not read-after-the-fact) assumes single-writer-per-
  memory-worktree** — true for every caller in this codebase today (one principal's memory, written
  sequentially by that principal's own turn loop), named as a residual rather than a structurally
  enforced guarantee against a future concurrent writer to the same principal's memory.

## 6. Evidence

`tests/test_memory_ranking_formula.cpp` (new file): R1 proves recency actually participates
(identical salience/keyword, higher `write_seq` scores strictly higher); R2 proves recency is
bounded, not raw (a much older, far more salient item beats a barely-relevant, 1000-seq-newer one);
R3 proves a zero-salience freshly-extracted item is not structurally rank-zero, and that a real
keyword match still measurably boosts it; a determinism check; a legacy-record control (`write_seq
== 0` with an all-zero batch produces a finite, non-NaN score, no division by zero).

`tests/test_memory_provider.cpp`'s pre-existing G4-R2/G4-R6 and `tests/test_memory_retrieval_
determinism.cpp`'s H3-R1 (unchanged assertions, re-verified against the new formula) continue to
pass — the new formula preserves every previously-tested ranking outcome while adding the
previously-absent recency term.

Full suite: green (`ctest`, this pass), zero regressions (`examples/08_memory.cpp`'s own exact-
content-match assertion was corrected alongside ADR-046's confidence-label change, in the same pass
that also touched this file — see that ADR's own evidence section).
