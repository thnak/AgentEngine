#pragma once
// Implements 030-Project-Workspace-and-Lifecycle.md §4 (directed lifecycle: pause is not idle
// eviction) and §8 Q4 (pause extends to workflow-supervising actors too). Milestone 6 Phase I
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// THE TYPE-ERASURE PROBLEM §4's OWN PROSE DOESN'T GRAPPLE WITH. §4 says "call `.passivate()` on
// every member session's `ActorRef<AgentSession>`" as if that were one type. It is not:
// `AgentSession<ChatClientT, StateT, HistoryProviderT>` (agent_session.hpp) is a THREE-parameter
// class template, so `ActorRef<AgentSession<C1,S1,H1>>` and `ActorRef<AgentSession<C2,S2,H2>>` are
// distinct C++ types the instant two member sessions use different backends -- which 030 §2's own
// model allows (a Project's members are just session ids; nothing constrains them to one
// instantiation). `quark::ActorRef<A>::passivate()` requires `A` to be concretely known at the
// call site (`static_assert(max_concurrency_of<A>() == 1, ...)`, actor_ref.hpp) -- there is no
// existing type-erased or polymorphic way to call it (verified: no `PassivatableRef`, no
// `std::variant<ActorRef<...>>`, no `std::any` wrapper anywhere in Quark or this repo). A
// `ProjectSupervisor` that could only hold ONE concrete `AgentSession<...>` type would contradict
// 030 §2's own model the moment two members differ.
//
// `PassivatableHandle` (below) closes this WITHOUT bypassing the safety the `static_assert`
// exists for. It is NOT built on `LocalRouter::request_passivate(ActorId)` (the untyped primitive
// underneath `ActorRef<A>::passivate()`) -- that would silently reintroduce the exact hazard the
// static_assert prevents (passivating a Reentrant/`MaxConcurrency<N>` actor, whose close-out is,
// per that assert's own wording, "out of scope" / unproven). Instead, `PassivatableHandle`'s
// constructor is a template that takes a concretely-typed `ActorRef<A>` and closes over a call to
// its OWN `.passivate()` in a `std::function<bool()>` -- the static_assert still fires, at
// construction, on the concrete `A` the caller already has in hand. Type erasure happens to the
// CLOSURE, never to the safety check.

#include <functional>
#include <string>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

#include "agentengine/project/project.hpp"

namespace agentengine {

// A `.passivate()` call, closed over a concretely-typed `ActorRef<A>` -- see this file's own
// header comment for why this is safe where a naive `ActorId`-only wrapper would not be.
class PassivatableHandle {  // ae-naming-lint: allow PassivatableHandle — this file's own new vocabulary; 027 has not been updated to list it (030 §4 names the VERB, not this handle type)
public:
    template <class A>
    explicit PassivatableHandle(quark::ActorRef<A> ref)
        : passivate_fn_([ref]() { return ref.passivate(); }) {}

    // Fire-and-forget, exactly like the `ActorRef<A>::passivate()` this closes over -- `false` iff
    // the underlying id never resolved to a live activation, `true` for "accepted," never "has
    // retired yet" (actor_ref.hpp's own contract, unchanged by this wrapper).
    bool passivate() const { return passivate_fn_(); }

private:
    std::function<bool()> passivate_fn_;
};

// 030 §6: "the Project's own supervising actor," addressed directly by `pause_project`/
// `restore_project` rather than through the registry. Its ENTIRE job (030 §1: "owns no turn loop,
// no history, no model calls") is being the live index of THIS Project's currently-registered
// member sessions and workflow-supervising actors, so `pause_project` (below) has something to
// iterate without external bookkeeping -- and being passivatable itself, last, per §4's ordering.
// Non-templated, matching `WorkflowSupervisor`'s own shape (supervisor.hpp) -- the heterogeneity
// problem is absorbed entirely by `PassivatableHandle`, not by this actor becoming a template.
class ProjectSupervisor : public quark::Actor<ProjectSupervisor, quark::Sequential> {
public:
    // No asks of its own yet -- `register_member`/`register_workflow` are plain synchronous
    // methods a caller on the SAME actor (or a test) invokes directly, mirroring
    // `WorkflowSupervisor::initialize()`'s own shape (a setup call, not a protocol message). A
    // real host wiring these across actor boundaries would route them through an ask; nothing here
    // forecloses that, it just isn't needed to prove §4's own claims this phase.
    using protocol = quark::Protocol<>;

