#pragma once
// Implements ADR-102 Phase 3 (identity-native sandbox/worktree design, ADR-099 §7 A3) --
// `SandboxRuntime`: the real `run(command)`-shaped verb this design's own A3 probe first closed --
// composes `Ledger<Store>` (core/ledger.hpp, Phase 2; `Store` defaulted to `InMemoryWorktreeObjectStore`,
// see this file's own ADR-132 comment below) + `RealIoFileSystem` (sandbox/real_io_filesystem.hpp)
// + any real `ExecutionSurface` conformer (sandbox/execution_surface.hpp) into one quota-gated,
// checkpoint-producing operation.
//
// Ported from docs/planning/proofs/execution_surface/sandbox_runtime.hpp (ADR-099's own standalone,
// five-round red-teamed, live-Docker-tested prove-phase original -- kept as-is, this is a new file).
// Real changes made during the port, not cosmetic:
//   - `probe::Principal` -> `agentengine::IdentityHandle` (ADR-102 Phase 1's naming decision).
//   - `probe::Ledger<>`/`probe::BranchHandle<>`/`probe::Checkpoint` ->
//     `agentengine::Ledger<>`/`agentengine::BranchHandle<>`/`agentengine::Checkpoint` (Phase 2).
//   - `probe::AsyncQuota<T>` -> `agentengine::rt::AsyncQuota<T>` (Phase 1).
//   - `probe::RealIoFileSystem` -> `agentengine::RealIoFileSystem` (this phase, sibling file).
//   - `probe::ExecutionSurface`/`probe::ExecOutcome` -> `agentengine::ExecutionSurface`/
//     `agentengine::SurfaceRunOutcome` (this phase, sibling file).
//   - `probe::error{message, code}` -> the real `agentengine::error{failure_class, message, code}`
//     (`fatal` for the internal staging-digest-computation failure -- the only error this file
//     constructs directly; every other error is passed through verbatim from `Ledger`/`RealIoFileSystem`/
//     the `ExecutionSurface` conformer, unwrapped, never re-classified).
//   - `probe::RunOutcome` -> `SandboxRunOutcome`: a real, registered name collision was found against
//     the existing, shipped `agentengine::a2a::RunOutcome` (protocol/a2a) during vocabulary
//     registration for this phase -- a different concept entirely (an A2A task's outcome vs. one
//     sandboxed command's outcome) that must not share a bare name, matching this whole design's own
//     "keep a distinct name where the concept genuinely differs" discipline (027 §1, already applied
//     to `IdentityHandle` vs. `Principal` in Phase 1).
//
// Every "REAL FINDING"/"REAL GAP" comment below documents a genuine defect an independent red-team
// pass on the prove-phase original found and fixed BEFORE this port -- carried forward verbatim as
// disclosure, not re-litigated here. This port's OWN red-team round (required before Phase 3 is
// considered done, matching Phase 1/2's own track record) is recorded in ADR-102, not in this file.
//
// ADR-132 -- `SandboxRuntime` gained a `Store` template parameter (defaulted to
// `agentengine::InMemoryWorktreeObjectStore`, matching `Ledger<Store>`'s own default), closing the
// "hardcoded to `Ledger<>`, not `Store`-generic" gap `ADR-130` §2 found and explicitly left as separate,
// larger follow-on work. Every member's `Ledger<>`/`BranchHandle<>` became `Ledger<Store>`/
// `BranchHandle<Store>`; every method BODY is otherwise unchanged -- this is a pure widening of what
// `Store` this class can be bound to, not a behavior change for the default case. Every existing,
// already-verified caller that never named `Store` explicitly (every real production tool, every test in
// this whole session's own task-branch/crash-recovery line) continues to compile and behave IDENTICALLY,
// since omitting the template argument (or relying on C++17 class template argument deduction from a
// `Ledger<>&` constructor argument) still resolves to the same `InMemoryWorktreeObjectStore` default it
// was hardcoded to before.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/ledger.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

