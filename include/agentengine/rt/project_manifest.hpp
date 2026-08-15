#pragma once
// ADR-037: agentengine::rt::ProjectRecord -- the Quark-actor-free replacement for
// `project/project.hpp`'s own manifest half (`ProjectRecord`, `save_project_snapshot`/
// `load_project_snapshot`) and `project/lifecycle.hpp`'s status-flip free functions
// (`pause_project`/`restore_project`/`archive_project`). Closes the last named residual from both
// `rt::ProjectRegistry`'s own banner ("the manifest is single-slot [tractable]... just not done in
// this pass") and `rt::ProjectSupervisor`'s own banner ("a future pass that ports project.hpp's
// manifest can layer pause_project/restore_project/archive_project's status-flip logic on top of
// THIS type's checkpoint_members_and_workflows() unchanged").
//
// SINGLE-SLOT, via `rt::SessionStore` -- NOT `rt::AppendLogStore`. Unlike the archived-member tail
// (project_archive.hpp), 030 §2's own model keeps `active_members` small by construction (a member
// moves OUT to the archived tail the moment its role completes, per §8 Q1's resolution), so the
// manifest is rewritten wholesale on every mutation exactly like `AgentSessionRecord`'s own snapshot
// -- there is no unbounded-growth shape here to force an append-only log, so this reuses the SAME
// `rt::SessionStore` concept and byte-encoding pattern `agent_session.hpp`'s own
// `save_agent_session_snapshot`/`load_agent_session_snapshot` already established (a `ProjectRecord`
// keyed by `project_id` is stored under the identical `SessionStore` shape a session keyed by
// `session_id` already uses -- one store type, two independent key spaces, no collision risk since a
// host chooses distinct root directories / distinct in-memory instances per record kind, matching how
// this codebase already treats `AgentSessionRecord` and `WorkflowCheckpointEntry` as separate
// concerns even when both could theoretically share one store instance).
//
// NO IN-FLIGHT GUARD NEEDED, unlike `AgentSession`'s own snapshot path. `agent_session.hpp`'s
// `save_agent_session_snapshot` locks `session_mutex_` because it reads a LIVE `AgentSession&`'s
// mutable state concurrently with `start_run()`/`resolve_interaction()`. `ProjectRecord` here is a
// plain VALUE the caller already holds (returned by `pause_project`/`restore_project`/
// `archive_project`, or read back via `load_project_snapshot`) -- there is no live, mutably-shared
// object this file's own functions read out from under a concurrent mutator, so `save_project_snapshot`
// is a plain synchronous function, not a coroutine.
//
// PAUSE/RESTORE/ARCHIVE -- REWORKED, NOT PORTED VERBATIM, because the Quark original's `pause_project`
// closed over `ActorRef<ProjectSupervisor>::passivate()` (evicting an actor from memory), a concept
// `rt::ProjectSupervisor`'s own banner already explains has NO rt:: equivalent (a plain host-held
// object has no runtime managing its residency to evict from). What survives is the OTHER half
// passivation always did: flushing durable state before giving up temporal ownership -- exactly what
// `rt::ProjectSupervisor::checkpoint_members_and_workflows()` already does. `pause_project` below
// calls that, then flips the record's status -- but ONLY if every checkpoint succeeded
// (`CheckpointReport::all_ok()`). This is a REAL design choice, not a mechanical translation of the
// Quark original (which had no error-reporting shape at all and always flipped status unconditionally):
// marking a Project "paused" while some member's durable state failed to flush would let an operator
// believe a resume later restores from durable state that was never actually written -- the same
// "fail closed rather than silently proceed on a partial failure" discipline this codebase already
// applies elsewhere (e.g. `delete_session`'s own two-outcome receipt). `archive_project` composes
// `pause_project` the same way the original did, inheriting the same fail-closed rule.
//
// `restore_project` needs no I/O at all (matching the Quark original exactly: "this does NOT need to
// eagerly reactivate every member session... the moment a host issues a Run against any member,
// [lazy activation] brings that one session back" -- in rt:: land there is no lazy activation to rely
// on either, but the SAME observation holds: a status flip alone is honest, since nothing else in this
// codebase's rt:: model auto-reactivates anything on a schedule this file could hook into), so it
// stays a plain, non-coroutine function.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/project_archive.hpp"
#include "agentengine/rt/project_supervisor.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// 030 §2's `status: Active | Paused | Archived`. Redefined here rather than reused across the Quark
// boundary (`agentengine::project_status`, project.hpp) -- same reasoning `ProjectMember` already
// established in project_archive.hpp: including project.hpp would drag in `quark/core/event_log.hpp`
// et al., defeating the point of this file staying Quark-free.
// ae-naming-lint: allow project_status — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class project_status { active, paused, archived };

