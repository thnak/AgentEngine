// Implements decisions/ADR-187-evaluation-harness.md §3.2 / E21's PRECONDITION: is a seeded lesson still
// among the items MemoryProvider injects after `on_turn_end` has written more episodic items?
//
// Recall is a top-`max_injected` ranking (default 3) by salience x recency x keyword (memory_provider.hpp
// memory_rank_score) with no weight for `kind`. `on_turn_end` writes an episodic item every turn with
// `salience` left at its default 0.0f. Round-2 red-team of ADR-187 read the source and claimed the lesson
// "drops out within a few turns". That holds only if the lesson's own salience is also ~0: the salience
// factor is (0.05 + salience) and the recency factor is bounded to [1, 2], so a lesson with salience >= ~0.1
// outranks every zero-salience episodic item regardless of how many are written. This test EXECUTES the
// real `rank_memory_items` over the real `write_memory_item` to settle it rather than argue from the
// formula.
//
// Claims:
//   R1  a lesson written at salience 0.0 IS evicted from the top 3 once three later episodic items exist
//       (the hazard is real for a floor-salience lesson);
//   R2  a lesson at salience 0.5 is still in the top 3 after 60 later zero-salience episodic items
//       (salience is the mitigation, and it is a WRITER's choice -- promotion must set it);
//   R3  the boundary: the smallest salience in a fixed grid that survives 60 later items is reported, and
//       is <= 0.1;
//   R4  CONTROL (so R2 could fail): when three later episodic items each CONTAIN the query text, the keyword
//       factor (1 hit vs the 0.1 floor = 10x) overturns a salience-0.2 lesson (5x over the floor) but NOT a
//       salience-1.0 one (21x); the threshold is measured on a grid.

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/memory_provider.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

constexpr char const* kLesson = "This team writes every date as DD-MM-YYYY.";

// Seeds one lesson at `lesson_salience`, then `later` episodic items shaped like on_turn_end's
// (kind episodic, model_inferred, salience left at the default 0.0f), and reports whether the lesson is in
// the top `max_results` for `query`. `later_text` builds each episodic item's content.
template <class TextFn>
bool lesson_survives(float lesson_salience, int later, std::string const& query, std::size_t max_results,
                     TextFn later_text) {
    ae::InMemoryWorktreeObjectStore object_store;
    ae::rt::InMemoryAppendLogStore ref_store;
    ae::Principal const principal{"p-recall", ""};
    if (!ae::ensure_memory_worktree(object_store, ref_store, principal)) return false;
    ae::Mount const mount = ae::memory_mount(principal);
    ae::cap::FsRead const read_cap{ae::memory_mount_id(principal), "", std::nullopt};
    ae::cap::FsWrite const write_cap{ae::memory_mount_id(principal), "", std::nullopt, std::nullopt};

    ae::MemoryItem lesson{};
    lesson.kind     = ae::memory_kind::procedural;
    lesson.content  = kLesson;
    lesson.salience = lesson_salience;
    lesson.origin   = ae::MemoryOrigin{ae::memory_source::model_inferred, "run-lesson", "0", principal};
    if (!ae::write_memory_item(object_store, ref_store, mount, write_cap, lesson)) return false;

    for (int i = 0; i < later; ++i) {
        ae::MemoryItem ep{};
        ep.kind    = ae::memory_kind::episodic;
        ep.content = later_text(i);
        ep.origin  = ae::MemoryOrigin{ae::memory_source::model_inferred, "run-" + std::to_string(i),
                                       std::to_string(i), principal};
        if (!ae::write_memory_item(object_store, ref_store, mount, write_cap, ep)) return false;
    }

    auto ranked = ae::rank_memory_items(object_store, ref_store, mount, read_cap, query, max_results);
    if (!ranked) return false;
    return std::any_of(ranked->begin(), ranked->end(),
                       [](ae::MemoryItem const& it) { return it.content == kLesson; });
}

}  // namespace

