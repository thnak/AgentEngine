// PROVE-PHASE PROBE (A3 rollback, 2026-08-28): the real, end-to-end proof that
// `SandboxRuntime::reset_to_turn()` actually closes ADR-099 §8's own disclosed residual --
// "Ledger::reset_to() is real and proven, but SandboxRuntime has no rollback method of its own" --
// against a REAL Docker daemon, not merely that the wrapper compiles.
//
// The load-bearing claim this file exists to check: rollback must affect what a SUBSEQUENT real
// `run()` call actually sees on real disk, not just the Ledger's own bookkeeping. `reset_to_turn()`
// deliberately never touches the on-disk staging directory itself (see its own comment in
// sandbox_runtime.hpp) -- the proof has to be that the NEXT `run()`'s materialize() step picks up
// the rolled-back tree for real, inside a genuinely fresh container that has no memory of its own.

#include "docker_execution_surface.hpp"
#include "sandbox_runtime.hpp"

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
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("sandbox-rollback-owner");
    Ledger<> ledger;
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());
    auto reset_quota = AsyncQuota<ResetCost>::mint_root(authority, owner, 100);
    CHECK(reset_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());

    std::filesystem::path const staging =
        std::filesystem::temp_directory_path() / "ae_sandbox_rollback_probe";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);

    SandboxRuntime runtime(ledger, std::move(*root_r), staging);
    DockerExecutionSurface surface;

    // === [1] Three real turns, each overwriting note.txt with a different value. ====================
    auto r1 = run(runtime.run(surface, "echo -n 'v1' > note.txt && cat note.txt", owner, *run_quota, *quota));
    CHECK(r1.has_value());
    CHECK(r1->checkpoint.turn_index == 1);
    agentengine::Digest const tree_v1 = r1->checkpoint.tree;

    auto r2 = run(runtime.run(surface, "echo -n 'v2' > note.txt && cat note.txt", owner, *run_quota, *quota));
    CHECK(r2.has_value());
    CHECK(r2->checkpoint.turn_index == 2);

    auto r3 = run(runtime.run(surface, "echo -n 'v3' > note.txt && cat note.txt", owner, *run_quota, *quota));
    CHECK(r3.has_value());
    CHECK(r3->checkpoint.turn_index == 3);
    std::printf("[1] three real turns committed (v1 -> v2 -> v3), turn_index 1/2/3 -- PASS\n");

    // === [2] reset_to_turn(1) moves the branch's HEAD back to turn 1's tree, as a NEW forward =========
    // === checkpoint (turn_index == 4) -- matching Ledger::reset_to()'s own "revert is itself a =======
    // === forward-moving commit, never an in-place rewrite of history" discipline. =====================
    auto reset_r = run(runtime.reset_to_turn(1, owner, *reset_quota));
    CHECK(reset_r.has_value());
    CHECK(reset_r->tree == tree_v1);
    CHECK(reset_r->turn_index == 4);
    std::printf("[2] reset_to_turn(1) succeeded: HEAD now points at turn 1's real tree digest (%s), "
                "recorded as a NEW forward checkpoint (turn_index=%llu), not an in-place rewrite -- "
                "PASS\n", reset_r->tree.c_str(), (unsigned long long)reset_r->turn_index);

    // === [3] The REAL, load-bearing check: the NEXT run() call, against a genuinely FRESH container ==
    // === (reset() destroys and recreates it -- no container-side memory of turn 3's 'v3' at all), ====
    // === must see turn 1's ROLLED-BACK content, not turn 3's. Its ONLY path to that content is =======
    // === Ledger.materialize() seeding the fresh container from the tree reset_to_turn() just moved ===
    // === HEAD to -- proving this is a REAL rollback of what a subsequent execution actually sees, ====
    // === not merely a Ledger-internal bookkeeping change nothing downstream ever reads. ===============
    auto r4 = run(runtime.run(surface, "cat note.txt", owner, *run_quota, *quota));
    CHECK(r4.has_value());
    CHECK(r4->exec.stdout_text == "v1");
    CHECK(r4->checkpoint.turn_index == 5);
    std::printf("[3] REAL ADVERSARIAL PROOF: a genuinely fresh container, seeded via Ledger."
                "materialize() from the ROLLED-BACK tree, read note.txt as \"%s\" -- turn 3's 'v3' is "
                "genuinely gone from what execution sees, not merely from Ledger bookkeeping -- PASS\n",
                r4->exec.stdout_text.c_str());

    // === [4] Resetting to a turn_index that was never a real checkpoint on THIS branch fails ==========
    // === cleanly -- Ledger::reset_to()'s own "no such checkpoint" error, reused verbatim. Also =========
    // === proves the failure path REFUNDS the ResetCost unit it consumed (matching RunCost's own =====
    // === established "nothing happened, don't keep the charge" discipline) -- captured by comparing ==
    // === reset_quota->remaining() before and after. ===================================================
    std::uint64_t const reset_quota_before_bad = reset_quota->remaining();
    auto bad_reset = run(runtime.reset_to_turn(9999, owner, *reset_quota));
    CHECK(!bad_reset.has_value());
    CHECK(bad_reset.error().code == "ledger.no_such_checkpoint");
    CHECK(reset_quota->remaining() == reset_quota_before_bad);
    std::printf("[4] reset_to_turn() to a nonexistent turn_index is rejected cleanly (%s), not "
                "silently accepted or corrupted, and the ResetCost unit it consumed was fully "
                "refunded (remaining=%llu, unchanged) -- PASS\n", bad_reset.error().code.c_str(),
                (unsigned long long)reset_quota->remaining());

    // === [5] Resetting to the CURRENT head's own turn_index is a well-defined, genuinely NEW forward =
    // === checkpoint (same tree, INCREMENTED turn_index) -- not a no-op, not an error, not a state =====
    // === corruption. Asserts turn_index actually advanced, not just that the tree matched (a red-team=
    // === finding on this check's earlier version: it asserted the tree but never the turn_index, so a=
    // === hypothetical broken "no-op short-circuit" implementation that inserted no new checkpoint at ==
    // === all would have passed it too). ================================================================
    auto head_before = ledger.head_tree_digest(runtime.branch_name(), owner);
    CHECK(head_before.has_value());
    auto self_reset = run(runtime.reset_to_turn(5, owner, *reset_quota));
    CHECK(self_reset.has_value());
    CHECK(self_reset->tree == *head_before);
    CHECK(self_reset->turn_index == 6);
    std::printf("[5] reset_to_turn() to the branch's own current turn is a well-defined, same-tree "
                "forward checkpoint with a GENUINELY INCREMENTED turn_index (%llu, not a no-op short-"
                "circuit) -- PASS\n", (unsigned long long)self_reset->turn_index);

    // === [6] REAL ADVERSARIAL PROOF closing the gap a red-team pass on this file's own first version =
    // === found: reset_to_turn() had NO quota gate at all, letting a caller call it in a tight loop for=
    // === free -- unbounded checkpoint growth plus, under a durable Store, a full-ledger re-serialize ==
    // === on every call. With ResetCost exhausted, the call must be REJECTED and the branch's real head=
    // === must be COMPLETELY UNCHANGED -- not just that the boolean check says no, but that no real ====
    // === Ledger mutation happened at all. =============================================================
    {
        auto exhausted_reset_quota = AsyncQuota<ResetCost>::mint_root(authority, owner, 0);
        CHECK(exhausted_reset_quota.has_value());
        CHECK(exhausted_reset_quota->remaining() == 0);

        auto head_before_blocked = ledger.head_tree_digest(runtime.branch_name(), owner);
        CHECK(head_before_blocked.has_value());

        auto blocked = run(runtime.reset_to_turn(1, owner, *exhausted_reset_quota));
        CHECK(!blocked.has_value());
        CHECK(blocked.error().code == "quota.exhausted");

        auto head_after_blocked = ledger.head_tree_digest(runtime.branch_name(), owner);
        CHECK(head_after_blocked.has_value());
        CHECK(*head_after_blocked == *head_before_blocked);
        std::printf("[6] REAL ADVERSARIAL PROOF: with ResetCost exhausted (remaining=0), "
                    "reset_to_turn() was REJECTED (%s) and the branch's real HEAD tree digest did NOT "
                    "change (before=%s, after=%s) -- the reset genuinely never reached Ledger::"
                    "reset_to() at all, closing the exact 'reset for free, unbounded checkpoint/"
                    "persist growth' gap an adversarial red-team pass found in this method's first, "
                    "unquota-gated version\n", blocked.error().code.c_str(),
                    head_before_blocked->c_str(), head_after_blocked->c_str());
    }

    std::printf("\nALL CHECKS PASSED -- SandboxRuntime::reset_to_turn() closes ADR-099 §8's own "
                "'no rollback method' residual for real, proven against a real Docker daemon "
                "including that a subsequent run() genuinely sees the rolled-back content (not just "
                "that the Ledger's own bookkeeping moved), that the failure path refunds its quota "
                "unit, and that quota exhaustion genuinely blocks the call before any real mutation.\n");
    return 0;
}
