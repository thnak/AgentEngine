// Proves ADR-102 Phase 3 (decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md's own
// roadmap) -- SandboxRuntime (sandbox/sandbox_runtime.hpp), driven against a REAL Docker daemon via
// DockerExecutionSurface (sandbox/docker_execution_surface.hpp), not a mock. Ported from
// docs/planning/proofs/execution_surface/{probe_execution_surface.cpp,probe_sandbox_rollback.cpp}
// (ADR-099's own standalone, red-teamed, live-Docker-tested prove-phase originals).
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- every check below shells
// out to a REAL container, matching this whole phase's own "never a mock" discipline.
//
//   [1]  run() materializes the branch's current tree, seeds a real, fresh Docker container from it,
//        runs a real command INSIDE the container, drains what changed, and commits a real Ledger
//        checkpoint -- exit_code/stdout are the real container's own.
//   [2]  The committed tree genuinely contains the file the container wrote, read back through the
//        real, identity-gated Ledger API.
//   [3]  A SECOND real turn, against a genuinely FRESH container (reset() destroys and recreates it),
//        can still see the first turn's file -- its only path there is Ledger.materialize() seeding
//        the new container from the real checkpoint chain, not container-level memory.
//   [4]  A non-zero exit code is a normal, meaningful SandboxRunOutcome, not a result<>-level error;
//        the turn still commits.
//   [5]  RunCost is consumed BEFORE the real command executes, not merely before commit: with
//        run_quota exhausted, run() is rejected AND the real Docker container count does not change.
//   [6]  RunCost is REFUNDED when the surface rejects a command before ever attempting it (a
//        pre-execution argv-safety refusal -- an embedded NUL byte, the one check that survives
//        `DockerCliBackend` no longer shelling commands through a host shell at all, see
//        docker_execution_surface.hpp's own top comment) -- not silently kept for zero real
//        execution.
//   [7]  reset_to_turn() moves the branch's HEAD back to an earlier real checkpoint as a NEW forward
//        checkpoint (never an in-place history rewrite).
//   [8]  The REAL, load-bearing rollback proof: the NEXT run(), against a genuinely fresh container,
//        sees the ROLLED-BACK content, not what a later turn had written -- proving materialize()
//        actually drives what execution sees, not merely Ledger-internal bookkeeping.
//   [9]  reset_to_turn() to a turn_index that was never a real checkpoint fails closed
//        (ledger.no_such_checkpoint) and fully refunds the ResetCost unit it consumed.
//   [10] With ResetCost exhausted, reset_to_turn() is rejected (async_quota.exhausted) and the
//        branch's real head tree digest does not change at all.

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

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

