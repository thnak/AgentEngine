#pragma once
// ADR-037: agentengine::rt::ProjectSupervisor, the Quark-actor-free replacement for
// agentengine::ProjectSupervisor (project/lifecycle.hpp). Implements 030-Project-Workspace-and-
// Lifecycle.md §4's "Pause" verb for a Project's member sessions and workflow-supervising actors.
//
// SCOPE OF THIS SLICE (matching the discipline rt::SpawnCostBudget's/rt::ProjectRegistry's own
// passes established): the genuinely hard, novel part only -- type-erased checkpoint orchestration
// across a Project's heterogeneously-instantiated member sessions, WITHOUT Quark's actor system to
// lean on. Does NOT yet migrate project/project.hpp's own ProjectRecord manifest snapshot or the
// status-flip free functions (pause_project/restore_project/archive_project) -- those are real,
// tractable follow-up (project_registry.hpp's own banner already names the manifest snapshot as
// single-slot-tractable), just not built in this pass. A future pass that ports project.hpp's
// manifest can layer pause_project/restore_project/archive_project's status-flip logic on top of
// THIS type's checkpoint_members_and_workflows() unchanged.
//
// WHY "CHECKPOINT", NOT "PASSIVATE" -- THE REAL SEMANTIC GAP ADR-037 OPENS. The Quark original
// exists to solve ONE problem: `quark::ActorRef<A>::passivate()` requires `A` concretely known at
// the call site, so a Project whose members are heterogeneously-instantiated
// `AgentSession<ChatClientT,StateT,HistoryProviderT>` types (030 §2's own model allows this, and
// this project's own `rt::AgentSession` inherits the same three-parameter shape) needs
// `PassivatableHandle` to type-erase the CLOSURE over `.passivate()`, not the safety check itself.
// Removing Quark removes `.passivate()` -- there is no actor engine here to evict an idle object
// from memory and lazily reactivate it later on the next ask; an `rt::AgentSession` is a plain,
// host-held C++ object with no runtime managing its residency. What SURVIVES the removal, and is
// still genuinely useful for 030 §4's "Pause" verb, is the OTHER half of what passivation
// accomplished: flushing durable state before giving up temporal ownership.
// `ProjectSupervisor::checkpoint_members_and_workflows()` below does exactly that -- calls
// `save_agent_session_snapshot`/`save_workflow_checkpoint` (both already built,
// rt/agent_session.hpp's and rt/workflow_supervisor.hpp's own Slice 2s) on every registered
// member/workflow -- and STOPS THERE. Whether the host then actually drops the in-memory object
// (the closest rt:: analogue of "evict") is the HOST's own decision, made outside this type -- the
// same "framework proposes, host disposes" split `rt::SessionStore`'s own banner already establishes
// for storage generally. This is a real, named narrowing, not an oversight: the original file's own
// "pause is not idle eviction" wording becomes, in rt:: land, MORE literally true than the Quark
// version ever fully controlled -- pause here is unambiguously "checkpoint," full stop; eviction was
// always a Quark runtime behavior this codebase never drove directly even in the original.
//
// A REAL, NAMED HAZARD `PassivatableHandle` DID NOT HAVE: `PassivatableHandle` held a
// `quark::ActorRef<A>` BY VALUE -- a cheap, location-independent handle, safe to hold even if the
// concrete actor is later passivated/reactivated elsewhere, because Quark's own activation table
// is the thing actually tracking the object's lifetime. `CheckpointHook` below instead captures its
// member's live `AgentSession&`/`StoreT&` BY REFERENCE, because rt:: land has no such indirection --
// a plain C++ reference IS the only handle available. This means a `ProjectSupervisor` must never
// outlive the sessions/stores it holds hooks to (the caller's own responsibility, mirroring the
// ordinary "don't outlive what you reference" C++ rule any raw-reference-capturing closure carries,
// not a new invariant this file invents) -- named explicitly here since the Quark original's own
// design made this hazard structurally impossible, so a reader who assumes rt:: is a drop-in
// replacement would miss that the safety net changed shape.

#include <cstddef>
#include <functional>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

