// Proof for 025-Worktree-and-Virtual-Filesystem.md §9 G3's concurrency claim, Milestone 3 Phase B4
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, scoped per that doc's
// decision 7): "N concurrent `branch` agents produce a deterministic merge result; every genuine
// conflict is surfaced, never silently resolved; no lost update over [a machine-safe bounded count
// of] randomized interleavings."
//
// Concurrency here is a single-threaded, discrete-event SIMULATION, not real OS threads: real
// threads racing on InMemoryWorktreeObjectStore (a plain unordered_map, no internal locking) would
// be a data race in the TEST HARNESS itself -- undefined behavior unrelated to what this is trying
// to prove -- and CLAUDE.md's Machine Safety section rules out spawning hardware_concurrency()
// threads regardless. (InMemoryStore/rt::InMemoryAppendLogStore does hold its own internal mutex,
// but this test's whole point is exercising the SEAM-level staleness check -- `merge_stale_parent`
// -- deterministically across many interleavings, not merely proving the store itself doesn't
// corrupt under raw concurrent access.) Instead: N agents all observe the SAME parent snapshot
// (the worst-case, maximally contentious scenario -- everyone started before anyone committed), then
// their merge attempts are dispatched one at a time in a RANDOMIZED order across many trials. Every
// agent but the first-in-order genuinely observes a stale parent on its first attempt (proven, not
// assumed -- checked below) and must recover via `retry_merge_branch_into_parent`.
//
// Per-trial internal steps are NOT individually logged (500+ trials x several agents would flood the
// output with routine successes) -- each trial either fully satisfies its invariant or contributes a
// specific, trial-numbered failure message; only the aggregate-over-all-trials result is asserted
// with AE_CHECK, which is what 025 §9 G3 actually claims ("no lost update over N interleavings", a
// property of the whole run, not of any one trial in isolation).

#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "agentengine/core/worktree.hpp"

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
        }                                                                                           \
    } while (0)

// A trial-scoped failure: printed immediately (so a real violation is never silently averaged away)
// but without the routine "ok" noise AE_CHECK would add for the thousands of per-trial successes.
void report_trial_failure(std::string const& what) {
    std::cerr << "FAIL (per-trial): " << what << "\n";
    ++g_failures;
}

Digest blob_of(InMemoryWorktreeObjectStore& store, std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return *store.put_blob(bytes);
}

// Test-only, deliberately WRONG merge: blindly commits the branch's own tree over the parent with no
// three-way merge check at all -- exactly the "auto-apply" 025 §4 forbids. Used only as a positive
// control (B4-C3, below) to prove the real test's "no lost update" assertion actually fails when the
// underlying merge logic is broken, not that it passes vacuously.
template <rt::AppendLogStore RS>
result<Ref> naive_last_writer_wins_merge(RS& ref_store, SubWorktree const& branch,
                                          std::string const& parent_name) {
    auto branch_ref = read_ref(ref_store, branch.backing_ref_name);
    if (!branch_ref || !branch_ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "branch ref missing", "test.naive_no_branch"});
    }
    return commit_ref(ref_store, parent_name, (*branch_ref)->tree_digest);
}

} // namespace

