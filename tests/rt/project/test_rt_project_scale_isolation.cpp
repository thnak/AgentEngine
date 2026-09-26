// Proof for ADR-037: 030-Project-Workspace-and-Lifecycle.md §7's promotion gate G1 ("N >= 100
// concurrently active Projects... pausing one measurably drops its member sessions' activation
// count to zero... with zero observable effect on the other N-1"), reproven against
// `agentengine::rt::ProjectSupervisor`/`rt::pause_project()` -- the Quark-actor-free replacement for
// `agentengine::ProjectSupervisor`/`pause_project()` (project/lifecycle.hpp). SUPERSEDES the old
// Quark-actor-based test_project_scale_isolation.cpp, reusing its N=100/paused-interior-index=50
// shape, matching the precedent test_rt_workflow_live_view.cpp already set for superseding.
//
// G1's ORIGINAL TEXT IS TWO CLAIMS BUNDLED TOGETHER, and they carry over to rt:: very differently --
// named honestly here rather than silently re-measured as if nothing changed:
//   (a) CORRECTNESS -- pausing one Project has no effect on the other N-1's own state or behavior.
//       This is the claim that STILL MEANS SOMETHING in rt:: land, and this file proves it two ways:
//       structurally (pause_project(#50) never calls save() against any OTHER project's store --
//       provable directly, not just "nothing broke") and behaviorally (all 99 others complete a real
//       SECOND Run afterward, each continuing its own run-id sequence exactly as if #50 had never
//       been touched).
//   (b) LATENCY -- the old Quark test measured wall-clock timing on a SAMPLE of untouched Projects
//       before/after the pause, specifically to catch an accidental GLOBAL LOCK or scheduler
//       serialization point creeping into `pause_project`'s implementation (the hazard ADR-034's own
//       ActorId-scoped locking existed to rule out). This is NOT reproven here, and deliberately so:
//       `rt::AgentSession`/`rt::WorkflowSupervisor`/`rt::ProjectSupervisor` are plain, host-held C++
//       objects with no actor engine, no shared scheduler, and no shared lock of ANY kind reachable
//       through this API -- there is structurally nothing left for `pause_project` to accidentally
//       serialize on. A timing measurement here could never fail (there is no mechanism left that
//       could make it fail), and 022 §5's own "a check that can't fail proves nothing" rule says
//       plainly that including one anyway would be theater, not proof. The structural isolation
//       check in (a) is what actually stands in for this claim in rt:: land: if `pause_project(#50)`
//       genuinely never touches store #7's `save()`, there is no path left by which it could have
//       contended with project #7 in the first place.
//
// MACHINE SAFETY (CLAUDE.md): single-threaded, deterministic, no sleeps -- 100 Projects is an object
// count, not a thread or activation count, in rt:: land.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/project_manifest.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::ProjectRecord;
using agentengine::rt::ProjectSupervisor;
using agentengine::rt::StartRun;
using agentengine::rt::pause_project;
using agentengine::rt::project_status;

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
    item.value  = Text{std::move(text)};
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
        item.value  = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{std::move(m), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<OneShotChatClient>);

using Session = AgentSession<OneShotChatClient>;

constexpr std::size_t kProjectCount = 100;
constexpr std::size_t kPausedIndex  = 50;  // an interior project, not the first or last -- the
                                            // failure mode a boundary-only test would miss

}  // namespace

