// Gap-audit finding 18 (docs/planning/2026-08-10-full-codebase-adr-gap-audit.md): default memory
// ranking was additive (`salience + keyword_overlap_score`), never using recency at all, contrary
// to 029 §5's own literal formula ("salience × recency × tag/keyword overlap with the current
// turn"). This file proves the real product formula (`memory_rank_score`, memory_provider.hpp)
// against the three properties a genuine product must have that the old additive one didn't:
//   R1 -- recency (MemoryItem::write_seq, a real Store SeqNo, not a full history scan) actually
//         participates in ranking, decisively when salience/keyword are tied.
//   R2 -- recency is BOUNDED (normalized against the current batch), so an old-but-far-more-
//         salient item still beats a merely-more-recent one -- the self-red-team catch that an
//         unbounded raw SeqNo factor would eventually make recency alone dominate everything.
//   R3 -- a freshly-extracted item with salience == 0.0 (MemoryProvider::on_turn_end()'s own real
//         behavior, it never sets salience) is NOT permanently rank-zero -- the other self-red-team
//         catch a literal multiplicative reading would have caused.
// Plus a determinism control (same inputs, same output) and a legacy-record control (write_seq==0,
// simulating a record written before this field existed, must not divide by zero or crash).

#include <cstdio>
#include <string>

#include "agentengine/core/memory_provider.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    using ae::MemoryItem;
    using ae::memory_kind;
    using ae::memory_source;
    namespace md = ae::memory_detail;

    // --- R1: with salience and keyword overlap held EQUAL, the more recent item (higher
    // write_seq) ranks higher -- recency actually participates, not just salience/keyword. -------
    {
        MemoryItem older{};
        older.kind = memory_kind::episodic;
        older.content = "fact A";
        older.salience = 0.3f;
        older.write_seq = 2;

        MemoryItem newer = older;
        newer.content = "fact B";
        newer.write_seq = 5;

        double const score_older = md::memory_rank_score(older, /*query_text=*/"", /*max_write_seq_in_batch=*/5);
        double const score_newer = md::memory_rank_score(newer, "", 5);
        check(score_newer > score_older,
              "R1: identical salience/keyword, higher write_seq (more recent) scores strictly "
              "higher -- recency is a real, participating factor, not vestigial");
    }

    // --- R2: recency is BOUNDED -- an old item with much higher salience still beats a merely-
    // more-recent, low-salience item. An UNBOUNDED raw-SeqNo factor (the first-draft mistake this
    // ADR's self-red-team caught) would eventually let recency alone win regardless of salience. --
    {
        MemoryItem important_but_old{};
        important_but_old.kind = memory_kind::semantic;
        important_but_old.content = "critical fact";
        important_but_old.salience = 0.95f;
        important_but_old.write_seq = 1;  // oldest possible in this batch

        MemoryItem trivial_but_new{};
        trivial_but_new.kind = memory_kind::episodic;
        trivial_but_new.content = "trivial fact";
        trivial_but_new.salience = 0.05f;
        trivial_but_new.write_seq = 1000;  // far more recent

        double const score_old_important =
            md::memory_rank_score(important_but_old, "", /*max_write_seq_in_batch=*/1000);
        double const score_new_trivial =
            md::memory_rank_score(trivial_but_new, "", 1000);
        check(score_old_important > score_new_trivial,
              "R2: recency is normalized to a bounded [1,2] range, not used as a raw unbounded "
              "SeqNo -- a much more salient but older item still outranks a barely-relevant, "
              "merely-more-recent one, proving the self-red-teamed unboundedness bug is actually "
              "fixed, not just described");
    }

    // --- R3: a freshly-extracted item (salience == 0.0f, MemoryProvider::on_turn_end()'s own real
    // behavior) is NOT structurally rank-zero -- it still ranks above a genuinely irrelevant item,
    // and a real keyword match on it still counts. A literal multiplicative reading over the RAW
    // salience value (0.0) would make this item's score exactly 0 no matter what else is true. ---
    {
        MemoryItem extracted{};  // matches on_turn_end()'s own construction -- salience never set
        extracted.kind = memory_kind::episodic;
        extracted.content = "the user prefers tea";
        extracted.write_seq = 3;
        check(extracted.salience == 0.0f, "R3 setup: matches on_turn_end()'s real default, salience 0.0f");

        double const score_extracted = md::memory_rank_score(extracted, "", /*max_write_seq_in_batch=*/3);
        check(score_extracted > 0.0,
              "R3: a zero-salience extracted item does NOT score exactly zero -- it remains "
              "rankable, not permanently invisible by construction");

        double const score_extracted_matched = md::memory_rank_score(extracted, "tea", 3);
        check(score_extracted_matched > score_extracted,
              "R3b: a real keyword match still measurably boosts a zero-salience item's score");
    }

    // --- Determinism: identical inputs -> identical score, every time. ---------------------------
    {
        MemoryItem item{};
        item.kind = memory_kind::episodic;
        item.content = "deterministic fact";
        item.salience = 0.4f;
        item.write_seq = 7;
        double const s1 = md::memory_rank_score(item, "fact", 10);
        double const s2 = md::memory_rank_score(item, "fact", 10);
        check(s1 == s2, "Determinism: the same item/query/batch produces the identical score twice");
    }

    // --- Legacy control: write_seq == 0 (a record written before this field existed, or the whole
    // batch has no recency signal at all, max_write_seq_in_batch == 0) must not divide by zero or
    // otherwise misbehave -- it degrades to a neutral recency factor instead. ----------------------
    {
        MemoryItem legacy{};
        legacy.kind = memory_kind::episodic;
        legacy.content = "pre-existing record";
        legacy.salience = 0.5f;
        legacy.write_seq = 0;

        double const score = md::memory_rank_score(legacy, "", /*max_write_seq_in_batch=*/0);
        check(score > 0.0 && score == score,  // score == score rules out NaN from a 0/0 division
              "Legacy control: write_seq==0 with an all-zero batch produces a finite, positive, "
              "non-NaN score -- no division-by-zero on records written before this field existed");
    }

    std::printf("test_memory_ranking_formula: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
