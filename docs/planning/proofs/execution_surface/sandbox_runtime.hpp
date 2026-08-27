#pragma once
// PROVE-PHASE PROBE (A3): closes this design's own "single largest remaining engineering unknown"
// (§11/§34.10) -- until this file, NOTHING in this design had a `run(command)`-shaped verb at all.
// `full_stack/real_sandbox_session.hpp`'s `SandboxSession` only ever does
// harvest_and_checkpoint()/reset_to_turn()/release_branch() against its in-memory `MediatedFileSystem`
// -- no code execution capability anywhere. `docker_sandbox/probe_docker_sandbox.cpp` (§31) proved a
// REAL Docker container can bridge into the REAL `RealIoFileSystem`/`Ledger` stack, but only as an
// AD HOC sequence of manual calls in one probe's `main()` -- never as a single, reusable, generic
// verb `SandboxSession` itself could call.
//
// `SandboxRuntime` is that verb, built fresh (new type, this file, not a modification of the
// existing `full_stack::SandboxSession`) and TEMPLATED over any `ExecutionSurface` conformer -- an
// honest note, not an established claim: exactly ONE conformer (`DockerExecutionSurface`) exists
// and has been proven against, so "generic" here means "the interface does not name Docker
// specifically," not "empirically demonstrated to fit an isolation technology of a different shape
// (e.g. a native-process/mediated-syscall backend with no separate filesystem namespace to copy
// into/out of, matching `native_jail`'s own real precedent)." A real second conformer would be
// needed to confirm the concept generalizes rather than being Docker-shaped by accident -- a REAL
// FINDING an independent architecture-fit red-team pass raised, disclosed here rather than left as
// an implicit assumption. Per explicit project-owner direction: whether a future real
// implementation folds this INTO `full_stack::SandboxSession`, replaces it, or keeps it separate is
// an implementation-time decision, not designed here.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../async_quota/async_quota.hpp"
#include "../common/result.hpp"
#include "../identity_authority/identity_authority.hpp"
#include "../worktree_io/real_io_filesystem.hpp"
#include "../worktree_io/worktree_ledger.hpp"
#include "execution_surface.hpp"

namespace probe {

// A distinct Kind tag for `AsyncQuota<RunCost>`, gating "is this principal allowed to run a
// command AT ALL" -- separate from `AsyncQuota<StorageBytes>`, which can only be sized once the
// run's REAL output is known (after it has already happened). REAL BUG a security-shaped red-team
// pass would have caught (found instead by tracing this file's own call order against its own
// comment's claim): the original version of this method claimed "the quota is consumed BEFORE the
// real command ever runs" but actually only consumed `StorageBytes` quota inside `Ledger::commit()`
// at the very END of the sequence -- AFTER `surface.run()` had already executed. A principal with
// zero remaining storage quota could run an arbitrary real command for free every single time,
// discovering the rejection only at commit -- a real, exploitable "run for free, pay nothing"
// bypass, not merely a misleading comment. Fixed by introducing this SEPARATE, small, pre-run gate
// (mirroring `AsyncQuota<BranchCost>`'s own established role gating `branch_from()` before its
// mutation), consumed before `surface.run()`, refunded (§35 finding 4's own established discipline)
// if the run's real output later fails to commit.
struct RunCost {};

struct RunOutcome {
    ExecOutcome exec;       // the real command's own exit_code/stdout -- a non-zero exit_code here
                             // is a normal, meaningful result (ExecutionSurface's own contract),
                             // never itself a `result<>` error
    Checkpoint checkpoint;  // the REAL Ledger checkpoint committing whatever the run changed
};

class SandboxRuntime {
public:
    SandboxRuntime(Ledger<>& ledger, BranchHandle<> branch, std::filesystem::path staging_root)
        : ledger_(&ledger), branch_(std::move(branch)), io_fs_(std::move(staging_root)),
          exclusivity_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    // THE real verb. Runs `command` inside `surface`, seeded with this branch's CURRENT head tree,
    // and commits whatever changed as a new, real Ledger checkpoint.
    //
    // REAL FINDING an independent architecture-fit red-team pass caught: unlike
    // `full_stack::SandboxSession::harvest_and_checkpoint()` (which holds ONE `exclusivity_` lock
    // across its entire turn), this method originally held no lock at all across its own
    // materialize -> reset -> run -> drain -> scan -> commit sequence -- two concurrent `run()`
    // calls on the SAME instance (sharing one `io_fs_` staging directory and, if passed the same
    // `surface`, one execution surface) could interleave mid-turn (e.g. call B's `reset()` racing
    // call A's still-running `surface.run()`/`drain_to()` against the same staging directory).
    // Fixed by taking `exclusivity_` for the whole call, matching the sibling type's own discipline
    // exactly, rather than leaving single-caller use as an undisclosed assumption.
    template <ExecutionSurface Surface>
    [[nodiscard]] agentengine::rt::task<result<RunOutcome>> run(
        Surface& surface, std::string command, Principal author,
        AsyncQuota<RunCost>& run_quota, AsyncQuota<StorageBytes>& storage_quota) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();

