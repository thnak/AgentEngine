// Milestone 4 Phase G1/G2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// 029 §2's "every principal owns a memory worktree... scoped one level up: principal:p-7 instead
// of session:s-42. No new storage primitive" and §3's "stored as ordinary blobs under a path
// derived from {kind, id}" had no implementation before this task. Proves: two principals'
// memory worktrees are real, isolated Refs (reusing A1's exact ActorId-isolation mechanism, one
// level up); ensure_memory_worktree() is idempotent; a MemoryItem round-trips through
// write_memory_item()/read_memory_item() with its id correctly derived from its own content
// (not assigned by the caller); and list_memory_items() answers "what memory exists" via an
// ordinary tree listing, exactly 029 §3's own framing.

#include <iostream>
#include <string>

#include "quark/core/persistence.hpp"

#include "agentengine/core/memory.hpp"

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

} // namespace

int main() {
    quark::InMemoryStore ref_store;
    ae::InMemoryWorktreeObjectStore object_store;

    ae::Principal const alice{"p-alice", "tenant-1"};
    ae::Principal const bob{"p-bob", "tenant-1"};

    // --- G1: memory worktree naming + real isolation, reusing A1's exact mechanism -------------
    AE_CHECK(ae::memory_ref_name(alice) == "principal:p-alice",
             "G1-C1: the memory ref name is 'principal:<id>', 029 §2's exact naming");
    AE_CHECK(ae::ref_actor_id(ae::memory_ref_name(alice)) != ae::ref_actor_id(ae::memory_ref_name(bob)),
             "G1-R1: two principals' memory worktrees resolve to distinct ActorIds -- the SAME "
             "isolation mechanism A1 proved for sessions, one level up, not a new one");

    auto ref1 = ae::ensure_memory_worktree(object_store, ref_store, alice);
    AE_CHECK(ref1.has_value(), "G1-R2: bootstrapping a principal's memory worktree succeeds");
    auto ref2 = ae::ensure_memory_worktree(object_store, ref_store, alice);
    AE_CHECK(ref2.has_value() && ref2->tree_digest == ref1->tree_digest,
             "G1-R3: ensure_memory_worktree() is idempotent -- calling it again returns the SAME "
             "worktree, never re-creating it");

    // --- G2: a MemoryItem round-trips through write/read, with a real content-derived id -------
    ae::Mount const alice_memory = ae::memory_mount(alice);
    ae::cap::FsWrite const write_all{ae::memory_mount_id(alice), "", std::nullopt, std::nullopt};
    ae::cap::FsRead const read_all{ae::memory_mount_id(alice), "", std::nullopt};

    ae::MemoryItem item{};
    item.kind = ae::memory_kind::episodic;
    item.content = "the user prefers dark mode";
    item.tags = {"preference", "ui"};
    item.salience = 0.8f;
    item.origin = ae::MemoryOrigin{ae::memory_source::user_stated, "run-1", "turn-3", alice};

    AE_CHECK(item.id.empty(), "setup: the item starts with no id -- 'identity, not assigned'");
    auto written = ae::write_memory_item(object_store, ref_store, alice_memory, write_all, item);
    AE_CHECK(written.has_value(), "G2-R1: writing a MemoryItem succeeds");
    AE_CHECK(!item.id.empty(), "G2-R2: write_memory_item() fills in item.id as a side effect");

    auto loaded = ae::read_memory_item(object_store, ref_store, alice_memory, read_all,
                                        ae::memory_kind::episodic, item.id);
    AE_CHECK(loaded.has_value() && loaded->content == item.content && loaded->tags == item.tags &&
                 loaded->id == item.id,
             "G2-R3: the item reads back bit-identical -- content, tags, and id all round-trip");
    AE_CHECK(loaded.has_value() && loaded->origin.source == ae::memory_source::user_stated &&
                 loaded->origin.principal.id == "p-alice",
             "G2-R4: provenance (MemoryOrigin) survives the round-trip too -- 029 §3's own "
             "'never an opaque blob of text' requirement");

    // Writing the SAME content again yields the SAME id (content-addressed, deterministic).
    ae::MemoryItem duplicate{};
    duplicate.kind = ae::memory_kind::episodic;
    duplicate.content = item.content;  // identical content
    duplicate.origin = item.origin;
    auto written2 = ae::write_memory_item(object_store, ref_store, alice_memory, write_all, duplicate);
    AE_CHECK(written2.has_value() && duplicate.id == item.id,
             "G2-R5: identical content produces the identical id -- content-addressed identity, "
             "matching 025 §2's own dedup claim reused one level up");

    // --- "What memory exists" is an ordinary tree listing -----------------------------------------
    ae::MemoryItem second{};
    second.kind = ae::memory_kind::semantic;
    second.content = "the project's build system is CMake";
    second.origin = ae::MemoryOrigin{ae::memory_source::tool_derived, "run-2", "turn-1", alice};
    auto written3 = ae::write_memory_item(object_store, ref_store, alice_memory, write_all, second);
    AE_CHECK(written3.has_value(), "setup: a second, distinct memory item is written");

    auto listed = ae::list_memory_items(object_store, ref_store, alice_memory, read_all);
    AE_CHECK(listed.has_value() && listed->size() == 2,
             "G2-R6: list_memory_items() finds exactly the 2 distinct items written (the duplicate "
             "write didn't create a third entry, matching content-addressed dedup)");

    // --- Cross-principal isolation: Bob's (never-touched) memory worktree is genuinely empty,
    // and Alice's own capability can't even be USED against Bob's mount (029 §9 G5, closed by
    // memory_mount_id()'s own per-principal derivation -- see its comment) --------------------------
    auto bob_ref = ae::ensure_memory_worktree(object_store, ref_store, bob);
    AE_CHECK(bob_ref.has_value(), "setup: Bob's own memory worktree bootstraps independently");
    ae::Mount const bob_memory = ae::memory_mount(bob);
    ae::cap::FsRead const bob_read_all{ae::memory_mount_id(bob), "", std::nullopt};

    AE_CHECK(ae::memory_mount_id(alice) != ae::memory_mount_id(bob),
             "G2-R7: two principals' memory mount_ids are guaranteed distinct by construction");

    auto crossed = ae::list_memory_items(object_store, ref_store, bob_memory, read_all);
    AE_CHECK(!crossed.has_value() && crossed.error().code == "worktree.mount_capability_mismatch",
             "G2-R8: Alice's OWN capability is rejected outright against Bob's mount -- never "
             "reaches the store at all, closing the exact 'shared mount_id' leakage hazard an "
             "earlier caller-supplied-mount_id design would have allowed");

    auto bob_listed = ae::list_memory_items(object_store, ref_store, bob_memory, bob_read_all);
    AE_CHECK(bob_listed.has_value() && bob_listed->empty(),
             "G2-R9: Bob's memory worktree has none of Alice's items -- distinct Refs, distinct "
             "storage, never shared by accident");

    std::cout << (g_failures == 0 ? "test_memory_worktree: OK\n" : "test_memory_worktree: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
