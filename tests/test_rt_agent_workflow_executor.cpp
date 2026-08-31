// OQ-19 (OpenQuestions.md; docs/planning/agent-as-workflow-executor-design-draft.md, red-teamed
// twice): proves the RUNTIME half of the agent-executor CapabilitySet bridge --
// rt/agent_workflow_executor.hpp's AgentExecutorBodyTag/agent_session_as_executor_body(), and the
// WorkflowSupervisor-side changes that make an `executor_kind::agent` node actually runnable
// (rt/workflow_supervisor.hpp). The GRAPH-layer half (Executor::capability_ceiling,
// check_workflow_executable(wf, contexts), the YAML compiler's refusal) is proven separately in
// tests/test_workflow_agent_executor_gate.cpp, which never links against rt:: at all. Covers:
//   T1 -- AgentExecutorBodyTag's positive control (design draft §5 item 4's own explicit ask): the
//         structural marker returns non-null for a REAL agent-backed body and null for an ordinary
//         function closure that merely happens to satisfy ExecutorBody's call signature.
//   T2 -- closes test_workflow_graph_validation.cpp's own stale-comment gap (design draft §3
//         RESIDUAL finding): a graph declaring an agent-kind node bound to a body that is NOT
//         structurally agent-backed makes WorkflowSupervisor::initialize() compute an invalid
//         supervisor, and run_workflow() reports workflow_status::invalid rather than silently
//         running the ordinary function as if it were the declared kind.
//   T3 -- a real end-to-end dispatch: a single agent-kind node, backed by a real (scripted)
//         AgentSession, produces the scripted response as the workflow's own selected output.
//   T4 -- capability-ceiling enforcement at initialize() time: a declared ceiling the caller's
//         granted CapabilitySet does not satisfy makes the supervisor invalid before any run starts.
//   T5 -- cyclic reuse WITHIN one run: an agent-kind node revisited across two rounds in the SAME
//         run accumulates real conversation history on the ONE AgentSession bound to it.
//   T6/T7 -- the concurrent-same-node duplicate-delivery quarantine (design draft §5 item 2): two
//         ordinary (non-fan_in) edges converging on the SAME agent-kind node in one round dispatch
//         the underlying AgentSession exactly ONCE (never a concurrent double-call -- the real hazard
//         this quarantine exists to prevent), with the SURVIVING delivery's result recorded either
//         way. T6 uses the default `fail` policy (the round fails cleanly, no hang/crash); T7 uses
//         `propagate` (the round completes normally, the duplicate's marker is silently absorbed by
//         the existing `deliver_once` same-round dedup -- "every OTHER delivery... completes
//         normally", the design draft's own central corrected-design claim, proven rather than
//         merely asserted).
//   T8 -- checkpoint/resume's documented, TESTED limitation (design draft §5 item 1): a resumed run's
//         agent-kind node gets a fresh, history-less AgentSession -- the caller-resupplied one bound
//         at the NEW WorkflowSupervisor's own initialize() call, never the pre-checkpoint session.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::AgentExecutorBodyTag;
using agentengine::rt::AgentSession;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::StartRun;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::rt::agent_session_as_executor_body;

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

// Drives an agentengine::rt::task<T> to completion. Safe here under the same "nothing genuinely
// suspends on an external, cross-thread wake" precondition test_rt_agent_session.cpp's own identical
// helper documents -- and, for T6/T7 specifically, under the ADDITIONAL guarantee
// rt/workflow_supervisor.hpp's own duplicate-delivery quarantine provides: this test never drives two
// concurrent deliveries into the same AgentSession, so session_mutex_ is never contended.
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// -- The scripted backend (same shape as test_rt_agent_session.cpp's own fixture) -------------------
struct ScriptedOutcome {
    Message message;
    Usage   usage;
};

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

using ScriptedSession = AgentSession<ScriptedChatClient>;

[[nodiscard]] Message text_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// agentengine::text_of() (core/tool_call_extraction.hpp, transitively included) already concatenates
// a Message's Text content items -- reused directly rather than shadowed with a local duplicate.

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in,
                                    EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ">" + name)};
    };
}

[[nodiscard]] Executor func_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}
[[nodiscard]] Executor agent_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::agent, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::request_port, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

}  // namespace