        // 0. Gate BEFORE the real command ever executes -- see RunCost's own comment above for why
        //    this is a separate quota from the storage-bytes one `commit()` charges at the end.
        auto run_consumed = co_await run_quota.try_consume(1, author);
        if (!run_consumed.has_value()) co_return std::unexpected(run_consumed.error());

        // 1. Read the branch's CURRENT head tree digest, identity-gated like every other Ledger read.
        auto head_digest = ledger_->head_tree_digest(branch_.name(), author);
        if (!head_digest.has_value()) {
            (void)co_await run_quota.refund(1);
            co_return std::unexpected(head_digest.error());
        }

        // 2. Materialize that tree onto real disk at staging_root -- the REAL rollback/checkout
        //    mechanism (§27), reused here for its other real purpose: giving an execution surface
        //    something real to seed from.
        auto materialized = co_await io_fs_.materialize(*ledger_, *head_digest, author);
        if (!materialized.has_value()) {
            (void)co_await run_quota.refund(1);
            co_return std::unexpected(materialized.error());
        }

        // 3. Push the real, materialized directory into the execution surface's own isolated view.
        auto reset_r = surface.reset(io_fs_.host_root());
        if (!reset_r.has_value()) {
            (void)co_await run_quota.refund(1);
            co_return std::unexpected(reset_r.error());
        }

        // 4. Run the real command INSIDE the surface -- never in this process. Once `exec_r` HAS a
        //    value, the RunCost charge from step 0 is what was actually paid to reach this point
        //    and is NOT refunded past that (a command that genuinely ran and produced real,
        //    attacker-or-caller-visible side effects -- real stdout, a real exit code -- is not a
        //    "nothing happened" case, matching StorageBytes/commit()'s own separate gate on the
        //    run's downstream persistence).
        //
        //    REAL FINDING an independent round-2 verification pass caught, reintroducing finding
        //    1's own bug class at a different site: `surface.run()` returning a `result<>` ERROR
        //    (as opposed to a value, even a non-zero-exit-code one) means, by `ExecutionSurface`'s
        //    own documented contract, that the command was never even ATTEMPTED inside the surface
        //    at all (e.g. `DockerExecutionSurface::run()` rejecting a command containing an
        //    ordinary double-quote via `reject_shell_breakout()` before ever calling `_popen`) --
        //    exactly the "nothing happened" case the RunCost charge should never survive. The
        //    original fix refunded every earlier failure path but not this one, so an entirely
        //    ordinary command (e.g. containing `"`) would silently burn RunCost budget for zero
        //    real execution, forever, with no attacker involved at all -- a self-inflicted
        //    quota-DoS mirroring the exact bug the RunCost fix exists to prevent.
        auto exec_r = surface.run(command);
        if (!exec_r.has_value()) {
            (void)co_await run_quota.refund(1);
            co_return std::unexpected(exec_r.error());
        }

        // 5. Pull whatever the surface produced back onto real disk at staging_root.
        auto drain_r = surface.drain_to(io_fs_.host_root());
        if (!drain_r.has_value()) co_return std::unexpected(drain_r.error());

        // 6. A REAL, full recursive scan (not the write()-tracked drain -- the surface's own writes
        //    were never staged through this object's own write() calls) captures every byte the
        //    surface actually produced, exactly §29 Attack 5's own disclosed fix direction
        //    (real_io_filesystem.hpp's own scan_and_drain_into_tree() doc comment). Also carries its
        //    own ACL-root-cap batch-validation fix (a second instance of §35 finding 10's bug class,
        //    found by the same red-team pass and fixed at its own definition site).
        auto tree = co_await io_fs_.scan_and_drain_into_tree(*ledger_, author);
        if (!tree.has_value()) co_return std::unexpected(tree.error());

        // 7. Commit through the REAL Ledger -- quota-checked (StorageBytes, sized by the run's
        //    REAL output, which could only be known now), ACL-checked, durable if `Store` is.
        auto cp = co_await ledger_->commit(branch_, *tree, author, storage_quota);
        if (!cp.has_value()) co_return std::unexpected(cp.error());

