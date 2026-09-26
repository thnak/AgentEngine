// Proof that `ledger_detail::find_case_folding_collision()` -- the search behind
// `Ledger::check_case_folding_collision()`, which rejects a tree whose entry names would collide on a
// case-insensitive filesystem (git's CVE-2014-9390 fix direction) -- finds exactly what the old
// nested loop found, only in O(n log n) instead of O(n^2).
//
// WHAT WAS WRONG. The old search re-allocated and re-lowercased the inner name on every one of its
// n(n-1)/2 comparisons. `Ledger::commit()` runs it on every commit, against the flat, whole-sandbox
// tree `RealIoFileSystem::scan_and_drain_into_tree()` builds (one entry per file), so it ran once per
// sandbox command and grew quadratically with the number of files in the sandbox.
//
// This is a security check, so a faster one is only acceptable if it rejects exactly the same trees
// and names exactly the same pair. The equivalence check (K3) is the real proof: `old_search()` below
// is the removed loop, copied verbatim, and the two must agree on every generated tree.
//
//   K1 (positive control) -- the canonical collision is found, and names the right pair.
//   K2 -- exact duplicate names are not a collision, as they never were; distinct folds are none.
//   K3 (the guard) -- on 20000 generated trees drawn from a tiny alphabet (so collisions, duplicates,
//         several collision groups and none at all are all common), the new search returns the
//         SAME optional pair as the old loop every time: same rejections, same first pair, so the
//         same rejection message.
//   K4 -- with two collision groups, the reported pair is the old loop's first (lowest first index),
//         not whichever group happens to sort first -- the one place a sort-based search could
//         silently name a different pair.
//   K5 -- scale on CLEAN trees (the old loop's worst case, and every ordinary commit's), printed not
//         asserted since a wall-clock bound would flake on a sanitizer leg: the new search on 20000
//         entries (plus a collision added last, still caught), both searches on 1000.
//
// The real rejection path, `Ledger::commit()` -> `ledger.case_folding_collision`, is covered by
// `test_ledger.cpp` check [10], which this change leaves unmodified.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/ledger.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <utility>

namespace ae = agentengine;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

