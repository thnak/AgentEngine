#pragma once
// Implements 030-Project-Workspace-and-Lifecycle.md §6's "create_project and list_projects are
// ordinary asks against a small Project-registry actor -- the shared index a new Project needs to
// register into, and a principal-scoped list needs to query." Milestone 6 Phase H
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// WHY A REGISTRY IS NEEDED AT ALL (not just "scan the Store"): Quark's `Store` concept
// (persistence.hpp) has no enumeration primitive -- every operation (`load_snapshot`, `read_log`,
// ...) takes a specific `ActorId` the caller already knows. There is no "list every snapshotted
// actor" call. Answering `list_projects(principal)` therefore needs an explicit INDEX somewhere,
// which is exactly what §6 says this actor's job is -- "an index, not a new execution unit" (§1),
// the same words applied one layer up from a `Project` itself to the thing that tracks Projects.
//
// NO SELF-PERSISTENCE, matching `project.hpp`'s own header note and this codebase's consistent I2
// discipline (`AgentSession`/`WorkflowSupervisor` never touch a `Store` from inside their own
// handlers either). `ProjectRegistry::handle(CreateProject)` mutates ONLY its own in-memory index
// -- a HOST calls `append_project_registered()` (durability for the registry's own index) and
// `save_project_snapshot()` (project.hpp; durability for the new Project's manifest) SEPARATELY,
// exactly mirroring `AgentSession::to_record()` + the external `save_agent_session_snapshot()`
// call a host drives afterward. Recovery is the same external shape: a host reads the registry's
// own durable log (`read_registry_log()`) and calls `restore_index()`, mirroring
// `restore_from_record()`.
//
// `project_id` IS CALLER-SUPPLIED, never minted here -- matching `session_id`'s own provenance
// (`AgentSession::initialize(std::string session_id, ...)`, agent_session.hpp:405): this codebase
// has no wired random source anywhere (I5: nondeterminism crosses a recorded seam), and inventing
// one here to mint ids would be exactly the kind of unrecorded nondeterministic input I5 forbids.
// The caller/host already has whatever real id-generation scheme it uses for sessions; a Project's
// id is the same kind of thing, one layer up.

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/ids.hpp"

namespace agentengine {

// 030 §6: `create_project`. `project_id` is caller-supplied (see this file's own header note).
// DELIBERATELY MINIMAL -- Quark's actor messages ride a fixed 192-byte inline pool cell
// (`detail::MessagePool::kMaxPayload`, message_pool.hpp); a query carrying a full `Principal`
// (which has its own `kind`/`on_behalf_of`/`delegation_depth` fields this actor never needs) plus
// `root_session_id`/`title`/`host_metadata` measured well over that budget. Only the two
// `Principal` fields `ProjectRecord` itself actually flattens (`principal_id`/
// `principal_tenant_id` -- the same flattening precedent `AgentSessionRecord` already set) travel
// in the ask; the registry's own job is registering an id against a principal, not carrying
// cosmetic manifest content it never persists anyway (see this file's own header note on why
// persistence is the host's job, not this actor's). A caller who wants `root_session_id`/`title`/
// `host_metadata` on the record builds the rest of `ProjectRecord` itself -- it already has every
// input needed to do so -- before calling `save_project_snapshot` (project.hpp).
struct CreateProject {  // ae-naming-lint: allow CreateProject — 030 §6 names this verb normatively; 027 has not been updated to list it
    std::string project_id;
    std::string principal_id;
    std::string principal_tenant_id;
};

struct CreateProjectResult {  // ae-naming-lint: allow CreateProjectResult — this file's own ask-reply pairing, mirrors WorkflowResult's naming
    bool ok = false;
    // "project.duplicate_id" when `project_id` is already registered -- fails closed rather than
    // silently overwriting a caller's earlier Project (a real risk: a caller retrying a create
    // after a lost reply must not stomp the original).
    std::string error_code;
};

// 030 §6: `list_projects`, "principal-scoped manifest query" -- 030 §7 G3's own bar.
struct ListProjects {  // ae-naming-lint: allow ListProjects — 030 §6 names this verb normatively; 027 has not been updated to list it
    std::string principal_id;
    std::string principal_tenant_id;
};

struct ListProjectsResult {  // ae-naming-lint: allow ListProjectsResult — this file's own ask-reply pairing, mirrors WorkflowResult's naming
    // project_id ONLY -- the registry stays an INDEX (030 §1), never a second copy of manifest
    // content. A caller who wants title/status/member-count per id calls `load_project_snapshot`
    // (project.hpp) for each one, keeping ProjectRecord the single source of truth.
    std::vector<std::string> project_ids;
};

// The registry's OWN durability record -- one per successful `create_project`, appended to an
// EventLog (never a Snapshot: the registry's own index grows exactly like the archived tail does,
// so it gets the same treatment for the same reason, project.hpp's own header comment).
struct ProjectRegistryEntry {  // ae-naming-lint: allow ProjectRegistryEntry — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::string project_id;
    std::string principal_id;
    std::string principal_tenant_id;

