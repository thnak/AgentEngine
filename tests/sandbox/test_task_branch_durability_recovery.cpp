// Closes a residual named by ADR-114 §5/§6 and repeated unchanged through ADR-117/119/124: "`active_`'s
// table has no durability of its own -- a process crash mid-task-branch strands it from this tool
// surface's own verbs even in a durable-`Store` `Ledger` configuration, though the underlying branch
// itself survives and remains reclaimable via the lower-level orphan-reclaim API." Inherited unchanged
// from the prove-phase original's own finding 6, never fixed since.
//
// `MandatorySandboxProvider::bind_sandbox()` now calls a new `recover_orphaned_task_branches()` after
// establishing `runtime_`: it walks `Ledger::orphaned_branches()` (populated by `load_durable_state()`
// at THIS Ledger's own construction -- i.e. every branch with no live handle anywhere, because the
// process that held it just exited, cleanly or via a crash) for names matching THIS root branch's own
// deterministic child-name prefix, and reclaims each one back into `task_branches_` via the
// already-proven `SandboxRuntime::reclaim_orphaned_child()` (the SAME primitive `commit_task_branch()`'s
// own conflict-retry path already uses).
//
// Mirrors `tests/test_ledger.cpp`'s own established "destroy + reconstruct against the SAME durable_dir"
// crash-simulation methodology exactly (that file's own comment: letting a `BranchHandle`/`Ledger` go
// out of scope normally is "at least as strong a test as the real crash case," since a real process
// exit runs no destructors at all either). Docker-independent throughout -- this file never calls
// `run_in_task_branch()` (the one verb that would need a real container), so a `FakeSurface` stand-in
// (mirroring `tests/test_mandatory_sandbox_provider_composed.cpp`'s own fixture) is enough to satisfy
// `MandatorySandboxProvider<Surface>`'s own template constraint.
//
// A REAL SCOPE BOUNDARY, found empirically (not assumed), and precisely distinguished, not conflated:
// `durable_dir` persists Ledger's own BRANCH/ACL bookkeeping ONLY -- `tests/test_ledger.cpp`'s own
// header comment already discloses that a durable OBJECT-STORE conformer (real BLOB/TREE content
// durability) was never ported. A recovered task branch's own METADATA (its existence, its ACL, its
// place in `task_branches_`) survives a simulated crash; its own real tree CONTENT, held only in the
// in-memory `store_`, does not. `commit_task_branch()` on a recovered handle therefore still fails --
// not with `task_branch_unknown_handle` (this fix's own claim: the handle IS found) but with a real
// `ledger.merge_tree_load_failed` from `merge_into()`'s own attempt to load base/ours/theirs tree
// content that the crash genuinely destroyed. `discard_task_branch()` needs no tree content at all (a
// pure branch-table erase), so it is this file's own proof of the METADATA-durability claim this fix
// actually makes -- not conflated with the separate, pre-existing, already-disclosed content-durability
// gap `commit_task_branch()` would still hit.
//
//   [1] a task branch started before a simulated crash is genuinely gone from an in-memory-only
//       provider's own task_branches_ after reconstruction (the baseline this fix improves on --
//       confirms the "crash strands it" half of the residual's own claim is real, not assumed).
//   [2] TWO task branches started against a DURABLE-dir Ledger are genuinely recoverable: after a
//       simulated crash and a fresh MandatorySandboxProvider bound against a NEWLY reclaimed root
//       branch on a NEW Ledger instance (same durable_dir), discard_task_branch() on the FIRST
//       original handle_id succeeds through this tool surface's own normal verb -- no direct
//       Ledger-level orphan API call for either CHILD branch anywhere in this check, only for the root
//       (a separate, named, still-open residual of its own -- ADR-102's own disclosed "bind_branch_
//       only() is real follow-on work" -- not something this fix attempts to close).
//   [3] the SECOND recovered handle_id genuinely reaches commit_task_branch()'s own real merge logic
//       (proving recovery itself worked -- the handle was found, not "unknown") but fails on the
//       separate, pre-existing content-durability gap described above, with the EXACT expected error
//       code -- distinguishing "recovery didn't work" from "a different, already-disclosed limitation"
//       precisely, not by assumption.
//   [4] a normal (non-recovered) discard_task_branch() call still works unchanged, and BranchCost is
//       refunded correctly under the fresh, post-crash quota -- this fix touches only bind_sandbox()'s
//       own recovery pass, never the four verbs' own bodies.
//
// [5]/[6] MUST-FIX (independent red-team, 2026-08-30): `recover_orphaned_task_branches()`'s ORIGINAL
// matching rule was a bare prefix check (`orphan_name` starts with `runtime_->branch_name() +
// "/child-"`), and its own comment claimed a grandchild (a branch forked from another branch that is
// itself already a child) "would carry a DIFFERENT prefix and is correctly left alone." That claim was
// FALSE: `Ledger::branch_from()` names a child as `parent.name() + "/child-" + id + "-" + seq`
// UNCONDITIONALLY, so a grandchild's name is `"<root>/child-A-B/child-C-D"`, which DOES start with
// `"<root>/child-"`. The ORIGINAL code would have silently misfiled such a grandchild into
// `task_branches_` as if this root had created it directly -- a real I4 attribution defect, latent only
// because nothing in THIS tool surface's own call graph currently chains `branch_from()` off a task
// branch (that invariant lives in `start_task_branch()`, not in the recovery method, and `Ledger::
// branch_from()` itself enforces no such restriction for any OTHER caller of the same `Ledger`). Fixed
// by additionally requiring no further `/` in the matched suffix. This case builds a genuine grandchild
// via DIRECT `Ledger::branch_from()` calls (bypassing the tool surface entirely, exactly the way a
// lower-level or future caller could) to prove the fix's boundary empirically, not by assuming the
// current call graph is the only thing keeping this safe:
//   [5] after a simulated crash, the grandchild orphan is correctly left unrecovered -- it never enters
//       `task_branches_` (discard_task_branch() on its name fails with task_branch_unknown_handle), and
//       remains a genuine orphan in the Ledger (still reachable via the lower-level
//       reclaim_orphaned_branch() API, exactly like any other out-of-scope orphan -- not silently lost).
//   [6] the TRUE direct child (same crash, same reconstruction) is still recovered normally --
//       confirming the added guard narrows correctly rather than breaking the [2]/[3] case above.

