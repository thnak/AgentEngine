// PROVE-PHASE PROBE: real three-way merge_trees(), using REAL SHA-256 digests for every entry
// (via the actual, linked agentengine::compute_digest), not synthetic placeholder digests.

#include "merge_trees.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
agentengine::Digest digest_of(std::string const& content) {
    std::vector<std::byte> bytes(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);
    auto d = agentengine::compute_digest(bytes);
    if (!d) { std::fprintf(stderr, "digest failed\n"); std::abort(); }
    return *d;
}
agentengine::TreeEntry entry(std::string name, std::string content) {
    return agentengine::TreeEntry{std::move(name), digest_of(content), false};
}
std::optional<agentengine::TreeEntry> find(agentengine::Tree const& t, std::string const& name) {
    for (auto const& e : t.entries) if (e.name == name) return e;
    return std::nullopt;
}
}  // namespace

int main() {
    using namespace probe;

    // === Case 1: clean, non-overlapping changes (both sides add different new files) ===============
    {
        agentengine::Tree base;
        base.entries.push_back(entry("shared.txt", "unchanged"));

        agentengine::Tree ours = base;
        ours.entries.push_back(entry("ours_new.txt", "added by ours"));

        agentengine::Tree theirs = base;
        theirs.entries.push_back(entry("theirs_new.txt", "added by theirs"));

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.empty());
        CHECK(result.merged.entries.size() == 3);
        CHECK(find(result.merged, "shared.txt").has_value());
        CHECK(find(result.merged, "ours_new.txt").has_value());
        CHECK(find(result.merged, "theirs_new.txt").has_value());
        std::printf("[1] non-overlapping additions from both sides: PASS (0 conflicts, all 3 "
                    "files present in merged result)\n");
    }

    // === Case 2: only ours modified an existing path -> take ours, no conflict =====================
    {
        agentengine::Tree base;
        base.entries.push_back(entry("a.txt", "original"));
        agentengine::Tree ours;
        ours.entries.push_back(entry("a.txt", "modified by ours"));
        agentengine::Tree theirs = base;  // unchanged

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.empty());
        auto a = find(result.merged, "a.txt");
        CHECK(a.has_value() && a->digest == digest_of("modified by ours"));
        std::printf("[2] only ours modified a.txt: PASS (merged a.txt = ours's version, no "
                    "conflict)\n");
    }

    // === Case 3: both sides modified the SAME path to the SAME new content -> no conflict ===========
    {
        agentengine::Tree base;
        base.entries.push_back(entry("a.txt", "original"));
        agentengine::Tree ours;
        ours.entries.push_back(entry("a.txt", "converged value"));
        agentengine::Tree theirs;
        theirs.entries.push_back(entry("a.txt", "converged value"));  // identical to ours

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.empty());
        auto a = find(result.merged, "a.txt");
        CHECK(a.has_value() && a->digest == digest_of("converged value"));
        std::printf("[3] both sides converged on the identical new value independently: PASS "
                    "(no conflict, even though both sides changed the same path)\n");
    }

    // === Case 4: REAL CONFLICT -- both sides modified the same path to DIFFERENT values =============
    {
        agentengine::Tree base;
        base.entries.push_back(entry("a.txt", "original"));
        agentengine::Tree ours;
        ours.entries.push_back(entry("a.txt", "ours version"));
        agentengine::Tree theirs;
        theirs.entries.push_back(entry("a.txt", "theirs version"));

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.size() == 1);
        auto const& c = result.conflicts[0];
        CHECK(c.path == "a.txt");
        CHECK(c.base_digest.has_value() && *c.base_digest == digest_of("original"));
        CHECK(c.ours_digest == digest_of("ours version"));
        CHECK(c.theirs_digest == digest_of("theirs version"));
        std::printf("[4] REAL CONFLICT: both sides changed a.txt differently: DETECTED "
                    "(base=%s..., ours=%s..., theirs=%s...)\n",
                    c.base_digest->substr(0, 8).c_str(), c.ours_digest.substr(0, 8).c_str(),
                    c.theirs_digest.substr(0, 8).c_str());
    }

    // === Case 5: delete-vs-modify is also a real, detected conflict =================================
    {
        agentengine::Tree base;
        base.entries.push_back(entry("a.txt", "original"));
        agentengine::Tree ours;  // ours deleted a.txt entirely
        agentengine::Tree theirs;
        theirs.entries.push_back(entry("a.txt", "modified by theirs"));

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.size() == 1);
        CHECK(result.conflicts[0].path == "a.txt");
        CHECK(result.conflicts[0].ours_digest == agentengine::Digest{"<deleted>"});
        std::printf("[5] delete-vs-modify (ours deleted a.txt, theirs modified it): DETECTED as a "
                    "real conflict, not silently resolved either way\n");
    }

    // === Case 6: a real, non-trivial merge combining all of the above in one call ===================
    {
        agentengine::Tree base;
        base.entries.push_back(entry("stable.txt", "never touched"));
        base.entries.push_back(entry("contested.txt", "original"));
        base.entries.push_back(entry("only_ours_touches.txt", "base value"));

        agentengine::Tree ours;
        ours.entries.push_back(entry("stable.txt", "never touched"));
        ours.entries.push_back(entry("contested.txt", "ours edit"));
        ours.entries.push_back(entry("only_ours_touches.txt", "ours changed this"));
        ours.entries.push_back(entry("ours_only_new.txt", "brand new from ours"));

        agentengine::Tree theirs;
        theirs.entries.push_back(entry("stable.txt", "never touched"));
        theirs.entries.push_back(entry("contested.txt", "theirs edit"));
        theirs.entries.push_back(entry("only_ours_touches.txt", "base value"));  // theirs left it alone

        auto result = merge_trees(base, ours, theirs);
        CHECK(result.conflicts.size() == 1);
        CHECK(result.conflicts[0].path == "contested.txt");
        CHECK(find(result.merged, "stable.txt").has_value());
        CHECK(find(result.merged, "only_ours_touches.txt")->digest == digest_of("ours changed this"));
        CHECK(find(result.merged, "ours_only_new.txt").has_value());
        std::printf("[6] combined real-world-shaped merge (4 files, 1 genuine conflict): PASS -- "
                    "exactly 1 conflict on \"contested.txt\", every other path resolved correctly\n");
    }

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
