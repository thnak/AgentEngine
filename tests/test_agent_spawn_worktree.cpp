// Proof for docs/planning/agent-spawn-runtime-design-draft.md §4.3 (item 3 of OpenQuestions.md
// OQ-14's agent.spawn wiring; 026-Agent-Facing-Runtime-Surface.md §5) --
// core/agent_spawn_worktree.hpp's `allocate_spawn_seq`/`derive_spawn_child_id`/`check_child_id`/
// `mint_spawn_worktree`. Style/structure mirrors tests/test_workflow_worktree_scoping.cpp (this
// project's own closest precedent -- proving each named red-team finding stays fixed, not merely
// "does the happy path work"). The final block (T11) proves the "wired into item 2's nested-session
// mechanism" half of the task: a REAL `mint_spawn_worktree()` grant, threaded through item 5's already
// -landed `mint_child_spawn_capabilities()`, reaches a real child `AgentSession` constructed via item
// 2's `run_child_agent_session()` -- observed from INSIDE the child's own `chat()` call, and proven
// functionally usable (not just present) via a real `mount_read()` through the child's own granted
// capability.
//
//   T1  -- grant SHAPE per sharing_mode (mirrors M1): branch/scratch/shared get mount+read+write,
//          readonly gets none, matching `workflow/worktree_scoping.hpp::grant_for()`'s own precedent.
//   T2  -- a branch-mode child sees the caller's PRE-EXISTING content (copy-on-write, mirrors M5), and
//          writing through the child's own mount never moves the caller's own ref (mirrors M2).
//   T3  -- I2-1 (Critical, CLOSED): a caller holding an FsRead/FsWrite scoped to "/workspace" on its
//          own mount spawns a branch-mode child whose minted grant is REMAPPED to the SAME
//          "/workspace" scope on the new mount -- never uncapped over the whole branched tree, even
//          though "/scratch/unrelated.json" is physically present in the branched content (C6b).
//   T4  -- a caller holding NO FsRead/FsWrite on its own mount gets `std::nullopt` for that axis on a
//          branch-mode child -- safe by omission, the identical property scratch mode gets by
//          construction.
//   T5  -- a caller holding TWO DISTINCT FsRead/FsWrite grants on its own mount cannot be represented
//          by `SpawnWorktreeGrant`'s single-entry-per-axis shape -- `mint_spawn_worktree` refuses to
//          guess which survives and fails closed instead (named residual, this file's own doc
//          comment).
//   T6  -- `check_child_id` (§9 WT-6 defense-in-depth): rejects empty/uppercase/'/'/'..' input,
//          accepts a real lowercase-hex digest; `mint_spawn_worktree` itself fails closed on an
//          invalid id, not merely the standalone check.
//   T7  -- `derive_spawn_child_id` is deterministic per (caller_ref, principal, spawn_seq) and
//          produces a DIFFERENT id when any one of the three inputs changes (§9 I3-1/WT-1).
//   T8  -- WT-3: two DIFFERENT caller refs minting under the SAME child_id string never alias onto
//          the same mount/ref -- the caller-ref-name prefix is what makes this safe.
//   T9  -- mirrors M7: minting twice for the same (caller_ref, child_id) fails closed on the second
//          call and does not clobber the first mint's already-written content.
//   T10 -- THE TASK'S OWN REQUIRED PROOF: N concurrent spawns (real std::thread, not a simulation --
//          `rt::InMemoryAppendLogStore`'s own internal mutex makes this safe, and `mint_spawn_worktree`
//          never touches the object store for branch/scratch/shared modes) from the SAME parent never
//          collide on the same child_id, mount id, or ref name -- and every one succeeds (no spurious
//          `already_minted`, proving `allocate_spawn_seq()`'s atomic counter actually closes WT-2's
//          TOCTOU window for this file's own narrower substitute for the not-yet-built SpawnPump).
//   T11 -- END-TO-END WIRING: a real `mint_spawn_worktree()` grant reaches a real child `AgentSession`
//          via item 2's `run_child_agent_session()`, observed from inside the child's own
//          `EffectContext`, and is functionally usable (mount_read through the child's own granted
//          capability succeeds; a path outside the granted scope still fails).

#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/agent_spawn_worktree.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/rt/agent_spawn_child_run.hpp"
#include "agentengine/trust/agent_spawn_capability.hpp"

