// Closes the "root-branch recovery remains the caller's own, separate, already-disclosed
// responsibility" residual ADR-126 §5 named -- itself tracing back to ADR-102's own disclosed
// "the unbound state owns no branch at all... a `bind_branch_only()` variant is real follow-on
// work" gap (`mandatory_sandbox_provider.hpp`'s own `bind_sandbox()` comment). ADR-126 closed the
// CHILD half of task-branch crash recovery (`task_branches_` rehydration); this closes the ROOT
// half: a host no longer has to know `Ledger`'s own deterministic root-name format
// (`"root-" + owner.id() [+ "-" + disambiguator]`) or hand-sequence `Ledger::
// reclaim_orphaned_branch()`/`create_root_branch()` itself in the right order before calling
// `bind_sandbox()` -- `MandatorySandboxProvider::bind_root_branch()` does both, choosing reclaim-
// if-orphaned or create-if-not by identity alone.
//
// Deliberately NOT closed (see `bind_root_branch()`'s own comment for the full scope note): the
// fuller ADR-099 §1 item 2 claim that an entirely UNBOUND session (before any bind call at all, in
// any process) should still own a branch -- a session-lifecycle question this method does not
// attempt. Also not closed: a second `bind_root_branch()` call for the SAME owner/disambiguator
// while the first branch is still LIVE (not orphaned) reaches the create-path and silently
// produces a duplicate-named, overwriting branch -- the EXACT SAME pre-existing hazard
// `Ledger::create_root_branch()` itself already documents (ADR-102 §43.2), not a new one this
// method introduces.
//
//   [1] a fresh owner with no branch anywhere: bind_root_branch() takes the create-path, and the
//       resulting binding is genuinely functional (a start/discard round trip through the normal
//       tool surface succeeds), not merely "returns success" with no usable state behind it.
//   [2] THE CORE CLAIM -- true crash recovery: after a simulated crash (mirroring
//       tests/test_task_branch_durability_recovery.cpp's own "destroy + reconstruct against the
//       SAME durable_dir" methodology), a SECOND bind_root_branch() call for the SAME
//       owner/disambiguator on a freshly reconstructed Ledger reattaches to the SAME branch
//       (reclaim, not create) -- proven by a task-branch handle minted before the crash still
//       being discoverable via discard_task_branch() through the tool surface, with NO manual
//       Ledger-level reclaim_orphaned_branch() call anywhere in this check (unlike
//       test_task_branch_durability_recovery.cpp's own Phase B, which still does that one step by
//       hand -- this is exactly the step this fix automates).
//   [3] two DIFFERENT disambiguators for the SAME owner resolve to two genuinely independent root
//       branches -- a positive control confirming bind_root_branch() does not collapse them.

#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include <algorithm>
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