    friend bool operator==(ProjectRegistryEntry const&, ProjectRegistryEntry const&) = default;
};
QUARK_SERIALIZE(ProjectRegistryEntry, (1, project_id), (2, principal_id), (3, principal_tenant_id))

// A SINGLE fixed ActorId: there is exactly one registry per deployment (030 §6 describes it as
// THE shared index), never one per Project or per principal -- unlike `project_actor_id()`, this
// takes no argument to hash.
inline constexpr quark::TypeKey kProjectRegistryTypeKey{0x4145'5052'4547'3031ULL};  // "AEPREG01"
[[nodiscard]] inline quark::ActorId project_registry_actor_id() noexcept {
    return quark::ActorId{kProjectRegistryTypeKey, 0};
}

template <quark::Store S>
[[nodiscard]] quark::result<quark::SeqNo> append_project_registered(S& store, quark::FenceToken fence,
                                                                    ProjectRegistryEntry entry) {
    auto const id = project_registry_actor_id();
    quark::EventLog<ProjectRegistryEntry, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(std::move(entry));
    return log.commit();
}

template <quark::Store S>
[[nodiscard]] quark::result<std::vector<ProjectRegistryEntry>> read_registry_log(S& store) {
    std::vector<ProjectRegistryEntry> out;
    auto last = quark::replay_tail<ProjectRegistryEntry>(
        store, project_registry_actor_id(), /*from=*/0, out,
        [](std::vector<ProjectRegistryEntry>& acc, ProjectRegistryEntry const& ev) {
            acc.push_back(ev);
        });
    if (!last) return std::unexpected(last.error());
    return out;
}

class ProjectRegistry : public quark::Actor<ProjectRegistry, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<CreateProject, CreateProjectResult>,
                                     quark::Ask<ListProjects, ListProjectsResult>>;

    // 030 §6. Registers the id against its owning principal in the in-memory index, synchronously
    // -- no Store access (see this file's own header comment on why persistence is the host's
    // job, and why this query carries only the two identity fields it needs).
    void handle(quark::Ask<CreateProject, CreateProjectResult> const& m) {
        if (known_ids_.contains(m.query.project_id)) {
            CreateProjectResult r;
            r.ok         = false;
            r.error_code = "project.duplicate_id";
            m.respond(std::move(r));
            return;
        }

        known_ids_.insert(m.query.project_id);
        entries_.push_back(ProjectRegistryEntry{m.query.project_id, m.query.principal_id,
                                                m.query.principal_tenant_id});

        CreateProjectResult r;
        r.ok = true;
        m.respond(std::move(r));
    }

    // 030 §6 / §7 G3. Cross-principal isolation is structural here, not a filter that could be
    // bypassed: an entry is matched on BOTH `principal_id` and `principal_tenant_id`, never id
    // alone -- two tenants using the same principal id (a real multi-tenant shape, 018 §6) must
    // not see each other's Projects.
    void handle(quark::Ask<ListProjects, ListProjectsResult> const& m) const {
        ListProjectsResult out;
        for (auto const& e : entries_) {
            if (e.principal_id == m.query.principal_id &&
                e.principal_tenant_id == m.query.principal_tenant_id) {
                out.project_ids.push_back(e.project_id);
            }
        }
        m.respond(std::move(out));
    }

    // Recovery (external, mirrors `AgentSession::restore_from_record`): a host reads
    // `read_registry_log()` and hands the result here to repopulate the in-memory index after a
    // process restart, WITHOUT reactivating a single member session or loading a single Project
    // manifest -- 030 §4's "restore... does not need to eagerly reactivate every member session",
    // applied one layer up to Projects themselves.
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
};

}  // namespace agentengine
