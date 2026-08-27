#pragma once
// A10 (2026-08-27): the missing piece the 2026-08-27 real-world-use-case research pass found --
// docs/research/2026-08-27-real-world-agent-use-case-coverage.md's #1 finding: every actively-
// developed coding agent surveyed (Claude Code `--worktree`, Cursor `/worktree`+`/best-of-n`,
// GitHub Copilot coding agent's per-session worktree) ships branch/worktree isolation as the
// PRIMARY mechanism an agent's own tool-calling loop can invoke -- "isolate one attempt, evaluate
// it, merge or discard" -- and this design had every underlying primitive (`Ledger::branch_from`/
// `merge`/`abandon`, `SandboxRuntime::spawn_child_branch`) proven standalone (§34, §36) but NO
// agent/tool-facing surface at all. This file is that surface, matching the real, shipped
// `RunShellTool`/`SessionShellSandbox` pattern found by direct inspection of
// `src/backends/native_jail/session_shell_wiring.hpp`/`sandbox_tool_provider.hpp`: a `Tool<>`
// declares schema/capability shape (see `probe_task_branch_tool.cpp`'s own fake-tool-call harness
// for the shape a real `Tool<>`+`make_tool_descriptor_with_invoke` binding would close over), and
// the REAL behavior lives in a session-scoped object constructed once per session -- here,
// `TaskBranchSandbox` -- exactly mirroring `SessionShellSandbox`'s own role.
//
// SCOPE, deliberately narrow: this closes "try/commit/discard within ONE already-running session,"
// the git-worktree-per-task pattern. It is NOT A9's `MandatorySandboxProvider` (§37, which clones a
// WHOLE session -- conversation state included -- for agent.spawn-shaped delegation) and does not
// replace it. The composition IS real, not merely asserted: `main` is taken as `SandboxRuntime
// const&`, matching exactly what `MandatorySandboxProvider::runtime()` already, deliberately, hands
// back (`SandboxRuntime const*`, §37) -- no new accessor is needed on that class for a spawned
// child session, per A9, to also use this mechanism for its own task-branch isolation. (An
// independent architecture-fit red-team round first found this composition asserted-but-undesigned
// when `merge_into`'s `parent` parameter was still non-const; re-typing it to `const&` -- all
// `Ledger::merge()` itself ever needed -- closed the gap for real rather than only disclosing it.)
//
// SECURITY SHAPE, stated up front (proven in probe_task_branch_tool.cpp, not just claimed): a
// `handle_id` names an entry in THIS OBJECT's own `active_` map, never a global/ambient registry.
// Even a guessable or sequential `handle_id` string is safe by construction -- a caller presenting
// it to a DIFFERENT session's `TaskBranchSandbox` instance simply gets "unknown handle," because
// that instance's own map never had the entry, exactly the same "possession of the right object,
// not knowledge of a name" discipline `BranchHandle` itself already establishes. This is deliberate:
// unguessability of the string was never the security boundary; per-session map scoping is. THIS
// PROPERTY ONLY HOLDS IF ONE `TaskBranchSandbox` INSTANCE IS CONSTRUCTED PER SESSION -- a pure host-
// discipline convention, not structurally enforced by this type, matching (not exceeding) the
// already-accepted precedent for `MandatorySandboxProvider::bind_sandbox()` (§37.1: "honestly
// weaker than compile-time enforcement, and named as such"). A host wiring bug that shared one
// instance across two sessions would silently let both act on each other's handles -- a real, named
// residual, not assumed away.
//
// REAL, ADVERSARIALLY-FOUND AND FIXED, 2026-08-27 (three independent parallel red-team passes --
// security/I2-I3, C++ correctness, architecture-fit -- run against this file's FIRST version):
//   1. [FATAL, all three passes independently found this] `active_` had NO synchronization of its
//      own, unlike every sibling mutable structure in this whole design (`Ledger::mutex_`,
//      `RealIoFileSystem::sync_mutex_`, `SandboxRuntime::exclusivity_` -- §36's own text records
//      the IDENTICAL bug class being found and fixed for `SandboxRuntime::run()` itself). Two
//      concurrent calls into ONE `TaskBranchSandbox` instance on the SAME handle_id (a real
//      possibility: modern tool-calling APIs dispatch multiple tool calls from one model turn
//      concurrently, and this substrate's own `AsyncMutex` "may resume on a different OS thread
//      than it suspended on") could race a lookup-then-use against a concurrent erase-then-consume,
//      or double-move the SAME map entry -- real memory corruption, not a logic bug. FIXED: an
//      `exclusivity_` guard (identical in kind to `SandboxRuntime`'s own) now wraps the FULL body
//      of every method that touches `active_`.
//   2. [MUST-FIX] `discard_task_branch()` never refunded the `BranchCost` unit `start_task_branch()`
//      spent -- an agent trying and discarding N approaches paid for N branches with nothing kept,
//      a self-inflicted quota exhaustion contradicting the design's own established refund-on-
//      "nothing kept" discipline (`RunCost`'s own precedent, `sandbox_runtime.hpp`). FIXED: a
//      successful discard now refunds 1 unit.
//   3. [FIXED, 2026-08-27, second pass] The classic "best-of-N" pattern (spawn N children from the
//      SAME still-unmoved base, evaluate, commit exactly ONE, discard the rest) commits with ZERO
//      conflict risk through this surface, proven in probe_task_branch_tool.cpp's own dedicated
//      check -- `merge_trees(base, ours=base, theirs=child)` is a pure fast-forward when `main` has
//      not moved since the children were spawned. A DIFFERENT, also-real pattern -- sequential/
//      interleaved commits of two branches that both modify the same path, where the second's
//      `base` goes stale the moment the first commits -- correctly REJECTS rather than corrupting
//      anything (proven in probe_task_branch_tool.cpp's conflict check), but the ORIGINAL version of
//      this file left the rejected branch's real work addressable ONLY through the lower-level A7
//      orphan-reclaim API, not through this tool surface, since `commit_task_branch()` erased the
//      handle before the merge ran and never re-surfaced anything on rejection. FIXED:
//      `commit_task_branch()` now calls the new `SandboxRuntime::reclaim_orphaned_child()` (thin
//      wrapper around the already-proven A7 `Ledger::reclaim_orphaned_branch()`) on any rejection
//      and re-inserts the reclaimed branch into `active_` under the SAME `handle_id` -- the caller
//      still sees the original rejection error, but the handle keeps working afterward (retry, run
//      more work, or discard) instead of becoming "unknown handle" on the very next call. Proven in
//      probe_task_branch_tool.cpp's own dedicated check, following directly from the existing
//      conflict check.
//   4. [FIXED, 2026-08-27, at the design level -- real Tool<> wiring still deferred] No
//      capability-declaration design existed for who may call `start_task_branch`/
//      `run_in_task_branch`/`commit_task_branch`/`discard_task_branch` at all, unlike `RunShellTool`'s
//      real `Capabilities<cap::decl::FsRead<"work">, ...>` precedent. Closed by
//      `task_branch_capability.hpp`: TWO tags, not one -- `cap::decl::TaskBranch` (required by
//      start/run/discard, which stay isolated on a child branch and never touch main) and
//      `cap::decl::TaskBranchCommit` (required ADDITIONALLY by commit, which merges real work INTO
//      main -- a meaningfully more consequential authority, confirmed as the right split with the
//      project owner before building it, mirroring `cap::FsRead`/`cap::FsWrite`'s own read-vs-write
//      distinction on one mount). Proven real, both what it establishes and what it does not:
//      `probe_task_branch_capability.cpp` confirms both tags compile as ordinary type arguments to
//      the REAL production `agentengine::Capabilities<...>`, AND confirms (by an actual isolated
//      compile attempt, not assumption) that driving them through a real `Tool<>`'s
//      `declared_capabilities()` does NOT compile today -- for two precise, documented reasons
//      (an ADL/namespace-nesting gap, and the real `Capability` variant being closed with no
//      `TaskBranch` alternative) that promotion into `agentengine::cap::decl` would close as a normal
//      consequence, not a surprise. The constructor-injected `AsyncQuota<BranchCost>&`/etc.
//      references remain the real, already-proven RATE gate (§39/§40); these two tags are the
//      separate, host-auditable MEMBERSHIP gate ("may this agent have task-branch tooling, and commit
//      with it, at all") that was actually missing.
//   5. [Disclosed, not fixed -- likely a non-issue] Whether `commit_task_branch`/`discard_task_branch`
//      should touch an `AgentSession`'s own conversation/turn history at all is unaddressed.
//      Claude Code's own `/rewind` (per the 2026-08-27 research) treats file-restore and
//      conversation-restore as independently selectable, not coupled by default -- the strongest
//      real precedent found suggests leaving these decoupled is the industry-accepted choice, not
//      an oversight, but this design says so explicitly now rather than by silence.
//   6. [Disclosed, not fixed] `active_`'s own table has no durability of its own -- a process crash
//      mid-task-branch strands it from THIS tool's own verbs even in a durable-`Store` Ledger
//      configuration, though the underlying branch itself survives and remains reclaimable via A7
//      at the lower level.

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "agentengine/rt/async_mutex.hpp"