// Mirrors tests/test_task_branch_durability_recovery.cpp's own fixture exactly -- this file never
// calls run_in_task_branch(), so a FakeSurface stand-in is enough.
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
    IdentityHandle const owner = authority.mint_root("root-branch-recovery-owner");

    fs::path const durable_dir = fs::temp_directory_path() / "ae_test_root_branch_recovery";
    fs::path const scratch_root = fs::temp_directory_path() / "ae_test_root_branch_recovery_scratch";
    std::error_code ec;
    fs::remove_all(durable_dir, ec);
    fs::remove_all(scratch_root, ec);

    // ---- [1] fresh owner, no branch anywhere: bind_root_branch() creates one, and it is genuinely
    // ---- usable, not just "returns success". ----------------------------------------------------
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "[1] setup: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        Provider provider;
        auto bound = provider.bind_root_branch(ledger, owner, scratch_root / "phase-1", *branch_quota_r,
                                                 *run_quota_r, *storage_quota_r, "fresh");
        provider.bind_task_branch_tools(*merge_quota_r);
        check(bound.has_value(), "[1] bind_root_branch() succeeds for a fresh owner/disambiguator pair");
        check(provider.is_bound(), "[1] the provider is genuinely bound afterward");

        auto started = drive(provider.start_task_branch(owner));
        check(started.has_value(), "[1] start_task_branch() works through the resulting binding");
        if (started.has_value()) {
            auto discard_r = drive(provider.discard_task_branch(started->handle_id));
            check(discard_r.has_value() && discard_r->ok,
                  "[1] the resulting binding is fully functional end to end (start+discard round trip)");
        }
    }

    // ---- [2] THE CORE CLAIM: true crash recovery, fully automated. --------------------------------
    std::string handle_before_crash;
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "[2] setup: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> durable_ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        Provider provider;
        auto bound = provider.bind_root_branch(durable_ledger, owner, scratch_root / "phase-2a",
                                                 *branch_quota_r, *run_quota_r, *storage_quota_r,
                                                 "crash-recovery");
        provider.bind_task_branch_tools(*merge_quota_r);
        check(bound.has_value(), "[2] setup: bind_root_branch() creates the root for the crash-recovery case");
        if (!bound.has_value()) return EXIT_FAILURE;

        auto started = drive(provider.start_task_branch(owner));
        check(started.has_value(), "[2] setup: a task branch starts before the simulated crash");
        if (!started.has_value()) return EXIT_FAILURE;
        handle_before_crash = started->handle_id;

        // durable_ledger (and every live BranchHandle/SandboxRuntime this provider holds) goes out
        // of scope HERE -- same crash-simulation reasoning test_task_branch_durability_recovery.cpp
        // and test_ledger.cpp both already establish.
    }
    check(!handle_before_crash.empty(), "[2] setup: a real handle_id was captured before the crash");
    if (handle_before_crash.empty()) return EXIT_FAILURE;

    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "[2] reconstruction: all four quota mint_root() calls succeed (a FRESH set, by "
              "design -- quotas are in-process state, not durable, unchanged and unrelated to this "
              "fix)");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> reopened_ledger(InMemoryWorktreeObjectStore{}, durable_dir);

        std::string const root_name_2 = "root-" + std::to_string(owner.id()) + "-crash-recovery";
        auto orphans_before = reopened_ledger.orphaned_branches();
        bool const root_orphaned_before =
            std::find(orphans_before.begin(), orphans_before.end(), root_name_2) != orphans_before.end();
        check(root_orphaned_before,
              "[2] setup: the reconstructed Ledger genuinely restores the root branch as an orphan "
              "(confirms the crash simulation actually orphaned it, before bind_root_branch() ever "
              "runs)");

        Provider provider;
        // THE WHOLE POINT: one call, no manual Ledger::orphaned_branches()/reclaim_orphaned_branch()
        // anywhere in this test -- unlike test_task_branch_durability_recovery.cpp's own Phase B,
        // which still does the root-branch half by hand.
        auto bound = provider.bind_root_branch(reopened_ledger, owner, scratch_root / "phase-2b",
                                                 *branch_quota_r, *run_quota_r, *storage_quota_r,
                                                 "crash-recovery");
        provider.bind_task_branch_tools(*merge_quota_r);
        check(bound.has_value(),
              "[2] bind_root_branch() succeeds after a simulated crash, with NO manual reclaim call");
        if (!bound.has_value()) return EXIT_FAILURE;

        // A DIRECT differentiator between reclaim and (incorrectly) always-create:
        // Ledger::reclaim_orphaned_branch() erases the name from orphaned_from_restart_ as part of a
        // genuine reclaim; Ledger::create_root_branch() never touches that set at all. If
        // bind_root_branch() wrongly took the create-path here, root_name_2 would still be listed as
        // an orphan afterward.
        auto orphans_after = reopened_ledger.orphaned_branches();
        check(std::find(orphans_after.begin(), orphans_after.end(), root_name_2) == orphans_after.end(),
              "[2] the root branch is no longer listed as an orphan after bind_root_branch() -- proving "
              "it genuinely took the RECLAIM path, not the create path");

        auto discard_r = drive(provider.discard_task_branch(handle_before_crash));
        check(discard_r.has_value() && discard_r->ok,
              "[2] the pre-crash task-branch handle is discoverable and discardable after the "
              "SECOND bind_root_branch() call -- proving it RECLAIMED the SAME root branch rather "
              "than silently creating a fresh, empty one under the same name (a fresh root would "
              "leave task_branches_ empty and this discard would fail with "
              "task_branch_unknown_handle instead)");
    }

    // ---- [3] two different disambiguators for the same owner are genuinely independent roots. ----
    {
        auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r.has_value() && run_quota_r.has_value() && storage_quota_r.has_value() &&
                  merge_quota_r.has_value(),
              "[3] setup: all four quota mint_root() calls succeed");
        if (!branch_quota_r.has_value() || !run_quota_r.has_value() || !storage_quota_r.has_value() ||
            !merge_quota_r.has_value()) {
            return EXIT_FAILURE;
        }

        Ledger<> ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        Provider provider_x;
        auto bound_x = provider_x.bind_root_branch(ledger, owner, scratch_root / "phase-3x",
                                                      *branch_quota_r, *run_quota_r, *storage_quota_r,
                                                      "disambiguator-x");
        provider_x.bind_task_branch_tools(*merge_quota_r);
        check(bound_x.has_value(), "[3] disambiguator \"x\" binds successfully");

        auto branch_quota_r2 = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
        auto run_quota_r2 = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
        auto storage_quota_r2 = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
        auto merge_quota_r2 = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
        check(branch_quota_r2.has_value() && run_quota_r2.has_value() && storage_quota_r2.has_value() &&
                  merge_quota_r2.has_value(),
              "[3] setup: a second, independent quota set mints successfully");
        if (!branch_quota_r2.has_value() || !run_quota_r2.has_value() || !storage_quota_r2.has_value() ||
            !merge_quota_r2.has_value()) {
            return EXIT_FAILURE;
        }
        Provider provider_y;
        auto bound_y = provider_y.bind_root_branch(ledger, owner, scratch_root / "phase-3y",
                                                      *branch_quota_r2, *run_quota_r2, *storage_quota_r2,
                                                      "disambiguator-y");
        provider_y.bind_task_branch_tools(*merge_quota_r2);
        check(bound_y.has_value(), "[3] disambiguator \"y\" binds successfully too, on the SAME ledger");

        auto started_x = drive(provider_x.start_task_branch(owner));
        auto started_y = drive(provider_y.start_task_branch(owner));
        check(started_x.has_value() && started_y.has_value(),
              "[3] both providers independently start their own task branches");
        check(started_x.has_value() && started_y.has_value() &&
                  started_x->handle_id != started_y->handle_id,
              "[3] the two providers' task branches are genuinely distinct, not aliased to the same "
              "underlying root");
    }

    fs::remove_all(durable_dir, ec);
    fs::remove_all(scratch_root, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- MandatorySandboxProvider::bind_root_branch() correctly "
                     "creates a fresh root branch for a never-before-seen owner/disambiguator pair, "
                     "and correctly RECLAIMS (never re-creates) the SAME root branch after a "
                     "simulated crash, fully automating the root-branch half of task-branch crash "
                     "recovery that test_task_branch_durability_recovery.cpp's own Phase B still did "
                     "by hand -- closing ADR-126 §5's own \"root-branch recovery remains the "
                     "caller's own responsibility\" residual.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