[[nodiscard]] std::size_t count_docker_containers() {
    auto r = docker_cli_detail::run_capture("docker ps -a -q");
    return static_cast<std::size_t>(std::count(r.stdout_text.begin(), r.stdout_text.end(), '\n'));
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("sandbox-runtime-test-owner");

    Ledger<> ledger;
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    check(storage_quota_r.has_value(), "AsyncQuota<StorageBytes>::mint_root(owner) succeeds");
    if (!storage_quota_r.has_value()) return EXIT_FAILURE;
    auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    check(run_quota_r.has_value(), "AsyncQuota<RunCost>::mint_root(owner) succeeds");
    if (!run_quota_r.has_value()) return EXIT_FAILURE;
    auto reset_quota_r = agentengine::rt::AsyncQuota<ResetCost>::mint_root(authority, owner, 100);
    check(reset_quota_r.has_value(), "AsyncQuota<ResetCost>::mint_root(owner) succeeds");
    if (!reset_quota_r.has_value()) return EXIT_FAILURE;
    auto& storage_quota = *storage_quota_r;
    auto& run_quota = *run_quota_r;
    auto& reset_quota = *reset_quota_r;

    auto root_r = drive(ledger.create_root_branch(owner));
    check(root_r.has_value(), "create_root_branch(owner) succeeds");
    if (!root_r.has_value()) return EXIT_FAILURE;

    std::filesystem::path const staging = std::filesystem::temp_directory_path() / "ae_test_sandbox_runtime";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);

    SandboxRuntime runtime(ledger, std::move(*root_r), staging);
    DockerExecutionSurface surface;

    // [1]/[2] Turn 1: a real command runs inside a real, fresh container and writes a real file.
    auto r1 = drive(runtime.run(surface, "echo -n 'turn-1 content' > note.txt && cat note.txt", owner,
                                  run_quota, storage_quota));
    check(r1.has_value(), "turn 1 run() succeeds");
    if (r1.has_value()) {
        check(r1->exec.exit_code == 0, "turn 1 exit_code == 0");
        check(r1->exec.stdout_text.find("turn-1 content") != std::string::npos,
              "turn 1 stdout contains the file's own content");
        check(r1->checkpoint.turn_index == 1, "turn 1 checkpoint.turn_index == 1");

        auto tree1 = ledger.get_tree_safe(r1->checkpoint.tree, owner);
        check(tree1.has_value(), "turn 1 committed tree is readable by its own author");
        if (tree1.has_value()) {
            bool found_note = false;
            agentengine::Digest note_digest;
            for (auto const& e : tree1->entries) {
                if (e.name == "note.txt") { found_note = true; note_digest = e.digest; }
            }
            check(found_note, "turn 1 committed tree contains note.txt");
            if (found_note) {
                auto note_bytes = ledger.get_blob_safe(note_digest, owner);
                check(note_bytes.has_value(), "turn 1 note.txt blob is readable");
                if (note_bytes.has_value()) {
                    std::string const content(reinterpret_cast<char const*>(note_bytes->data()),
                                                 note_bytes->size());
                    check(content == "turn-1 content", "turn 1 note.txt content matches exactly");
                }
            }
        }
    }

    // [3] Turn 2: a genuinely FRESH container (reset() destroys and recreates it) can still see turn
    // 1's file -- its only path there is Ledger.materialize() seeding the new container for real.
    auto r2 = drive(runtime.run(surface, "cat note.txt && echo -n ' + turn-2 addition' >> note.txt", owner,
                                  run_quota, storage_quota));
    check(r2.has_value(), "turn 2 run() succeeds");
    if (r2.has_value()) {
        check(r2->exec.exit_code == 0, "turn 2 exit_code == 0");
        check(r2->exec.stdout_text.find("turn-1 content") != std::string::npos,
              "turn 2's fresh container genuinely saw turn 1's content via materialize()");
        check(r2->checkpoint.turn_index == 2, "turn 2 checkpoint.turn_index == 2");
    }

    // [4] A non-zero exit code is a normal result, not a result<>-level error -- the turn still commits.
    auto r3 = drive(runtime.run(surface, "exit 7", owner, run_quota, storage_quota));
    check(r3.has_value(), "a command exiting non-zero is still a successful run()");
    if (r3.has_value()) {
        check(r3->exec.exit_code == 7, "non-zero exit_code passed through as a normal result");
        check(r3->checkpoint.turn_index == 3, "turn 3 checkpoint.turn_index == 3");
    }

    // [5] REAL ADVERSARIAL PROOF: RunCost is consumed BEFORE the real command executes. With
    // run_quota exhausted to zero, run() must be rejected AND the real Docker container count on the
    // host must not change -- not merely that commit() fails afterward.
    {
        auto exhausted_run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 0);
        check(exhausted_run_quota_r.has_value(), "mint_root(RunCost, 0) succeeds");
        if (exhausted_run_quota_r.has_value()) {
            check(exhausted_run_quota_r->remaining() == 0, "exhausted run_quota remaining() == 0");
            std::size_t const before = count_docker_containers();
            auto blocked = drive(runtime.run(surface, "echo SHOULD_NEVER_RUN", owner,
                                                *exhausted_run_quota_r, storage_quota));
            check(!blocked.has_value(), "run() with exhausted RunCost is rejected");
            if (!blocked.has_value()) {
                check(blocked.error().code == "async_quota.exhausted",
                      "rejection carries async_quota.exhausted");
            }
            std::size_t const after = count_docker_containers();
            check(after == before, "real Docker container count did not change on RunCost rejection");
        }
    }

    // [6] REAL ADVERSARIAL PROOF: RunCost is REFUNDED when the surface rejects a command before ever
    // attempting it (a pre-execution argv-safety refusal) -- not silently kept for zero real
    // execution. Post-argv-hardening, a double-quote/`%`/`^` no longer trips any guard at all (that
    // was exactly issue #50's own over-blocking defect -- those characters are only dangerous relative
    // to a host shell DockerCliBackend no longer has, docker_execution_surface.hpp's own top comment)
    // -- the one check that still rejects a command before it ever reaches `docker` is an embedded NUL
    // byte, matching `ContainerdCliBackend`'s own identical, already-shipped posture.
    {
        std::uint64_t const before_remaining = run_quota.remaining();
        std::string command_with_embedded_nul = "echo this command has an embedded NUL byte -> ";
        command_with_embedded_nul.push_back('\0');
        command_with_embedded_nul += "<- right there";
        auto rejected = drive(runtime.run(surface, command_with_embedded_nul, owner, run_quota, storage_quota));
        check(!rejected.has_value(), "a NUL-byte command fails run() before ever reaching docker");
        if (!rejected.has_value()) {
            check(rejected.error().code == "docker_cli_backend.unsafe_argv_value",
                  "rejection carries docker_cli_backend.unsafe_argv_value");
        }
        check(run_quota.remaining() == before_remaining,
              "RunCost fully refunded when the surface never attempted the command");
    }

    // [7]/[8] reset_to_turn(1) moves HEAD back to turn 1's tree as a NEW forward checkpoint (turn 4),
    // and the NEXT run() against a genuinely fresh container sees the rolled-back content for real.
    if (r1.has_value()) {
        agentengine::Digest const tree_v1 = r1->checkpoint.tree;
        auto reset_r = drive(runtime.reset_to_turn(1, owner, reset_quota));
        check(reset_r.has_value(), "reset_to_turn(1) succeeds");
        if (reset_r.has_value()) {
            check(reset_r->tree == tree_v1, "reset_to_turn(1) HEAD tree matches turn 1's real tree digest");
            check(reset_r->turn_index == 4, "reset_to_turn(1) recorded as a NEW forward checkpoint (turn 4)");
        }

        auto r4 = drive(runtime.run(surface, "cat note.txt", owner, run_quota, storage_quota));
        check(r4.has_value(), "post-rollback run() succeeds");
        if (r4.has_value()) {
            check(r4->exec.stdout_text == "turn-1 content",
                  "a genuinely fresh container, seeded from the rolled-back tree, reads turn 1's content");
            check(r4->checkpoint.turn_index == 5, "post-rollback run() checkpoint.turn_index == 5");
        }
    }

    // [9] reset_to_turn() to a turn_index that was never a real checkpoint fails closed and fully
    // refunds the ResetCost unit it consumed.
    {
        std::uint64_t const before_remaining = reset_quota.remaining();
        auto bad_reset = drive(runtime.reset_to_turn(9999, owner, reset_quota));
        check(!bad_reset.has_value(), "reset_to_turn() to a nonexistent turn is rejected");
        if (!bad_reset.has_value()) {
            check(bad_reset.error().code == "ledger.no_such_checkpoint",
                  "rejection carries ledger.no_such_checkpoint");
        }
        check(reset_quota.remaining() == before_remaining,
              "ResetCost fully refunded on a nonexistent-checkpoint rejection");
    }

    // [10] REAL ADVERSARIAL PROOF: with ResetCost exhausted, reset_to_turn() is rejected and the
    // branch's real head tree digest does not change at all -- not merely that the call reports
    // failure while a real mutation still happened.
    {
        auto exhausted_reset_quota_r = agentengine::rt::AsyncQuota<ResetCost>::mint_root(authority, owner, 0);
        check(exhausted_reset_quota_r.has_value(), "mint_root(ResetCost, 0) succeeds");
        if (exhausted_reset_quota_r.has_value()) {
            auto head_before = ledger.head_tree_digest(runtime.branch_name(), owner);
            check(head_before.has_value(), "head_tree_digest() readable before the blocked reset");
            auto blocked = drive(runtime.reset_to_turn(1, owner, *exhausted_reset_quota_r));
            check(!blocked.has_value(), "reset_to_turn() with exhausted ResetCost is rejected");
            if (!blocked.has_value()) {
                check(blocked.error().code == "async_quota.exhausted",
                      "rejection carries async_quota.exhausted");
            }
            auto head_after = ledger.head_tree_digest(runtime.branch_name(), owner);
            check(head_after.has_value(), "head_tree_digest() still readable after the blocked reset");
            if (head_before.has_value() && head_after.has_value()) {
                check(*head_before == *head_after,
                      "branch's real head tree digest did not change on ResetCost rejection");
            }
        }
    }

    std::filesystem::remove_all(staging, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- SandboxRuntime::run()/reset_to_turn() work end to end against "
                     "a REAL Docker daemon, including multi-turn persistence through the real Ledger "
                     "checkpoint chain, RunCost/ResetCost pre-mutation gating with refund-on-failure, "
                     "and a real rollback that a subsequent run() genuinely observes.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
