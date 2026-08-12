#pragma once
// ADR-037: agentengine::rt::ProjectRegistry, the Quark-actor-free replacement for
// agentengine::ProjectRegistry (project/registry.hpp). Implements 030-Project-Workspace-and-
// Lifecycle.md Sec6's "create_project and list_projects are ordinary asks against a small
// Project-registry actor -- the shared index a new Project needs to register into." Read the
// original's own banner for the full "why a registry, not a Store scan" and "no self-persistence"
// reasoning -- both carry over unchanged, just re-expressed over rt::AsyncMutex instead of
// quark::Actor<Sequential>.
//
// SCOPE OF THIS SLICE (matching the same discipline rt::AgentSession's/rt::WorkflowSupervisor's own
// Slice 1 established): the in-memory index only -- create_project/list_projects, duplicate-id
// rejection, principal+tenant-scoped listing. Does NOT yet migrate:
//   - Durability (append_project_registered/read_registry_log, the original's own EventLog-backed
//     index recovery). The original's registry log is genuinely append-only in USAGE (one entry per
//     create_project, replayed in full on restart) but its CONTENT need is simple enough that a
//     single-slot rt::SessionStore snapshot of the whole current index (matching
//     save_agent_session_snapshot's own shape) would serve identically for recovery -- this is
//     real, tractable follow-up work, just not done in this pass.
//   - project/project.hpp's own ProjectRecord manifest snapshot AND archived-member tail are a
//     SEPARATE, larger gap: the manifest is single-slot (tractable the same way), but the archived
//     tail is genuinely, structurally append-only -- a real project's lifetime can archive
//     thousands of members one at a time (test_project_registry.cpp's own G4 grows one to 2000),
//     and modeling that as "read the whole growing list, append one, write the whole list back"
//     on every single archive call would turn an O(1)-per-append log into an O(n) per-append
//     read-modify-write -- a real performance regression across a project's whole lifetime, not a
//     narrowing rt::SessionStore's single-slot contract can absorb for free the way AgentSession's/
//     WorkflowSupervisor's own checkpoints could. This needs a genuine append-only log store
//     primitive nothing in this codebase has built yet (the same gap WorkflowSupervisor's own
//     retained_checkpoints()/time-travel is blocked on) -- named here as real, shared, not-yet-built
//     infrastructure, not silently worked around with a shape that would quietly regress.

#include <string>
#include <unordered_set>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

struct CreateProject {
    std::string project_id;
    std::string principal_id;
    std::string principal_tenant_id;
};

struct CreateProjectResult {
    bool ok = false;
    std::string error_code;  // "project.duplicate_id" -- fails closed rather than silently
                              // overwriting a caller's earlier Project, same as the original.
};

struct ListProjects {
    std::string principal_id;
    std::string principal_tenant_id;
};

struct ListProjectsResult {
    std::vector<std::string> project_ids;
};

// The registry's own record shape -- same fields as the original's ProjectRegistryEntry, kept here
// rather than reused across the Quark boundary (that type is QUARK_SERIALIZE-tagged) so this file
// stays fully Quark-free.
struct ProjectRegistryEntry {
    std::string project_id;
    std::string principal_id;
    std::string principal_tenant_id;

    friend bool operator==(ProjectRegistryEntry const&, ProjectRegistryEntry const&) = default;
};

class ProjectRegistry {
public:
    // I1-shaped: create_project's own check-and-insert (is this id already known?) must be one
    // atomic step under session_mutex_-equivalent serialization, or two concurrent create_project
    // calls with the SAME id could both observe "not yet known" and both insert -- the exact
    // double-registration hazard rt::SpawnCostBudget's own file banner names for a consumed pool,
    // one layer up.
    [[nodiscard]] task<CreateProjectResult> create_project(CreateProject request) {
        AsyncMutex::Guard guard = co_await mutex_.lock();
        if (known_ids_.contains(request.project_id)) {
            co_return CreateProjectResult{false, "project.duplicate_id"};
        }
        known_ids_.insert(request.project_id);
        entries_.push_back(ProjectRegistryEntry{std::move(request.project_id),
                                                  std::move(request.principal_id),
                                                  std::move(request.principal_tenant_id)});
        co_return CreateProjectResult{true, {}};
    }

    // Read-only, so it does NOT need mutex_ serialization against another concurrent list_projects
    // call (two concurrent reads never race each other) -- but IS locked, matching AgentSession's
    // own "every public entry point" discipline, since a concurrent create_project must not be
    // observed mid-mutation (a torn read of entries_/known_ids_ while a create_project's own
    // guard-protected block is running). Scoped on BOTH principal_id and principal_tenant_id, never
    // id alone -- 018 Sec6's multi-tenant shape: two tenants using the same principal id must not
    // see each other's Projects.
    [[nodiscard]] task<ListProjectsResult> list_projects(ListProjects request) {
        AsyncMutex::Guard guard = co_await mutex_.lock();
        ListProjectsResult out;
        for (auto const& e : entries_) {
            if (e.principal_id == request.principal_id &&
                e.principal_tenant_id == request.principal_tenant_id) {
                out.project_ids.push_back(e.project_id);
            }
        }
        co_return out;
    }

    // Recovery (external, mirrors AgentSession::restore_from_record) -- a host repopulates the
    // in-memory index from wherever it persisted entries(), after a process restart. Unlocked,
    // matching the original's own restore_index() -- a host calls this before the registry is
    // reachable by any concurrent caller, the same precondition every other rt:: restore_from_record()
    // carries.
    void restore_index(std::vector<ProjectRegistryEntry> const& entries) {
        entries_ = entries;
        known_ids_.clear();
        known_ids_.reserve(entries.size());
        for (auto const& e : entries) known_ids_.insert(e.project_id);
    }

    [[nodiscard]] std::vector<ProjectRegistryEntry> const& entries() const noexcept { return entries_; }

private:
    std::vector<ProjectRegistryEntry> entries_;
    std::unordered_set<std::string>   known_ids_;
    AsyncMutex                         mutex_;
};

}  // namespace agentengine::rt