// The removed search, verbatim apart from returning the pair instead of building the error.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> old_search(ae::Tree const& tree) {
    for (std::size_t i = 0; i < tree.entries.size(); ++i) {
        std::string folded_i = tree.entries[i].name;
        for (char& c : folded_i) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (std::size_t j = i + 1; j < tree.entries.size(); ++j) {
            std::string folded_j = tree.entries[j].name;
            for (char& c : folded_j) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (folded_i == folded_j && tree.entries[i].name != tree.entries[j].name) {
                return std::pair{i, j};
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] ae::Tree tree_of(std::initializer_list<char const*> names) {
    ae::Tree t;
    for (char const* n : names) t.entries.push_back(ae::TreeEntry{n, "digest", false});
    return t;
}

[[nodiscard]] std::string describe(std::optional<std::pair<std::size_t, std::size_t>> const& p) {
    return p.has_value() ? "{" + std::to_string(p->first) + "," + std::to_string(p->second) + "}" : "none";
}

}  // namespace

int main() {
    using ae::ledger_detail::find_case_folding_collision;

    // ---- K1: the positive control.
    {
        auto const found = find_case_folding_collision(tree_of({"readme.txt", "README.txt"}));
        check(found.has_value() && found->first == 0 && found->second == 1,
              "K1 (positive control): readme.txt / README.txt is a collision at {0,1}, got " + describe(found));
    }

    // ---- K2: not collisions.
    {
        check(!find_case_folding_collision(tree_of({"same", "same"})).has_value(),
              "K2: two entries with the exact same name are not a case-folding collision");
        check(!find_case_folding_collision(tree_of({"a", "b", "src/A.cpp", "src/B.cpp"})).has_value(),
              "K2: distinct folds are not a collision");
        check(!find_case_folding_collision(ae::Tree{}).has_value(), "K2: an empty tree has none");
    }

    // ---- K3: equivalence with the removed loop.
    {
        std::mt19937 rng(20260914);  // fixed: a failure must be reproducible
        char const alphabet[] = {'a', 'A', 'b', 'B', '/'};
        int disagreements = 0;
        int with_collision = 0;
        std::string first_disagreement;
        int const trials = 20000;
        for (int t = 0; t < trials; ++t) {
            ae::Tree tree;
            std::size_t const n = std::uniform_int_distribution<std::size_t>(0, 24)(rng);
            for (std::size_t e = 0; e < n; ++e) {
                std::size_t const len = std::uniform_int_distribution<std::size_t>(1, 3)(rng);
                std::string name;
                for (std::size_t k = 0; k < len; ++k) {
                    name.push_back(alphabet[std::uniform_int_distribution<int>(0, 4)(rng)]);
                }
                tree.entries.push_back(ae::TreeEntry{name, "d", false});
            }
            auto const expected = old_search(tree);
            auto const actual = find_case_folding_collision(tree);
            with_collision += expected.has_value() ? 1 : 0;
            if (expected != actual) {
                if (disagreements++ == 0) {
                    first_disagreement = "trial " + std::to_string(t) + ": old " + describe(expected) +
                                         ", new " + describe(actual);
                }
            }
        }
        check(with_collision > trials / 10 && with_collision < trials - trials / 10,
              "K3: the generated trees exercise both outcomes (" + std::to_string(with_collision) + " of " +
                  std::to_string(trials) + " contain a collision), so agreement is not vacuous");
        check(disagreements == 0,
              "K3: the new search returns the SAME pair as the removed nested loop on all " +
                  std::to_string(trials) + " trees" +
                  (disagreements == 0 ? std::string{} : " -- " + std::to_string(disagreements) +
                                                           " disagreements, first " + first_disagreement));
    }

    // ---- K4: the first pair is the lowest colliding index, not the first run in sorted order.
    {
        // Sorted by fold, "lib/a" comes before "src/main.cpp", but the old loop reports the pair whose
        // FIRST index is lowest: "src/Main.cpp" at index 1 collides before "lib/a" at index 2 does.
        auto const found =
            find_case_folding_collision(tree_of({"docs/x.md", "src/Main.cpp", "lib/a", "src/main.cpp", "LIB/A"}));
        check(found.has_value() && found->first == 1 && found->second == 3,
              "K4: with two collision groups the reported pair is {1,3}, the old loop's first, not the "
              "group that sorts first; got " + describe(found));
    }

    // ---- K5: scale, printed.
    //
    // Both trees below have NO collision, which is the case every ordinary commit takes and the old
    // loop's worst case: it can only stop early by finding one, so a clean sandbox paid all n(n-1)/2
    // comparisons. (A collision placed at index 0 would let the old loop exit after n comparisons and
    // make it look faster than the sort -- measuring that would be measuring the wrong thing.) For the
    // record, a standalone /O2 build of both searches on no-collision trees measured:
    //     500 files: 14.6 ms -> 0.11 ms;  2000: 236.7 ms -> 0.56 ms;  5000: 1500 ms -> 1.30 ms.
    // The sizes here are small enough that the old loop stays cheap under Debug and ASan in CI.
    {
        auto clean_tree = [](std::size_t n) {
            ae::Tree t;
            for (std::size_t i = 0; i < n; ++i) {
                t.entries.push_back(ae::TreeEntry{"workspace/src/module_" + std::to_string(i) + "/file.cpp", "d", false});
            }
            return t;
        };
        using clock = std::chrono::steady_clock;
        auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

        ae::Tree t20k = clean_tree(20000);
        auto t0 = clock::now();
        auto const clean20k = find_case_folding_collision(t20k);
        double const new20k = ms(clock::now() - t0);
        check(!clean20k.has_value(), "K5: a clean 20000-entry tree has no collision");
        t20k.entries.push_back(ae::TreeEntry{"WORKSPACE/SRC/MODULE_0/FILE.CPP", "d", false});
        auto const late = find_case_folding_collision(t20k);
        check(late.has_value() && late->first == 0 && late->second == 20000,
              "K5: ... and one collision added at the very end of it is still caught at {0,20000}");

        ae::Tree const t1k = clean_tree(1000);
        t0 = clock::now();
        auto const new1k_found = find_case_folding_collision(t1k);
        double const new1k = ms(clock::now() - t0);
        t0 = clock::now();
        auto const old1k_found = old_search(t1k);
        double const old1k = ms(clock::now() - t0);
        check(new1k_found == old1k_found, "K5: both searches agree on the clean 1000-entry tree");
        std::printf("[info] clean-tree search time (this build): 1000 entries -- old %.1f ms, new %.2f ms; "
                    "20000 entries -- new %.2f ms\n",
                    old1k, new1k, new20k);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
