// PROVE-PHASE PROBE (A10 dispatch follow-up, 2026-08-28): the real, live proof that
// `task_branch_dispatch.hpp`'s `dispatch_tool_call()` function genuinely authorizes BEFORE invoking -- that a
// call the two-tag gating logic rejects never reaches the real, mutating `TaskBranchSandbox` verb at
// all, and that a call it accepts really does reach it, against a REAL `Ledger`/`SandboxRuntime` stack
// and live Docker (matching every other probe in this design's own bar -- no mocks). Run against a REAL
// Docker daemon.
//
// This upgrades `probe_task_branch_capability_enforcement.cpp`'s already-proven claim (the two-tag
// GATING BEHAVIOR is correct, checked via a pure boolean `try_invoke()` that never calls anything real)
// by one more level of end-to-end-ness: same gating algorithm, now sitting in front of a real dispatch
// path into a real backing object, with real observable state (Ledger head tree digest, real Docker
// container count, TaskBranchSandbox::active_count()/has_active_handle()) checked before and after a
// rejection to confirm the rejected call had genuinely NO effect -- not merely that the boolean check
// returned false.
//
// STILL NOT THE REAL agentengine::Tool<>. See task_branch_dispatch.hpp's own header comment for the
// exact, unchanged negative result this file does not re-attempt or claim to have closed.

#include "task_branch_dispatch.hpp"

#include "../execution_surface/docker_execution_surface.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                       \
        }                                                                                        \
    } while (0)

using namespace probe;
using namespace probe::enforcement;
using namespace probe::dispatch;

namespace {

template <class T>
T run(agentengine::rt::task<T> t) {
    t.resume();
    return t.take_value();
}

bool tree_has_file(Ledger<>& ledger, agentengine::Digest const& tree_digest, Principal owner,
                     std::string const& name, std::string const& expect_content) {
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

std::size_t docker_container_count() {
    auto r = docker_detail::run_capture("docker ps -a -q");
    return static_cast<std::size_t>(std::count(r.stdout_text.begin(), r.stdout_text.end(), '\n'));
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("task-branch-dispatch-probe-owner");
    Ledger<> ledger;
    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 50'000'000);
    CHECK(storage_quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());
    auto branch_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    CHECK(branch_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());

    std::filesystem::path const staging =
        std::filesystem::temp_directory_path() / "ae_task_branch_dispatch_probe";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);

    SandboxRuntime main_runtime(ledger, std::move(*root_r), staging / "main");
    DockerExecutionSurface surface;
    TaskBranchSandbox<DockerExecutionSurface> session(
        main_runtime, surface, owner, *branch_quota, *run_quota, *storage_quota,
        staging / "task_branches");

    // === [D1]-[D4]: a caller holding ONLY {TaskBranch} =================================================
    MirroredCapabilitySet const held_isolated_only = MirroredCapabilitySet::grant({cap::TaskBranch{}});

    // [D1] start_task_branch, dispatched through the mirrored pipeline, reaches the REAL sandbox verb.
    auto d_start = run(dispatch_tool_call<TaskBranchStartTool>(
        held_isolated_only, session, TaskBranchStartArgs{"dispatch harness: try adding dispatch.txt"}));
    CHECK(d_start.has_value());
    CHECK(session.active_count() == 1);
    std::string const handle_id = d_start->handle_id;
    std::printf("[D1] TaskBranch-only dispatch of TaskBranchStartTool reached the REAL "
                "TaskBranchSandbox::start_task_branch() -- a real, isolated child branch was created "
                "(handle=%s) -- PASS\n", handle_id.c_str());

    // [D2] run_in_task_branch, same caller, same dispatch path -- a real command in a real container.
    auto d_run = run(dispatch_tool_call<TaskBranchRunTool>(
        held_isolated_only, session,
        TaskBranchRunArgs{handle_id, "echo -n 'dispatch-v1' > dispatch.txt && cat dispatch.txt"}));
    CHECK(d_run.has_value());
    CHECK(d_run->exit_code == 0);
    CHECK(d_run->stdout_text.find("dispatch-v1") != std::string::npos);
    std::printf("[D2] TaskBranch-only dispatch of TaskBranchRunTool reached the REAL "
                "TaskBranchSandbox::run_in_task_branch() -- a real command ran in a real container and "
                "wrote a real file on the child branch -- PASS\n");