// A distinct Kind tag for `agentengine::rt::AsyncQuota<RunCost>`, gating "is this identity allowed to
// run a command AT ALL" -- separate from `AsyncQuota<StorageBytes>` (core/ledger.hpp), which can only
// be sized once the run's REAL output is known (after it has already happened). REAL BUG the
// prove-phase original's own red-team pass caught (carried forward as disclosure, not re-found here):
// an earlier version of `run()` below claimed, in comment only, "the quota is consumed BEFORE the real
// command ever runs" but actually consumed `StorageBytes` quota only inside `Ledger::commit()`, at the
// very END of the sequence -- AFTER `surface.run()` had already executed. An identity with zero
// remaining storage quota could run an arbitrary real command for free every time, discovering the
// rejection only at commit -- a real "run for free, pay nothing" bypass, not merely a misleading
// comment. Fixed by this separate, small, pre-run gate, consumed before `surface.run()`, refunded (see
// `run()`'s own step-by-step comments) if the run's real output later fails to commit.
struct RunCost {};

// A distinct Kind tag for `agentengine::rt::AsyncQuota<ResetCost>`, gating `reset_to_turn()` -- REAL
// GAP the prove-phase original's own adversarial pass found: an earlier version of `reset_to_turn()`
// took no quota at all, unlike every other mutating verb on this class. A caller already holding a
// bound `SandboxRuntime` could call `reset_to_turn()` in a tight loop for free: each call inserts a new,
// never-evicted entry into `Ledger`'s own per-branch checkpoint map (unbounded growth), and when a
// durable `Store` is configured, triggers a full re-serialization of every branch and checkpoint on
// every call -- a real, unbudgeted resource-exhaustion vector this project's own I8 invariant exists to
// prevent. Mirrors `BranchCost`'s own role exactly: gates "is this identity allowed to reset AT ALL"
// before the mutation, consumed first, refunded on any failure.
struct ResetCost {};

struct SandboxRunOutcome {
    agentengine::SurfaceRunOutcome exec;  // the real command's own exit_code/stdout -- a non-zero
                                            // exit_code is a normal, meaningful result, never itself a
                                            // `result<>` error
    agentengine::Checkpoint checkpoint;    // the REAL Ledger checkpoint committing whatever the run
                                            // changed
};

template <class Store = agentengine::InMemoryWorktreeObjectStore>
class SandboxRuntime {
public:
    SandboxRuntime(agentengine::Ledger<Store>& ledger, agentengine::BranchHandle<Store> branch,
                    std::filesystem::path staging_root)
        : ledger_(&ledger), branch_(std::move(branch)), io_fs_(std::move(staging_root)),
          exclusivity_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    // THE real verb. Runs `command` inside `surface`, seeded with this branch's CURRENT head tree, and
    // commits whatever changed as a new, real Ledger checkpoint.
    //
    // REAL FINDING the prove-phase original's own architecture-fit red-team pass caught: an earlier
    // version held no lock at all across its own materialize -> reset -> run -> drain -> scan -> commit
    // sequence -- two concurrent `run()` calls on the SAME instance (sharing one `io_fs_` staging
    // directory and, if passed the same `surface`, one execution surface) could interleave mid-turn.
    // Fixed by taking `exclusivity_` for the whole call.
    template <ExecutionSurface Surface>
    [[nodiscard]] agentengine::rt::task<agentengine::result<SandboxRunOutcome>> run(
        Surface& surface, std::string command, agentengine::IdentityHandle author,
        agentengine::rt::AsyncQuota<RunCost>& run_quota,
        agentengine::rt::AsyncQuota<StorageBytes>& storage_quota) {
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
        //    mechanism, reused here for its other real purpose: giving an execution surface something
        //    real to seed from.
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
        //    value, the RunCost charge from step 0 is what was actually paid to reach this point and is
        //    NOT refunded past that (a command that genuinely ran and produced real, caller-visible
        //    side effects -- real stdout, a real exit code -- is not a "nothing happened" case).
        //
        //    REAL FINDING the prove-phase original's own round-2 verification pass caught, reintroducing
        //    finding 0's own bug class at a different site: `surface.run()` returning a `result<>`
        //    ERROR (as opposed to a value, even a non-zero-exit-code one) means, by `ExecutionSurface`'s
        //    own documented contract, that the command was never even ATTEMPTED inside the surface at
        //    all (e.g. `DockerExecutionSurface::run()` rejecting a command via `docker_cli_reject_shell_
        //    breakout()` before ever shelling out) -- exactly the "nothing happened" case the RunCost
        //    charge should never survive. Fixed here by refunding this path too, not only the earlier
        //    ones.
        auto exec_r = surface.run(command);
        if (!exec_r.has_value()) {
            (void)co_await run_quota.refund(1);
            co_return std::unexpected(exec_r.error());
        }

        // 5. Pull whatever the surface produced back onto real disk at staging_root.
        auto drain_r = surface.drain_to(io_fs_.host_root());
        if (!drain_r.has_value()) co_return std::unexpected(drain_r.error());

        // 6. A REAL, full recursive scan (not a write()-tracked drain -- the surface's own writes were
        //    never staged through this object's own write() calls) captures every byte the surface
        //    actually produced.
        auto tree = co_await io_fs_.scan_and_drain_into_tree(*ledger_, author);
        if (!tree.has_value()) co_return std::unexpected(tree.error());

        // 7. Commit through the REAL Ledger -- quota-checked (StorageBytes, sized by the run's REAL
        //    output, which could only be known now), ACL-checked, durable if `Store` is.
        auto cp = co_await ledger_->commit(branch_, *tree, author, storage_quota);
        if (!cp.has_value()) co_return std::unexpected(cp.error());

        // REAL FINDING the prove-phase original's own architecture-fit red-team pass caught: this
        // turn-boundary operation never ran the same per-commit maintenance step its sibling
        // (`full_stack::SandboxSession::harvest_and_checkpoint()`) treats as load-bearing -- the
        // orphan-reclaim mechanism simply never fired on this path. Fixed to match.
        (void)co_await ledger_->reap_pending_abandons();

        co_return SandboxRunOutcome{*exec_r, *cp};
    }

