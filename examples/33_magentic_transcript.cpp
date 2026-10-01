// AgentEngine "get started" examples, 33 -- issue #28 (Magentic-shaped workflows), items 5 + 6 together.
//
// A manager and two participants run a Magentic-style cyclic plan (manager -> researcher -> manager -> writer
// -> manager -> done), the way MAF's Magentic samples do. This shows the two things #28 tracked last:
//   * the typed live-event stream (enable_event_stream(), ADR-152): the caller polls WorkflowEvents while the
//     run is in flight -- a UI would render these as they arrive;
//   * the full transcript (enable_transcript(), ADR-213): every visit in order, manager revisits included.
//     WorkflowResult::partial can't show this -- it keeps only each executor's LAST message.
//
// Fully offline (scripted executor bodies, no model, no network). Run: ./agentengine_example_33_magentic_transcript

#include <atomic>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/magentic.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Magentic.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Magentic.ReportMsg");

namespace {

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    MagenticWorkflowBuilder<TaskMsg, ReportMsg> builder("research-and-write");
    builder.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "manager", .capability_ceiling = {}});
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "researcher", .capability_ceiling = {}});
    builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "writer", .capability_ceiling = {}});
    builder.max_rounds(20);
    auto built = builder.build();
    if (!built) {
        std::fprintf(stderr, "build failed: %s\n", built.error().message.c_str());
        return 2;
    }

    // The manager's ledger: researcher first, then writer, then declare done.
    std::atomic<int> step{0};
    std::vector<ExecutorBody> bodies = {
        [&step](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            int const n = step.fetch_add(1);
            std::vector<std::string> route = n == 0 ? std::vector<std::string>{"researcher"}
                                             : n == 1 ? std::vector<std::string>{"writer"}
                                                      : std::vector<std::string>{"done"};
            return ExecutorOutcome{text_message(text_of(in)), route};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + " | researcher: found 3 sources")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(text_of(in) + " | writer: drafted summary")};
        },
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
    };

    WorkflowSupervisor sup;
    sup.initialize(built->graph, bodies, {}, built->manager_id);
    sup.enable_transcript();
    WorkflowEventStream events = sup.enable_event_stream(std::pmr::get_default_resource());

    agentengine::rt::task<WorkflowResult> run = sup.run_workflow(RunWorkflow{text_message("task: summarize X")});
    std::puts("== live events ==");
    while (!run.done()) {
        run.resume();
        while (auto ev = events.next()) {
            if (ev->kind == workflow_event_kind::executor_completed) {
                auto const& r = std::get<workflow_event_payload::ExecutorResult>(ev->payload);
                std::printf("  round %u: %s completed (%s)\n", ev->round, r.executor_id.c_str(), r.ok ? "ok" : "failed");
            }
        }
    }
    WorkflowResult result = run.take_value();
    while (auto ev = events.next()) {  // anything queued after the last resume
        if (ev->kind == workflow_event_kind::executor_completed) {
            auto const& r = std::get<workflow_event_payload::ExecutorResult>(ev->payload);
            std::printf("  round %u: %s completed (%s)\n", ev->round, r.executor_id.c_str(), r.ok ? "ok" : "failed");
        }
    }

    std::puts("\n== full transcript (every visit, in order) ==");
    for (auto const& visit : result.transcript) {
        std::printf("  [round %u] %-10s -> %s\n", visit.round, visit.executor_id.c_str(), text_of(visit.payload).c_str());
    }
    std::printf("\nvisits: %zu   distinct executors in partial: %zu   truncated: %s\n", result.transcript.size(),
                result.partial.size(), result.transcript_truncated ? "yes" : "no");

    bool const ok = result.status == workflow_status::completed && result.transcript.size() == 6 &&
                    result.partial.size() == 4 && !result.transcript_truncated &&
                    text_of(result.output) == "task: summarize X | researcher: found 3 sources | writer: drafted summary";
    std::puts(ok ? "\nOK" : "\nFAILED");
    return ok ? 0 : 1;
}
