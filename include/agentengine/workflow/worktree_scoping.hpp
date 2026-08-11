#pragma once
// Implements 014-Workflow-and-Orchestration.md §1's named gap ("nothing states whether a workflow
// executor or a handoff target gets its own sub-worktree, inherits the caller's, or shares one, and
// no code wires an executor to `create_sub_worktree` today") using 025-Worktree-and-Virtual-
// Filesystem.md §3's already-real, already-tested `SubWorktree`/`sharing_mode` primitive.
// ADR-032 (decisions/ADR-032-workflow-executor-worktree-scoping.md).
//
// SCOPE OF THIS FILE, stated up front because it is narrower than "wire workflows to worktrees"
// sounds: this is the POLICY + MINTING layer only -- deciding each executor's `sharing_mode`
// (`Executor::worktree_mode`, graph.hpp) and turning it into a real `SubWorktree` plus a
// guest-facing `Mount`/capability pair. It does NOT wire the result into `FunctionExecutor`'s
// `EffectContext`/`ExecutorBody` inside `WorkflowSupervisor`'s own construction path -- no
// production host builds a `FunctionExecutor` fleet today (confirmed by grep over src/, tools/), so
// that wiring has no real caller yet to design against, the same "prove the mechanism, wire it for
// real later" split ADR-028 already used for session-scoped stateful tools. It does NOT touch
// `agent`- or `sub_workflow`-kind executors (`check_workflow_executable`, graph.hpp, still rejects
// both -- they are not built). It does NOT implement merge-on-join (025 §4) for a `branch`
// executor's worktree -- WHEN a branch folds back into its parent is a separate question this file
// does not answer.

#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::workflow {

// One executor's worktree grant. `mount`/`read`/`write` are all `nullopt` together, exactly when
// `sub.mode == sharing_mode::readonly` -- see the FATAL red-team finding this design incorporates:
// `Mount`/`mount_read` (core/worktree.hpp) have NO concept of a pinned-digest `SubWorktree` at all --
// they unconditionally `read_ref(ref_store, mount.ref_name)`, and a `readonly` `SubWorktree` has no
// backing ref (`backing_ref_name` is empty by construction, `create_sub_worktree`). Building a
// `Mount` for one anyway would either 404 every read (`worktree.mount_ref_missing`) or, worse,
// silently alias every OTHER readonly grant in the system onto the same empty-string ref name.
// Fixing this for real needs a pinned-digest read path added to `Mount`/`mount_read` itself -- a
// core/worktree.hpp change, out of scope for this file (see ADR-032 §5). A `readonly` executor
// still gets a real, correct `SubWorktree` (`sub.pinned_digest` is valid and independently readable
// via `read_sub_worktree`) -- it just has no capability-gated guest-facing view yet.
struct ExecutorWorktreeGrant {
    SubWorktree               sub;
    std::optional<Mount>      mount;
    std::optional<cap::FsRead>  read;
    std::optional<cap::FsWrite> write;  // also nullopt for readonly even once mount exists in a
                                        // future revision -- `write_sub_worktree` already fails
                                        // closed on `readonly`, and a capability that could never
                                        // succeed is a hole, not a convenience (007 §3 property 5)
};

namespace detail {

[[nodiscard]] inline result<void> check_executor_id(std::string const& id) {
    // Redundant with `validate_workflow`'s own check (graph.hpp) BY DESIGN, not an oversight: this
    // function must not assume its caller already validated the graph -- the failure mode of
    // skipping this check is a security-relevant string splice into a worktree ref name / guest-
    // facing mount id, not merely a malformed graph, so it gets its own defense here too (defense in
    // depth for the one check that matters most, matching 025 §5's "path escape is a security bug,
    // not a bug").
    if (id.find('/') != std::string::npos) {
        return std::unexpected(error{failure_class::contract,
                                      "executor id '" + id + "' must not contain '/'",
                                      "worktree_scoping.executor_id_contains_slash"});
    }
    return {};
}

[[nodiscard]] inline ExecutorWorktreeGrant grant_for(SubWorktree sub, std::string const& executor_id) {
    ExecutorWorktreeGrant grant;
    grant.sub = sub;
    if (sub.mode != sharing_mode::readonly) {
        std::string const mount_id = "/agents/" + executor_id;
        grant.mount = Mount{mount_id, sub.backing_ref_name, ""};
        grant.read  = cap::FsRead{mount_id, "", std::nullopt};
        grant.write = cap::FsWrite{mount_id, "", std::nullopt, std::nullopt};
    }
    return grant;
}

}  // namespace detail