int main() {
    std::string const query = "What day is the release scheduled for?";
    auto const unrelated = [](int i) { return "Turn " + std::to_string(i) + ": the user asked about build times."; };

    // R1: floor-salience lesson, three later items -> evicted from the top 3.
    AE_CHECK(lesson_survives(0.0f, 0, query, 3, unrelated), "control: with no later items the lesson is recalled");
    AE_CHECK(lesson_survives(0.0f, 2, query, 3, unrelated), "R1 setup: with 2 later items a floor-salience lesson is still in the top 3");
    AE_CHECK(!lesson_survives(0.0f, 3, query, 3, unrelated),
             "R1: with 3 later on_turn_end-style items a salience-0.0 lesson IS evicted from the top 3");
    AE_CHECK(!lesson_survives(0.0f, 60, query, 3, unrelated), "R1: still evicted after 60");

    // R2: a lesson with real salience survives arbitrarily many later floor-salience items.
    AE_CHECK(lesson_survives(0.5f, 60, query, 3, unrelated),
             "R2: a salience-0.5 lesson is still in the top 3 after 60 later episodic items");

    // R3: the boundary over a fixed grid.
    float smallest = -1.0f;
    for (float s : {0.0f, 0.02f, 0.05f, 0.08f, 0.1f, 0.15f, 0.2f, 0.5f, 1.0f}) {
        bool const ok = lesson_survives(s, 60, query, 3, unrelated);
        std::cout << "  .. salience " << s << " survives 60 later items: " << (ok ? "yes" : "no") << "\n";
        if (ok && smallest < 0.0f) smallest = s;
    }
    std::cout << "  .. smallest surviving salience on the grid: " << smallest << "\n";
    AE_CHECK(smallest >= 0.0f && smallest <= 0.1f, "R3: the surviving threshold on the grid is <= 0.1");

    // R4: CONTROL. Episodic items that quote the user's words get the keyword factor (1 hit vs the 0.1 floor,
    // i.e. 10x). The salience factor is (0.05 + s): 1.05 for s=1.0 (21x the floor-salience 0.05) but only 0.25 for
    // s=0.2 (5x). So the keyword advantage overturns a MID-salience lesson and not a full-salience one. (My first
    // draft of this test predicted salience 1.0 would be evicted; running it showed the arithmetic above.)
    auto const quoting = [&](int i) { return "Turn " + std::to_string(i) + " user said: " + query; };
    AE_CHECK(!lesson_survives(0.2f, 3, query, 3, quoting),
             "R4: three episodic items that CONTAIN the query text evict a salience-0.2 lesson");
    AE_CHECK(lesson_survives(0.2f, 2, query, 3, quoting),
             "R4 control: with only two such items the salience-0.2 lesson still fits in the top 3");
    AE_CHECK(lesson_survives(1.0f, 60, query, 3, quoting),
             "R4b: a salience-1.0 lesson survives 60 items that quote the query (21x salience beats 10x keyword)");
    float smallest_quoting = -1.0f;
    for (float s : {0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.8f, 1.0f}) {
        bool const ok = lesson_survives(s, 60, query, 3, quoting);
        std::cout << "  .. vs quoting items: salience " << s << " survives 60: " << (ok ? "yes" : "no") << '\n';
        if (ok && smallest_quoting < 0.0f) smallest_quoting = s;
    }
    std::cout << "  .. smallest salience surviving 60 QUOTING items: " << smallest_quoting << '\n';
    AE_CHECK(smallest_quoting > 0.2f && smallest_quoting <= 1.0f,
             "R4c: surviving quoting items needs salience above 0.2 (a higher bar than surviving unrelated items)");

    // R5 (round 3): the R4 competitors were zero-salience. A REAL fact has some salience. Three facts of salience
    // 0.1 that quote the query score (0.05+0.1) x recency x 1 = up to 0.30; a salience-1.0 lesson with no keyword
    // hit scores 1.05 x ~1 x 0.1 = ~0.105. So even salience 1.0 is evicted by three ordinary quoting facts.
    {
        auto run = [&](float lesson_sal, float fact_sal, int facts, std::string const& q) {
            ae::InMemoryWorktreeObjectStore object_store;
            ae::rt::InMemoryAppendLogStore ref_store;
            ae::Principal const principal{"p-recall-r5", ""};
            (void)ae::ensure_memory_worktree(object_store, ref_store, principal);
            ae::Mount const mount = ae::memory_mount(principal);
            ae::cap::FsRead const rc{ae::memory_mount_id(principal), "", std::nullopt};
            ae::cap::FsWrite const wc{ae::memory_mount_id(principal), "", std::nullopt, std::nullopt};
            ae::MemoryItem lesson{};
            lesson.kind = ae::memory_kind::procedural; lesson.content = kLesson; lesson.salience = lesson_sal;
            lesson.origin = ae::MemoryOrigin{ae::memory_source::model_inferred, "run-l", "0", principal};
            (void)ae::write_memory_item(object_store, ref_store, mount, wc, lesson);
            for (int i = 0; i < facts; ++i) {
                ae::MemoryItem f{};
                f.kind = ae::memory_kind::semantic;
                f.content = "Fact " + std::to_string(i) + " about: " + q;
                f.salience = fact_sal;
                f.origin = ae::MemoryOrigin{ae::memory_source::user_stated, "run-f", std::to_string(i), principal};
                (void)ae::write_memory_item(object_store, ref_store, mount, wc, f);
            }
            auto ranked = ae::rank_memory_items(object_store, ref_store, mount, rc, q, 3);
            return ranked && std::any_of(ranked->begin(), ranked->end(),
                                          [](ae::MemoryItem const& it) { return it.content == kLesson; });
        };
        AE_CHECK(!run(1.0f, 0.1f, 3, query),
                 "R5: three salience-0.1 facts that quote the query evict even a salience-1.0 lesson");
        AE_CHECK(run(1.0f, 0.1f, 2, query), "R5 control: two such facts do not");
        // R6: a one-character query is a substring of nearly everything, so the keyword factor is uniform and
        // salience decides: the salience-1.0 lesson wins.
        AE_CHECK(run(1.0f, 0.1f, 20, "a"),
                 "R6: with a one-character query (keyword hit on every item) a salience-1.0 lesson wins on salience");
    }

    // R7 (round 3): promoted lessons compete with EACH OTHER. All promoted lessons share one salience constant, so
    // once more than `max_results` exist, recency alone decides and the oldest are evicted.
    {
        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore ref_store;
        ae::Principal const principal{"p-recall-r7", ""};
        (void)ae::ensure_memory_worktree(object_store, ref_store, principal);
        ae::Mount const mount = ae::memory_mount(principal);
        ae::cap::FsRead const rc{ae::memory_mount_id(principal), "", std::nullopt};
        ae::cap::FsWrite const wc{ae::memory_mount_id(principal), "", std::nullopt, std::nullopt};
        for (int i = 0; i < 5; ++i) {
            ae::MemoryItem l{};
            l.kind = ae::memory_kind::procedural; l.content = "Lesson number " + std::to_string(i);
            l.salience = 1.0f;
            l.origin = ae::MemoryOrigin{ae::memory_source::model_inferred, "run-l", std::to_string(i), principal};
            (void)ae::write_memory_item(object_store, ref_store, mount, wc, l);
        }
        auto ranked = ae::rank_memory_items(object_store, ref_store, mount, rc, "unrelated question", 3);
        bool oldest_two_gone = ranked.has_value() && ranked->size() == 3;
        if (ranked) {
            for (auto const& it : *ranked) {
                if (it.content == "Lesson number 0" || it.content == "Lesson number 1") oldest_two_gone = false;
            }
        }
        AE_CHECK(oldest_two_gone,
                 "R7: with 5 equal-salience promoted lessons and max_results=3 only the 3 NEWEST are recalled");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_memory_lesson_recall: all checks passed\n";
    return 0;
}