    // Closes this design's own disclosed §5/ADR-099 residual -- "Ledger::reset_to() is real and proven,
    // but SandboxRuntime has no rollback method of its own." Thin wrapper: the REAL work is `Ledger::
    // reset_to()`'s (already-proven, Phase 2) job, this method only supplies the branch this runtime
    // owns, the `ResetCost` quota gate (see that tag's own comment above), and the concurrency
    // discipline `run()` itself established. Deliberately does NOT touch `io_fs_`/the real staging
    // directory on disk -- moving the Ledger's own HEAD pointer back is the whole of what "rollback"
    // means at this layer; the NEXT `run()` call's own step 2 already, unconditionally, re-seeds
    // staging from whatever the CURRENT head tree is.
    //
    // `exclusivity_` is taken for the same reason `run()` takes it: a concurrent `run()` racing this
    // call could otherwise materialize from a head that is moving out from under it mid-turn. Same
    // reentrancy caveat as `run()`/`spawn_child_branch()`, disclosed rather than left implicit a second
    // time: calling `reset_to_turn()` reentrantly, on the same OS thread, from code already running
    // inside an in-flight `run()` (or `spawn_child_branch()`) call on the SAME instance would
    // self-deadlock on the same, already-held, non-reentrant `exclusivity_` lock -- not reachable
    // through any real caller this phase builds (a host-level API, no tool body synchronously re-enters
    // it), but a real structural constraint worth stating rather than assuming.
    //
    // core/ledger.hpp's own `reset_to()` disclosure applies here unchanged, not silently repeated by
    // omission: `Ledger::reset_to()` performs no `authorized_for()` check of its own -- possession of
    // this `SandboxRuntime` is the entire authorization boundary, the same discipline `discard()`/
    // `abandon()` already rely on. Not a regression this port introduced, and not currently reachable
    // from anything derived from model output (no `Tool<>` wires this yet, I2/I3).
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Checkpoint>> reset_to_turn(
        std::uint64_t target_turn_index, agentengine::IdentityHandle requested_by,
        agentengine::rt::AsyncQuota<ResetCost>& reset_quota) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto consumed = co_await reset_quota.try_consume(1, requested_by);
        if (!consumed.has_value()) co_return std::unexpected(consumed.error());
        auto outcome = co_await ledger_->reset_to(branch_, target_turn_index, requested_by);
        if (!outcome.has_value()) {
            (void)co_await reset_quota.refund(1);
            co_return std::unexpected(outcome.error());
        }
        co_return *outcome;
    }

    [[nodiscard]] std::string const& branch_name() const noexcept { return branch_.name(); }

    // Lets a caller minting a SIBLING staging directory for a spawned child derive it relative to this
    // instance's own root, without this class needing to know anything about session-naming schemes
    // itself.
    [[nodiscard]] std::filesystem::path const& staging_root() const noexcept {
        return io_fs_.host_root();
    }

    // The real COW-branch verb a mandatory-sandbox-per-session design needs for fork_from()/agent.spawn
    // to create a genuinely fresh child sandbox, never aliasing the parent's. Thin wrapper around the
    // already-proven `Ledger::branch_from()` -- deliberately does not expose `branch_` itself (no raw
    // reference out), matching this design's "possession, not reference" discipline for `BranchHandle`
    // everywhere else.
    //
    // REAL FINDING the prove-phase original's own architecture-fit red-team pass caught: an earlier
    // version took an opaque, caller-supplied `child_staging_root` verbatim -- nothing derived it
    // uniquely, so two children forked from the same parent with the same (or colliding) caller-supplied
    // path would have their `RealIoFileSystem`s materialize/scan/drain into the SAME real host
    // directory, corrupting each other. Fixed by deriving the child's staging directory INTERNALLY from
    // the real, just-minted child branch's own name -- unique by construction (`Ledger::branch_from()`'s
    // own internal sequence counter) -- via `compute_digest()`, the same digest-based per-session
    // subdirectory naming precedent `decisions/ADR-096-...` already established and shipped in this
    // codebase for the identical hazard class.
    //
    // `const`: legitimately so, not a cast-away -- `Ledger::branch_from()` takes its parent
    // `BranchHandle` by `const&`, and locking `exclusivity_` (a `unique_ptr<AsyncMutex>`) mutates the
    // pointee, not the pointer, so nothing this method touches actually needs `*this` mutable. Lets a
    // caller holding only a `SandboxRuntime const&` still spawn a child from it.
    [[nodiscard]] agentengine::rt::task<agentengine::result<SandboxRuntime>> spawn_child_branch(
        agentengine::IdentityHandle creator, agentengine::rt::AsyncQuota<BranchCost>& branch_quota,
        std::filesystem::path staging_parent_dir) const {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto child_branch = co_await ledger_->branch_from(branch_, creator, branch_quota);
        if (!child_branch.has_value()) co_return std::unexpected(child_branch.error());
        std::string const& name = child_branch->name();
        std::vector<std::byte> name_bytes(name.size());
        for (std::size_t i = 0; i < name.size(); ++i) name_bytes[i] = static_cast<std::byte>(name[i]);
        auto digest = agentengine::compute_digest(name_bytes);
        if (!digest) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                             digest.error().message,
                                                             "sandbox_runtime.staging_digest_failed"});
        }
        co_return SandboxRuntime(*ledger_, std::move(*child_branch), staging_parent_dir / *digest);
    }

    // A10-shaped: the "commit" half of try/commit/discard. Consumes `this` by rvalue -- matching this
    // whole class's "possession, not reference" discipline for `BranchHandle` (`spawn_child_branch()`'s
    // own comment states this precedent) -- and folds the real work this runtime's branch accumulated
    // into `parent`'s own branch via `Ledger::merge()`'s already-proven real three-way merge. `parent`
    // is taken as a `SandboxRuntime const&`; same-class private access lets it reach `parent.branch_`
    // directly even through a const reference, so `branch_` itself is still never exposed outside this
    // class to any OTHER caller.
    // A rejection (conflict, unauthorized reference, missing tree) is NOT this method's own failure --
    // it is `Ledger::merge()`'s real, already-adversarially-proven behavior, reused verbatim, including
    // its own real fix (Phase 2 §12): a rejected merge registers the child branch into Ledger's own
    // orphan set rather than losing it, so the work is not destroyed on a failed commit, only left in a
    // state this method's own caller does not automatically re-surface as a fresh, addressable handle.
    //
    // REAL FINDING an independent red-team pass on this port caught (2026-08-28): this method existed
    // identically in the prove-phase original (surviving five prior red-team rounds on that file
    // without being named) with NO `exclusivity_` lock, unlike every other method that reads or
    // mutates `branch_` (`run()`/`reset_to_turn()`/`spawn_child_branch()` all take it first). `Ledger::
    // merge()` takes its `child` parameter BY VALUE, so `std::move(branch_)` below synchronously
    // mutates this object's own `branch_` member (via `BranchHandle`'s plain, unsynchronized move
    // constructor) before any lock is held -- a concurrent in-flight `run()` on the SAME instance
    // reading `branch_` (e.g. its own `branch_.name()`/`commit(branch_, ...)` call) could observe a
    // torn read against this method's own move, the identical interleaving hazard class `run()`'s own
    // top comment already names `exclusivity_` as existing to prevent, just via a different method
    // pair than the ones already covered. Not reachable through any real caller this phase builds (a
    // host-level API, no concurrent orchestrator wiring exists yet), but fixed here rather than left as
    // a silent gap in this class's own stated locking discipline, ahead of Phase 4/5 wiring a real
    // concurrent caller to it.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Checkpoint>> merge_into(
        SandboxRuntime const& parent, agentengine::IdentityHandle requested_by,
        agentengine::rt::AsyncQuota<MergeCost>& merge_quota) && {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        co_return co_await ledger_->merge(std::move(branch_), parent.branch_, requested_by, merge_quota);
    }

    // Reclaims a branch this runtime's own `merge_into()` just orphaned via a REJECTED merge (or any
    // other orphan `requested_by` is authorized for) and returns it as a live, addressable
    // `SandboxRuntime` again. Thin wrapper around the already-proven, ACL-gated `Ledger::
    // reclaim_orphaned_branch()` -- never a raw handle-construction bypass (that stays `Ledger`-friend-
    // only), and never widens authority: `reclaim_orphaned_branch()` itself requires `requested_by` be
    // ALREADY authorized for the orphaned branch's current head tree, the identical check every other
    // read uses. `const` for the same reason `spawn_child_branch()` is. Deliberately NOT a method a
    // caller could reach directly on `Ledger` -- routing it through `SandboxRuntime` keeps `Ledger`
    // itself unreachable from a tool-surface layer.
    [[nodiscard]] agentengine::rt::task<agentengine::result<SandboxRuntime>> reclaim_orphaned_child(
        std::string const& branch_name, agentengine::IdentityHandle requested_by,
        std::filesystem::path staging_parent_dir) const {
        auto reclaimed = ledger_->reclaim_orphaned_branch(branch_name, requested_by);
        if (!reclaimed.has_value()) co_return std::unexpected(reclaimed.error());
        std::vector<std::byte> name_bytes(branch_name.size());
        for (std::size_t i = 0; i < branch_name.size(); ++i) {
            name_bytes[i] = static_cast<std::byte>(branch_name[i]);
        }
        auto digest = agentengine::compute_digest(name_bytes);
        if (!digest) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                             digest.error().message,
                                                             "sandbox_runtime.staging_digest_failed"});
        }
        co_return SandboxRuntime(*ledger_, std::move(*reclaimed), staging_parent_dir / *digest);
    }

    // The "discard" half. Consumes `this`, abandons the branch outright -- `Ledger::abandon()` performs
    // no authorization check of its own (by design: possessing the `BranchHandle` at all already
    // required an authorized `spawn_child_branch()` call to obtain it -- possession IS the
    // authorization). The parent branch this runtime was forked from is completely untouched.
    // Takes `exclusivity_` for the same real reason `merge_into()`'s own comment above states: `Ledger::
    // abandon()` also takes its `child` parameter by value, so `std::move(branch_)` here is the same
    // unsynchronized-mutation-without-a-lock hazard class, closed the same way.
    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> discard() && {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        co_return co_await ledger_->abandon(std::move(branch_));
    }

private:
    agentengine::Ledger<Store>* ledger_;
    agentengine::BranchHandle<Store> branch_;
    agentengine::RealIoFileSystem io_fs_;
    std::unique_ptr<agentengine::rt::AsyncMutex> exclusivity_;
};

}  // namespace agentengine