#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// Mirrors tests/test_mandatory_sandbox_provider_composed.cpp's own fixture exactly -- never actually
// reset()/run()/drain_to()'d here (this file never calls run_in_task_branch()), so it never touches a
// real process or Docker.
struct FakeSurface {
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
    [[nodiscard]] agentengine::result<SurfaceRunOutcome> run(std::string const&) {
        return SurfaceRunOutcome{0, ""};
    }
    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
};
static_assert(ExecutionSurface<FakeSurface>);

using Provider = MandatorySandboxProvider<FakeSurface>;

}  // namespace

int main() {
    namespace fs = std::filesystem;
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle const owner = authority.mint_root("task-branch-durability-owner");

    fs::path const durable_dir = fs::temp_directory_path() / "ae_test_task_branch_durability_recovery";
    fs::path const scratch_root = fs::temp_directory_path() / "ae_test_task_branch_durability_recovery_scratch";
    std::error_code ec;
    fs::remove_all(durable_dir, ec);
    fs::remove_all(scratch_root, ec);

    std::string root_branch_name;
    std::string handle_to_discard;
    std::string handle_to_commit;

    // ---- Phase A: mint quotas, bind, start TWO task branches, then simulate a crash. ----------------
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "Phase A: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> durable_ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        auto root_r = drive(durable_ledger.create_root_branch(owner, "durability"));
        check(root_r.has_value(), "Phase A: create_root_branch() on a durable ledger succeeds");
        if (!root_r.has_value()) return EXIT_FAILURE;
        root_branch_name = root_r->name();

        Provider provider;
        provider.bind_sandbox(durable_ledger, std::move(*root_r), owner, scratch_root / "phase-a",
                                *branch_quota_r, *run_quota_r, *storage_quota_r);
        provider.bind_task_branch_tools(*merge_quota_r);

        auto started1 = drive(provider.start_task_branch(owner));
        auto started2 = drive(provider.start_task_branch(owner));
        check(started1.has_value() && started2.has_value(),
              "Phase A: both start_task_branch() calls succeed");
        if (started1.has_value()) handle_to_discard = started1->handle_id;
        if (started2.has_value()) handle_to_commit = started2->handle_id;

        // [1] baseline: confirms task_branches_ is genuinely in-process-only state by construction.
        check(!handle_to_discard.empty() && !handle_to_commit.empty(),
              "[1] two real, distinct handle_ids were minted before the simulated crash");

        // durable_ledger (and every live BranchHandle/SandboxRuntime this provider holds) goes out of
        // scope HERE -- a real process exit runs no destructor at all either, so this is at least as
        // strong a test as the real crash case (tests/test_ledger.cpp's own established reasoning).
    }

    check(!root_branch_name.empty() && !handle_to_discard.empty() && !handle_to_commit.empty(),
          "setup: all three names captured before Phase A's own scope exit");
    if (root_branch_name.empty() || handle_to_discard.empty() || handle_to_commit.empty()) {
        return EXIT_FAILURE;
    }

    // ---- Phase B: reconstruct against the SAME durable_dir, recover the root, bind, and prove both --
    // ---- recovered handle_ids are usable again through the normal tool surface. ---------------------
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "Phase B: all four quota mint_root() calls succeed (a FRESH AsyncQuota set, by design --"
              " quotas are in-process state, not durable, unchanged and unrelated to this fix)");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> reopened_ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        auto reopened_orphans = reopened_ledger.orphaned_branches();
        bool const root_restored = std::find(reopened_orphans.begin(), reopened_orphans.end(),
                                               root_branch_name) != reopened_orphans.end();
        check(root_restored, "setup: a NEW Ledger against the SAME durable_dir restores the ROOT "
                               "branch as a real orphan too (this fix does not attempt to automate "
                               "root-branch recovery -- a separate, named, still-open residual --"
                               " so the test does this one step by hand, the same way a real host's "
                               "own recovery flow would need to)");
        if (!root_restored) return EXIT_FAILURE;

        auto root_reclaim_r = reopened_ledger.reclaim_orphaned_branch(root_branch_name, owner);
        check(root_reclaim_r.has_value(), "setup: the restored root branch is genuinely reclaimable");
        if (!root_reclaim_r.has_value()) return EXIT_FAILURE;

        Provider provider;
        provider.bind_sandbox(reopened_ledger, std::move(*root_reclaim_r), owner, scratch_root / "phase-b",
                                *branch_quota_r, *run_quota_r, *storage_quota_r);
        provider.bind_task_branch_tools(*merge_quota_r);

        // [2] THE CORE CLAIM: discard_task_branch() on an ORIGINAL handle_id -- minted by a DIFFERENT,
        // now-destroyed provider instance, in a DIFFERENT process lifetime by the crash-simulation's
        // own framing -- succeeds through this tool surface's own normal verb, with NO direct
        // Ledger-level orphan-reclaim call for either CHILD branch anywhere in this check. A pure
        // metadata operation (branch-table erase), so it needs no tree content the crash destroyed.
        auto discard_r = drive(provider.discard_task_branch(handle_to_discard));
        check(discard_r.has_value() && discard_r->ok,
              "[2] discard_task_branch() on a handle_id minted before the simulated crash succeeds "
              "through the normal tool surface after bind_sandbox() alone recovered it -- "
              "task_branches_'s own metadata-durability gap is closed");

        // [3] the second recovered handle reaches commit_task_branch()'s own real merge logic (proving
        // recovery itself worked) but fails on the SEPARATE, pre-existing content-durability gap, with
        // the precise expected error -- not "unknown handle," which would mean recovery failed.
        auto commit_r = drive(provider.commit_task_branch(handle_to_commit, owner));
        check(!commit_r.has_value() && commit_r.error().code != "mandatory_sandbox_provider.task_branch_unknown_handle",
              "[3] the recovered handle is genuinely found (NOT task_branch_unknown_handle) -- "
              "recovery itself worked");
        check(!commit_r.has_value() && commit_r.error().code == "ledger.merge_tree_load_failed",
              "[3] the recovered handle's own commit fails on the SEPARATE, pre-existing, already-"
              "disclosed content-durability gap (real tree content lives only in the in-memory store_, "
              "which a simulated crash genuinely destroys) -- the EXACT expected error, not a different "
              "or unexplained failure");
    }

    // ---- Phase C: a normal (non-recovered) discard, unaffected by this fix. -------------------------
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "Phase C: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> ledger2(InMemoryWorktreeObjectStore{}, durable_dir);
        Provider provider;
        auto root_r2 = drive(ledger2.create_root_branch(owner, "durability-round2"));
        check(root_r2.has_value(), "Phase C: a fresh root branch for round 2 is created");
        if (!root_r2.has_value()) return EXIT_FAILURE;
        provider.bind_sandbox(ledger2, std::move(*root_r2), owner, scratch_root / "phase-c",
                                *branch_quota_r, *run_quota_r, *storage_quota_r);
        provider.bind_task_branch_tools(*merge_quota_r);

        std::uint64_t const branch_before = branch_quota_r->remaining();
        auto started3 = drive(provider.start_task_branch(owner));
        check(started3.has_value(), "Phase C: a third, ordinary task branch starts successfully");
        if (!started3.has_value()) return EXIT_FAILURE;

        auto discard_r2 = drive(provider.discard_task_branch(started3->handle_id));
        check(discard_r2.has_value() && discard_r2->ok,
              "[4] discard_task_branch() on a NORMALLY-started (not recovered) handle still works "
              "unchanged -- this fix touches only bind_sandbox()'s own recovery pass, never the four "
              "verbs' own bodies");
        check(branch_quota_r->remaining() == branch_before,
              "[4] BranchCost is refunded correctly for an ordinary discard (back to its pre-start "
              "level), unaffected by this fix");
    }

    // ---- Phase D: MUST-FIX regression -- a Ledger-API-constructed GRANDCHILD orphan must NOT be -------
    // ---- recovered as if it were a direct child, even though its name textually starts with the -------
    // ---- same root-branch child-prefix (see the [5]/[6] header comment above for the full finding). ---
    std::string root_d_name;
    std::string direct_child_name;
    std::string grandchild_name;
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value(), "Phase D setup: BranchCost quota mint_root() succeeds");
        if (!branch_quota_r.has_value()) return EXIT_FAILURE;

        Ledger<> durable_ledger_d(InMemoryWorktreeObjectStore{}, durable_dir);
        auto root_d_r = drive(durable_ledger_d.create_root_branch(owner, "grandchild-boundary"));
        check(root_d_r.has_value(), "Phase D setup: create_root_branch() succeeds");
        if (!root_d_r.has_value()) return EXIT_FAILURE;
        root_d_name = root_d_r->name();

        // A direct child, via the SAME primitive SandboxRuntime::spawn_child_branch() uses internally --
        // this one MUST be recovered.
        auto child_r = drive(durable_ledger_d.branch_from(*root_d_r, owner, *branch_quota_r));
        check(child_r.has_value(), "Phase D setup: direct child branch_from() succeeds");
        if (!child_r.has_value()) return EXIT_FAILURE;
        direct_child_name = child_r->name();

        // A GRANDCHILD -- branch_from() called with the CHILD (not the root) as parent. Nothing in
        // MandatorySandboxProvider's own call graph does this today (start_task_branch() only ever
        // calls spawn_child_branch() on `runtime_` itself), but Ledger::branch_from() itself enforces no
        // such restriction for any OTHER caller -- this is exactly what tests the recovery method's OWN
        // filtering discipline, not the rest of the codebase's current call-graph shape.
        auto grandchild_r = drive(durable_ledger_d.branch_from(*child_r, owner, *branch_quota_r));
        check(grandchild_r.has_value(), "Phase D setup: grandchild branch_from() succeeds");
        if (!grandchild_r.has_value()) return EXIT_FAILURE;
        grandchild_name = grandchild_r->name();

        std::string const naive_prefix = root_d_name + "/child-";
        check(grandchild_name.compare(0, naive_prefix.size(), naive_prefix) == 0,
              "Phase D setup: sanity -- the grandchild's real name DOES textually start with the root's "
              "own child-prefix (this is exactly the false claim the ORIGINAL comment made about "
              "grandchildren carrying a \"different prefix\" -- disproven here directly)");

        // durable_ledger_d (and every live BranchHandle it holds) goes out of scope HERE -- same
        // crash-simulation reasoning as Phase A.
    }

    check(!root_d_name.empty() && !direct_child_name.empty() && !grandchild_name.empty(),
          "Phase D setup: all three names captured before scope exit");
    if (root_d_name.empty() || direct_child_name.empty() || grandchild_name.empty()) return EXIT_FAILURE;

    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "Phase D: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> reopened_ledger_d(InMemoryWorktreeObjectStore{}, durable_dir);
        auto orphans_d = reopened_ledger_d.orphaned_branches();
        bool const root_d_restored =
            std::find(orphans_d.begin(), orphans_d.end(), root_d_name) != orphans_d.end();
        bool const grandchild_restored_as_orphan =
            std::find(orphans_d.begin(), orphans_d.end(), grandchild_name) != orphans_d.end();
        check(root_d_restored, "Phase D: the root branch is restored as a real orphan too");
        check(grandchild_restored_as_orphan,
              "Phase D setup: the grandchild is ALSO restored as a real orphan by load_durable_state() "
              "(it has no live handle either) -- this is what recover_orphaned_task_branches() must "
              "correctly decline to touch");
        if (!root_d_restored) return EXIT_FAILURE;

        auto root_d_reclaim_r = reopened_ledger_d.reclaim_orphaned_branch(root_d_name, owner);
        check(root_d_reclaim_r.has_value(), "Phase D setup: the restored root branch is reclaimable");
        if (!root_d_reclaim_r.has_value()) return EXIT_FAILURE;

        Provider provider;
        provider.bind_sandbox(reopened_ledger_d, std::move(*root_d_reclaim_r), owner,
                                scratch_root / "phase-d", *branch_quota_r, *run_quota_r, *storage_quota_r);
        provider.bind_task_branch_tools(*merge_quota_r);

        // [5] THE MUST-FIX CLAIM: the grandchild must NOT have been recovered into task_branches_ --
        // discard_task_branch() on its name must fail with task_branch_unknown_handle, exactly as it
        // would for any name this provider never saw.
        auto grandchild_discard_r = drive(provider.discard_task_branch(grandchild_name));
        check(!grandchild_discard_r.has_value() &&
                  grandchild_discard_r.error().code ==
                      "mandatory_sandbox_provider.task_branch_unknown_handle",
              "[5] the grandchild orphan is correctly left unrecovered by bind_sandbox() -- "
              "discard_task_branch() on its name fails with task_branch_unknown_handle, not success "
              "(reverting the recover_orphaned_task_branches() fix makes this fail: the grandchild would "
              "have been wrongly recovered and this discard would succeed instead)");

        // And it must still be a genuine orphan afterward -- not silently lost, just correctly left for
        // the lower-level API, matching this method's own "not the authoritative list" framing.
        auto orphans_after = reopened_ledger_d.orphaned_branches();
        check(std::find(orphans_after.begin(), orphans_after.end(), grandchild_name) != orphans_after.end(),
              "[5] the grandchild remains a genuine, un-consumed orphan after bind_sandbox() -- "
              "correctly left for Ledger::reclaim_orphaned_branch() directly, not lost or double-touched");

        // [6] THE TRUE direct child, same crash, same reconstruction, must still be recovered normally --
        // proving the added guard narrows correctly rather than over-broadly excluding real children.
        auto direct_child_discard_r = drive(provider.discard_task_branch(direct_child_name));
        check(direct_child_discard_r.has_value() && direct_child_discard_r->ok,
              "[6] the TRUE direct child is still recovered and usable through the normal tool surface "
              "after the same crash+reconstruction -- the grandchild-exclusion fix does not regress the "
              "ordinary direct-child recovery case");
    }

    fs::remove_all(durable_dir, ec);
    fs::remove_all(scratch_root, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- MandatorySandboxProvider's task_branches_ genuinely survives "
                     "a simulated process crash: bind_sandbox() alone, against a durable-Store Ledger "
                     "carrying an already-reclaimed root branch, rediscovers and rehydrates every real "
                     "child task branch that was live-but-unresolved at the moment of the crash, so "
                     "discard_task_branch() on the ORIGINAL handle_id succeeds through this tool "
                     "surface's own normal verb, and commit_task_branch() genuinely finds its own "
                     "recovered handle (failing only on the separate, pre-existing, precisely-"
                     "distinguished content-durability gap) -- closing ADR-114 §5/§6's own \"active_'s "
                     "table has no durability of its own\" residual.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