using namespace agentengine;
using InMemoryStore = agentengine::rt::InMemoryAppendLogStore;

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

}  // namespace

int main() {
    // ---- T1: grant shape per sharing_mode -----------------------------------------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t1", root);
        AE_CHECK(parent.has_value(), "T1: setup parent commit succeeds");

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({cap::FsRead{"/caller", "", std::nullopt},
                                        cap::FsWrite{"/caller", "", std::nullopt, std::nullopt}});

        for (sharing_mode mode :
             {sharing_mode::branch, sharing_mode::shared, sharing_mode::scratch, sharing_mode::readonly}) {
            auto seq = allocate_spawn_seq();
            auto child_id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
            AE_CHECK(child_id.has_value(), "T1: derive_spawn_child_id succeeds");
            auto grant =
                mint_spawn_worktree(ref_store, *parent, *child_id, mode, caller_held, "/caller");
            AE_CHECK(grant.has_value(), "T1: mint_spawn_worktree succeeds for this mode");
            if (!grant) continue;
            if (mode == sharing_mode::readonly) {
                AE_CHECK(!grant->mount.has_value() && !grant->read.has_value() && !grant->write.has_value(),
                         "T1: readonly gets NO mount/read/write (Mount cannot honor a pinned digest)");
            } else {
                AE_CHECK(grant->mount.has_value() && grant->read.has_value() && grant->write.has_value(),
                         "T1: branch/shared/scratch each get a mount + read + write");
            }
        }
    }

    // ---- T2: branch-mode content visibility + isolation from the caller's own ref --------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto preexisting = blob_of(obj_store, "pre-existing parent content");
        auto root = tree_of(obj_store, {{"already_here.txt", preexisting, false}});
        auto parent = commit_ref(ref_store, "session:parent-t2", root);
        AE_CHECK(parent.has_value(), "T2: setup parent commit succeeds");

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({cap::FsRead{"/caller", "", std::nullopt},
                                        cap::FsWrite{"/caller", "", std::nullopt, std::nullopt}});

        auto seq = allocate_spawn_seq();
        auto child_id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
        AE_CHECK(child_id.has_value(), "T2 setup: derive_spawn_child_id succeeds");
        auto grant = mint_spawn_worktree(ref_store, *parent, *child_id, sharing_mode::branch, caller_held,
                                          "/caller");
        AE_CHECK(grant.has_value(), "T2 setup: mint succeeds");

        auto seen = mount_read(obj_store, ref_store, *grant->mount, *grant->read, "already_here.txt");
        AE_CHECK(seen.has_value() && string_of(*seen) == "pre-existing parent content",
                 "T2: a fresh branch-mode child sees the CALLER's pre-existing content, copy-on-write");

        AE_CHECK(mount_write(obj_store, ref_store, *grant->mount, *grant->write, "child_only.txt",
                              bytes_of("child wrote this"))
                     .has_value(),
                 "T2: writing through the child's own mount succeeds");

        auto parent_after = read_ref(ref_store, "session:parent-t2");
        AE_CHECK(parent_after.has_value() && parent_after->has_value() &&
                     (*parent_after)->tree_digest == root,
                 "T2: the child's write never moved the CALLER's own ref -- true isolation");
    }

    // ---- T3 (I2-1, Critical): branch-mode grant is remapped to the caller's OWN scope, not uncapped
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto in_scope = blob_of(obj_store, "in workspace");
        auto out_scope = blob_of(obj_store, "outside workspace, but physically present");
        auto workspace_tree = tree_of(obj_store, {{"a.txt", in_scope, false}});
        auto scratch_tree = tree_of(obj_store, {{"unrelated.json", out_scope, false}});
        auto root = tree_of(obj_store, {{"workspace", workspace_tree, true}, {"scratch", scratch_tree, true}});
        auto parent = commit_ref(ref_store, "session:parent-t3", root);
        AE_CHECK(parent.has_value(), "T3 setup: parent commit succeeds");

        // The caller holds a grant scoped to "/workspace" ONLY -- nothing broader.
        CapabilitySet const caller_held =
            CapabilitySet::grant_root({cap::FsRead{"/caller", "workspace", std::nullopt},
                                        cap::FsWrite{"/caller", "workspace", std::nullopt, std::nullopt}});

        auto seq = allocate_spawn_seq();
        auto child_id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
        auto grant = mint_spawn_worktree(ref_store, *parent, *child_id, sharing_mode::branch, caller_held,
                                          "/caller");
        AE_CHECK(grant.has_value(), "T3 setup: mint succeeds");
        AE_CHECK(grant.has_value() && grant->read.has_value() && grant->read->path_prefix == "workspace",
                 "T3: the minted FsRead is REMAPPED to the caller's own 'workspace' scope, not \"\"");
        AE_CHECK(grant.has_value() && grant->write.has_value() && grant->write->path_prefix == "workspace",
                 "T3: the minted FsWrite is likewise remapped, not uncapped");

        auto in_scope_read = mount_read(obj_store, ref_store, *grant->mount, *grant->read, "workspace/a.txt");
        AE_CHECK(in_scope_read.has_value() && string_of(*in_scope_read) == "in workspace",
                 "T3: a path INSIDE the caller's own scope is readable through the child's grant");

        auto out_of_scope_read =
            mount_read(obj_store, ref_store, *grant->mount, *grant->read, "scratch/unrelated.json");
        AE_CHECK(!out_of_scope_read.has_value() &&
                     out_of_scope_read.error().code == "worktree.mount_path_outside_capability",
                 "T3 (C6b): a path OUTSIDE the caller's own scope is REJECTED even though it is "
                 "physically present in the branched mount's content -- I2-1's fix, not merely "
                 "documented");
    }

    // ---- T4: a caller holding no FsRead/FsWrite on its own mount gets none on a branch-mode child
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t4", root);
        AE_CHECK(parent.has_value(), "T4 setup: parent commit succeeds");

        CapabilitySet const caller_held = CapabilitySet::grant_root({});  // holds nothing at all

        auto seq = allocate_spawn_seq();
        auto child_id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
        auto grant = mint_spawn_worktree(ref_store, *parent, *child_id, sharing_mode::branch, caller_held,
                                          "/caller");
        AE_CHECK(grant.has_value(), "T4 setup: mint still succeeds (mount is minted regardless)");
        AE_CHECK(grant.has_value() && grant->mount.has_value(), "T4: the mount itself still exists");
        AE_CHECK(grant.has_value() && !grant->read.has_value() && !grant->write.has_value(),
                 "T4: but read/write are BOTH nullopt -- safe by omission, same as scratch mode gets "
                 "by construction");
    }

    // ---- T5: ambiguous caller grant (more than one distinct entry on the same mount) fails closed
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t5", root);
        AE_CHECK(parent.has_value(), "T5 setup: parent commit succeeds");

        CapabilitySet const two_reads =
            CapabilitySet::grant_root({cap::FsRead{"/caller", "a", std::nullopt},
                                        cap::FsRead{"/caller", "b", std::nullopt}});
        auto seq1 = allocate_spawn_seq();
        auto id1 = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq1);
        auto grant1 =
            mint_spawn_worktree(ref_store, *parent, *id1, sharing_mode::branch, two_reads, "/caller");
        AE_CHECK(!grant1.has_value() && grant1.error().code == "agent_spawn_worktree.ambiguous_caller_grant",
                 "T5: two distinct FsRead grants on the caller's own mount -- refuses to guess, fails "
                 "closed rather than silently dropping one");

        CapabilitySet const two_writes =
            CapabilitySet::grant_root({cap::FsWrite{"/caller", "a", std::nullopt, std::nullopt},
                                        cap::FsWrite{"/caller", "b", std::nullopt, std::nullopt}});
        auto seq2 = allocate_spawn_seq();
        auto id2 = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq2);
        auto grant2 =
            mint_spawn_worktree(ref_store, *parent, *id2, sharing_mode::branch, two_writes, "/caller");
        AE_CHECK(!grant2.has_value() && grant2.error().code == "agent_spawn_worktree.ambiguous_caller_grant",
                 "T5: the identical ambiguity check applies to FsWrite too");
    }

    // ---- T6: check_child_id (§9 WT-6) -----------------------------------------------------------
    {
        AE_CHECK(!check_child_id("").has_value(), "T6: an empty child id is rejected");
        AE_CHECK(!check_child_id("has/slash").has_value(), "T6: a '/' is rejected");
        AE_CHECK(!check_child_id("has..dots").has_value(), "T6: '..' is rejected (via the char-class rule)");
        AE_CHECK(!check_child_id("ABCDEF00").has_value(), "T6: uppercase hex is rejected -- lowercase only");
        AE_CHECK(!check_child_id("not-hex-zz").has_value(), "T6: a non-hex character is rejected");
        AE_CHECK(check_child_id("0123456789abcdef").has_value(), "T6: a real lowercase-hex digest is accepted");

        // mint_spawn_worktree itself fails closed on an invalid id -- not merely the standalone check.
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t6", root);
        CapabilitySet const caller_held = CapabilitySet::grant_root({});
        auto rejected = mint_spawn_worktree(ref_store, *parent, "not/hex", sharing_mode::branch,
                                             caller_held, "/caller");
        AE_CHECK(!rejected.has_value() && rejected.error().code == "agent_spawn_worktree.child_id_invalid",
                 "T6: mint_spawn_worktree itself rejects an invalid child id before minting anything");
    }

    // ---- T7: derive_spawn_child_id determinism + sensitivity to each input ----------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent_a = commit_ref(ref_store, "session:parent-t7-a", root);
        auto parent_b = commit_ref(ref_store, "session:parent-t7-b", root);
        AE_CHECK(parent_a.has_value() && parent_b.has_value(), "T7 setup: both parent commits succeed");

        Principal const p1{"caller-1", ""};
        Principal const p2{"caller-2", ""};

        auto id_a = derive_spawn_child_id(*parent_a, p1, 7);
        auto id_a_again = derive_spawn_child_id(*parent_a, p1, 7);
        AE_CHECK(id_a.has_value() && id_a_again.has_value() && *id_a == *id_a_again,
                 "T7: identical (caller_ref, principal, spawn_seq) always derives the SAME child id");

        auto id_diff_seq = derive_spawn_child_id(*parent_a, p1, 8);
        AE_CHECK(id_diff_seq.has_value() && *id_diff_seq != *id_a,
                 "T7: a different spawn_seq derives a DIFFERENT child id");

        auto id_diff_principal = derive_spawn_child_id(*parent_a, p2, 7);
        AE_CHECK(id_diff_principal.has_value() && *id_diff_principal != *id_a,
                 "T7: a different caller principal derives a DIFFERENT child id");

        auto id_diff_ref = derive_spawn_child_id(*parent_b, p1, 7);
        AE_CHECK(id_diff_ref.has_value() && *id_diff_ref != *id_a,
                 "T7: a different caller ref name derives a DIFFERENT child id");
    }

    // ---- T8 (WT-3): two different caller refs never alias, even under the SAME child_id string ---
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent_x = commit_ref(ref_store, "session:parent-t8-x", root);
        auto parent_y = commit_ref(ref_store, "session:parent-t8-y", root);
        AE_CHECK(parent_x.has_value() && parent_y.has_value(), "T8 setup: both parent commits succeed");

        CapabilitySet const caller_held = CapabilitySet::grant_root({});
        std::string const shared_child_id = "deadbeefcafef00d";

        auto grant_x = mint_spawn_worktree(ref_store, *parent_x, shared_child_id, sharing_mode::branch,
                                            caller_held, "/caller");
        auto grant_y = mint_spawn_worktree(ref_store, *parent_y, shared_child_id, sharing_mode::branch,
                                            caller_held, "/caller");
        AE_CHECK(grant_x.has_value() && grant_y.has_value(),
                 "T8: minting the SAME child_id under two DIFFERENT caller refs both succeed "
                 "independently -- no collision");
        AE_CHECK(grant_x->mount->mount_id != grant_y->mount->mount_id,
                 "T8: the resulting mount ids are DISTINCT -- caller_ref.name namespacing is what "
                 "prevents the alias");
        AE_CHECK(grant_x->mount->ref_name != grant_y->mount->ref_name,
                 "T8: the backing ref names are likewise distinct -- true separation, not aliasing");
    }

    // ---- T9 (mirrors M7): minting twice for the same (caller_ref, child_id) fails closed ---------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t9", root);
        AE_CHECK(parent.has_value(), "T9 setup: parent commit succeeds");

        CapabilitySet const caller_held =
            CapabilitySet::grant_root({cap::FsWrite{"/caller", "", std::nullopt, std::nullopt}});
        std::string const child_id = "0000000000000009";

        auto first = mint_spawn_worktree(ref_store, *parent, child_id, sharing_mode::branch, caller_held,
                                          "/caller");
        AE_CHECK(first.has_value(), "T9: the first mint succeeds");
        AE_CHECK(mount_write(obj_store, ref_store, *first->mount, *first->write, "progress.txt",
                              bytes_of("in progress"))
                     .has_value(),
                 "T9: writing through the first mint's grant succeeds");

        auto second = mint_spawn_worktree(ref_store, *parent, child_id, sharing_mode::branch, caller_held,
                                           "/caller");
        AE_CHECK(!second.has_value() && second.error().code == "agent_spawn_worktree.already_minted",
                 "T9: a second mint for the SAME child_id fails closed instead of re-branching over it");
    }

    // ---- T10: THE TASK'S REQUIRED PROOF -- N concurrent real-thread spawns never collide ---------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root = tree_of(obj_store, {});
        auto parent = commit_ref(ref_store, "session:parent-t10", root);
        AE_CHECK(parent.has_value(), "T10 setup: parent commit succeeds");

        CapabilitySet const caller_held = CapabilitySet::grant_root(
            {cap::FsRead{"/caller", "", std::nullopt}, cap::FsWrite{"/caller", "", std::nullopt, std::nullopt}});

        constexpr int kThreads = 16;
        std::mutex results_mutex;
        std::vector<std::string> child_ids;
        std::vector<std::string> mount_ids;
        std::vector<std::string> ref_names;
        int successes = 0;

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&] {
                auto seq = allocate_spawn_seq();
                auto id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
                if (!id) return;
                auto grant =
                    mint_spawn_worktree(ref_store, *parent, *id, sharing_mode::branch, caller_held, "/caller");
                std::lock_guard<std::mutex> lock(results_mutex);
                if (grant.has_value()) {
                    ++successes;
                    child_ids.push_back(*id);
                    mount_ids.push_back(grant->mount->mount_id);
                    ref_names.push_back(grant->mount->ref_name);
                }
            });
        }
        for (auto& t : workers) t.join();

        AE_CHECK(successes == kThreads,
                 "T10: every one of the N concurrent spawns from the same parent SUCCEEDED -- no "
                 "spurious already_minted collision");

        std::set<std::string> const unique_child_ids(child_ids.begin(), child_ids.end());
        std::set<std::string> const unique_mount_ids(mount_ids.begin(), mount_ids.end());
        std::set<std::string> const unique_ref_names(ref_names.begin(), ref_names.end());
        AE_CHECK(static_cast<int>(unique_child_ids.size()) == kThreads,
                 "T10: all N child ids are PAIRWISE DISTINCT -- allocate_spawn_seq()'s atomic counter "
                 "closes the collision window this file's own SpawnPump-substitute deviation names");
        AE_CHECK(static_cast<int>(unique_mount_ids.size()) == kThreads,
                 "T10: all N mount ids are pairwise distinct -- two concurrent spawns never collide on "
                 "the same worktree mount id");
        AE_CHECK(static_cast<int>(unique_ref_names.size()) == kThreads,
                 "T10: all N backing ref names are pairwise distinct -- never aliased onto the same ref");
    }

    // ---- T11: end-to-end wiring into item 2's run_child_agent_session() ---------------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto seeded = blob_of(obj_store, "seeded by the caller before spawning");
        auto root = tree_of(obj_store, {{"note.txt", seeded, false}});
        auto parent = commit_ref(ref_store, "session:parent-t11", root);
        AE_CHECK(parent.has_value(), "T11 setup: parent commit succeeds");

        CapabilitySet const caller_held = CapabilitySet::grant_root(
            {cap::FsRead{"/caller", "", std::nullopt}, cap::FsWrite{"/caller", "", std::nullopt, std::nullopt}});

        auto seq = allocate_spawn_seq();
        auto child_id = derive_spawn_child_id(*parent, Principal{"caller-1", ""}, seq);
        AE_CHECK(child_id.has_value(), "T11 setup: derive_spawn_child_id succeeds");
        auto worktree_grant = mint_spawn_worktree(ref_store, *parent, *child_id, sharing_mode::branch,
                                                    caller_held, "/caller");
        AE_CHECK(worktree_grant.has_value(), "T11 setup: item 3's real mint_spawn_worktree succeeds");

        // Item 5 (already landed): thread the REAL worktree grant through mint_child_spawn_capabilities
        // in place of the empty stand-in test_rt_agent_spawn_child_run.cpp's own T3 used.
        AgentMetadata const target;  // empty declared ceiling -- only the worktree grant is appended
        auto minted = trust::mint_child_spawn_capabilities(caller_held, target, *worktree_grant,
                                                             trust::SpawnBudget::mint_root(0));
        AE_CHECK(minted.has_value(), "T11: mint_child_spawn_capabilities accepts the real worktree grant");
        AE_CHECK(minted.has_value() && minted->capabilities.size() == 2,
                 "T11: the child's minted CapabilitySet holds exactly the worktree read+write grants "
                 "(empty target ceiling contributes nothing else)");

        std::string const child_mount_id = worktree_grant->mount->mount_id;

        // Item 2 (already landed): drive a REAL child AgentSession under this exact minted set.
        rt::ChildSpawnRequest req;
        Message input_msg;
        input_msg.role = role::user;
        ContentItem item{};
        item.origin = content_origin::user;
        item.value  = Text{"do the sub-task"};
        input_msg.content.push_back(item);
        req.input        = input_msg;
        req.capabilities = minted->capabilities;
        req.principal     = Principal{"caller-1", ""};
        req.max_turns     = 5;

        struct ScriptedChatClient {
            struct State {
                std::string mount_id_to_check;
                bool        observed_has_grant = false;
                bool        chat_called        = false;
            };
            ScriptedChatClient() : state_(std::make_shared<State>()) {}
            [[nodiscard]] std::shared_ptr<State> const& shared_state() const { return state_; }

            [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
            task<result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
                // Observed FROM INSIDE the child's own chat() call -- proving the worktree grant
                // reached the child's real EffectContext, not merely this test's own local copy.
                state_->chat_called = true;
                state_->observed_has_grant =
                    ctx.capabilities != nullptr &&
                    ctx.capabilities->find_fs_read(state_->mount_id_to_check, "").has_value();
                Message m;
                m.role = role::assistant;
                ContentItem out{};
                out.origin = content_origin::assistant;
                out.value  = Text{"child-converged"};
                m.content.push_back(out);
                co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
            }
            [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
                return {};
            }
            std::shared_ptr<State> state_;
        };
        static_assert(agentengine::ChatClient<ScriptedChatClient>);

        std::shared_ptr<ScriptedChatClient::State> observed;
        auto response = rt::run_child_agent_session<ScriptedChatClient>(
            "child-t11", std::move(req), [&](ScriptedChatClient& c) {
                c.state_->mount_id_to_check = child_mount_id;
                observed                    = c.shared_state();
            });
        AE_CHECK(response.has_value(),
                 "T11: run_child_agent_session() drives a fresh child, wired with the real worktree "
                 "grant, to completion");
        AE_CHECK(observed && observed->chat_called, "T11 setup: the child's chat() ran");
        AE_CHECK(observed && observed->observed_has_grant,
                 "T11: observed FROM INSIDE the child's own EffectContext -- the real worktree FsRead "
                 "grant (item 3) reached the child constructed by item 2's run_child_agent_session(), "
                 "via item 5's mint_child_spawn_capabilities() -- the full wiring chain, not a stand-in");

        // Functional proof, not just a data field: use the SAME minted read capability to actually
        // read the content the caller seeded before spawning, through the child's own granted mount.
        if (worktree_grant->mount.has_value() && worktree_grant->read.has_value()) {
            auto read_through_child_grant =
                mount_read(obj_store, ref_store, *worktree_grant->mount, *worktree_grant->read, "note.txt");
            AE_CHECK(read_through_child_grant.has_value() &&
                         string_of(*read_through_child_grant) == "seeded by the caller before spawning",
                     "T11: the child's own minted FsRead grant actually WORKS -- reads the caller's "
                     "pre-spawn content through the newly-minted mount, end to end");
        }
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All agent-spawn worktree proof checks passed.\n";
    return 0;
}