int main() {
    // ---- T1: AgentExecutorBodyTag's positive control ----------------------------------------------
    {
        ScriptedSession session;
        session.initialize("t1", Principal{"p1", ""});

        ExecutorBody agent_body = agent_session_as_executor_body(session);
        check(agent_body.target<AgentExecutorBodyTag>() != nullptr,
              "T1: a genuinely agent-backed body IS detected via std::function::target<>()");

        ExecutorBody plain_body = [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{in};
        };
        check(plain_body.target<AgentExecutorBodyTag>() == nullptr,
              "T1: an ordinary function body is NOT mistaken for an agent-backed one");
    }

    // ---- T2: an agent-kind node bound to a NON-agent-backed body makes the supervisor invalid -----
    {
        Workflow wf;
        wf.id = "t2";
        wf.executors = {agent_desc("a")};
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 1;

        WorkflowSupervisor sup;
        sup.initialize(wf, {appender("not-really-an-agent")});  // ExecutorBody, but no tag

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
        check(r.status == workflow_status::invalid,
              "T2: a mis-bound agent-kind node refuses to run rather than silently running as a "
              "plain function (closes test_workflow_graph_validation.cpp's stale-comment gap)");
    }

    // ---- T3: a real end-to-end dispatch through a scripted AgentSession ---------------------------
    {
        ScriptedSession session;
        session.initialize("t3", Principal{"p3", ""});
        session.emplace_chat_client().set_script(
            {{text_message("agent-said-hello"), Usage{1, 1, 0, 0, 0.0}}});

        Workflow wf;
        wf.id = "t3";
        wf.executors = {agent_desc("a")};
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 1;

        WorkflowSupervisor sup;
        sup.initialize(wf, {agent_session_as_executor_body(session)});

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
        check(r.status == workflow_status::completed, "T3: the run completes");
        check(text_of(r.output) == "agent-said-hello",
              "T3: the workflow's output is the real AgentSession's own scripted response");
    }

    // ---- T4: a declared ceiling the granted CapabilitySet does not satisfy -> invalid -------------
    {
        ScriptedSession session;
        session.initialize("t4", Principal{"p4", ""});
        session.emplace_chat_client().set_script({{text_message("x"), Usage{}}});

        Workflow wf;
        wf.id = "t4";
        Executor a = agent_desc("a");
        a.capability_ceiling = {
            cap::FsRead{.mount_id = "work", .path_prefix = "", .size_cap_bytes = std::nullopt}};
        wf.executors = {a};
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 1;

        WorkflowSupervisor sup;
        // No contexts supplied -- contexts_[0].capabilities defaults null, which does not satisfy
        // the declared ceiling.
        sup.initialize(wf, {agent_session_as_executor_body(session)});

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
        check(r.status == workflow_status::invalid,
              "T4: an unsatisfied capability_ceiling refuses the run before it ever starts");
    }

    // ---- T5: cyclic reuse within ONE run accumulates real conversation history --------------------
    {
        ScriptedSession session;
        session.initialize("t5", Principal{"p5", ""});
        session.emplace_chat_client().set_script({
            {text_message("round-one"), Usage{}},
            {text_message("round-two"), Usage{}},
        });

        Workflow wf;
        wf.id = "t5";
        wf.executors = {agent_desc("a"), func_desc("bounce")};
        wf.edges.push_back(Edge{"a", "bounce", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"bounce", "a", edge_kind::direct, {}});
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 3;  // a -> bounce -> a -> bound

        WorkflowSupervisor sup;
        sup.initialize(wf, {agent_session_as_executor_body(session), appender("bounce")});

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
        check(r.status == workflow_status::bound_max_rounds, "T5: the round bound stops the cycle");
        check(session.history().size() == 4,
              "T5: the ONE AgentSession bound to 'a' was genuinely called twice across two rounds of "
              "the SAME run -- history holds both exchanges (2 messages each), proving real reuse, "
              "not a fresh session per visit");
    }

    // ---- T6: duplicate-delivery quarantine, default `fail` policy -- no hang/crash, exactly one ---
    // ---- real dispatch, the surviving delivery's result is still recorded. ------------------------
    {
        ScriptedSession session;
        session.initialize("t6", Principal{"p6", ""});
        session.emplace_chat_client().set_script({{text_message("survivor"), Usage{}}});

        Workflow wf;
        wf.id = "t6";
        wf.executors = {func_desc("start"), func_desc("src1"), func_desc("src2"), agent_desc("ag")};
        wf.edges.push_back(Edge{"start", "src1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "src2", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src1", "ag", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"src2", "ag", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("ag");
        wf.bound.max_rounds = 5;

        WorkflowSupervisor sup;
        sup.initialize(wf, {appender("start"), appender("src1"), appender("src2"),
                            agent_session_as_executor_body(session)});

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed,
              "T6: the quarantined duplicate fails the round through the ORDINARY executor-failure "
              "path (default `fail` policy) -- not a hang, not a crash, not a whole-round abort "
              "invented just for this case");
        check(r.failed_executor == "ag", "T6: the failed executor is correctly identified");
        check(session.history().size() == 2,
              "T6: the AgentSession was genuinely dispatched exactly ONCE this round, not twice -- "
              "the quarantine prevented the concurrent second call before it ever reached "
              "start_run(), which is what makes this test able to finish at all rather than hang");
    }

    // ---- T7: duplicate-delivery quarantine, `propagate` policy -- the round completes normally, ---
    // ---- and the surviving real payload (not a failure marker) reaches the downstream sink. -------
    {
        ScriptedSession session;
        session.initialize("t7", Principal{"p7", ""});
        session.emplace_chat_client().set_script({{text_message("survivor-2"), Usage{}}});

        Workflow wf;
        wf.id = "t7";
        wf.executors = {func_desc("start"), func_desc("src1"), func_desc("src2"), agent_desc("ag"),
                        func_desc("sink")};
        wf.edges.push_back(Edge{"start", "src1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "src2", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"src1", "ag", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"src2", "ag", edge_kind::direct, {}});
        Edge ag_to_sink{"ag", "sink", edge_kind::direct, {}};
        ag_to_sink.on_failure.kind = edge_failure_policy::propagate;
        wf.edges.push_back(ag_to_sink);
        wf.start = "start";
        wf.output_selection.push_back("sink");
        wf.bound.max_rounds = 5;

        WorkflowSupervisor sup;
        sup.initialize(wf, {appender("start"), appender("src1"), appender("src2"),
                            agent_session_as_executor_body(session), appender("sink")});

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::completed,
              "T7: with `propagate` on the surviving node's own outgoing edge, the round completes "
              "normally -- the quarantine does not force a whole-round failure regardless of policy");
        check(text_of(r.output) == "survivor-2>sink",
              "T7: 'sink' received the REAL survivor payload, not a failure marker -- the quarantined "
              "duplicate's marker was silently absorbed by the existing same-round deliver_once() "
              "dedup, exactly as the design draft's 'every OTHER delivery completes normally' claim "
              "requires");
        check(session.history().size() == 2,
              "T7: here too, the AgentSession was dispatched exactly once this round");
    }

    // ---- T8: checkpoint/resume's documented, TESTED fresh-session limitation ----------------------
    {
        Workflow wf;
        wf.id = "t8";
        wf.executors = {agent_desc("a"), port_desc("p")};
        wf.edges.push_back(Edge{"a", "p", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"p", "a", edge_kind::direct, {}});
        wf.start = "a";
        wf.output_selection.push_back("a");
        wf.bound.max_rounds = 6;

        ScriptedSession session1;
        session1.initialize("t8-session-1", Principal{"p8a", ""});
        session1.emplace_chat_client().set_script({{text_message("pre-checkpoint"), Usage{}}});

        WorkflowSupervisor sup1;
        sup1.initialize(wf, {agent_session_as_executor_body(session1), ExecutorBody{}});
        WorkflowResult r1 = drive(sup1.run_workflow(RunWorkflow{text_message("go")}));
        check(r1.status == workflow_status::suspended,
              "T8 setup: the run suspends at the request_port after 'a' ran once");
        check(session1.history().size() == 2, "T8 setup: session1 saw exactly one exchange");

        RunStateRecord rec = sup1.to_record();
        check(!r1.open_interactions.empty(), "T8 setup: a real open interaction exists to resume");
        std::string const interaction_id =
            r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;

        // A genuinely NEW WorkflowSupervisor, with a genuinely NEW, fresh, history-less
        // AgentSession bound to the same graph node -- the caller's own responsibility per this
        // adapter's own CALLER CONTRACT (rt/agent_workflow_executor.hpp), not something
        // restore_from_record() does for you.
        ScriptedSession session2;
        session2.initialize("t8-session-2", Principal{"p8b", ""});
        session2.emplace_chat_client().set_script({{text_message("post-checkpoint"), Usage{}}});

        WorkflowSupervisor sup2;
        sup2.initialize(wf, {agent_session_as_executor_body(session2), ExecutorBody{}});
        sup2.restore_from_record(rec);

        WorkflowResult r2 =
            drive(sup2.resume_workflow(ResumeWorkflow{interaction_id, text_message("resumed"), {}}));
        check(r2.status != workflow_status::invalid, "T8: the resume itself is accepted");
        check(session2.history().size() == 2,
              "T8: session2's history holds ONLY the post-resume exchange (2 messages) -- the "
              "pre-checkpoint conversation on session1 did NOT carry over, because bodies_ (and the "
              "AgentSession each one closes over) is caller-resupplied fresh at initialize(), never "
              "reconnected by restore_from_record()");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_workflow_executor: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_workflow_executor: %d FAILURE(S)\n", g_failures);
    return 1;
}
