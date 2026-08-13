// Proof for ADR-037: agentengine::rt::ProjectRegistry's archived-member tail
// (include/agentengine/rt/project_archive.hpp) -- rt::AppendLogStore wired into 030 Sec6/§7's
// archived-tail requirement, closing the gap rt::ProjectRegistry's own banner named as deferred.
// Deterministic, offline, single-threaded. Covers:
//   A1 -- an untouched archived tail reads back empty, not an error.
//   A2 -- members archived in order read back in EXACTLY that order, with every field intact --
//         the old suite's own H5 claim (test_project_registry.cpp:171-191), ported.
//   A3 -- two Projects' archived tails never leak into each other (isolation by project_id).
//   A4 -- AT SCALE: archiving 2000 members (matching the old suite's own G4 scale,
//         test_project_registry.cpp:214-220) still reads back the full tail, in order, with no
//         entries dropped or reordered -- the functional half of "an unbounded-growth archived tail
//         works," without the old G4's byte-size/timing assertions (there is no ProjectRecord
//         manifest ported yet to measure against in this pass; write-cost-independent-of-history is
//         a structural property of AppendLogStore::append() itself, not a per-call benchmark this
//         suite re-measures).
//   D1 -- DURABILITY: archiving through one FileAppendLogStore instance and reading back through a
//         SECOND instance rooted at the same directory (simulating a process restart) returns the
//         identical tail -- a genuinely new claim the old suite never made (it only ever exercised a
//         single quark::Store handle within one test process).

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "agentengine/rt/project_archive.hpp"

using agentengine::rt::FileAppendLogStore;
using agentengine::rt::InMemoryAppendLogStore;
using agentengine::rt::ProjectMember;
using agentengine::rt::archive_project_member;
using agentengine::rt::read_archived_members;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

[[nodiscard]] ProjectMember make_member(std::string session_id, std::string status) {
    ProjectMember m;
    m.session_id         = std::move(session_id);
    m.parent_session_id  = "s-root";
    m.role                 = "worker";
    m.spawned_at_ns        = 42;
    m.worktree_ref          = std::string(64, 'a');  // stand-in hex digest
    m.status                 = std::move(status);
    return m;
}

void test_a1_empty_tail() {
    InMemoryAppendLogStore store;
    auto tail = read_archived_members(store, "proj-empty");
    check(tail.has_value(), "A1: read_archived_members on an untouched tail does not error");
    check(tail.has_value() && tail->empty(), "A1: an untouched archived tail reads back empty");
}

void test_a2_order_and_fidelity() {
    InMemoryAppendLogStore store;
    std::vector<ProjectMember> to_archive = {
        make_member("s-h1", "completed"),
        make_member("s-h2", "cancelled"),
        make_member("s-h3", "completed"),
    };
    for (auto const& m : to_archive) {
        auto r = archive_project_member(store, "proj-1", m);
        check(r.has_value(), "A2: archive_project_member succeeds");
    }
    auto tail = read_archived_members(store, "proj-1");
    check(tail.has_value(), "A2: read_archived_members succeeds");
    check(tail.has_value() && *tail == to_archive,
          "A2: the archived tail reads back in EXACTLY the order members were archived, fields intact");
}

void test_a3_isolation_across_projects() {
    InMemoryAppendLogStore store;
    auto r1 = archive_project_member(store, "proj-a", make_member("s-a1", "completed"));
    auto r2 = archive_project_member(store, "proj-b", make_member("s-b1", "completed"));
    auto r3 = archive_project_member(store, "proj-b", make_member("s-b2", "completed"));
    check(r1.has_value() && r2.has_value() && r3.has_value(), "A3: archives to distinct projects succeed");

    auto tail_a = read_archived_members(store, "proj-a");
    auto tail_b = read_archived_members(store, "proj-b");
    check(tail_a.has_value() && tail_a->size() == 1 && (*tail_a)[0].session_id == "s-a1",
          "A3: proj-a's tail holds only its own member");
    check(tail_b.has_value() && tail_b->size() == 2, "A3: proj-b's tail holds only its own two members");
}

void test_a4_scale() {
    InMemoryAppendLogStore store;
    constexpr int kCount = 2000;
    for (int i = 0; i < kCount; ++i) {
        auto r = archive_project_member(store, "proj-scale",
                                          make_member("s-h" + std::to_string(i), "completed"));
        if (!r.has_value()) {
            check(false, "A4: archive_project_member succeeds at scale");
            return;
        }
    }
    auto tail = read_archived_members(store, "proj-scale");
    check(tail.has_value() && tail->size() == static_cast<std::size_t>(kCount),
          "A4: all 2000 archived members read back, none dropped");
    bool in_order = true;
    if (tail.has_value()) {
        for (int i = 0; i < kCount; ++i) {
            if ((*tail)[static_cast<std::size_t>(i)].session_id != "s-h" + std::to_string(i)) {
                in_order = false;
                break;
            }
        }
    }
    check(in_order, "A4: 2000 archived members stay in exact archival order");
}

void test_d1_durability_across_store_instances(std::filesystem::path const& root) {
    {
        FileAppendLogStore store(root);
        auto r1 = archive_project_member(store, "proj-durable", make_member("s-d1", "completed"));
        auto r2 = archive_project_member(store, "proj-durable", make_member("s-d2", "completed"));
        check(r1.has_value() && r2.has_value(), "D1: archives through the first store instance succeed");
    }  // store instance goes out of scope -- simulates a process restart

    FileAppendLogStore reopened(root);
    auto tail = read_archived_members(reopened, "proj-durable");
    check(tail.has_value() && tail->size() == 2,
          "D1: a SECOND FileAppendLogStore instance rooted at the same directory sees both members");
    check(tail.has_value() && tail->size() == 2 && (*tail)[0].session_id == "s-d1" &&
              (*tail)[1].session_id == "s-d2",
          "D1: order survives the simulated restart");
}

}  // namespace

int main() {
    test_a1_empty_tail();
    test_a2_order_and_fidelity();
    test_a3_isolation_across_projects();
    test_a4_scale();

    std::filesystem::path root =
        std::filesystem::temp_directory_path() / "agentengine_test_rt_project_archive";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    test_d1_durability_across_store_instances(root);
    std::filesystem::remove_all(root, ec);

    if (g_failures == 0) {
        std::fprintf(stderr, "All checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
