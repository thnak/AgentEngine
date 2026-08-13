#pragma once
// ADR-037: agentengine::rt::ProjectRegistry's own archived-member tail (030-Project-Workspace-and-
// Lifecycle.md Sec6/§7), wired onto `rt::AppendLogStore` -- the primitive `project_registry.hpp`'s
// own banner names as the missing dependency: "the archived tail is genuinely, structurally
// append-only -- a real project's lifetime can archive thousands of members one at a time
// (test_project_registry.cpp's own G4 grows one to 2000), and modeling that as read-modify-write-
// the-whole-list on every archive call would turn an O(1)-per-append log into an O(n) per-append
// read-modify-write." That primitive was built and proven standalone in an earlier ADR-037 pass
// (append_log_store.hpp) and is wired here, the same way it was already wired into
// `rt::WorkflowSupervisor`'s time-travel surface (workflow_time_travel.hpp) -- the two files this
// project's own AppendLogStore banner names as the SAME shared gap, not two separate problems.
//
// PORTS `project/project.hpp`'s own `archive_project_member`/`read_archived_members`
// (quark::EventLog<ProjectMember,S>-backed) behaviorally: an archived member is appended once, in
// full, and the tail is always read back in the exact order members were archived (the old suite's
// own H5 claim, ported verbatim below). `ProjectMember`'s `worktree_ref` field is a hex SHA-256
// digest string (`agentengine::Digest`'s own representation, core/worktree.hpp) -- kept as a plain
// `std::string` here rather than pulling in that header, since the field is opaque to this file.
//
// NO TWO-PHASE PENDING->COMMITTED, unlike `workflow_time_travel.hpp`'s checkpoint entries. A
// checkpoint needed two phases because it is PROVISIONAL until explicitly committed (a caller may
// stage one and never commit it, e.g. a crash mid-checkpoint-sequence). Archiving a member has no
// such provisional state in 030's own model -- `archive_project_member` is a single, terminal fact
// the instant it is called (the member moved to the archived tail; there is no "pending archive" the
// original ever modeled either, per its own single `EventLog::commit()` call at project.hpp:141-143)
// -- so this mirrors the simpler single-append shape `read_rewind_audit_log` already uses, not the
// checkpoint log's two-phase one. `AppendLogStore::append()` is itself all-or-nothing per entry
// (append_log_store.hpp's own contract), which is the only atomicity this operation ever needed.
//
// NOT PORTED THIS PASS: `project/project.hpp`'s `ProjectRecord` manifest snapshot
// (`save_project_snapshot`/`load_project_snapshot`) and the `pause_project`/`restore_project`/
// `archive_project` status-flip functions. The manifest is single-slot-tractable (an
// `rt::SessionStore` snapshot, matching `save_agent_session_snapshot`'s own shape) and does not need
// `AppendLogStore` at all -- named here as a separate, smaller, still-open follow-up, not silently
// folded into this file's scope.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine::rt {

// Same fields as project/project.hpp's own ProjectMember, kept here rather than reused across the
// Quark boundary (that type is QUARK_SERIALIZE-tagged) so this file stays fully Quark-free -- the
// same reasoning project_registry.hpp's own ProjectRegistryEntry already established.
struct ProjectMember {
    std::string   session_id;
    std::string   parent_session_id;  // empty means this member IS the Project's root session
    std::string   role;
    std::int64_t  spawned_at_ns = 0;
    std::string   worktree_ref;       // hex SHA-256 digest (agentengine::Digest's own representation)
    std::string   status;             // caller-opaque

    friend bool operator==(ProjectMember const&, ProjectMember const&) = default;
};

[[nodiscard]] inline agentengine::json::Value project_member_to_json(ProjectMember const& m) {
    return agentengine::json::Value::make_object({
        {"session_id", agentengine::json::Value::make_string(m.session_id)},
        {"parent_session_id", agentengine::json::Value::make_string(m.parent_session_id)},
        {"role", agentengine::json::Value::make_string(m.role)},
        {"spawned_at_ns",
         agentengine::json::Value::make_number(static_cast<double>(m.spawned_at_ns))},
        {"worktree_ref", agentengine::json::Value::make_string(m.worktree_ref)},
        {"status", agentengine::json::Value::make_string(m.status)},
    });
}

[[nodiscard]] inline agentengine::result<ProjectMember> project_member_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* session_id_v = v.find("session_id");
    agentengine::json::Value const* parent_v      = v.find("parent_session_id");
    agentengine::json::Value const* role_v        = v.find("role");
    agentengine::json::Value const* spawned_v     = v.find("spawned_at_ns");
    agentengine::json::Value const* worktree_v    = v.find("worktree_ref");
    agentengine::json::Value const* status_v      = v.find("status");
    if (session_id_v == nullptr || !session_id_v->is_string() || parent_v == nullptr ||
        !parent_v->is_string() || role_v == nullptr || !role_v->is_string() ||
        spawned_v == nullptr || !spawned_v->is_number() || worktree_v == nullptr ||
        !worktree_v->is_string() || status_v == nullptr || !status_v->is_string()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed ProjectMember",
                                                    "rt.project_archive.member.malformed"});
    }
    ProjectMember out;
    out.session_id         = session_id_v->as_string();
    out.parent_session_id  = parent_v->as_string();
    out.role                = role_v->as_string();
    out.spawned_at_ns       = static_cast<std::int64_t>(spawned_v->as_number());
    out.worktree_ref        = worktree_v->as_string();
    out.status               = status_v->as_string();
    return out;
}

// One archive log per Project, keyed by project_id -- mirrors the original's own separate
// kProjectArchiveTypeKey ActorId (project.hpp:105-112), just re-expressed as a LogId string instead
// of a second hashed ActorId space.
[[nodiscard]] inline LogId project_archive_log_id(std::string_view project_id) {
    return std::string(project_id) + ":archived_members";
}

template <AppendLogStore StoreT>
[[nodiscard]] result<void> archive_project_member(StoreT& store, std::string_view project_id,
                                                    ProjectMember member) {
    std::string const text = agentengine::json::dump(project_member_to_json(member));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(project_archive_log_id(project_id), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());
    return {};
}

// Every archived member for a Project, in the order they were archived -- the old suite's own H5
// claim (test_project_registry.cpp:181-190). An O(N) read over the tail, matching the original's own
// documented asymmetry (project.hpp:146-147: "G4 bounds WRITE cost, not read cost").
template <AppendLogStore StoreT>
[[nodiscard]] result<std::vector<ProjectMember>> read_archived_members(StoreT const& store,
                                                                         std::string_view project_id) {
    auto raw = store.read_from(project_archive_log_id(project_id), 0);
    if (!raw) return std::unexpected(raw.error());

    std::vector<ProjectMember> out;
    out.reserve(raw->size());
    for (std::vector<std::byte> const& bytes : *raw) {
        std::string text;
        text.reserve(bytes.size());
        for (std::byte b : bytes) text.push_back(static_cast<char>(b));
        auto parsed = agentengine::json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto member = project_member_from_json(*parsed);
        if (!member) return std::unexpected(member.error());
        out.push_back(std::move(*member));
    }
    return out;
}

}  // namespace agentengine::rt
