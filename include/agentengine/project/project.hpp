#pragma once
// Implements 030-Project-Workspace-and-Lifecycle.md §2 (the model) and §3 (persistence: "a new
// record type, stored through Quark 012's Store seam exactly like AgentSession is"). Milestone 6
// Phase H (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// TWO SEPARATE Store-backed shapes under DIFFERENT ActorIds, not one -- a deliberate departure
// from §3's own literal "always snapshot-mode, never event-sourced" sentence, forced by §7 G4's
// own arithmetic, not a preference:
//
//   - `ProjectRecord` (this file's "active manifest"): Snapshot mode, matching §3's text as
//     written. Rewritten wholesale on every mutation -- cheap because §2's own model keeps
//     `active_members` small by construction (a member moves OUT to the archived tail the moment
//     its role completes, per §8 Q1's resolution).
//   - The archived tail: EventSourced (`EventLog<ProjectMember,S>`), one APPEND per member
//     archived, held under a SEPARATE ActorId, never inside the snapshot blob above.
//
// WHY THE SPLIT IS FORCED. §8 Q1 resolved that the archived tail "can grow unboundedly over a
// Project's long lifetime," and §7 G4 requires "a member session moving to the archived tail
// never triggers a full manifest rewrite proportional to archived-tail size." A single Snapshot
// record containing BOTH lists cannot satisfy that the moment the tail grows past trivial size:
// Quark's `Store::save_snapshot` (persistence.hpp) is an overwrite-the-whole-blob write, so its
// cost is O(current blob size) on EVERY call, regardless of how small the actual delta is --
// archiving member #10,001 into an existing 10,000-entry list would cost O(10,000) if both lists
// shared one record. `EventLog::commit()` (event_log.hpp, ADR-009 C7) is a bounded APPEND: its
// cost is a function of the batch just staged, never of how many events came before. That is
// exactly the property G4 asks for, and it is a real, load-bearing distinction between Quark's two
// persistence models (012 §"Two models"), not an implementation detail either model would serve
// equally. §3's "always snapshot-mode" sentence is read here as the choice for the manifest's own
// identity/status/active-member fields specifically -- it does not, and structurally cannot, also
// hold for the one field §8 Q1 separately proves is unbounded. This is the exact same lesson
// Milestone 6 Phase F already learned for `WorkflowSupervisor`'s own checkpoint log (014 §5's
// "rewind to any retained checkpoint" forced EventLog over Snapshot for the same reason: Store's
// snapshot slot is latest-only, and the thing needed here genuinely grows without bound).
//
// NO SELF-PERSISTENCE, matching this codebase's own consistent I2 discipline: neither
// `AgentSession` nor `WorkflowSupervisor` ever reaches for a `Store` from inside their own
// handlers (`save_agent_session_snapshot`/`save_workflow_checkpoint` are external free functions a
// HOST calls). The free functions below follow the identical shape -- callable by whatever holds
// an explicit `Activation&`/`Store&`/`FenceToken`, never assumed ambient.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quark/core/event_log.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/snapshot.hpp"

#include "agentengine/core/worktree.hpp"

namespace agentengine {

// 030 §2's `status: Active | Paused | Archived`.
enum class project_status { active, paused, archived };  // ae-naming-lint: allow project_status — 030 §2 names this concept normatively; 027 has not been updated to list it

// One entry in either `active_members` or `archived_members` (030 §2's per-member shape).
// `status` is deliberately an opaque, caller-defined string -- mirroring `host_metadata`'s own
// opacity below -- because §2 lists the FIELD but never enumerates its values; inventing a closed
// enum here would be normative content this RFC does not actually specify.
struct ProjectMember {  // ae-naming-lint: allow ProjectMember — 030 §2 names this concept normatively; 027 has not been updated to list it
    std::string  session_id;
    std::string  parent_session_id;  // empty means this member IS the Project's root session
    std::string  role;
    std::int64_t spawned_at_ns = 0;  // 001 §7's usual caveat: no real wall-clock source wired yet
    Digest       worktree_ref;       // 025's own per-session worktree; NOT shared across members (§2)
    std::string  status;             // caller-opaque; 030 §2 does not enumerate values