int main() {
    constexpr int kAgents = 5;
    constexpr int kTrials = 500;  // machine-safe bounded count (decision 7): pure in-memory CPU
                                   // work, no real process/OS resource per iteration (unlike
                                   // native_jail's 300-cycle reduction from 008 §9 G4's 10^5, which
                                   // is bounded by real AppContainer/Job-object cost per cycle) --
                                   // still deliberately far below the RFC's own 10^4, not attempting
                                   // to hit that bar, per this milestone's stated scope.

    // B4-C1 (no lost update, disjoint edits, randomized dispatch order): every trial ends with the
    // parent tree containing ALL kAgents' additions, regardless of which order the (initially
    // equally-stale) agents were dispatched in.
    {
        long long total_stale_first_attempts = 0;
        int trials_all_clean = 0;

        for (int trial = 0; trial < kTrials; ++trial) {
            InMemoryStore ref_store;
            InMemoryWorktreeObjectStore obj_store;
            std::string const parent_name = "session:conc-" + std::to_string(trial);

            auto base = blob_of(obj_store, "base-content");
            auto base_tree = *obj_store.put_tree(Tree{{TreeEntry{"base.txt", base, false}}});
            auto parent = commit_ref(ref_store, parent_name, base_tree);
            if (!parent) {
                report_trial_failure("B4-C1 trial " + std::to_string(trial) + ": setup parent commit failed");
                continue;
            }

            std::vector<SubWorktree> branches;
            bool setup_ok = true;
            for (int i = 0; i < kAgents && setup_ok; ++i) {
                auto branch = create_sub_worktree(ref_store, *parent,
                                                   parent_name + "/agents/a" + std::to_string(i),
                                                   sharing_mode::branch);
                auto content = blob_of(obj_store, "agent-" + std::to_string(i) + "-content");
                auto branch_tree = *obj_store.put_tree(
                    Tree{{TreeEntry{"base.txt", base, false},
                          TreeEntry{"agent-" + std::to_string(i) + ".txt", content, false}}});
                setup_ok = branch.has_value() && write_sub_worktree(ref_store, *branch, branch_tree).has_value();
                if (setup_ok) branches.push_back(*branch);
            }
            if (!setup_ok) {
                report_trial_failure("B4-C1 trial " + std::to_string(trial) + ": branch setup failed");
                continue;
            }

            // All kAgents "observed" the parent at the identical initial instant -- the maximally
            // contentious starting point every agent races from.
            Ref const shared_initial_snapshot = *parent;

            std::vector<int> order(kAgents);
            std::iota(order.begin(), order.end(), 0);
            std::mt19937 rng(static_cast<unsigned>(trial));
            std::shuffle(order.begin(), order.end(), rng);

            bool trial_clean = true;
            for (std::size_t pos = 0; pos < order.size(); ++pos) {
                int idx = order[pos];
                auto first_attempt =
                    merge_branch_into_parent(obj_store, ref_store, branches[idx], shared_initial_snapshot);

                if (pos == 0) {
                    if (!first_attempt.has_value() || !first_attempt->ok()) {
                        trial_clean = false;
                        report_trial_failure("B4-C1 trial " + std::to_string(trial) +
                                              ": the first-dispatched agent's initial attempt did not "
                                              "succeed cleanly");
                    }
                    continue;
                }

                // Every subsequent agent's first attempt races against a snapshot some earlier agent
                // already moved past -- it MUST be rejected as stale, not silently let through.
                if (!first_attempt.has_value() && first_attempt.error().code == "worktree.merge_stale_parent") {
                    ++total_stale_first_attempts;
                } else {
                    trial_clean = false;
                    report_trial_failure("B4-C1 trial " + std::to_string(trial) +
                                          ": a non-first agent's stale initial snapshot was unexpectedly "
                                          "accepted or failed with a different error");
                    continue;
                }

                auto retried = retry_merge_branch_into_parent(obj_store, ref_store, branches[idx],
                                                                shared_initial_snapshot,
                                                                /*max_attempts=*/kAgents + 1);
                if (!retried.has_value() || !retried->ok()) {
                    trial_clean = false;
                    report_trial_failure("B4-C1 trial " + std::to_string(trial) +
                                          ": retry_merge_branch_into_parent failed to recover a "
                                          "genuinely disjoint edit under contention");
                }
            }
            if (trial_clean) ++trials_all_clean;

            auto final_parent = read_ref(ref_store, parent_name);
            bool final_ok = final_parent.has_value() && final_parent->has_value();
            if (final_ok) {
                auto final_tree = obj_store.get_tree((*final_parent)->tree_digest);
                final_ok = final_tree.has_value() &&
                           final_tree->entries.size() == static_cast<std::size_t>(kAgents + 1);
                if (final_ok) {
                    for (int i = 0; i < kAgents; ++i) {
                        std::string want = "agent-" + std::to_string(i) + ".txt";
                        bool found = std::ranges::any_of(
                            final_tree->entries, [&](TreeEntry const& e) { return e.name == want; });
                        if (!found) final_ok = false;
                    }
                }
            }
            if (!final_ok) {
                report_trial_failure("B4-C1 trial " + std::to_string(trial) +
                                      ": final parent tree is missing at least one agent's disjoint "
                                      "addition -- a lost update");
            }
        }

        AE_CHECK(trials_all_clean == kTrials,
                 "B4-C1: every one of " + std::to_string(kTrials) +
                     " randomized-order trials ended with no lost update");
        AE_CHECK(total_stale_first_attempts == static_cast<long long>(kTrials) * (kAgents - 1),
                 "B4-C1: the stale-parent path was genuinely exercised exactly (kAgents-1) times per "
                 "trial -- not zero (this test isn't vacuous) and not more than expected");
    }

    // B4-C2 (genuine conflicts are ALWAYS surfaced, regardless of dispatch order -- never
    // last-writer-wins): two agents edit the SAME file differently; across randomized orderings,
    // exactly one wins the race and the other reliably surfaces a real conflict.
    {
        int trials_conflict_surfaced = 0;

        for (int trial = 0; trial < kTrials; ++trial) {
            InMemoryStore ref_store;
            InMemoryWorktreeObjectStore obj_store;
            std::string const parent_name = "session:cconc-" + std::to_string(trial);

            auto a_v1 = blob_of(obj_store, "shared-file-v1");
            auto base_tree = *obj_store.put_tree(Tree{{TreeEntry{"shared.txt", a_v1, false}}});
            auto parent = commit_ref(ref_store, parent_name, base_tree);

            auto branch_x =
                create_sub_worktree(ref_store, *parent, parent_name + "/agents/x", sharing_mode::branch);
            auto branch_y =
                create_sub_worktree(ref_store, *parent, parent_name + "/agents/y", sharing_mode::branch);
            if (!parent || !branch_x.has_value() || !branch_y.has_value()) {
                report_trial_failure("B4-C2 trial " + std::to_string(trial) + ": setup failed");
                continue;
            }

            auto x_edit = blob_of(obj_store, "x-edit-trial-" + std::to_string(trial));
            auto y_edit = blob_of(obj_store, "y-edit-trial-" + std::to_string(trial));
            auto x_tree = *obj_store.put_tree(Tree{{TreeEntry{"shared.txt", x_edit, false}}});
            auto y_tree = *obj_store.put_tree(Tree{{TreeEntry{"shared.txt", y_edit, false}}});
            bool writes_ok = write_sub_worktree(ref_store, *branch_x, x_tree).has_value() &&
                              write_sub_worktree(ref_store, *branch_y, y_tree).has_value();
            if (!writes_ok) {
                report_trial_failure("B4-C2 trial " + std::to_string(trial) + ": setup writes failed");
                continue;
            }

            Ref const shared_initial_snapshot = *parent;
            std::vector<SubWorktree> both = {*branch_x, *branch_y};
            std::mt19937 rng(static_cast<unsigned>(trial) * 7919u + 1);
            if (rng() % 2 == 0) std::swap(both[0], both[1]);

            auto first_outcome =
                merge_branch_into_parent(obj_store, ref_store, both[0], shared_initial_snapshot);
            if (!first_outcome.has_value() || !first_outcome->ok()) {
                report_trial_failure("B4-C2 trial " + std::to_string(trial) +
                                      ": the first-dispatched agent's edit did not merge cleanly");
                continue;
            }

            auto second_outcome =
                retry_merge_branch_into_parent(obj_store, ref_store, both[1], shared_initial_snapshot, 3);
            bool conflict_surfaced =
                second_outcome.has_value() && !second_outcome->ok() && !second_outcome->conflicts.empty();
            if (conflict_surfaced) {
                ++trials_conflict_surfaced;
            } else {
                report_trial_failure("B4-C2 trial " + std::to_string(trial) +
                                      ": the second agent's genuinely divergent edit did NOT surface as "
                                      "a conflict -- staleness recovery silently dropped the collision");
            }

            auto final_parent = read_ref(ref_store, parent_name);
            if (!final_parent.has_value() || !final_parent->has_value()) {
                report_trial_failure("B4-C2 trial " + std::to_string(trial) +
                                      ": parent ref stopped resolving after the conflict");
            }
        }

        AE_CHECK(trials_conflict_surfaced == kTrials,
                 "B4-C2: every one of " + std::to_string(kTrials) +
                     " trials surfaced the genuine conflict, regardless of race order -- never "
                     "silently resolved, never last-writer-wins");
    }

    // B4-C3 (positive control): re-run B4-C1's exact disjoint-edit scenario, but through the
    // deliberately miswired `naive_last_writer_wins_merge` instead of the real merge path -- if this
    // does NOT reliably lose updates, B4-C1's "no lost update" assertion above would be proving
    // nothing (022 §5: a check that can't fail proves nothing).
    {
        constexpr int kControlTrials = 50;  // fewer: this is a control, not the claim being measured
        int trials_with_lost_update = 0;

        for (int trial = 0; trial < kControlTrials; ++trial) {
            InMemoryStore ref_store;
            InMemoryWorktreeObjectStore obj_store;
            std::string const parent_name = "session:naive-" + std::to_string(trial);

            auto base = blob_of(obj_store, "base-content");
            auto base_tree = *obj_store.put_tree(Tree{{TreeEntry{"base.txt", base, false}}});
            auto parent = commit_ref(ref_store, parent_name, base_tree);

            std::vector<SubWorktree> branches;
            bool setup_ok = parent.has_value();
            for (int i = 0; i < kAgents && setup_ok; ++i) {
                auto branch = create_sub_worktree(ref_store, *parent,
                                                   parent_name + "/agents/a" + std::to_string(i),
                                                   sharing_mode::branch);
                auto content = blob_of(obj_store, "agent-" + std::to_string(i) + "-content");
                auto branch_tree = *obj_store.put_tree(
                    Tree{{TreeEntry{"base.txt", base, false},
                          TreeEntry{"agent-" + std::to_string(i) + ".txt", content, false}}});
                setup_ok = branch.has_value() && write_sub_worktree(ref_store, *branch, branch_tree).has_value();
                if (setup_ok) branches.push_back(*branch);
            }
            if (!setup_ok) {
                report_trial_failure("B4-C3 trial " + std::to_string(trial) + ": setup failed");
                continue;
            }

            std::vector<int> order(kAgents);
            std::iota(order.begin(), order.end(), 0);
            std::mt19937 rng(static_cast<unsigned>(trial) * 104729u + 3);
            std::shuffle(order.begin(), order.end(), rng);

            // No staleness handling at all -- every agent blindly overwrites the parent with only its
            // OWN tree, discarding whatever any earlier agent already committed.
            for (int idx : order) {
                auto naive_result = naive_last_writer_wins_merge(ref_store, branches[idx], parent_name);
                if (!naive_result.has_value()) {
                    report_trial_failure("B4-C3 trial " + std::to_string(trial) +
                                          ": naive_last_writer_wins_merge itself failed unexpectedly");
                }
            }

            auto final_parent = read_ref(ref_store, parent_name);
            bool lost_update = true;
            if (final_parent.has_value() && final_parent->has_value()) {
                auto final_tree = obj_store.get_tree((*final_parent)->tree_digest);
                if (final_tree.has_value() &&
                    final_tree->entries.size() == static_cast<std::size_t>(kAgents + 1)) {
                    lost_update = false;
                }
            }
            if (lost_update) ++trials_with_lost_update;
        }

        AE_CHECK(trials_with_lost_update > 0,
                 "B4-C3 (positive control): the naive last-writer-wins path DOES lose updates -- "
                 "proving B4-C1's identical check is a real gate, not one that would pass no matter "
                 "what the merge logic did");
        AE_CHECK(trials_with_lost_update == kControlTrials,
                 "B4-C3 (positive control): in fact it loses updates on EVERY trial (blind overwrite "
                 "with no merge at all can never coincidentally preserve every agent's file)");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree branch-concurrency proof checks passed.\n";
    return 0;
}
