// ADR-213 (GitHub issue #28 item 6): WorkflowSupervisor::enable_transcript() records EVERY executor visit in
// completion order in WorkflowResult::transcript, where `partial` keeps only each executor's latest.
//   T1 -- off by default: transcript stays empty and costs nothing.
//   T2 -- on: a cyclic Magentic run (mgr -> p1 -> mgr -> p2 -> mgr -> done) yields all six visits in order,
//          while `partial` still holds only the four distinct executors.
//   T3 -- the cap: enable_transcript(3) keeps the first three visits and sets transcript_truncated.
//   T4 -- a fresh run starts a fresh transcript (no carry-over from the previous run).

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/magentic.hpp"

using agentengine::Message;
using agentengine::workflow::MagenticGraph;
using agentengine::workflow::MagenticWorkflowBuilder;
using agentengine::workflow::TypedExecutor;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

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

[[nodiscard]] Message text_message(std::string text) {
    agentengine::ContentItem item{};
    item.origin = agentengine::content_origin::user;
    item.value  = agentengine::Text{std::move(text)};
    Message m{};
    m.role = agentengine::role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<agentengine::Text>(&item.value)) return t->text;
    }
    return {};
}

}  // namespace

struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Magentic.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Magentic.ReportMsg");

namespace {

struct Fixture {
    MagenticGraph graph;
    std::atomic<int> step{0};
    std::vector<ExecutorBody> bodies;
};

void make(Fixture& f) {
    MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("transcript-test");
    b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
    b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
    b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p2", .capability_ceiling = {}});
    b.max_rounds(50);
    auto built = b.build();
    if (!built) {
        std::fprintf(stderr, "setup failed\n");
        std::exit(2);
    }
    f.graph = std::move(*built);
    f.bodies = {
        [&f](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
            int const n = f.step.fetch_add(1);
            std::vector<std::string> route = n % 3 == 0 ? std::vector<std::string>{"p1"}
                                             : n % 3 == 1 ? std::vector<std::string>{"p2"}
                                                          : std::vector<std::string>{"done"};
            return ExecutorOutcome{text_message(text_of(in)), route};
        },
        [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">p1")};
        },
        [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + ">p2")};
        },
        [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
            return ExecutorOutcome{in};
        },
    };
}

}  // namespace

int main() {
    {
        Fixture f;
        make(f);
        WorkflowSupervisor sup;
        sup.initialize(f.graph.graph, f.bodies, {}, f.graph.manager_id);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
        check(r.status == workflow_status::completed, "T1 setup: run completes");
        check(r.transcript.empty() && !r.transcript_truncated, "T1: transcript is empty by default");
    }
    {
        Fixture f;
        make(f);
        WorkflowSupervisor sup;
        sup.initialize(f.graph.graph, f.bodies, {}, f.graph.manager_id);
        sup.enable_transcript();
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
        std::vector<std::string> ids;
        for (auto const& o : r.transcript) ids.push_back(o.executor_id);
        check(ids == std::vector<std::string>({"mgr", "p1", "mgr", "p2", "mgr", "done"}),
              "T2: every visit is recorded, in order, including the manager's revisits");
        check(r.partial.size() == 4, "T2: partial still holds one entry per executor (unchanged behavior)");
        check(!r.transcript_truncated, "T2: not truncated under the default cap");
        bool rounds_nondecreasing = true;
        for (std::size_t i = 1; i < r.transcript.size(); ++i)
            rounds_nondecreasing = rounds_nondecreasing && r.transcript[i].round >= r.transcript[i - 1].round;
        check(rounds_nondecreasing, "T2: round numbers never go backwards");
        check(text_of(r.transcript[3].payload) == "start>p1>p2", "T2: payloads are the real per-visit messages");

        f.step = 0;
        WorkflowResult r2 = drive(sup.run_workflow(RunWorkflow{text_message("again")}));
        check(r2.transcript.size() == 6, "T4: a fresh run starts a fresh transcript");
    }
    {
        Fixture f;
        make(f);
        WorkflowSupervisor sup;
        sup.initialize(f.graph.graph, f.bodies, {}, f.graph.manager_id);
        sup.enable_transcript(3);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
        check(r.transcript.size() == 3 && r.transcript_truncated, "T3: the cap keeps the first N visits and flags truncation");
        check(r.status == workflow_status::completed, "T3: truncation never affects the run itself");
    }
    return g_failures == 0 ? 0 : 1;
}