    template <class A>
    void register_member(quark::ActorRef<A> ref) {
        member_sessions_.emplace_back(ref);
    }
    template <class A>
    void register_workflow(quark::ActorRef<A> ref) {
        workflow_supervisors_.emplace_back(ref);
    }

    [[nodiscard]] std::size_t member_count() const noexcept { return member_sessions_.size(); }
    [[nodiscard]] std::size_t workflow_count() const noexcept { return workflow_supervisors_.size(); }

    // 030 §4 / §8 Q4's own ordering (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md
    // Phase I: "member sessions, then workflow supervisors... then the Project's own actor, last").
    // This actor's OWN passivation is deliberately NOT done here -- an actor cannot meaningfully
    // passivate itself mid-handler (passivation drains the queue AFTER the current handler
    // returns), so `pause_project` (below) calls it externally, after this returns, exactly the
    // shape `WorkflowSupervisor`'s own E4 test already established (`sup.passivate()` called by
    // the harness, never by the actor on itself).
    void passivate_members_and_workflows() {
        for (auto const& h : member_sessions_) h.passivate();
        for (auto const& h : workflow_supervisors_) h.passivate();
    }

private:
    std::vector<PassivatableHandle> member_sessions_;
    std::vector<PassivatableHandle> workflow_supervisors_;
};

// A stable ActorId tag for a Project's supervising actor -- distinct from `kProjectManifestTypeKey`/
// `kProjectArchiveTypeKey` (project.hpp) and `kProjectRegistryTypeKey` (registry.hpp), packing
// "AEPROJS1" (PROJect Supervisor) as 8 ASCII bytes.
inline constexpr quark::TypeKey kProjectSupervisorTypeKey{0x4145'5052'4F4A'5331ULL};  // "AEPROJS1"
[[nodiscard]] inline quark::ActorId project_supervisor_actor_id(std::string_view project_id) noexcept {
    return quark::ActorId{kProjectSupervisorTypeKey, std::hash<std::string_view>{}(project_id)};
}

// 030 §4's Pause: "call .passivate() on every member session's ActorRef<AgentSession> (and on the
// Project's own supervising actor, last), then mark the manifest Paused." `sup` is this Project's
// OWN `ActorRef<ProjectSupervisor>` -- the caller's, since only the caller can pass it across the
// actor boundary correctly (an actor does not hold a ref to itself in this codebase's existing
// shape, matching `WorkflowSupervisor`/`AgentSession` precedent).
//
// Does NOT itself persist the manifest -- matching this codebase's consistent I2 discipline
// (project.hpp/registry.hpp's own header notes): the caller separately calls
// `save_project_snapshot` with the returned, status-updated record.
[[nodiscard]] inline ProjectRecord pause_project(quark::ActorRef<ProjectSupervisor> sup,
                                                 ProjectSupervisor& live, ProjectRecord rec) {
    live.passivate_members_and_workflows();
    sup.passivate();
    rec.status = project_status::paused;
    return rec;
}

// 030 §4's Restore: "read the manifest back by project_id. This does NOT need to eagerly
// reactivate every member session." Deliberately just that -- a caller who already has the loaded
// `ProjectRecord` (project.hpp's `load_project_snapshot`) flips its status; no session, no
// workflow supervisor, no `ProjectSupervisor` itself is touched. §4's own words: "the moment a
// host issues a Run against any member, Quark's lazy activation... brings that one session back."
[[nodiscard]] inline ProjectRecord restore_project(ProjectRecord rec) {
    rec.status = project_status::active;
    return rec;
}

// 030 §4's Archive: "pause, then apply retention policy." No retention/GC mechanism exists in this
// codebase yet (025 §6/019 §5's own policy machinery is out of THIS phase's scope, and §8 Q2
// already resolved that archiving must NOT eagerly trigger reclaim -- "archived means hidden, not
// shrunk"), so this phase's own honest scope is exactly the status flip, after an ordinary pause.
[[nodiscard]] inline ProjectRecord archive_project(quark::ActorRef<ProjectSupervisor> sup,
                                                   ProjectSupervisor& live, ProjectRecord rec) {
    ProjectRecord paused = pause_project(sup, live, std::move(rec));
    paused.status         = project_status::archived;
    return paused;
}

}  // namespace agentengine