// Mint a fresh worktree for every executor in `wf`, branching/sharing/scratching off
// `run_parent_ref` per each executor's declared `worktree_mode`.
//
// **Call this AT MOST ONCE per run**, before any `FunctionExecutor` that will use the resulting
// grants is spawned. Two preconditions on the CALLER, both enforced here rather than left as
// documentation-only (the must-fix red-team findings this design incorporates):
//
//   1. `run_parent_ref.name` must already be unique PER RUN, not merely per workflow graph
//      definition -- e.g. a host-minted run identifier, never `WorkflowSupervisor::run_id()`
//      (`run_id_` is assigned only inside `handle(Ask<RunWorkflow,...>)`, strictly AFTER
//      `initialize()` -- i.e. after any pre-spawn wiring would need it; a real caller needs its own
//      independent pre-run identity source, which this file does not invent).
//   2. This function must not be called twice for the same `run_parent_ref` (whether by an
//      accidental double-mint or by two DIFFERENT runs colliding on the same parent-ref name by
//      caller error).
//
// Precondition 2 is enforced structurally: before minting executor `ex`'s worktree, this function
// reads whether a worktree already exists for it under `run_parent_ref` and FAILS CLOSED
// (`worktree_scoping.already_minted`) rather than silently re-branching over -- and discarding --
// whatever that existing branch had already accumulated. `create_sub_worktree`'s `branch`/`scratch`
// cases call `commit_ref` unconditionally ("mint or move", no existence check of its own, by
// design -- core/worktree.hpp's own comment), so this existence check is this file's job, not
// something the primitive underneath does for it.
template <quark::Store RS>
[[nodiscard]] result<std::vector<ExecutorWorktreeGrant>> mint_executor_worktrees(
    RS& ref_store, Ref const& run_parent_ref, Workflow const& wf) {
    std::vector<ExecutorWorktreeGrant> out;
    out.reserve(wf.executors.size());

    for (auto const& ex : wf.executors) {
        auto id_ok = detail::check_executor_id(ex.id);
        if (!id_ok) return std::unexpected(id_ok.error());

        std::string const child_name = run_parent_ref.name + "/agents/" + ex.id;

        auto existing = read_ref(ref_store, child_name);
        if (!existing) return std::unexpected(existing.error());
        if (existing->has_value()) {
            return std::unexpected(error{
                failure_class::contract,
                "a worktree already exists for executor '" + ex.id + "' under parent ref '" +
                    run_parent_ref.name +
                    "' -- mint_executor_worktrees must run at most once per run; a resumed run "
                    "should call resume_executor_worktrees instead",
                "worktree_scoping.already_minted"});
        }

        auto sub = create_sub_worktree(ref_store, run_parent_ref, child_name, ex.worktree_mode);
        if (!sub) return std::unexpected(sub.error());

        out.push_back(detail::grant_for(*sub, ex.id));
    }
    return out;
}

// Reconstruct a RESUMED run's worktree grants, for the `shared`/`branch`/`scratch` executors a
// prior `mint_executor_worktrees` call already minted under `run_parent_ref`. Read-only over
// `ref_store` -- it never mints, so calling it any number of times is safe.
//
// **Named residual, not a bug in this function**: a `readonly` executor's `SubWorktree.pinned_digest`
// is captured ONLY at mint time and lives nowhere durable (`readonly` never touches `ref_store` --
// `create_sub_worktree`'s `readonly` case has no `commit_ref` call at all). Reconstructing it here
// would need it persisted as part of 014 §5's own checkpoint record (`RunStateRecord`,
// workflow/checkpoint.hpp) -- a change to that schema this file does not make. A workflow with a
// `readonly` executor therefore fails closed on resume (`worktree_scoping.readonly_resume_unsupported`)
// rather than silently fabricating a stale or empty digest. See ADR-032 §5.
//
// The `branch` case also does not reconstruct `SubWorktree::base_digest` (the three-way-merge
// ancestor, 025 §4) -- also not durably stored anywhere today. A resumed `branch` grant can still
// be read from and written to correctly (`read_sub_worktree`/`write_sub_worktree` need only
// `backing_ref_name`), but `merge_branch_into_parent` on a resumed branch would need that ancestor
// re-supplied by the same future checkpoint-schema change. Named, not fixed, matching the
// `readonly` gap above.
template <quark::Store RS>
[[nodiscard]] result<std::vector<ExecutorWorktreeGrant>> resume_executor_worktrees(
    RS& ref_store, Ref const& run_parent_ref, Workflow const& wf) {
    std::vector<ExecutorWorktreeGrant> out;
    out.reserve(wf.executors.size());

    for (auto const& ex : wf.executors) {
        auto id_ok = detail::check_executor_id(ex.id);
        if (!id_ok) return std::unexpected(id_ok.error());

        if (ex.worktree_mode == sharing_mode::readonly) {
            return std::unexpected(error{
                failure_class::contract,
                "executor '" + ex.id +
                    "' is a readonly worktree, which cannot be resumed: its pinned digest is not "
                    "durably reconstructible from the ref store alone (see ADR-032 §5)",
                "worktree_scoping.readonly_resume_unsupported"});
        }

        std::string const child_name = run_parent_ref.name + "/agents/" + ex.id;

        SubWorktree sub;
        if (ex.worktree_mode == sharing_mode::shared) {
            // `create_sub_worktree`'s own `shared` case never commits a ref under `child_name` at
            // all -- it reuses the parent's ref directly -- so there is nothing to "find"; this is
            // reconstructed the identical way `create_sub_worktree` builds it the first time.
            sub = SubWorktree{child_name, run_parent_ref.name, sharing_mode::shared, {}, {}};
        } else {
            auto existing = read_ref(ref_store, child_name);
            if (!existing) return std::unexpected(existing.error());
            if (!existing->has_value()) {
                return std::unexpected(error{
                    failure_class::contract,
                    "no minted worktree found for executor '" + ex.id + "' under parent ref '" +
                        run_parent_ref.name +
                        "' -- resume_executor_worktrees requires a prior mint_executor_worktrees "
                        "call for this parent ref",
                    "worktree_scoping.resume_not_minted"});
            }
            sub = SubWorktree{child_name, child_name, ex.worktree_mode, {}, {}};
        }

        out.push_back(detail::grant_for(sub, ex.id));
    }
    return out;
}

}  // namespace agentengine::workflow