        // REAL FINDING an independent architecture-fit red-team pass caught: this turn-boundary
        // operation never ran the same per-commit maintenance step its sibling
        // (`harvest_and_checkpoint()`) treats as load-bearing -- A7/§34.6's orphan-reclaim
        // mechanism simply never fired on this path. Fixed to match.
        (void)co_await ledger_->reap_pending_abandons();

        co_return RunOutcome{*exec_r, *cp};
    }

    [[nodiscard]] std::string const& branch_name() const noexcept { return branch_.name(); }

    // A9: lets a caller minting a SIBLING staging directory for a spawned child (e.g.
    // `mandatory_sandbox/`'s own fork-via-copy-assignment) derive it relative to this instance's
    // own root, without this class needing to know anything about session-naming schemes itself.
    [[nodiscard]] std::filesystem::path const& staging_root() const noexcept {
        return io_fs_.host_root();
    }

    // A9: the real COW-branch verb a mandatory-sandbox-per-session design needs for fork_from()/
    // agent.spawn to create a genuinely fresh child sandbox, never aliasing the parent's. Thin
    // wrapper around the already-proven `Ledger::branch_from()` -- deliberately does not expose
    // `branch_` itself (no raw reference out), matching this design's "possession, not reference"
    // discipline for `BranchHandle` everywhere else.
    //
    // REAL FINDING an independent architecture-fit red-team pass caught: this used to take an
    // opaque, caller-supplied `child_staging_root` verbatim -- nothing derived it uniquely, so two
    // children forked from the same parent with the same (or colliding) caller-supplied path would
    // have their `RealIoFileSystem`s materialize/scan/drain into the SAME real host directory,
    // corrupting each other. Fixed by deriving the child's staging directory INTERNALLY from the
    // real, just-minted child branch's own name -- unique by construction (`branch_seq_` in
    // `Ledger::branch_from()`) -- via `compute_digest()`, the SAME digest-based per-session
    // subdirectory naming precedent `decisions/ADR-096-...:147-153` (C8) already established and
    // shipped in this codebase for the identical hazard class, reused correctly this time (an
    // earlier version of this file cited a DIFFERENT, never-shipped prototype as its precedent
    // instead of this real one -- fixed).
    // `const`: legitimately so, not a cast-away -- `Ledger::branch_from()` takes its parent
    // `BranchHandle` by `const&`, and locking `exclusivity_` (a `unique_ptr<AsyncMutex>`) mutates
    // the pointee, not the pointer, so nothing this method touches actually needs `*this` mutable.
    // Lets a caller holding only a `SandboxRuntime const&` (e.g. `MandatorySandboxProvider::
    // operator=(T const&)`'s own `other` parameter) still spawn a child from it.
    [[nodiscard]] agentengine::rt::task<result<SandboxRuntime>> spawn_child_branch(
        Principal creator, AsyncQuota<BranchCost>& branch_quota,
        std::filesystem::path staging_parent_dir) const {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto child_branch = co_await ledger_->branch_from(branch_, creator, branch_quota);
        if (!child_branch.has_value()) co_return std::unexpected(child_branch.error());
        std::string const& name = child_branch->name();
        std::vector<std::byte> name_bytes(name.size());
        for (std::size_t i = 0; i < name.size(); ++i) name_bytes[i] = static_cast<std::byte>(name[i]);
        auto digest = agentengine::compute_digest(name_bytes);
        if (!digest) {
            co_return std::unexpected(error{digest.error().message,
                                              "sandbox_runtime.staging_digest_failed"});
        }
        co_return SandboxRuntime(*ledger_, std::move(*child_branch), staging_parent_dir / *digest);
    }