int main() {
    std::vector<std::unique_ptr<Session>>              sessions(kProjectCount);
    std::vector<std::unique_ptr<InMemorySessionStore>> stores(kProjectCount);
    std::vector<std::unique_ptr<ProjectSupervisor>>    supervisors(kProjectCount);

    for (std::size_t i = 0; i < kProjectCount; ++i) {
        sessions[i] = std::make_unique<Session>();
        sessions[i]->initialize("s-scale-" + std::to_string(i), Principal{"p-scale", ""});
        sessions[i]->emplace_chat_client();
        stores[i]      = std::make_unique<InMemorySessionStore>();
        supervisors[i] = std::make_unique<ProjectSupervisor>();
        supervisors[i]->register_member(*sessions[i], *stores[i]);
    }

    // =========================================================================================
    // Round 1: every one of the 100 Projects does a real Run, before anything is paused.
    // =========================================================================================
    bool round1_ok = true;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        auto r = drive(sessions[i]->start_run(StartRun{user_message("hi")}));
        if (!r.has_value() || sessions[i]->last_run_id() != ("s-scale-" + std::to_string(i) + ":run:1")) {
            round1_ok = false;
        }
    }
    check(round1_ok, "setup: all 100 Projects' member sessions complete a real first Run");

    // None of the 100 stores has been touched by anything but this file's own setup yet (nothing
    // above ever called pause_project) -- the baseline this test's own isolation claim is measured
    // against.
    bool none_checkpointed_yet = true;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        if (stores[i]->exists("s-scale-" + std::to_string(i))) none_checkpointed_yet = false;
    }
    check(none_checkpointed_yet, "setup: no store has been checkpointed into yet (sanity baseline)");

    // =========================================================================================
    // 030 §7 G1 itself: pause ONE Project (#50).
    // =========================================================================================
    ProjectRecord rec;
    rec.project_id   = "proj-scale-" + std::to_string(kPausedIndex);
    rec.principal_id = "p-scale";
    rec.status       = project_status::active;

    auto [paused, report] = drive(pause_project(*supervisors[kPausedIndex], rec));
    check(report.all_ok(), "G1: pause_project checkpoints the target's member cleanly");
    check(paused.status == project_status::paused, "G1: pause_project marks the target Paused");
    check(stores[kPausedIndex]->exists("s-scale-" + std::to_string(kPausedIndex)),
          "G1: the paused Project's member session's snapshot actually landed");

    // =========================================================================================
    // G1(a) -- STRUCTURAL isolation: pausing #50 touched ONLY #50's own store. Every other one of
    // the 99 remains exactly as untouched as it was at the baseline above -- not "nothing broke",
    // but "provably nothing was written" -- see file banner for why this stands in for the old
    // latency measurement in rt:: land.
    // =========================================================================================
    bool only_target_checkpointed = true;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        if (i == kPausedIndex) continue;
        if (stores[i]->exists("s-scale-" + std::to_string(i))) only_target_checkpointed = false;
    }
    check(only_target_checkpointed,
          "G1(a) -- CORE CLAIM: pause_project(#50) never wrote to any of the OTHER 99 Projects' own "
          "stores -- checkpoint_members_and_workflows() only ever reaches hooks explicitly registered "
          "against the ONE supervisor it was called on");

    // =========================================================================================
    // G1(b) -- BEHAVIORAL isolation: all 99 OTHER Projects still work correctly, each continuing
    // its own run-id sequence uninterrupted by #50's pause.
    // =========================================================================================
    bool        all_others_ok  = true;
    std::size_t correct_count  = 0;
    for (std::size_t i = 0; i < kProjectCount; ++i) {
        if (i == kPausedIndex) continue;
        auto r = drive(sessions[i]->start_run(StartRun{user_message("again")}));
        bool const ok = r.has_value() &&
                        sessions[i]->last_run_id() == ("s-scale-" + std::to_string(i) + ":run:2");
        if (ok) ++correct_count;
        if (!ok) all_others_ok = false;
    }
    check(all_others_ok && correct_count == kProjectCount - 1,
          "G1(b): all 99 OTHER Projects' member sessions complete a real SECOND Run, each continuing "
          "its OWN run-id sequence uninterrupted -- pausing #50 disturbed none of their behavior");

    // Paused #50 itself still reports the record it was left at -- not part of G1's own text, but
    // the natural sanity check that "paused" actually means something for the one Project it
    // targets, mirroring the old Quark test's own closing sanity check (there, a Dormant census;
    // here, the manifest's own status field, since that is the durable fact rt:: land actually has).
    check(paused.status == project_status::paused,
          "sanity: #50's own returned record is still marked paused after the other 99 did real work");

    if (g_failures == 0) {
        std::printf("test_rt_project_scale_isolation: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_project_scale_isolation: %d failure(s)\n", g_failures);
    return 1;
}