// A checkpoint closure, type-erased over whichever concrete AgentSession<ChatClientT,StateT,
// HistoryProviderT>/StoreT pairing (or WorkflowSupervisor/StoreT pairing) the caller has in hand --
// see file banner for why this replaces PassivatableHandle's ActorRef-closure trick rather than
// porting it literally. co_await'ed serially by checkpoint_members_and_workflows() below, one hook
// at a time, matching the Quark original's own unhurried "iterate the vector" shape (030 §4 does
// not ask for concurrent passivation, and concurrent checkpoint I/O against possibly-shared stores
// is exactly the kind of thing this project's own I1 discipline avoids by default).
using CheckpointHook = std::function<task<result<void>>()>;

// One checkpoint attempt's outcome, when it failed. checkpoint_members_and_workflows() collects
// ALL failures rather than stopping at the first one (deliberately more informative than the Quark
// original's own fire-and-forget `bool passivate()`, which had no error-reporting shape at all) --
// an operator pausing a Project with many members should learn about every member whose checkpoint
// failed, not just the first, since skipping the REST of the checkpoints just because member #2
// failed transiently would leave members #3..N's durable state stale for no good reason.
struct CheckpointFailure {
    std::size_t index = 0;      // position within its own kind's registration order
    bool        is_workflow = false;  // false: a member session's hook; true: a workflow's hook
    error       err;
};

struct CheckpointReport {
    std::vector<CheckpointFailure> failures;
    [[nodiscard]] bool all_ok() const noexcept { return failures.empty(); }
};

// 030 §6: "the Project's own supervising actor," addressed directly by `pause_project` rather than
// through the registry. Its job (030 §1: "owns no turn loop, no history, no model calls") is being
// the live index of a Project's currently-registered member sessions and workflow-supervising
// actors, so a pause operation has something to iterate without external bookkeeping. Non-actor,
// non-templated -- the heterogeneity problem is absorbed entirely by CheckpointHook's type erasure,
// not by this type becoming a template (matching the Quark original's own "non-templated, mirrors
// WorkflowSupervisor's own shape" precedent).
class ProjectSupervisor {
public:
    // `session`/`store` must outlive both this call and every subsequent
    // checkpoint_members_and_workflows() call that reaches this hook -- see file banner's own
    // "real, named hazard" paragraph.
    template <class ChatClientT, class StateT, class HistoryProviderT, SessionStore StoreT>
    void register_member(AgentSession<ChatClientT, StateT, HistoryProviderT>& session, StoreT& store) {
        member_hooks_.push_back([&session, &store]() -> task<result<void>> {
            co_return co_await save_agent_session_snapshot(session, store);
        });
    }

    // Same outliving requirement as register_member(), for `supervisor`/`store`.
    template <SessionStore StoreT>
    void register_workflow(WorkflowSupervisor& supervisor, StoreT& store) {
        workflow_hooks_.push_back([&supervisor, &store]() -> task<result<void>> {
            co_return co_await save_workflow_checkpoint(supervisor, store);
        });
    }

    [[nodiscard]] std::size_t member_count() const noexcept { return member_hooks_.size(); }
    [[nodiscard]] std::size_t workflow_count() const noexcept { return workflow_hooks_.size(); }

    // 030 §4 / §8 Q4's own ordering (member sessions, then workflow supervisors). Runs every
    // registered hook to completion regardless of earlier failures -- see CheckpointReport's own
    // comment for why "collect all failures" beats "stop at the first one" here.
    [[nodiscard]] task<CheckpointReport> checkpoint_members_and_workflows() {
        CheckpointReport report;
        for (std::size_t i = 0; i < member_hooks_.size(); ++i) {
            result<void> r = co_await member_hooks_[i]();
            if (!r) report.failures.push_back(CheckpointFailure{i, false, r.error()});
        }
        for (std::size_t i = 0; i < workflow_hooks_.size(); ++i) {
            result<void> r = co_await workflow_hooks_[i]();
            if (!r) report.failures.push_back(CheckpointFailure{i, true, r.error()});
        }
        co_return report;
    }

private:
    std::vector<CheckpointHook> member_hooks_;
    std::vector<CheckpointHook> workflow_hooks_;
};

}  // namespace agentengine::rt
