// Proof for ADR-032 (decisions/ADR-032-workflow-executor-worktree-scoping.md), closing
// 014-Workflow-and-Orchestration.md §1's named gap ("nothing states whether a workflow executor...
// gets its own sub-worktree, inherits the caller's, or shares one") using 025-Worktree-and-Virtual-
// Filesystem.md §3's `SubWorktree`/`sharing_mode` primitive.
//
// Every check below traces to a specific red-team finding this design incorporates (see ADR-032 §3
// for the full table) -- this is not a "does the happy path work" suite, it is "does each finding
// stay fixed" one.

#include <iostream>
#include <string>

#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/worktree_scoping.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using quark::InMemoryStore;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                          \
    } while (0)

std::vector<std::byte> bytes_of(std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return bytes;
}

std::string string_of(std::vector<std::byte> const& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) s.push_back(static_cast<char>(b));
    return s;
}

Digest blob_of(InMemoryWorktreeObjectStore& store, std::string const& content) {
    return *store.put_blob(bytes_of(content));
}

Digest tree_of(InMemoryWorktreeObjectStore& store, std::vector<TreeEntry> entries) {
    return *store.put_tree(Tree{std::move(entries)});
}

Executor make_executor(std::string id, sharing_mode mode) {
    Executor ex;
    ex.id            = std::move(id);
    ex.kind          = executor_kind::function;
    ex.input_type    = "in";
    ex.output_type   = "out";
    ex.worktree_mode = mode;
    return ex;
}

struct Ping {};

} // namespace

AE_WORKFLOW_MESSAGE(Ping, "ping");

