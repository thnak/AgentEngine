// Proof for ADR-037: agentengine::rt::ProjectRecord's manifest snapshot and directed lifecycle
// (include/agentengine/rt/project_manifest.hpp) -- the last named residual from both
// rt::ProjectRegistry's and rt::ProjectSupervisor's own banners. Deterministic, offline,
// single-threaded. Covers:
//   M1 -- a project_id with no snapshot loads nullopt, not an error; save then load round-trips a
//         ProjectRecord (including a non-empty active_members list) byte-for-byte.
//   M2 -- a second save_project_snapshot() under the same project_id overwrites the latest snapshot
//         (single-slot semantics, matching save_agent_session_snapshot's own precedent).
//   P1 -- pause_project(): checkpointing healthy members/workflow succeeds, and the record's status
//         flips to paused ONLY then.
//   P2 -- pause_project() FAIL-CLOSED: a failing member store means the checkpoint report is not
//         all_ok(), and the returned record's status is left UNCHANGED (still active) -- an operator
//         must never be told a Project is "paused" when its durable state did not actually flush.
//   P3 -- restore_project() is a pure status flip back to active, no I/O.
//   P4 -- archive_project(): a healthy pause followed by a further flip to archived.
//   P5 -- archive_project() inherits pause's own fail-closed rule: a failing checkpoint leaves status
//         unchanged, never landing at paused OR archived.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/project_manifest.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::ProjectMember;
using agentengine::rt::ProjectRecord;
using agentengine::rt::ProjectSupervisor;
using agentengine::rt::StartRun;
using agentengine::rt::archive_project;
using agentengine::rt::load_project_snapshot;
using agentengine::rt::pause_project;
using agentengine::rt::project_status;
using agentengine::rt::restore_project;
using agentengine::rt::save_project_snapshot;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

class OneShotChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    agentengine::rt::task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{std::move(m), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<OneShotChatClient>);

// Same fixture test_rt_project_supervisor.cpp's own Q3 uses -- a SessionStore whose save() always
// fails, so a member's checkpoint can be made to fail deterministically without real I/O.
class AlwaysFailingStore {
public:
    [[nodiscard]] agentengine::result<void> save(agentengine::rt::SessionId const&,
                                                    std::vector<std::byte>) {
        return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                    "AlwaysFailingStore refuses every save()",
                                                    "test.always_failing_store.save"});
    }
    [[nodiscard]] agentengine::result<std::vector<std::byte>> load(
        agentengine::rt::SessionId const&) const {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "unused",
                                                    "test.always_failing_store.load"});
    }
    [[nodiscard]] bool exists(agentengine::rt::SessionId const&) const { return false; }
    [[nodiscard]] agentengine::result<void> remove(agentengine::rt::SessionId const&) { return {}; }
};
static_assert(agentengine::rt::SessionStore<AlwaysFailingStore>);

[[nodiscard]] ProjectRecord make_record(std::string project_id) {
    ProjectRecord rec;
    rec.project_id           = std::move(project_id);
    rec.principal_id           = "p1";
    rec.principal_tenant_id    = "tenant-1";
    rec.root_session_id         = "s-root";
    rec.active_members = {
        ProjectMember{"s-root", "", "root", 10, std::string(64, 'a'), "active"},
        ProjectMember{"s-worker", "s-root", "worker", 20, std::string(64, 'b'), "active"},
    };
    rec.status              = project_status::active;
    rec.created_at_ns        = 100;
    rec.updated_at_ns         = 200;
    rec.last_active_at_ns      = 300;
    rec.title                    = "My Project";
    rec.host_metadata             = "{\"tab\":\"1\"}";
    return rec;
}

void test_m1_round_trip_and_absent() {
    InMemorySessionStore store;

    auto absent = load_project_snapshot(store, "proj-never-saved");
    check(absent.has_value(), "M1: load on an unsaved project_id does not error");
    check(absent.has_value() && !absent->has_value(),
          "M1: a project_id with no snapshot loads nullopt, not an error");

    ProjectRecord rec = make_record("proj-1");
    auto saved = save_project_snapshot(store, rec);
    check(saved.has_value(), "M1: save_project_snapshot succeeds");

    auto loaded = load_project_snapshot(store, "proj-1");
    check(loaded.has_value() && loaded->has_value(), "M1: load_project_snapshot finds the saved record");
    check(loaded.has_value() && loaded->has_value() && **loaded == rec,
          "M1: the loaded record is byte-for-byte identical, including its active_members list");
}

void test_m2_overwrite_latest() {
    InMemorySessionStore store;
    ProjectRecord first = make_record("proj-2");
    (void)save_project_snapshot(store, first);

    ProjectRecord second = first;
    second.status = project_status::paused;
    second.title  = "Renamed";
    (void)save_project_snapshot(store, second);

    auto loaded = load_project_snapshot(store, "proj-2");
    check(loaded.has_value() && loaded->has_value() && **loaded == second,
          "M2: a second save under the same project_id overwrites -- load returns the NEWEST, "
          "not the first");
}