#include "../common/result.hpp"
#include "../execution_surface/execution_surface.hpp"
#include "../execution_surface/sandbox_runtime.hpp"
#include "../identity_authority/identity_authority.hpp"

namespace probe {

struct TaskBranchStartArgs {
    std::string label;  // agent-supplied, free text, for logging/description ONLY -- I3: never
                          // used to select which branch/authority is operated on. The tool's own
                          // Args schema (in a real Tool<>) would carry nothing else, deliberately:
                          // there is no caller-suppliable handle_id/path/principal field anywhere
                          // in this surface -- every operation's authority comes from the calling
                          // session's own already-bound TaskBranchSandbox and already-held quota,
                          // never from anything the model's own tool-call arguments could name.
};
struct TaskBranchStartReply {
    std::string handle_id;
};

struct TaskBranchRunArgs {
    std::string handle_id;
    std::string command;
};
struct TaskBranchRunReply {
    int exit_code;
    std::string stdout_text;
};

struct TaskBranchCommitArgs {
    std::string handle_id;
};
struct TaskBranchCommitReply {
    std::uint64_t turn_index;
};

struct TaskBranchDiscardArgs {
    std::string handle_id;
};
struct TaskBranchDiscardReply {};

// Session-scoped, constructed once per session -- matching `SessionShellSandbox`'s own real, shipped
// role (`src/backends/native_jail/session_shell_wiring.hpp`). Owns the session's own root
// `SandboxRuntime` (the "main" line an already-existing, already-proven binding mechanism -- e.g.
// A9's `MandatorySandboxProvider`, or whatever a real implementation wires -- would have already
// established) BY CONST REFERENCE, never constructing or owning root authority itself (I2: this
// object mints no capability, it only narrows/spends what the session already holds) and never
// needing write access to it (every mutation this class performs lands on a CHILD branch; the one
// place `main` participates in a mutation, `merge_into()`, is itself `Ledger::merge()`-shaped and
// only ever needs `main`'s branch NAME, read-only -- see `sandbox_runtime.hpp`'s own comment on
// `merge_into`'s signature).
template <ExecutionSurface Surface>
class TaskBranchSandbox {
public:
    TaskBranchSandbox(SandboxRuntime const& main, Surface& surface, Principal owner,
                        AsyncQuota<BranchCost>& branch_quota, AsyncQuota<RunCost>& run_quota,
                        AsyncQuota<StorageBytes>& storage_quota, std::filesystem::path staging_parent_dir)
        : main_(&main), surface_(&surface), owner_(owner), branch_quota_(&branch_quota),
          run_quota_(&run_quota), storage_quota_(&storage_quota),
          staging_parent_dir_(std::move(staging_parent_dir)),
          exclusivity_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    // start_task_branch: forks a real, isolated child branch from the session's CURRENT main head
    // (via the already-proven `SandboxRuntime::spawn_child_branch()`, §37.2's own real
    // uniqueness-by-construction fix reused verbatim) and returns an opaque handle naming it in
    // THIS object's own table. Never touches `main_`'s own branch.
    [[nodiscard]] agentengine::rt::task<result<TaskBranchStartReply>> start_task_branch(
        TaskBranchStartArgs /*args*/) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto child = co_await main_->spawn_child_branch(owner_, *branch_quota_, staging_parent_dir_);
        if (!child.has_value()) co_return std::unexpected(child.error());
        std::string handle_id = child->branch_name();  // unique by construction (branch_seq_) --
                                                          // see this file's own header comment for
                                                          // why unguessability is not load-bearing.
        active_.insert_or_assign(handle_id, std::move(*child));
        co_return TaskBranchStartReply{std::move(handle_id)};
    }

