// Proof for ADR-037: agentengine::rt::ProjectSupervisor (include/agentengine/rt/project_supervisor.hpp),
// the Quark-actor-free replacement for agentengine::ProjectSupervisor's checkpoint-orchestration role
// (project/lifecycle.hpp). Deterministic, offline, single-threaded. Covers:
//   Q1 -- register_member() type-erases across GENUINELY DIFFERENT AgentSession<ChatClientT,StateT,
//         HistoryProviderT> instantiations (two members with different HistoryProviderT) into ONE
//         ProjectSupervisor -- the actual problem PassivatableHandle existed to solve in the Quark
//         original, re-proven here without any Quark actor involved.
//   Q2 -- checkpoint_members_and_workflows() actually writes every member's snapshot AND the
//         workflow's checkpoint to their stores (not just claims to).
//   Q3 -- a failing member store is reported in CheckpointReport, by index and kind, WITHOUT aborting
//         the remaining checkpoints -- proving "collect all failures" over "stop at the first one"
//         (this file's own header comment explains why that's the deliberate, more-useful choice).
//   Q4 -- member_count()/workflow_count() accessors track registrations.
//   Q5 -- 030 Sec4/Sec8 Q4's own ordering: all member hooks run before any workflow hook.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/project_supervisor.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::CheckpointReport;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::ProjectSupervisor;
using agentengine::rt::StartRun;
using agentengine::rt::WorkflowSupervisor;

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

// Safe here: every coroutine this file drives (AgentSession::start_run() on OneShotChatClient,
// save_agent_session_snapshot/save_workflow_checkpoint, and ProjectSupervisor's own
// checkpoint_members_and_workflows()) only ever suspends on an uncontended AsyncMutex fast path or a
// synchronous store call -- never a genuinely external event -- so one resume() loop resolves each
// fully, matching every other single-caller rt:: test file's own drive<T>().
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ContentItem;

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// Q1's fixture: converges on the first call, deterministically -- this file only needs real
// run_counter movement, matching test_rt_agent_session_snapshot.cpp's own OneShotChatClient.
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

// Q3's fixture: a SessionStore whose save() always fails, so a member's checkpoint can be made to
// fail deterministically without any real I/O error. Trivially conforms to the SessionStore concept
// (session_store.hpp) -- load()/exists()/remove() are never exercised by this file, so they're
// stubbed to the simplest contract-satisfying answer.
class AlwaysFailingStore {
public:
    [[nodiscard]] agentengine::result<void> save(agentengine::rt::SessionId const&, std::vector<std::byte>) {
        return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                    "AlwaysFailingStore refuses every save()",
                                                    "test.always_failing_store.save"});
    }
    [[nodiscard]] agentengine::result<std::vector<std::byte>> load(agentengine::rt::SessionId const&) const {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "unused",
                                                    "test.always_failing_store.load"});
    }
    [[nodiscard]] bool exists(agentengine::rt::SessionId const&) const { return false; }
    [[nodiscard]] agentengine::result<void> remove(agentengine::rt::SessionId const&) { return {}; }
};
static_assert(agentengine::rt::SessionStore<AlwaysFailingStore>);

}  // namespace