void test_p1_pause_healthy() {
    AgentSession<OneShotChatClient> member;
    member.initialize("member-p1", Principal{"p1", ""});
    member.emplace_chat_client();
    (void)drive(member.start_run(StartRun{user_message("hi")}));

    InMemorySessionStore member_store;
    ProjectSupervisor sup;
    sup.register_member(member, member_store);

    ProjectRecord rec = make_record("proj-pause-ok");
    auto [paused, report] = drive(pause_project(sup, rec));
    check(report.all_ok(), "P1: checkpointing a healthy member succeeds");
    check(paused.status == project_status::paused,
          "P1: the record's status flips to paused only after a successful checkpoint");
    check(member_store.exists("member-p1"), "P1: the member's snapshot actually landed");

    // I4 (030 §4: "the pause/restore cycle is invisible to the run"), reframed honestly for rt::
    // land: there is no actor to evict and lazily reactivate here (rt::ProjectSupervisor's own
    // banner -- checkpointing is read-only observation of a live, host-held object, never a teardown
    // of it), so "resume" has nothing to undo. What DOES still need proving is that the checkpoint
    // itself never disturbs the member session's own live state -- a real, SECOND Run issued right
    // after pause_project() must continue the SAME run-id sequence exactly as if nothing had
    // happened, the same claim test_agent_session_suspend_resume.cpp proves for a session paused
    // directly, now proven for one checkpointed through a Project.
    auto second = drive(member.start_run(StartRun{user_message("again")}));
    check(second.has_value() && member.last_run_id() == "member-p1:run:2",
          "I4: a Run against the member session right after pause_project() continues the SAME run "
          "sequence uninterrupted -- checkpointing is read-only, it never disturbs the live session");
}

void test_p2_pause_fail_closed() {
    AgentSession<OneShotChatClient> member;
    member.initialize("member-p2", Principal{"p1", ""});
    member.emplace_chat_client();
    (void)drive(member.start_run(StartRun{user_message("hi")}));

    AlwaysFailingStore failing_store;
    ProjectSupervisor sup;
    sup.register_member(member, failing_store);

    ProjectRecord rec = make_record("proj-pause-fail");
    check(rec.status == project_status::active, "P2 setup: the record starts active");
    auto [result_rec, report] = drive(pause_project(sup, rec));
    check(!report.all_ok(), "P2: a failing member's checkpoint is reported as a real failure");
    check(result_rec.status == project_status::active,
          "P2: FAIL CLOSED -- status is left UNCHANGED (still active), never falsely marked paused");
}

void test_p3_restore_pure_status_flip() {
    ProjectRecord rec = make_record("proj-restore");
    rec.status = project_status::paused;
    ProjectRecord restored = restore_project(rec);
    check(restored.status == project_status::active, "P3: restore_project flips status back to active");
    ProjectRecord expected_unchanged = rec;
    expected_unchanged.status = project_status::active;
    check(restored == expected_unchanged,
          "P3: restore_project changes ONLY status -- every other field is untouched");
}

void test_p4_archive_healthy() {
    AgentSession<OneShotChatClient> member;
    member.initialize("member-p4", Principal{"p1", ""});
    member.emplace_chat_client();
    (void)drive(member.start_run(StartRun{user_message("hi")}));

    InMemorySessionStore member_store;
    ProjectSupervisor sup;
    sup.register_member(member, member_store);

    ProjectRecord rec = make_record("proj-archive-ok");
    auto [archived, report] = drive(archive_project(sup, rec));
    check(report.all_ok(), "P4: checkpointing a healthy member during archive succeeds");
    check(archived.status == project_status::archived,
          "P4: a healthy archive_project() lands the record at archived (via paused first)");
    check(member_store.exists("member-p4"), "P4: the member's snapshot landed during the pause step");
}

void test_p5_archive_fail_closed() {
    AgentSession<OneShotChatClient> member;
    member.initialize("member-p5", Principal{"p1", ""});
    member.emplace_chat_client();
    (void)drive(member.start_run(StartRun{user_message("hi")}));

    AlwaysFailingStore failing_store;
    ProjectSupervisor sup;
    sup.register_member(member, failing_store);

    ProjectRecord rec = make_record("proj-archive-fail");
    auto [result_rec, report] = drive(archive_project(sup, rec));
    check(!report.all_ok(), "P5: a failing member's checkpoint is reported during archive too");
    check(result_rec.status == project_status::active,
          "P5: FAIL CLOSED -- archive_project() inherits pause's own rule, status stays active, "
          "never lands at paused OR archived");
}

}  // namespace

int main() {
    test_m1_round_trip_and_absent();
    test_m2_overwrite_latest();
    test_p1_pause_healthy();
    test_p2_pause_fail_closed();
    test_p3_restore_pure_status_flip();
    test_p4_archive_healthy();
    test_p5_archive_fail_closed();

    if (g_failures == 0) {
        std::fprintf(stderr, "All checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