    friend bool operator==(ProjectMember const&, ProjectMember const&) = default;
};
QUARK_SERIALIZE(ProjectMember, (1, session_id), (2, parent_session_id), (3, role), (4, spawned_at_ns),
                (5, worktree_ref), (6, status))

// 030 §2's `Project` struct, minus the archived tail (see this file's own header comment). Fields
// are flattened (`principal_id`/`principal_tenant_id`, not a nested `Principal`) -- the same
// pattern `AgentSessionRecord` already established (agent_session.hpp), not an untested first use.
// `host_metadata` is opaque text (030 §2: "opaque to the engine; a WinUI host's tab title/icon
// lives here") -- the engine never parses it, same treatment `ToolCall::arguments_json` gets.
struct ProjectRecord {  // ae-naming-lint: allow ProjectRecord — 030 §2 names "Project" normatively; 027 has not been updated to list a durable-record type
    std::string   project_id;
    std::string   principal_id;
    std::string   principal_tenant_id;
    std::string   root_session_id;
    std::vector<ProjectMember> active_members;
    project_status status = project_status::active;
    std::int64_t  created_at_ns    = 0;
    std::int64_t  updated_at_ns    = 0;
    std::int64_t  last_active_at_ns = 0;
    std::string   title;
    std::string   host_metadata;

    friend bool operator==(ProjectRecord const&, ProjectRecord const&) = default;
};
QUARK_SERIALIZE(ProjectRecord, (1, project_id), (2, principal_id), (3, principal_tenant_id),
                (4, root_session_id), (5, active_members), (6, status), (7, created_at_ns),
                (8, updated_at_ns), (9, last_active_at_ns), (10, title), (11, host_metadata))

// A stable ActorId tag for a Project's ACTIVE manifest snapshot -- mirrors `kAgentSessionTypeKey`
// exactly (agent_session.hpp), packing "AEPROJM1" (PROJect Manifest) as 8 ASCII bytes.
inline constexpr quark::TypeKey kProjectManifestTypeKey{0x4145'5052'4F4A'4D31ULL};  // "AEPROJM1"
// A SEPARATE tag for the archived-tail event log -- must differ from the manifest's own tag (this
// file's header comment explains why the two live under different ActorIds at all), packing
// "AEPROJA1" (PROJect Archive).
inline constexpr quark::TypeKey kProjectArchiveTypeKey{0x4145'5052'4F4A'4131ULL};  // "AEPROJA1"

[[nodiscard]] inline quark::ActorId project_actor_id(std::string_view project_id) noexcept {
    return quark::ActorId{kProjectManifestTypeKey, std::hash<std::string_view>{}(project_id)};
}
[[nodiscard]] inline quark::ActorId project_archive_actor_id(std::string_view project_id) noexcept {
    return quark::ActorId{kProjectArchiveTypeKey, std::hash<std::string_view>{}(project_id)};
}

// ---- Active manifest: Snapshot model, mirrors save_agent_session_snapshot/load_agent_session_snapshot

template <quark::Store S>
[[nodiscard]] quark::result<void> save_project_snapshot(quark::Activation& act, S& store,
                                                         ProjectRecord const& rec,
                                                         quark::FenceToken fence) {
    return quark::snapshot_sequential<ProjectRecord>(act, store, project_actor_id(rec.project_id),
                                                      fence, /*through_seq=*/0, rec);
}

template <quark::Store S>
[[nodiscard]] quark::result<std::optional<ProjectRecord>> load_project_snapshot(
    S& store, std::string_view project_id) {
    auto rec = quark::load_snapshot<ProjectRecord>(store, project_actor_id(project_id));
    if (!rec) return std::unexpected(rec.error());
    if (!rec->has_value()) return std::optional<ProjectRecord>{};
    return std::optional<ProjectRecord>{std::move((*rec)->state)};
}

// ---- Archived tail: EventSourced model, one append per member archived (this file's own header
// comment explains why -- G4's bounded-write-cost requirement is unsatisfiable any other way).

template <quark::Store S>
[[nodiscard]] quark::result<quark::SeqNo> archive_project_member(S& store, std::string_view project_id,
                                                                  quark::FenceToken fence,
                                                                  ProjectMember member) {
    auto const id = project_archive_actor_id(project_id);
    quark::EventLog<ProjectMember, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(std::move(member));
    return log.commit();
}

// Every archived member for a Project, in the order they were archived. An O(N) READ over the
// tail -- G4 bounds WRITE cost, not read cost, the same asymmetry any append-only log has.
template <quark::Store S>
[[nodiscard]] quark::result<std::vector<ProjectMember>> read_archived_members(
    S& store, std::string_view project_id) {
    std::vector<ProjectMember> out;
    auto last = quark::replay_tail<ProjectMember>(
        store, project_archive_actor_id(project_id), /*from=*/0, out,
        [](std::vector<ProjectMember>& acc, ProjectMember const& ev) { acc.push_back(ev); });
    if (!last) return std::unexpected(last.error());
    return out;
}

}  // namespace agentengine