    // [D3] commit_task_branch: the SAME caller's dispatch is REJECTED -- proven to happen BEFORE
    // TaskBranchSandbox::commit_task_branch() is ever called, not merely that a boolean said no.
    auto main_head_before = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_before.has_value());
    std::size_t const containers_before_commit_attempt = docker_container_count();

    auto d_commit_denied =
        run(dispatch_tool_call<TaskBranchCommitTool>(held_isolated_only, session, TaskBranchCommitArgs{handle_id}));
    CHECK(!d_commit_denied.has_value());
    // The DISPATCH layer's own error code, distinct from "task_branch.unknown_handle" (what the real
    // sandbox itself would report for a bad handle) -- proves the rejection happened in
    // dispatch_tool_call()'s own capability loop, never inside TaskBranchSandbox::commit_task_branch()
    // at all.
    CHECK(d_commit_denied.error().code == "task_branch.dispatch_capability_not_held");

    auto main_head_after_commit_attempt = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_commit_attempt.has_value());
    CHECK(*main_head_after_commit_attempt == *main_head_before);  // byte-for-byte: no real Ledger
                                                                     // merge ever ran.
    std::size_t const containers_after_commit_attempt = docker_container_count();
    CHECK(containers_after_commit_attempt == containers_before_commit_attempt);  // defense-in-depth:
        // commit_task_branch()/Ledger::merge() never touches Docker by its own documented design
        // (sandbox_runtime.hpp's merge_into()), so this is not the primary proof here -- but checking
        // it anyway catches anything this probe does not already know about.
    CHECK(session.active_count() == 1);           // the handle was never erased -- commit_task_branch()
    CHECK(session.has_active_handle(handle_id));   // was never called, so it never got the chance to.

    // Strongest possible proof the rejected commit attempt did not even PARTIALLY mutate the handle's
    // own child-branch state: the SAME handle, dispatched again through run_in_task_branch (still
    // gated by {TaskBranch} alone, which this caller holds), still sees exactly the file [D2] wrote,
    // completely undisturbed.
    auto d_reread = run(dispatch_tool_call<TaskBranchRunTool>(
        held_isolated_only, session, TaskBranchRunArgs{handle_id, "cat dispatch.txt"}));
    CHECK(d_reread.has_value());
    CHECK(d_reread->stdout_text.find("dispatch-v1") != std::string::npos);

    std::printf("[D3] REAL ADVERSARIAL PROOF: a TaskBranch-only caller's dispatch of "
                "TaskBranchCommitTool is REJECTED (%s) by the dispatch layer's own authorize-before-\n"
                "     invoke loop -- verified as a genuine no-op, not just \"the boolean said no\": "
                "main's own head tree digest is byte-for-byte unchanged, the real Docker container "
                "count on the host is unchanged, TaskBranchSandbox::active_count()/has_active_handle() "
                "show the handle was never erased (commit_task_branch() was never entered), and a "
                "fresh dispatched run_in_task_branch() on the SAME handle afterward still reads back "
                "exactly \"dispatch-v1\", proving the rejected commit attempt left the handle's own "
                "state completely undisturbed -- PASS\n", d_commit_denied.error().code.c_str());

    // [D4] discard_task_branch: same TaskBranch-only caller, this verb IS in its ceiling.
    auto d_discard = run(
        dispatch_tool_call<TaskBranchDiscardTool>(held_isolated_only, session, TaskBranchDiscardArgs{handle_id}));
    CHECK(d_discard.has_value());
    CHECK(session.active_count() == 0);
    auto main_head_after_discard = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_discard.has_value());
    CHECK(*main_head_after_discard == *main_head_before);  // discard never touched main either.
    std::printf("[D4] TaskBranch-only dispatch of TaskBranchDiscardTool reached the REAL "
                "TaskBranchSandbox::discard_task_branch() -- the real work is thrown away, main's own "
                "head is unchanged (still identical to before [D1]) -- PASS\n");

    // === [D5]: a caller holding {TaskBranch, TaskBranchCommit} CAN commit for real. ====================
    MirroredCapabilitySet const held_both =
        MirroredCapabilitySet::grant({cap::TaskBranch{}, cap::TaskBranchCommit{}});

    auto d_start2 = run(dispatch_tool_call<TaskBranchStartTool>(
        held_both, session, TaskBranchStartArgs{"dispatch harness: committed.txt"}));
    CHECK(d_start2.has_value());
    std::string const handle_id2 = d_start2->handle_id;

    auto d_run2 = run(dispatch_tool_call<TaskBranchRunTool>(
        held_both, session,
        TaskBranchRunArgs{handle_id2, "echo -n 'committed-via-dispatch' > committed.txt"}));
    CHECK(d_run2.has_value());
    CHECK(d_run2->exit_code == 0);

    auto d_commit_ok =
        run(dispatch_tool_call<TaskBranchCommitTool>(held_both, session, TaskBranchCommitArgs{handle_id2}));
    CHECK(d_commit_ok.has_value());
    CHECK(session.active_count() == 0);
    CHECK(!session.has_active_handle(handle_id2));

    // Read the committed work back through the REAL Ledger API -- not inferred from a return code.
    auto main_head_after_real_commit = ledger.head_tree_digest(main_runtime.branch_name(), owner);
    CHECK(main_head_after_real_commit.has_value());
    CHECK(*main_head_after_real_commit != *main_head_before);  // main genuinely moved this time.
    CHECK(tree_has_file(ledger, *main_head_after_real_commit, owner, "committed.txt",
                          "committed-via-dispatch"));
    std::printf("[D5] REAL PROOF: a caller holding {TaskBranch, TaskBranchCommit} dispatches "
                "TaskBranchCommitTool successfully (turn_index=%llu) -- the real work genuinely landed "
                "in main, confirmed by reading it BACK through the real Ledger::get_tree_safe()/"
                "get_blob_safe() API (committed.txt=\"committed-via-dispatch\"), not inferred from the "
                "dispatch call's own return value -- PASS\n",
                (unsigned long long)d_commit_ok->turn_index);

    // === [D6] bonus: the "inert grant" claim (TaskBranchCommit alone) proven end-to-end, not just ====
    // === as a boolean -- a caller lacking TaskBranch entirely is rejected even for start_task_branch, =
    // === and genuinely never spends the BranchCost quota start_task_branch would have charged. =========
    MirroredCapabilitySet const held_commit_only =
        MirroredCapabilitySet::grant({cap::TaskBranchCommit{}});
    std::uint64_t const branch_quota_before = branch_quota->remaining();

    auto d_start_denied = run(dispatch_tool_call<TaskBranchStartTool>(
        held_commit_only, session, TaskBranchStartArgs{"should never reach the real sandbox"}));
    CHECK(!d_start_denied.has_value());
    CHECK(d_start_denied.error().code == "task_branch.dispatch_capability_not_held");
    CHECK(session.active_count() == 0);  // no branch was created at all.
    CHECK(branch_quota->remaining() == branch_quota_before);  // BranchCost genuinely never spent --
        // start_task_branch()'s own quota-consuming call was never reached, the same "no real side
        // effect" style probe_execution_surface.cpp's own quota-exhaustion check (check [6]) uses,
        // adapted here to the resource TaskBranchStartTool's own dispatch actually gates.
    std::printf("[D6] REAL PROOF of the \"inert grant\" claim: a caller holding TaskBranchCommit alone "
                "(no TaskBranch) is REJECTED (%s) by dispatch_tool_call() even for TaskBranchStartTool -- "
                "TaskBranchSandbox::start_task_branch() was never entered (active_count stays 0) and "
                "the BranchCost quota it would have spent is genuinely untouched (remaining=%llu, "
                "unchanged) -- PASS\n", d_start_denied.error().code.c_str(),
                (unsigned long long)branch_quota->remaining());

    std::filesystem::remove_all(staging, ec);
    std::printf("\nALL CHECKS PASSED -- the dispatch layer (task_branch_dispatch.hpp) genuinely "
                "authorizes BEFORE invoking, against a REAL TaskBranchSandbox backed by a real "
                "Ledger/SandboxRuntime stack and live Docker: a rejected call leaves no real side "
                "effect (Ledger head unchanged, Docker container count unchanged, active_count()/\n"
                "has_active_handle() unchanged, BranchCost quota unspent), and an accepted call's work "
                "is verified by reading it back through the real Ledger API. This NARROWS the gap "
                "between task_branch_capability_enforcement.hpp's pure-boolean gating proof and a real "
                "Tool<>-shaped dispatch -- it does NOT close the still-open, honestly-disclosed "
                "negative result that the real agentengine::Tool<>/Capabilities<> cannot yet carry "
                "these two tags (task_branch_capability.hpp's own header comment).\n");
    return 0;
}