    // A10 (2026-08-27, task-branch tool surface): the "commit" half of try/commit/discard. Consumes
    // `this` by rvalue -- matching this whole class's "possession, not reference" discipline for
    // `BranchHandle` (spawn_child_branch's own comment states this precedent) -- and folds the real
    // work this runtime's branch accumulated into `parent`'s own branch via `Ledger::merge()`'s
    // already-proven (§34.4/§34.7) real three-way merge. `parent` is taken as a `SandboxRuntime
    // const&`, never a raw `BranchHandle&` -- `Ledger::merge()`'s own `parent` parameter is itself
    // `BranchHandle<Store> const&` (read-only), so a `const&` is all this method genuinely needs;
    // same-class private access lets it reach `parent.branch_` directly even through a const
    // reference, so `branch_` itself is STILL never exposed outside this class to any caller (the
    // "no raw reference out" discipline `spawn_child_branch()` already established stays intact --
    // only two `SandboxRuntime`s talking to each other can see either one's branch). Taking `parent`
    // as `const&` specifically (not `&`) is what lets a real caller compose this directly with A9's
    // `MandatorySandboxProvider::runtime()` accessor, which already, deliberately, only ever hands
    // back a `SandboxRuntime const*` (§37) -- no new accessor needed on that class for this to work,
    // an architecture-fit red-team pass's own "the stated A9 composition is asserted, not designed"
    // finding, closed for real by this signature choice rather than merely disclosed.
    // A rejection (conflict, unauthorized reference, missing tree) is NOT this method's own failure
    // -- it is `Ledger::merge()`'s real, already-adversarially-proven behavior, reused verbatim,
    // including its own real fix (§32.4's finding): a rejected merge registers the child branch
    // into `orphaned_from_restart_` rather than losing it, so the work is not destroyed on a failed
    // commit, only left in a state this method's own caller (the task-branch tool wrapper) does not
    // automatically re-surface as a fresh, addressable handle -- a disclosed, deliberate scope
    // boundary, not an oversight (see probe_task_branch_tool.cpp's own write-up).
    [[nodiscard]] agentengine::rt::task<result<Checkpoint>> merge_into(
        SandboxRuntime const& parent, Principal requested_by) && {
        co_return co_await ledger_->merge(std::move(branch_), parent.branch_, requested_by);
    }

    // A10 fix (2026-08-27, closing the "stranded loser" gap `merge_into()`'s own comment named as a
    // disclosed, deliberate scope boundary rather than an oversight): reclaims a branch this
    // runtime's own `merge_into()` just orphaned via a REJECTED merge (or any other orphan
    // `requested_by` is authorized for) and returns it as a live, addressable `SandboxRuntime`
    // again. Thin wrapper around the already-proven, ACL-gated A7 `Ledger::reclaim_orphaned_branch()`
    // -- never a raw handle-construction bypass (that stays `Ledger`-friend-only), and never widens
    // authority: `reclaim_orphaned_branch()` itself requires `requested_by` be ALREADY authorized
    // for the orphaned branch's current head tree, the identical check every other read uses. `const`
    // for the same reason `spawn_child_branch()` is: this touches only `ledger_` (a pointer, not the
    // pointee) and never `this->branch_`, so a caller holding only a `SandboxRuntime const&` (e.g.
    // `main_` inside `TaskBranchSandbox`, held exactly that way per its own header comment) can still
    // call this. Deliberately NOT a method on `Ledger` directly reachable from `TaskBranchSandbox` --
    // routing it through `SandboxRuntime` keeps `Ledger` itself unreachable from the tool-surface
    // layer, matching this whole design's "possession of a `SandboxRuntime`, never a raw `Ledger&`,
    // is what a tool-facing wrapper is trusted with" discipline.
    [[nodiscard]] agentengine::rt::task<result<SandboxRuntime>> reclaim_orphaned_child(
        std::string const& branch_name, Principal requested_by,
        std::filesystem::path staging_parent_dir) const {
        auto reclaimed = ledger_->reclaim_orphaned_branch(branch_name, requested_by);
        if (!reclaimed.has_value()) co_return std::unexpected(reclaimed.error());
        std::vector<std::byte> name_bytes(branch_name.size());
        for (std::size_t i = 0; i < branch_name.size(); ++i) {
            name_bytes[i] = static_cast<std::byte>(branch_name[i]);
        }
        auto digest = agentengine::compute_digest(name_bytes);
        if (!digest) {
            co_return std::unexpected(error{digest.error().message,
                                              "sandbox_runtime.staging_digest_failed"});
        }
        co_return SandboxRuntime(*ledger_, std::move(*reclaimed), staging_parent_dir / *digest);
    }

    // A10: the "discard" half. Consumes `this`, abandons the branch outright -- `Ledger::abandon()`
    // performs no authorization check of its own (by design: possessing the `BranchHandle` at all
    // already required an authorized `spawn_child_branch()` call to obtain it -- possession IS the
    // authorization, the same discipline every other mutating Ledger call in this design already
    // follows). The parent branch this runtime was forked from is completely untouched.
    [[nodiscard]] agentengine::rt::task<result<void>> discard() && {
        co_return co_await ledger_->abandon(std::move(branch_));
    }

private:
    Ledger<>* ledger_;
    BranchHandle<> branch_;
    RealIoFileSystem io_fs_;
    std::unique_ptr<agentengine::rt::AsyncMutex> exclusivity_;
};

}  // namespace probe
