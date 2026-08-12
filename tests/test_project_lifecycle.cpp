// Implements 030-Project-Workspace-and-Lifecycle.md §4 (directed lifecycle) and §8 Q4 (pause
// extends to workflow-supervising actors too). Milestone 6 Phase I
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// I1 -- the real point of PassivatableHandle: ONE ProjectSupervisor holds a REAL
// AgentSession<CannedChatClient> (a member session) and a REAL WorkflowSupervisor (a
// workflow-supervising actor hosted under a session, §8 Q4's own case) -- two genuinely different
// C++ actor types -- and passivates BOTH uniformly. This is not a hypothetical: 030 §2's own model
// allows a Project's members to differ in ChatClientT, and nothing before this file has ever
// exercised holding two different concrete actor types under one caller-side list.
// I2 -- 030 §7 G1's own bar, one Project: pausing drops ALL THREE activations (session, workflow,
// the ProjectSupervisor itself, passivated LAST per §4's ordering) to Dormant, by census.
// I3 -- restore reads the manifest without touching anything live (030 §4: "does not need to
// eagerly reactivate").
// I4 -- resume is invisible to the run: a real Run issued against the (Dormant) member session
// after restore produces the same kind of result an uninterrupted session would -- 030 §4's own
// "the pause/restore cycle is invisible to the run itself," applied for the first time to a
// session reached THROUGH a Project rather than directly (test_agent_session_suspend_resume.cpp's
// own precedent, one layer up).
// I5 -- archive flips status to Archived via the same pause sequence, never eagerly reclaiming
// anything (030 §8 Q2).
//
// MACHINE SAFETY (CLAUDE.md): 1 worker / 1 shard (mirrors test_agent_session_suspend_resume.cpp's
// own Engine sizing), no sleeps beyond a short bounded census wait.

#include <chrono>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/project/lifecycle.hpp"
#include "agentengine/project/project.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/placement.hpp"
#include "agentengine/workflow/supervisor.hpp"

using namespace quark;
using namespace agentengine;
using namespace agentengine::workflow;

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class Pred>
[[nodiscard]] bool wait_until(Pred p, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::yield();
    }
    return p();
}

// Mirrors test_agent_session_checkpoint.cpp's own CannedChatClient exactly.
class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"ok"};
        item.origin = content_origin::assistant;
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = Text{"ok"};
        upd.is_final     = true;
        upd.usage        = Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<CannedChatClient>, "CannedChatClient must satisfy the ChatClient concept");

using Session = AgentSession<CannedChatClient>;

[[nodiscard]] Message user_turn(std::string text, std::string message_id) {
    ContentItem item{};
    item.value  = Text{std::move(text)};
    item.origin = content_origin::user;
    Message m{};
    m.role       = role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> ae::result<ExecutorOutcome> {
        std::string text;
        for (auto const& item : in.content) {
            if (auto const* t = std::get_if<Text>(&item.value)) text = t->text;
        }
        Message out;
        out.role = role::assistant;
        ContentItem oi{};
        oi.value = Text{text + ">" + name};
        out.content.push_back(oi);
        return out;
    };
}

}  // namespace