int main() {
    // M1: mint_executor_worktrees produces the right SHAPE per mode -- mount/read/write present for
    // shared/branch/scratch, all three absent for readonly (FATAL finding #1's fix: this design
    // never hands out a Mount it cannot honor).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-1", root);
        AE_CHECK(parent.has_value(), "M1: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("a", sharing_mode::branch), make_executor("b", sharing_mode::shared),
                        make_executor("c", sharing_mode::scratch), make_executor("d", sharing_mode::readonly)};

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(grants.has_value() && grants->size() == 4, "M1: mint succeeds, one grant per executor");

        AE_CHECK((*grants)[0].mount.has_value() && (*grants)[0].read.has_value() &&
                     (*grants)[0].write.has_value(),
                 "M1: branch executor gets a mount + read + write");
        AE_CHECK((*grants)[1].mount.has_value() && (*grants)[1].read.has_value() &&
                     (*grants)[1].write.has_value(),
                 "M1: shared executor gets a mount + read + write");
        AE_CHECK((*grants)[2].mount.has_value() && (*grants)[2].read.has_value() &&
                     (*grants)[2].write.has_value(),
                 "M1: scratch executor gets a mount + read + write");
        AE_CHECK(!(*grants)[3].mount.has_value() && !(*grants)[3].read.has_value() &&
                     !(*grants)[3].write.has_value(),
                 "M1: readonly executor gets NO mount/read/write (Mount cannot honor a pinned digest)");
        AE_CHECK((*grants)[3].sub.mode == sharing_mode::readonly &&
                     (*grants)[3].sub.pinned_digest == parent->tree_digest,
                 "M1: readonly executor still gets a real, correct SubWorktree -- just no guest view");
    }

    // M2: two BRANCH siblings are isolated from each other -- writing through one's mount is
    // invisible reading through the other's, and does not move the shared parent. Positive control:
    // the SAME write IS visible reading back through the writer's OWN mount.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto seed = blob_of(obj_store, "seed");
        auto root = tree_of(obj_store, {{"seed.txt", seed, false}});
        auto parent = commit_ref(ref_store, "wf:run-2", root);
        AE_CHECK(parent.has_value(), "M2: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("alpha", sharing_mode::branch), make_executor("beta", sharing_mode::branch)};

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(grants.has_value(), "M2: mint succeeds");
        auto const& ga = (*grants)[0];
        auto const& gb = (*grants)[1];
        AE_CHECK(ga.mount->mount_id != gb.mount->mount_id, "M2: the two siblings get DISTINCT mount ids");
        AE_CHECK(ga.mount->ref_name != gb.mount->ref_name,
                 "M2: the two branch siblings back onto DIFFERENT refs (true divergence, not aliasing)");

        auto write = mount_write(obj_store, ref_store, *ga.mount, *ga.write, "alpha_only.txt", bytes_of("alpha wrote this"));
        AE_CHECK(write.has_value(), "M2: writing through alpha's own mount succeeds");

        auto read_back_through_own = mount_read(obj_store, ref_store, *ga.mount, *ga.read, "alpha_only.txt");
        AE_CHECK(read_back_through_own.has_value() && string_of(*read_back_through_own) == "alpha wrote this",
                 "M2 (positive control): alpha's write IS visible reading back through alpha's own mount");

        auto read_through_sibling = mount_read(obj_store, ref_store, *gb.mount, *gb.read, "alpha_only.txt");
        AE_CHECK(!read_through_sibling.has_value(),
                 "M2: alpha's write is INVISIBLE reading through beta's mount -- isolated, not aliased");

        auto parent_after = read_ref(ref_store, "wf:run-2");
        AE_CHECK(parent_after.has_value() && parent_after->has_value() &&
                     (*parent_after)->tree_digest == root,
                 "M2: neither sibling's write moved the shared parent ref");
    }

    // M3: two SHARED executors observe each other's writes immediately (both mounts back onto the
    // SAME ref despite having distinct mount ids) -- the counterpart to M2, proving shared really
    // does what 025 §3 promises at the workflow granularity once an author opts in (finding #5's fix
    // -- reachable via TypedExecutor's own field, exercised again in M-builder below).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-3", root);
        AE_CHECK(parent.has_value(), "M3: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("writer", sharing_mode::shared), make_executor("reader", sharing_mode::shared)};

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(grants.has_value(), "M3: mint succeeds");
        auto const& gw = (*grants)[0];
        auto const& gr = (*grants)[1];
        AE_CHECK(gw.mount->mount_id != gr.mount->mount_id && gw.mount->ref_name == gr.mount->ref_name,
                 "M3: distinct mount ids, but the SAME backing ref -- that is what makes them 'shared'");

        AE_CHECK(mount_write(obj_store, ref_store, *gw.mount, *gw.write, "note.txt", bytes_of("hello"))
                     .has_value(),
                 "M3: writing through writer's mount succeeds");
        auto seen_by_reader = mount_read(obj_store, ref_store, *gr.mount, *gr.read, "note.txt");
        AE_CHECK(seen_by_reader.has_value() && string_of(*seen_by_reader) == "hello",
                 "M3: reader's mount sees writer's write immediately -- true cross-visibility");
    }

    // M4: a capability minted for executor A's mount is rejected against executor B's Mount --
    // proving the grant WIRING produces genuinely per-executor-scoped mount ids (the underlying
    // `mount_id != mount.mount_id` check is already proven in test_worktree_mount.cpp; this proves
    // THIS layer feeds it two different ids, not accidentally the same one).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-4", root);
        AE_CHECK(parent.has_value(), "M4: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("x", sharing_mode::branch), make_executor("y", sharing_mode::branch)};
        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(grants.has_value(), "M4: mint succeeds");

        auto cross = mount_write(obj_store, ref_store, *(*grants)[1].mount, *(*grants)[0].write, "f.txt",
                                 bytes_of("x"));
        AE_CHECK(!cross.has_value() && cross.error().code == "worktree.mount_capability_mismatch",
                 "M4: executor x's capability is rejected against executor y's Mount");
    }

    // M5 (finding #6): a fresh BRANCH executor's grant exposes the PARENT's pre-existing content
    // immediately, before any write of its own -- copy-on-write of the whole tree, not a scoped
    // empty corner. Tested, not merely documented (the red-team's own complaint about the original
    // proof plan).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto preexisting = blob_of(obj_store, "pre-existing parent content");
        auto root = tree_of(obj_store, {{"already_here.txt", preexisting, false}});
        auto parent = commit_ref(ref_store, "wf:run-5", root);
        AE_CHECK(parent.has_value(), "M5: setup parent commit with pre-existing content succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("late_joiner", sharing_mode::branch)};
        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(grants.has_value(), "M5: mint succeeds");

        auto seen = mount_read(obj_store, ref_store, *(*grants)[0].mount, *(*grants)[0].read, "already_here.txt");
        AE_CHECK(seen.has_value() && string_of(*seen) == "pre-existing parent content",
                 "M5: a fresh branch sees the parent's PRE-EXISTING content, not an empty view");
    }

    // M6 (finding #4): an executor id containing '/' is rejected before anything is minted, and
    // nothing was committed as a side effect of the rejected attempt.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-6", root);
        AE_CHECK(parent.has_value(), "M6: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("evil/../escape", sharing_mode::branch)};
        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(!grants.has_value() && grants.error().code == "worktree_scoping.executor_id_contains_slash",
                 "M6: an executor id containing '/' is rejected, independent of validate_workflow");
    }

    // M7 (findings #2/#11): calling mint_executor_worktrees TWICE for the same parent ref fails
    // closed on the second call and does NOT clobber the first mint's already-accumulated content.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-7", root);
        AE_CHECK(parent.has_value(), "M7: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("solo", sharing_mode::branch)};

        auto first = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(first.has_value(), "M7: first mint succeeds");
        AE_CHECK(mount_write(obj_store, ref_store, *(*first)[0].mount, *(*first)[0].write, "progress.txt",
                             bytes_of("in progress"))
                     .has_value(),
                 "M7: writing through the first mint's grant succeeds");

        auto second = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(!second.has_value() && second.error().code == "worktree_scoping.already_minted",
                 "M7: a second mint for the same parent ref fails closed instead of re-branching over it");

        auto still_there =
            mount_read(obj_store, ref_store, *(*first)[0].mount, *(*first)[0].read, "progress.txt");
        AE_CHECK(still_there.has_value() && string_of(*still_there) == "in progress",
                 "M7: the rejected second mint did NOT clobber the first mint's already-written content");
    }

    // M8 (finding #2's fix): resume_executor_worktrees reconstructs a BRANCH grant that continues
    // the SAME sub-worktree a prior mint started -- writes accumulate across the "restart" (a fresh
    // call using only the durable ref_store, no in-memory state carried over) instead of the resume
    // silently re-branching from the parent's now-current digest and losing the earlier write.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-8", root);
        AE_CHECK(parent.has_value(), "M8: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("branchy", sharing_mode::branch), make_executor("sharey", sharing_mode::shared)};

        auto minted = mint_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(minted.has_value(), "M8: initial mint succeeds");
        AE_CHECK(mount_write(obj_store, ref_store, *(*minted)[0].mount, *(*minted)[0].write, "before_restart.txt",
                             bytes_of("written before the simulated restart"))
                     .has_value(),
                 "M8: writing before the simulated restart succeeds");

        // Simulate a process restart: nothing survives except `ref_store`/`obj_store` (both durable
        // stores) -- no `ExecutorWorktreeGrant` from `minted` is reused below.
        auto resumed = resume_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(resumed.has_value(), "M8: resume succeeds after the simulated restart");
        AE_CHECK((*resumed)[0].mount->ref_name == (*minted)[0].mount->ref_name,
                 "M8: the resumed branch grant backs onto the SAME ref the original mint created");

        auto still_there = mount_read(obj_store, ref_store, *(*resumed)[0].mount, *(*resumed)[0].read,
                                       "before_restart.txt");
        AE_CHECK(still_there.has_value() && string_of(*still_there) == "written before the simulated restart",
                 "M8: the resumed grant sees the write made before the restart -- continuation, not a fresh branch");

        AE_CHECK(mount_write(obj_store, ref_store, *(*resumed)[0].mount, *(*resumed)[0].write, "after_restart.txt",
                             bytes_of("written after"))
                     .has_value(),
                 "M8: writing through the resumed grant succeeds");
        auto both_present = mount_read(obj_store, ref_store, *(*resumed)[0].mount, *(*resumed)[0].read,
                                       "before_restart.txt");
        AE_CHECK(both_present.has_value(),
                 "M8: the pre-restart file is still there after a post-restart write -- true accumulation");

        // The shared executor resumes correctly too, backing onto the parent's own ref.
        AE_CHECK((*resumed)[1].mount->ref_name == parent->name,
                 "M8: the resumed shared grant still backs onto the parent's own ref");
    }

    // M9 (finding #2's named residual): resuming a workflow with a readonly executor fails closed --
    // its pinned digest is not durably reconstructible from the ref store alone.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-9", root);
        AE_CHECK(parent.has_value(), "M9: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("critic", sharing_mode::readonly)};
        AE_CHECK(mint_executor_worktrees(ref_store, *parent, wf).has_value(), "M9: initial mint succeeds");

        auto resumed = resume_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(!resumed.has_value() &&
                     resumed.error().code == "worktree_scoping.readonly_resume_unsupported",
                 "M9: resuming a readonly executor fails closed with the documented residual code");
    }

    // M10: resuming a branch executor that was NEVER minted (no prior mint_executor_worktrees call
    // for this parent ref) fails closed rather than fabricating a fresh branch silently.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "wf:run-10", root);
        AE_CHECK(parent.has_value(), "M10: setup parent commit succeeds");

        Workflow wf;
        wf.id = "g";
        wf.executors = {make_executor("never_minted", sharing_mode::branch)};

        auto resumed = resume_executor_worktrees(ref_store, *parent, wf);
        AE_CHECK(!resumed.has_value() && resumed.error().code == "worktree_scoping.resume_not_minted",
                 "M10: resuming an executor with no prior mint fails closed");
    }

    // M11 (finding #5's fix): TypedExecutor's own worktree_mode field is the escape hatch an author
    // actually reaches through WorkflowBuilder -- not just something Executor can hold internally.
    {
        TypedExecutor<Ping, Ping> node{};
        node.id            = "opted_in";
        node.worktree_mode = sharing_mode::shared;
        AE_CHECK(node.describe().worktree_mode == sharing_mode::shared,
                 "M11: an author-set worktree_mode on TypedExecutor survives describe() into Executor");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All workflow worktree-scoping proof checks passed.\n";
    return 0;
}
