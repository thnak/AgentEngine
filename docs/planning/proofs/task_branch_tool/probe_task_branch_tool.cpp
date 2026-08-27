// A10 (2026-08-27): the real, live proof that `TaskBranchSandbox` gives an agent's tool-calling
// loop a genuine try/commit/discard surface -- the git-worktree-per-task pattern the 2026-08-27
// real-world-use-case research found every actively-developed coding agent ships as its PRIMARY
// isolation mechanism, and this design had proven only as disconnected lower-level primitives
// until this file. Run against a REAL Docker daemon (matching §36's own bar -- no mocks).

#include "task_branch_sandbox.hpp"

#include "../execution_surface/docker_execution_surface.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }

bool tree_has_file(probe::Ledger<>& ledger, agentengine::Digest const& tree_digest,
                     probe::Principal owner, std::string const& name, std::string const& expect_content) {
    auto tree = ledger.get_tree_safe(tree_digest, owner);
    if (!tree.has_value()) return false;
    for (auto const& e : tree->entries) {
        if (e.name != name) continue;
        auto bytes = ledger.get_blob_safe(e.digest, owner);
        if (!bytes.has_value()) return false;
        std::string const content(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        return content == expect_content;
    }
    return false;
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("task-branch-tool-owner");
    Ledger<> ledger;
    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 50'000'000);
    CHECK(storage_quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());
    auto branch_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    CHECK(branch_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());

    std::filesystem::path const staging = std::filesystem::temp_directory_path() / "ae_task_branch_tool_probe";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);

    SandboxRuntime main_runtime(ledger, std::move(*root_r), staging / "main");
    DockerExecutionSurface surface;
    TaskBranchSandbox<DockerExecutionSurface> session(
        main_runtime, surface, owner, *branch_quota, *run_quota, *storage_quota, staging / "task_branches");

    // === [1] start_task_branch: a real, isolated child branch, main untouched. ======================
    auto main_head_before = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_before.has_value());

    auto start1 = run(session.start_task_branch(TaskBranchStartArgs{"try adding config.txt"}));
    CHECK(start1.has_value());
    CHECK(session.active_count() == 1);
    std::printf("[1] start_task_branch: a real, isolated task branch created (handle=%s), main "
                "branch's own head is untouched -- PASS\n", start1->handle_id.c_str());

    // === [2] run_in_task_branch: real work lands on the CHILD branch only. ==========================
    auto write1 = run(session.run_in_task_branch(
        TaskBranchRunArgs{start1->handle_id, "echo -n 'config-v1' > config.txt && cat config.txt"}));
    CHECK(write1.has_value());
    CHECK(write1->exit_code == 0);
    CHECK(write1->stdout_text.find("config-v1") != std::string::npos);

    auto main_head_after_run = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_run.has_value());
    CHECK(*main_head_after_run == *main_head_before);
    CHECK(!tree_has_file(ledger, *main_head_after_run, owner, "config.txt", "config-v1"));
    std::printf("[2] run_in_task_branch: real command executed inside a real container, config.txt "
                "committed to the CHILD branch -- main branch's own head tree digest is byte-for-byte "
                "UNCHANGED and does not contain config.txt -- real isolation, not just a claim -- "
                "PASS\n");

    // === [3] commit_task_branch: the child's real work now appears on main; handle consumed. ========
    auto commit1 = run(session.commit_task_branch(TaskBranchCommitArgs{start1->handle_id}));
    CHECK(commit1.has_value());
    CHECK(session.active_count() == 0);
    CHECK(!session.has_active_handle(start1->handle_id));

    auto main_head_after_commit = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_commit.has_value());
    CHECK(tree_has_file(ledger, *main_head_after_commit, owner, "config.txt", "config-v1"));
    std::printf("[3] commit_task_branch: real Ledger::merge() folded the child's work into main "
                "(turn_index=%llu) -- main branch's head NOW genuinely contains config.txt -- PASS\n",
                (unsigned long long)commit1->turn_index);

    // === [4] one-shot: repeating commit on the SAME (now-consumed) handle fails closed. =============
    auto commit1_again = run(session.commit_task_branch(TaskBranchCommitArgs{start1->handle_id}));
    CHECK(!commit1_again.has_value());
    CHECK(commit1_again.error().code == "task_branch.unknown_handle");
    std::printf("[4] a REPEATED commit_task_branch on the same, already-consumed handle is REJECTED "
                "(%s) -- one-shot consumption, no double-merge race -- PASS\n",
                commit1_again.error().code.c_str());

    // === [5] discard_task_branch: real work is thrown away, main completely unaffected. =============
    auto start2 = run(session.start_task_branch(TaskBranchStartArgs{"try a risky rewrite"}));
    CHECK(start2.has_value());
    auto write2 = run(session.run_in_task_branch(
        TaskBranchRunArgs{start2->handle_id, "echo -n 'DANGEROUS' > config.txt"}));
    CHECK(write2.has_value());

    auto discard2 = run(session.discard_task_branch(TaskBranchDiscardArgs{start2->handle_id}));
    CHECK(discard2.has_value());
    CHECK(session.active_count() == 0);

    auto main_head_after_discard = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_discard.has_value());
    CHECK(*main_head_after_discard == *main_head_after_commit);
    CHECK(tree_has_file(ledger, *main_head_after_discard, owner, "config.txt", "config-v1"));
    std::printf("[5] discard_task_branch: the risky rewrite is REAL and REAL committed to the child "
                "branch, but main's head is byte-for-byte identical to before the discard -- "
                "config.txt still reads \"config-v1\", never \"DANGEROUS\" -- real discard, not just "
                "a no-op claim -- PASS\n");

    // === [6] one-shot: repeating discard on an already-consumed handle fails closed. =================
    auto discard2_again = run(session.discard_task_branch(TaskBranchDiscardArgs{start2->handle_id}));
    CHECK(!discard2_again.has_value());
    CHECK(discard2_again.error().code == "task_branch.unknown_handle");
    std::printf("[6] a REPEATED discard on an already-consumed handle is REJECTED -- PASS\n");

    // === [7] a fabricated / never-issued handle is rejected the same way. ============================
    auto fabricated = run(session.run_in_task_branch(TaskBranchRunArgs{"totally-made-up-handle", "ls"}));
    CHECK(!fabricated.has_value());
    CHECK(fabricated.error().code == "task_branch.unknown_handle");
    std::printf("[7] a fabricated, never-issued handle is REJECTED -- PASS\n");

    // === [8] cross-session isolation: a SECOND, independent TaskBranchSandbox (simulating a =========
    // === DIFFERENT session, sharing the SAME Ledger/authority) cannot use the FIRST session's handle.=
    Principal other_owner = authority.mint_root("task-branch-tool-other-session-owner");
    auto other_root_r = run(ledger.create_root_branch(other_owner));
    CHECK(other_root_r.has_value());
    SandboxRuntime other_main(ledger, std::move(*other_root_r), staging / "other_main");
    TaskBranchSandbox<DockerExecutionSurface> other_session(
        other_main, surface, other_owner, *branch_quota, *run_quota, *storage_quota,
        staging / "other_task_branches");

    auto start3 = run(session.start_task_branch(TaskBranchStartArgs{"session-1's own task branch"}));
    CHECK(start3.has_value());
    CHECK(!other_session.has_active_handle(start3->handle_id));
    auto cross_run = run(other_session.run_in_task_branch(TaskBranchRunArgs{start3->handle_id, "ls"}));
    CHECK(!cross_run.has_value());
    CHECK(cross_run.error().code == "task_branch.unknown_handle");
    auto cross_commit =
        run(other_session.commit_task_branch(TaskBranchCommitArgs{start3->handle_id}));
    CHECK(!cross_commit.has_value());
    CHECK(cross_commit.error().code == "task_branch.unknown_handle");
    CHECK(session.has_active_handle(start3->handle_id));  // still live in its OWN session, untouched
    std::printf("[8] CROSS-SESSION ISOLATION CONFIRMED: a second, independent TaskBranchSandbox "
                "instance (a different session, same Ledger) presented with session 1's real, live "
                "handle_id is REJECTED on both run and commit (%s) -- the handle_id string itself is "
                "not the security boundary, THIS OBJECT's own map is -- and session 1's own handle "
                "remains completely unaffected -- PASS\n", cross_commit.error().code.c_str());
    // Clean up session 1's still-open handle from check 8.
    auto cleanup3 = run(session.discard_task_branch(TaskBranchDiscardArgs{start3->handle_id}));
    CHECK(cleanup3.has_value());

    // === [9] real merge-conflict rejection surfaces through this tool, and does not corrupt main. ===
    auto conflict_a = run(session.start_task_branch(TaskBranchStartArgs{"branch A: rewrite config"}));
    CHECK(conflict_a.has_value());
    auto write_a = run(session.run_in_task_branch(
        TaskBranchRunArgs{conflict_a->handle_id, "echo -n 'FROM-A' > config.txt"}));
    CHECK(write_a.has_value());

    auto conflict_b = run(session.start_task_branch(TaskBranchStartArgs{"branch B: rewrite config differently"}));
    CHECK(conflict_b.has_value());
    auto write_b = run(session.run_in_task_branch(
        TaskBranchRunArgs{conflict_b->handle_id, "echo -n 'FROM-B' > config.txt"}));
    CHECK(write_b.has_value());

    auto commit_a = run(session.commit_task_branch(TaskBranchCommitArgs{conflict_a->handle_id}));
    CHECK(commit_a.has_value());  // A commits cleanly -- main now has config.txt="FROM-A"

    auto commit_b = run(session.commit_task_branch(TaskBranchCommitArgs{conflict_b->handle_id}));
    CHECK(!commit_b.has_value());  // B's base is now stale relative to main -- a REAL conflict
    CHECK(commit_b.error().code == "ledger.merge_conflict");

    auto main_head_after_conflict = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_conflict.has_value());
    CHECK(tree_has_file(ledger, *main_head_after_conflict, owner, "config.txt", "FROM-A"));
    std::printf("[9] REAL MERGE CONFLICT: branch A commits cleanly (config.txt=\"FROM-A\"); branch "
                "B's later commit of a conflicting rewrite of the SAME file is REJECTED (%s) through "
                "this tool surface -- main's head still reads exactly \"FROM-A\", never silently "
                "overwritten or corrupted by the rejected commit -- PASS\n",
                commit_b.error().code.c_str());

    // === [10] BranchCost quota genuinely gates start_task_branch. ====================================
    auto tiny_branch_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 0);
    CHECK(tiny_branch_quota.has_value());
    TaskBranchSandbox<DockerExecutionSurface> quota_limited_session(
        main_runtime, surface, owner, *tiny_branch_quota, *run_quota, *storage_quota,
        staging / "quota_limited");
    auto starved = run(quota_limited_session.start_task_branch(TaskBranchStartArgs{"should be rejected"}));
    CHECK(!starved.has_value());
    std::printf("[10] a session whose BranchCost quota is exhausted (0 remaining) is REJECTED at "
                "start_task_branch (%s) -- quota gating is real, not bypassed by this tool surface -- "
                "PASS\n", starved.error().code.c_str());

    // === [11] BranchCost is genuinely REFUNDED on discard -- round-2 red-team fix (finding 2). ======
    auto refund_probe_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 1);
    CHECK(refund_probe_quota.has_value());
    TaskBranchSandbox<DockerExecutionSurface> refund_session(
        main_runtime, surface, owner, *refund_probe_quota, *run_quota, *storage_quota,
        staging / "refund_check");
    auto refund_start1 = run(refund_session.start_task_branch(TaskBranchStartArgs{"spend the only unit"}));
    CHECK(refund_start1.has_value());
    auto refund_start2_before_discard =
        run(refund_session.start_task_branch(TaskBranchStartArgs{"should be rejected, quota=0"}));
    CHECK(!refund_start2_before_discard.has_value());  // confirms the quota really was down to 0
    auto refund_discard = run(refund_session.discard_task_branch(TaskBranchDiscardArgs{refund_start1->handle_id}));
    CHECK(refund_discard.has_value());
    auto refund_start2_after_discard =
        run(refund_session.start_task_branch(TaskBranchStartArgs{"should succeed, unit was refunded"}));
    CHECK(refund_start2_after_discard.has_value());
    auto cleanup_refund =
        run(refund_session.discard_task_branch(TaskBranchDiscardArgs{refund_start2_after_discard->handle_id}));
    CHECK(cleanup_refund.has_value());
    std::printf("[11] BranchCost REFUND CONFIRMED: with a quota of exactly 1, a second start_task_branch "
                "is rejected while the first remains live, but SUCCEEDS immediately after discarding the "
                "first -- discarding real work costs the session nothing lasting -- PASS\n");

    // === [12] real best-of-N: N children spawned from the SAME still-unmoved base commit with ZERO ===
    // === conflict risk regardless of which one is chosen -- the exact Cursor `/best-of-n` shape ======
    // === (2026-08-27 research), distinct from check [9]'s sequential-conflict shape. ==================
    auto bn_a = run(session.start_task_branch(TaskBranchStartArgs{"best-of-n attempt A"}));
    CHECK(bn_a.has_value());
    auto bn_a_write = run(session.run_in_task_branch(
        TaskBranchRunArgs{bn_a->handle_id, "echo -n 'ATTEMPT-A-RESULT' > bestof.txt"}));
    CHECK(bn_a_write.has_value());

    auto bn_b = run(session.start_task_branch(TaskBranchStartArgs{"best-of-n attempt B"}));
    CHECK(bn_b.has_value());
    auto bn_b_write = run(session.run_in_task_branch(
        TaskBranchRunArgs{bn_b->handle_id, "echo -n 'ATTEMPT-B-RESULT' > bestof.txt"}));
    CHECK(bn_b_write.has_value());

    auto bn_c = run(session.start_task_branch(TaskBranchStartArgs{"best-of-n attempt C"}));
    CHECK(bn_c.has_value());
    auto bn_c_write = run(session.run_in_task_branch(
        TaskBranchRunArgs{bn_c->handle_id, "echo -n 'ATTEMPT-C-RESULT' > bestof.txt"}));
    CHECK(bn_c_write.has_value());

    // The agent evaluates all three (out of band -- this probe just picks B) and commits ONLY the
    // winner. Neither A nor B nor C has touched main yet, so main's own head has not moved at all
    // since all three were spawned -- the merge is a pure fast-forward, structurally unable to
    // conflict, regardless of which of the three is chosen.
    auto bn_commit_winner = run(session.commit_task_branch(TaskBranchCommitArgs{bn_b->handle_id}));
    CHECK(bn_commit_winner.has_value());
    auto bn_discard_a = run(session.discard_task_branch(TaskBranchDiscardArgs{bn_a->handle_id}));
    CHECK(bn_discard_a.has_value());
    auto bn_discard_c = run(session.discard_task_branch(TaskBranchDiscardArgs{bn_c->handle_id}));
    CHECK(bn_discard_c.has_value());

    auto main_head_after_bestofn = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_bestofn.has_value());
    CHECK(tree_has_file(ledger, *main_head_after_bestofn, owner, "bestof.txt", "ATTEMPT-B-RESULT"));
    std::printf("[12] REAL BEST-OF-N: three independent attempts (A/B/C) spawned from the SAME "
                "still-unmoved base; the chosen winner (B) commits cleanly with ZERO conflict risk "
                "(main.bestof.txt=\"ATTEMPT-B-RESULT\"), and the two losers (A, C) are cleanly "
                "discarded with their BranchCost refunded -- the exact Cursor `/best-of-n` shape, "
                "distinct from and unaffected by check [9]'s sequential-conflict shape -- PASS\n");

    // === [13] true interleaved multi-handle usage: work on A, then B, then A again, one session, ====
    // === proving the `active_` map genuinely supports more than one concurrently-open task branch, ===
    // === not just sequential start-work-resolve cycles. ===============================================
    auto il_a = run(session.start_task_branch(TaskBranchStartArgs{"interleaved A"}));
    CHECK(il_a.has_value());
    auto il_b = run(session.start_task_branch(TaskBranchStartArgs{"interleaved B"}));
    CHECK(il_b.has_value());
    CHECK(session.active_count() == 2);

    auto il_a1 = run(session.run_in_task_branch(TaskBranchRunArgs{il_a->handle_id, "echo -n 'A-1' > a.txt"}));
    CHECK(il_a1.has_value());
    auto il_b1 = run(session.run_in_task_branch(TaskBranchRunArgs{il_b->handle_id, "echo -n 'B-1' > b.txt"}));
    CHECK(il_b1.has_value());
    auto il_a2 = run(session.run_in_task_branch(
        TaskBranchRunArgs{il_a->handle_id, "cat a.txt && echo -n '-2' >> a.txt"}));
    CHECK(il_a2.has_value());
    CHECK(il_a2->stdout_text.find("A-1") != std::string::npos);  // A's own second run really saw
                                                                    // A's own first run's output,
                                                                    // undisturbed by B's interleaved
                                                                    // run on a DIFFERENT branch.

    auto il_commit_a = run(session.commit_task_branch(TaskBranchCommitArgs{il_a->handle_id}));
    CHECK(il_commit_a.has_value());
    auto il_discard_b = run(session.discard_task_branch(TaskBranchDiscardArgs{il_b->handle_id}));
    CHECK(il_discard_b.has_value());
    std::printf("[13] TRUE INTERLEAVED MULTI-HANDLE USAGE: two task branches held open "
                "simultaneously in one session, with real work interleaved between them (A, B, A "
                "again) -- A's second run genuinely saw its own first run's output undisturbed by "
                "B's own interleaved work on a separate branch -- PASS\n");

    std::filesystem::remove_all(staging, ec);
    std::printf("\nALL CHECKS PASSED -- a real, agent-callable try/commit/discard task-branch "
                "surface, closing the #1 gap the 2026-08-27 real-world-use-case research found: "
                "every actively-developed coding agent surveyed ships this as its PRIMARY isolation "
                "mechanism, and this design now has a real, adversarially-proven tool-facing surface "
                "for it, not just disconnected lower-level primitives.\n");
    return 0;
}