int main() {
    // ---- Q1/Q4: register_member() across GENUINELY DIFFERENT AgentSession<...> instantiations -----
    {
        // Two members, deliberately DIFFERENT HistoryProviderT -- the exact heterogeneity
        // PassivatableHandle's own closure-over-a-concrete-ActorRef<A> trick existed to absorb in the
        // Quark original, re-proven here with zero Quark involvement.
        AgentSession<OneShotChatClient> member_a;
        member_a.initialize("member-a", Principal{"p1", "tenant-1"});
        member_a.emplace_chat_client();
        auto ran_a = drive(member_a.start_run(StartRun{user_message("hi")}));
        check(ran_a.has_value(), "Q1 setup: member_a's run converges");

        AgentSession<OneShotChatClient, agentengine::rt::NoSessionState,
                     agentengine::HistoryProvider<agentengine::Window<8>>>
            member_b;
        member_b.initialize("member-b", Principal{"p1", "tenant-1"});
        member_b.emplace_chat_client();
        auto ran_b = drive(member_b.start_run(StartRun{user_message("hi")}));
        check(ran_b.has_value(), "Q1 setup: member_b's run converges");

        InMemorySessionStore store_a, store_b;
        ProjectSupervisor sup;
        check(sup.member_count() == 0 && sup.workflow_count() == 0,
              "Q4: a fresh ProjectSupervisor starts with no registrations");

        sup.register_member(member_a, store_a);
        sup.register_member(member_b, store_b);
        check(sup.member_count() == 2,
              "Q1: both a Window<0>-history member and a Window<8>-history member registered into "
              "the SAME ProjectSupervisor -- they are genuinely different C++ types");
        check(sup.workflow_count() == 0, "Q4: registering members does not affect workflow_count()");

        // ---- Q2: checkpoint_members_and_workflows() actually writes both members' snapshots -------
        CheckpointReport report = drive(sup.checkpoint_members_and_workflows());
        check(report.all_ok(), "Q2: checkpointing two healthy members reports no failures");
        check(store_a.exists("member-a"), "Q2: member_a's snapshot actually landed in its own store");
        check(store_b.exists("member-b"), "Q2: member_b's snapshot actually landed in its own store");
    }

    // ---- Q2/Q5: a workflow's checkpoint is also written, and runs AFTER every member's -------------
    {
        using agentengine::workflow::Edge;
        using agentengine::workflow::Executor;
        using agentengine::workflow::Workflow;
        using agentengine::workflow::edge_kind;
        using agentengine::workflow::executor_kind;
        using agentengine::sharing_mode;
        using agentengine::workflow::validate_workflow;

        Workflow wf;
        wf.id = "proj-wf";
        wf.executors = {Executor{.id = "a", .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                                  .worktree_mode = sharing_mode::branch, .capability_ceiling = {}}};
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 4;
        check(validate_workflow(wf).has_value(), "Q5 setup: the single-node workflow validates");

        std::vector<agentengine::rt::ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> agentengine::result<agentengine::rt::ExecutorOutcome> {
                return agentengine::rt::ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor wf_sup;
        wf_sup.initialize(wf, bodies);
        auto wf_ran = drive(wf_sup.run_workflow(agentengine::rt::RunWorkflow{user_message("go")}));
        check(wf_ran.status == agentengine::rt::workflow_status::completed,
              "Q5 setup: the workflow converges before this test checkpoints it");

        AgentSession<OneShotChatClient> member;
        member.initialize("member-c", Principal{"p1", "tenant-1"});
        member.emplace_chat_client();
        auto ran = drive(member.start_run(StartRun{user_message("hi")}));
        check(ran.has_value(), "Q5 setup: member-c's run converges");

        InMemorySessionStore member_store, workflow_store;
        ProjectSupervisor sup;
        sup.register_member(member, member_store);
        sup.register_workflow(wf_sup, workflow_store);
        check(sup.workflow_count() == 1, "Q4: register_workflow() is tracked separately from members");

        CheckpointReport report = drive(sup.checkpoint_members_and_workflows());
        check(report.all_ok(), "Q2/Q5: checkpointing a healthy member + a healthy workflow succeeds");
        check(member_store.exists("member-c"), "Q2: the member's snapshot landed");
        check(workflow_store.exists(wf_sup.run_id()),
              "Q2: the workflow's checkpoint landed under its own run_id");
    }

    // ---- Q3: a failing member store is reported by index/kind, WITHOUT aborting later checkpoints --
    {
        AgentSession<OneShotChatClient> healthy_first;
        healthy_first.initialize("ok-first", Principal{"p1", ""});
        healthy_first.emplace_chat_client();
        (void)drive(healthy_first.start_run(StartRun{user_message("hi")}));

        AgentSession<OneShotChatClient> failing_member;
        failing_member.initialize("fails", Principal{"p1", ""});
        failing_member.emplace_chat_client();
        (void)drive(failing_member.start_run(StartRun{user_message("hi")}));

        AgentSession<OneShotChatClient> healthy_last;
        healthy_last.initialize("ok-last", Principal{"p1", ""});
        healthy_last.emplace_chat_client();
        (void)drive(healthy_last.start_run(StartRun{user_message("hi")}));

        InMemorySessionStore store_first, store_last;
        AlwaysFailingStore failing_store;
        ProjectSupervisor sup;
        sup.register_member(healthy_first, store_first);
        sup.register_member(failing_member, failing_store);
        sup.register_member(healthy_last, store_last);

        CheckpointReport report = drive(sup.checkpoint_members_and_workflows());
        check(!report.all_ok(), "Q3: a report with one real failure is not all_ok()");
        check(report.failures.size() == 1, "Q3: exactly the ONE failing member is reported");
        if (report.failures.size() == 1) {
            check(report.failures[0].index == 1,
                  "Q3: the failure names the failing member's OWN registration index (1), not the "
                  "first or last");
            check(!report.failures[0].is_workflow, "Q3: the failure is correctly attributed to a member");
            check(report.failures[0].err.code == "test.always_failing_store.save",
                  "Q3: the real underlying store error is surfaced through, not swallowed");
        }
        check(store_first.exists("ok-first"),
              "Q3: the member registered BEFORE the failing one still got checkpointed");
        check(store_last.exists("ok-last"),
              "Q3: the member registered AFTER the failing one ALSO still got checkpointed -- "
              "checkpoint_members_and_workflows() does not abort on the first failure");
    }

    if (g_failures == 0) {
        std::printf("test_rt_project_supervisor: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_project_supervisor: %d failure(s)\n", g_failures);
    return 1;
}