// 030 §2's `Project` struct, minus the archived tail (project_archive.hpp owns that separately, same
// split the Quark original enforced and the SAME reasoning: §7 G4 forbids the archived tail from
// living inside a record that gets rewritten wholesale on every mutation).
// ae-naming-lint: allow ProjectRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ProjectRecord {
    std::string                project_id;
    std::string                principal_id;
    std::string                principal_tenant_id;
    std::string                root_session_id;
    std::vector<ProjectMember> active_members;
    project_status              status = project_status::active;
    std::int64_t                created_at_ns     = 0;
    std::int64_t                updated_at_ns     = 0;
    std::int64_t                last_active_at_ns = 0;
    std::string                  title;
    std::string                  host_metadata;

    friend bool operator==(ProjectRecord const&, ProjectRecord const&) = default;
};

[[nodiscard]] inline std::string project_status_to_string(project_status s) {
    switch (s) {
        case project_status::active: return "active";
        case project_status::paused: return "paused";
        case project_status::archived: return "archived";
    }
    return "active";
}

[[nodiscard]] inline agentengine::result<project_status> project_status_from_string(
    std::string const& s) {
    if (s == "active") return project_status::active;
    if (s == "paused") return project_status::paused;
    if (s == "archived") return project_status::archived;
    return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                "unknown project_status: " + s,
                                                "rt.project_manifest.status.malformed"});
}

[[nodiscard]] inline agentengine::json::Value project_record_to_json(ProjectRecord const& r) {
    std::vector<agentengine::json::Value> members;
    members.reserve(r.active_members.size());
    for (ProjectMember const& m : r.active_members) members.push_back(project_member_to_json(m));

    return agentengine::json::Value::make_object({
        {"project_id", agentengine::json::Value::make_string(r.project_id)},
        {"principal_id", agentengine::json::Value::make_string(r.principal_id)},
        {"principal_tenant_id", agentengine::json::Value::make_string(r.principal_tenant_id)},
        {"root_session_id", agentengine::json::Value::make_string(r.root_session_id)},
        {"active_members", agentengine::json::Value::make_array(std::move(members))},
        {"status", agentengine::json::Value::make_string(project_status_to_string(r.status))},
        {"created_at_ns", agentengine::json::Value::make_number(static_cast<double>(r.created_at_ns))},
        {"updated_at_ns", agentengine::json::Value::make_number(static_cast<double>(r.updated_at_ns))},
        {"last_active_at_ns",
         agentengine::json::Value::make_number(static_cast<double>(r.last_active_at_ns))},
        {"title", agentengine::json::Value::make_string(r.title)},
        {"host_metadata", agentengine::json::Value::make_string(r.host_metadata)},
    });
}

[[nodiscard]] inline agentengine::result<ProjectRecord> project_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* project_id_v   = v.find("project_id");
    agentengine::json::Value const* principal_id_v  = v.find("principal_id");
    agentengine::json::Value const* tenant_v         = v.find("principal_tenant_id");
    agentengine::json::Value const* root_v           = v.find("root_session_id");
    agentengine::json::Value const* members_v        = v.find("active_members");
    agentengine::json::Value const* status_v         = v.find("status");
    agentengine::json::Value const* created_v        = v.find("created_at_ns");
    agentengine::json::Value const* updated_v        = v.find("updated_at_ns");
    agentengine::json::Value const* last_active_v    = v.find("last_active_at_ns");
    agentengine::json::Value const* title_v          = v.find("title");
    agentengine::json::Value const* host_metadata_v  = v.find("host_metadata");
    if (project_id_v == nullptr || !project_id_v->is_string() || principal_id_v == nullptr ||
        !principal_id_v->is_string() || tenant_v == nullptr || !tenant_v->is_string() ||
        root_v == nullptr || !root_v->is_string() || members_v == nullptr ||
        !members_v->is_array() || status_v == nullptr || !status_v->is_string() ||
        created_v == nullptr || !created_v->is_number() || updated_v == nullptr ||
        !updated_v->is_number() || last_active_v == nullptr || !last_active_v->is_number() ||
        title_v == nullptr || !title_v->is_string() || host_metadata_v == nullptr ||
        !host_metadata_v->is_string()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed ProjectRecord",
                                                    "rt.project_manifest.record.malformed"});
    }

    ProjectRecord out;
    out.project_id           = project_id_v->as_string();
    out.principal_id          = principal_id_v->as_string();
    out.principal_tenant_id   = tenant_v->as_string();
    out.root_session_id        = root_v->as_string();
    out.active_members.reserve(members_v->as_array().size());
    for (agentengine::json::Value const& item : members_v->as_array()) {
        auto member = project_member_from_json(item);
        if (!member) return std::unexpected(member.error());
        out.active_members.push_back(std::move(*member));
    }
    auto status = project_status_from_string(status_v->as_string());
    if (!status) return std::unexpected(status.error());
    out.status               = *status;
    out.created_at_ns         = static_cast<std::int64_t>(created_v->as_number());
    out.updated_at_ns          = static_cast<std::int64_t>(updated_v->as_number());
    out.last_active_at_ns       = static_cast<std::int64_t>(last_active_v->as_number());
    out.title                    = title_v->as_string();
    out.host_metadata             = host_metadata_v->as_string();
    return out;
}