    // run_in_task_branch: the SAME `SandboxRuntime::run()` every other execution path already uses
    // (§36) -- no separate execution logic, no separate quota-gating logic, just addressed at the
    // child instead of main. Fails closed with "unknown handle" for anything not in THIS session's
    // own table -- the real security check this whole surface rests on. Held under `exclusivity_`
    // for its FULL body (not just the lookup) -- finding 1's own fix -- so a concurrent commit/
    // discard on the same handle cannot erase the map entry this call is still using mid-`co_await`.
    [[nodiscard]] agentengine::rt::task<result<TaskBranchRunReply>> run_in_task_branch(
        TaskBranchRunArgs args) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto it = active_.find(args.handle_id);
        if (it == active_.end()) {
            co_return std::unexpected(
                error{"unknown task-branch handle: " + args.handle_id, "task_branch.unknown_handle"});
        }
        auto outcome =
            co_await it->second.run(*surface_, std::move(args.command), owner_, *run_quota_, *storage_quota_);
        if (!outcome.has_value()) co_return std::unexpected(outcome.error());
        co_return TaskBranchRunReply{outcome->exec.exit_code, outcome->exec.stdout_text};
    }

    // commit_task_branch: one-shot -- the handle is ERASED from the table before the merge even
    // runs, so a repeated call with the same handle_id always sees "unknown handle." Held under
    // `exclusivity_` for its FULL body -- a concurrent call (same or different handle) cannot
    // observe or race the erase-then-move sequence. See this file's header comment (finding 3) for
    // the precise, now-separated best-of-N (always clean) vs. sequential-conflict (rejected, real
    // work reclaimable only at the lower A7 layer, not through this handle) distinction.
    [[nodiscard]] agentengine::rt::task<result<TaskBranchCommitReply>> commit_task_branch(
        TaskBranchCommitArgs args) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto it = active_.find(args.handle_id);
        if (it == active_.end()) {
            co_return std::unexpected(
                error{"unknown task-branch handle: " + args.handle_id, "task_branch.unknown_handle"});
        }
        SandboxRuntime child = std::move(it->second);
        active_.erase(it);
        std::string const branch_name = child.branch_name();  // captured BEFORE merge_into() moves child
        auto cp = co_await std::move(child).merge_into(*main_, owner_);
        if (cp.has_value()) co_return TaskBranchCommitReply{cp->turn_index};

        // A10 fix (2026-08-27, closing header-comment finding 3 for real): `Ledger::merge()` already
        // registers `branch_name` into its own `orphaned_from_restart_` set on EVERY rejection path
        // (§32.4's fix, worktree_ledger.hpp) -- reclaim it immediately via the already-proven A7 API
        // and re-surface it under the SAME `handle_id`, so a rejected commit (a real merge conflict,
        // most commonly -- see this file's own header comment on the best-of-N vs. sequential-
        // conflict distinction) no longer strands the caller at the lower-level Ledger API. The
        // caller sees the ORIGINAL rejection error either way; what changes is that `handle_id`
        // keeps working afterward (retry, run more work, or discard through THIS surface) instead of
        // becoming "unknown handle" on the very next call.
        // Correctness note (correctness/concurrency red-team, 2026-08-27): between this call and
        // the already-moved-from `child` finally going out of scope at this coroutine frame's own
        // destruction, both objects briefly hold a `RealIoFileSystem` addressing the IDENTICAL
        // staging directory (same `compute_digest(branch_name)`-derived path, since `branch_name` is
        // unchanged by a reclaim). Confirmed benign: `RealIoFileSystem` has no user-declared
        // destructor and performs no filesystem cleanup or cross-instance mutex aliasing on
        // destruction, so the moved-from `child`'s later destruction is inert -- but this is
        // non-obvious on a first read, hence this note.
        auto reclaimed = co_await main_->reclaim_orphaned_child(branch_name, owner_, staging_parent_dir_);
        if (!reclaimed.has_value()) {
            // A genuine internal inconsistency, not the ordinary conflict case -- merge() registers
            // every rejection as an orphan unconditionally, so a reclaim failure here means something
            // else is wrong (e.g. an authorization mismatch this class's own invariants should have
            // prevented). Surfaced distinctly so it is never mistaken for an ordinary conflict.
            co_return std::unexpected(error{
                "commit was rejected (" + cp.error().message +
                    ") and the branch could not be reclaimed for retry (" + reclaimed.error().message +
                    ") -- the work may be reachable only via the lower-level Ledger orphan-reclaim API",
                "task_branch.commit_rejected_and_reclaim_failed"});
        }
        active_.insert_or_assign(args.handle_id, std::move(*reclaimed));
        co_return std::unexpected(cp.error());
    }

    // discard_task_branch: one-shot, same erase-before-act discipline as commit, same `exclusivity_`
    // coverage (finding 1). `Ledger::abandon()` has no failure mode (see this file's header comment
    // on why no authorization check is needed there) -- the only way this returns an error is
    // "unknown handle." Refunds the `BranchCost` unit `start_task_branch` spent (finding 2's fix):
    // discarded work costs the session nothing lasting, matching `RunCost`'s own established
    // refund-on-"nothing kept" precedent (`sandbox_runtime.hpp`).
    [[nodiscard]] agentengine::rt::task<result<TaskBranchDiscardReply>> discard_task_branch(
        TaskBranchDiscardArgs args) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        auto it = active_.find(args.handle_id);
        if (it == active_.end()) {
            co_return std::unexpected(
                error{"unknown task-branch handle: " + args.handle_id, "task_branch.unknown_handle"});
        }
        SandboxRuntime child = std::move(it->second);
        active_.erase(it);
        auto discarded = co_await std::move(child).discard();
        if (!discarded.has_value()) co_return std::unexpected(discarded.error());
        (void)co_await branch_quota_->refund(1);
        co_return TaskBranchDiscardReply{};
    }

    // Test/introspection only -- lets a probe confirm a handle is (or isn't) live without a real
    // tool call; a real Tool<> surface would not expose this.
    [[nodiscard]] bool has_active_handle(std::string const& handle_id) const {
        return active_.contains(handle_id);
    }
    [[nodiscard]] std::size_t active_count() const { return active_.size(); }

private:
    SandboxRuntime const* main_;
    Surface* surface_;
    Principal owner_;
    AsyncQuota<BranchCost>* branch_quota_;
    AsyncQuota<RunCost>* run_quota_;
    AsyncQuota<StorageBytes>* storage_quota_;
    std::filesystem::path staging_parent_dir_;
    std::map<std::string, SandboxRuntime> active_;
    std::unique_ptr<agentengine::rt::AsyncMutex> exclusivity_;
};

}  // namespace probe