int main() {
    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    if (!built) {
        std::fprintf(stderr, "engine config failed\n");
        return 1;
    }

    Engine<>                   eng(*built);
    quark::detail::MessagePool pool(64);
    LocalRouter                router(eng.post_courier(), pool);

    // ---- One member session (a REAL AgentSession<CannedChatClient>) -------------------------------
    Session session_actor;
    session_actor.initialize("s-member-1", ae::Principal{"p-nora", ""});
    Activation session_act{&session_actor, Session::dispatch_table(), pool.sink()};
    eng.register_activation(actor_id_of<Session>(1), session_act);
    ActorRef<Session> session_ref = router.get<Session>(1);

    // ---- One workflow-supervising actor (a REAL WorkflowSupervisor, §8 Q4's own case) -------------
    Workflow wf;
    wf.id                = "lifecycle-wf";
    wf.executors         = {Executor{"n0", executor_kind::function, "T", "T"}};
    wf.start              = "n0";
    wf.output_selection   = {"n0"};
    wf.bound.max_rounds   = 4;

    auto node = std::make_unique<FunctionExecutor>();
    node->initialize("n0", appender("n0"), EffectContext{});
    auto node_keys = spread_executor_keys(1, eng.shard_count(), [&](std::uint64_t k) {
        return eng.shard_of(actor_id_of<FunctionExecutor>(k));
    });
    auto node_act = make_workflow_activation(*node, pool.sink());
    eng.register_activation(actor_id_of<FunctionExecutor>(node_keys[0]), *node_act);

    WorkflowSupervisor workflow_actor;
    workflow_actor.initialize(wf, {router.get<FunctionExecutor>(node_keys[0])});
    auto workflow_act = make_workflow_activation(workflow_actor, pool.sink());
    eng.register_activation(actor_id_of<WorkflowSupervisor>(1), *workflow_act);
    ActorRef<WorkflowSupervisor> workflow_ref = router.get<WorkflowSupervisor>(1);

    // ---- The Project's own supervising actor -------------------------------------------------------
    ProjectSupervisor project_actor;
    Activation project_act{&project_actor, ProjectSupervisor::dispatch_table(), pool.sink()};
    eng.register_activation(actor_id_of<ProjectSupervisor>(1), project_act);
    ActorRef<ProjectSupervisor> project_ref = router.get<ProjectSupervisor>(1);

    eng.start();

    // =========================================================================================
    // I1 -- register two GENUINELY DIFFERENT actor types on the same ProjectSupervisor.
    // =========================================================================================
    project_actor.register_member(session_ref);
    project_actor.register_workflow(workflow_ref);
    check(project_actor.member_count() == 1 && project_actor.workflow_count() == 1,
          "I1: the ProjectSupervisor holds one member session and one workflow supervisor -- "
          "two different C++ actor types, registered through the SAME PassivatableHandle API");

    // ---- Real work against both, before pausing -----------------------------------------------
    auto session_r1 = block_on(session_ref.ask<AgentResponse>(StartRun{user_turn("hi", "m-1")}));
    check(session_r1.has_value(), "I1 setup: a real turn against the member session succeeds");
    auto workflow_r1 =
        block_on(workflow_ref.ask<WorkflowResult>(RunWorkflow{user_turn("in", "m-w1")}));
    check(workflow_r1.has_value() && workflow_r1->status == workflow_status::completed,
          "I1 setup: a real run against the workflow supervisor completes");

    // =========================================================================================
    // I2 -- 030 §7 G1's own bar: pausing drops ALL THREE activations to Dormant, by census. Order
    // per the breakdown doc's own Phase I text: member sessions, then workflow supervisors, then
    // the Project's own actor, LAST.
    // =========================================================================================
    ProjectRecord rec;
    rec.project_id   = "proj-lifecycle";
    rec.principal_id = "p-nora";
    rec.status        = project_status::active;

    ProjectRecord paused = pause_project(project_ref, project_actor, rec);
    check(paused.status == project_status::paused, "I2: pause_project returns the manifest marked Paused");

    check(wait_until([&] { return session_act.went_dormant(); }, std::chrono::seconds(2)),
          "I2: the member session's activation reaches Dormant");
    check(wait_until([&] { return workflow_act->went_dormant(); }, std::chrono::seconds(2)),
          "I2: the workflow supervisor's activation reaches Dormant");
    check(wait_until([&] { return project_act.went_dormant(); }, std::chrono::seconds(2)),
          "I2: the Project's OWN activation reaches Dormant too -- passivated last, per §4's "
          "ordering, not left live while its children are torn down");

    // =========================================================================================
    // I3 -- restore reads the manifest without eagerly touching anything live. Nothing above is
    // reactivated by this call -- there is nothing here TO reactivate; restore_project only ever
    // touches the plain ProjectRecord value.
    // =========================================================================================
    ProjectRecord restored = restore_project(paused);
    check(restored.status == project_status::active,
          "I3: restore_project flips the manifest back to Active");
    check(session_act.went_dormant() && workflow_act->went_dormant() && project_act.went_dormant(),
          "I3: restoring did NOT reactivate a single one of the three activations -- 030 §4's own "
          "'does not need to eagerly reactivate every member session'");

    // =========================================================================================
    // I4 -- resume is invisible to the run: issuing a REAL Run against the (still Dormant) member
    // session after restore reactivates it on demand and produces an ordinary result -- the same
    // claim test_agent_session_suspend_resume.cpp already proves for a session paused directly,
    // now proven for one paused THROUGH a Project.
    // =========================================================================================
    auto session_r2 = block_on(session_ref.ask<AgentResponse>(StartRun{user_turn("again", "m-2")}));
    check(session_r2.has_value() && session_actor.last_run_id() == "s-member-1:run:2",
          "I4: a Run against the member session after restore reactivates it and continues the "
          "SAME run sequence -- the pause/restore cycle left no trace on the run itself");

    // =========================================================================================
    // I5 -- archive: pause (same sequence, re-proven cheaply since everything is already Dormant --
    // passivate() on an already-Dormant/no-longer-live id just returns false, harmlessly) then flip
    // to Archived. No retention/GC call anywhere -- 030 §8 Q2's 'archived means hidden, not shrunk'.
    // =========================================================================================
    ProjectRecord archived = archive_project(project_ref, project_actor, restored);
    check(archived.status == project_status::archived,
          "I5: archive_project marks the manifest Archived, via the same pause sequence");

    eng.stop();

    if (g_failures == 0) {
        std::printf("test_project_lifecycle: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_project_lifecycle: %d failure(s)\n", g_failures);
    return 1;
}