[[nodiscard]] inline std::vector<std::byte> encode_project_record(ProjectRecord const& rec) {
    std::string const text = agentengine::json::dump(project_record_to_json(rec));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] inline agentengine::result<ProjectRecord> decode_project_record(
    std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
    auto parsed = agentengine::json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return project_record_from_json(*parsed);
}

// ---- Active manifest: single-slot, mirrors save_agent_session_snapshot/load_agent_session_snapshot
// (agent_session.hpp) but plain/synchronous -- see file banner for why no in-flight guard is needed.

template <SessionStore StoreT>
[[nodiscard]] result<void> save_project_snapshot(StoreT& store, ProjectRecord const& rec) {
    return store.save(rec.project_id, encode_project_record(rec));
}

template <SessionStore StoreT>
[[nodiscard]] result<std::optional<ProjectRecord>> load_project_snapshot(
    StoreT const& store, std::string_view project_id) {
    std::string const id{project_id};
    if (!store.exists(id)) return std::optional<ProjectRecord>{};
    result<std::vector<std::byte>> bytes = store.load(id);
    if (!bytes) return std::unexpected(bytes.error());
    result<ProjectRecord> rec = decode_project_record(*bytes);
    if (!rec) return std::unexpected(rec.error());
    return std::optional<ProjectRecord>{std::move(*rec)};
}

// ---- Directed lifecycle (030 §4) -- see file banner for why these are reworked, not ported
// verbatim, and for the fail-closed rule ("status only flips if every checkpoint succeeded").

// 030 §4's Pause: checkpoint every registered member/workflow through `sup`, then mark the record
// Paused -- ONLY if every checkpoint succeeded. Does NOT itself persist the manifest, matching this
// codebase's consistent I2 discipline (project.hpp's own header note, carried over unchanged): the
// caller separately calls `save_project_snapshot` with the returned, possibly-updated record.
[[nodiscard]] inline task<std::pair<ProjectRecord, CheckpointReport>> pause_project(
    ProjectSupervisor& sup, ProjectRecord rec) {
    CheckpointReport report = co_await sup.checkpoint_members_and_workflows();
    if (report.all_ok()) rec.status = project_status::paused;
    co_return std::pair<ProjectRecord, CheckpointReport>{std::move(rec), std::move(report)};
}

// 030 §4's Restore: a pure status flip, no I/O -- see file banner for why no reactivation step is
// needed here either, in rt:: land just as in the Quark original.
[[nodiscard]] inline ProjectRecord restore_project(ProjectRecord rec) {
    rec.status = project_status::active;
    return rec;
}

// 030 §4's Archive: pause, then apply retention policy. No retention/GC mechanism exists in this
// codebase yet (matching the Quark original's own honest scope -- "archived means hidden, not
// shrunk"), so this is exactly the status flip, after an ordinary pause, inheriting pause's own
// fail-closed rule (archiving does not proceed past `paused` if a checkpoint failed).
[[nodiscard]] inline task<std::pair<ProjectRecord, CheckpointReport>> archive_project(
    ProjectSupervisor& sup, ProjectRecord rec) {
    auto [paused, report] = co_await pause_project(sup, std::move(rec));
    if (report.all_ok()) paused.status = project_status::archived;
    co_return std::pair<ProjectRecord, CheckpointReport>{std::move(paused), std::move(report)};
}

}  // namespace agentengine::rt
