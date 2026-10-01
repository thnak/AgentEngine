// AgentEngine "get started" examples, 34 -- issue #45: review points (014 §4, workflow/review_points.hpp).
//
// MAF's SequentialBuilder/ConcurrentBuilder `.with_request_info()` (agent-framework samples
// python/samples/03-workflows/human-in-the-loop/sequential_request_info.py and concurrent_request_info.py),
// AgentEngine-style: the builder puts an ordinary request port after each selected step, so a human sees
// the step's output before the workflow moves on. Two runs:
//
//   1. Sequential (outline -> write -> polish), a review after every step, with revision allowed. The
//      scripted reviewer approves the outline, sends the draft back once ("revise"), approves the redo,
//      and amends the final text.
//   2. Concurrent (dispatch -> {legal, finance, tech} -> merge), reviews on legal and finance only. Two
//      reviews open at once (one per selected branch, distinct ids); tech runs without pausing; merge runs
//      once, after both answers.
//
// How a reviewer answers (ResumeWorkflow): no routes = approve -- the answer message is what flows on, so
// answering with the ask (open_interaction_asks()) passes the step's output through, and an edited message
// amends it. Routes {"revise"} (revisable points only) sends the answer back to the step for another pass.
//
// Fully offline (scripted executor bodies and reviewer, no model, no network).
// Run: ./agentengine_example_34_request_info_review

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/review_points.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

struct Draft {};
struct Question {};
struct Opinion {};
struct Summary {};
AE_WORKFLOW_MESSAGE(Draft, "Example.Draft");
AE_WORKFLOW_MESSAGE(Question, "Example.Question");
AE_WORKFLOW_MESSAGE(Opinion, "Example.Opinion");
AE_WORKFLOW_MESSAGE(Summary, "Example.Summary");

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
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += (out.empty() ? "" : " + ") + t->text;
    }
    return out;
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] ExecutorBody step(std::string what) {
    return [what = std::move(what)](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(what + "(" + text_of(in) + ")")};
    };
}

[[nodiscard]] Message ask_of(WorkflowSupervisor const& sup, std::string const& interaction_id) {
    for (auto const& a : sup.open_interaction_asks()) {
        if (a.interaction.interaction_id == interaction_id) return a.ask;
    }
    return {};
}

bool run_sequential() {
    std::puts("== 1. sequential, review after every step ==");
    using Step = TypedExecutor<Draft, Draft>;
    auto built = SequentialWorkflowBuilder<Draft>("article")
                     .participant(Step{.id = "outline", .capability_ceiling = {}})
                     .participant(Step{.id = "write", .capability_ceiling = {}})
                     .participant(Step{.id = "polish", .capability_ceiling = {}})
                     .with_request_info({.only = {}, .allow_revision = true})
                     .max_rounds(32)
                     .build();
    if (!built) {
        std::fprintf(stderr, "build failed: %s\n", built.error().message.c_str());
        return false;
    }
    std::printf("graph: %zu executors (%zu review ports), %zu edges\n", built->executors.size(),
                built->executors.size() - 3, built->edges.size());

    // Bodies: the participants in order, then one empty body per review port.
    WorkflowSupervisor sup;
    sup.initialize(*built, {step("outline"), step("write"), step("polish"), {}, {}, {}});

    WorkflowResult r      = drive(sup.run_workflow(RunWorkflow{text_message("topic")}));
    int            writes = 0;
    while (r.status == workflow_status::suspended) {
        auto const&       open = r.open_interactions.front();  // one review open at a time in a chain
        std::string const ask  = text_of(ask_of(sup, open.interaction_id));
        ResumeWorkflow answer{open.interaction_id, ask_of(sup, open.interaction_id), {}};
        if (open.interaction_id.find(":port:write.review:") != std::string::npos && writes++ == 0) {
            answer.response = text_message("shorter, please");
            answer.routes   = {std::string(kReviseRoute)};
            std::printf("  review %-14s sees %-40s -> revise\n", "write.review", ask.c_str());
        } else if (open.interaction_id.find(":port:polish.review:") != std::string::npos) {
            answer.response = text_message(ask + " [reviewer: fixed a typo]");
            std::printf("  review %-14s sees %-40s -> amend and approve\n", "polish.review", ask.c_str());
        } else {
            std::printf("  review %-14s sees %-40s -> approve\n",
                        open.interaction_id.find("outline") != std::string::npos ? "outline.review" : "write.review",
                        ask.c_str());
        }
        r = drive(sup.resume_workflow(answer));
    }
    std::printf("result: %s\n\n", text_of(r.output).c_str());
    return r.status == workflow_status::completed &&
           text_of(r.output) == "polish(write(shorter, please)) [reviewer: fixed a typo]";
}

bool run_concurrent() {
    std::puts("== 2. concurrent, review the legal and finance branches before merging ==");
    auto built = ConcurrentWorkflowBuilder<Question, Opinion, Summary>("due-diligence")
                     .dispatcher(TypedExecutor<Question, Question>{.id = "dispatch", .capability_ceiling = {}})
                     .participant(TypedExecutor<Question, Opinion>{.id = "legal", .capability_ceiling = {}})
                     .participant(TypedExecutor<Question, Opinion>{.id = "finance", .capability_ceiling = {}})
                     .participant(TypedExecutor<Question, Opinion>{.id = "tech", .capability_ceiling = {}})
                     .aggregator(TypedExecutor<Opinion, Summary>{.id = "merge", .capability_ceiling = {}})
                     .with_request_info({"legal", "finance"})
                     .max_rounds(16)
                     .build();
    if (!built) {
        std::fprintf(stderr, "build failed: %s\n", built.error().message.c_str());
        return false;
    }
    // Bodies: dispatcher, participants in order, aggregator, then one empty body per review port.
    WorkflowSupervisor sup;
    sup.initialize(*built, {[](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
                            step("legal"), step("finance"), step("tech"), step("merge"), {}, {}});

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("acquire X?")}));
    std::printf("suspended with %zu open reviews:\n", r.open_interactions.size());
    for (auto const& i : r.open_interactions) std::printf("  %s\n", i.interaction_id.c_str());
    std::size_t const opened = r.open_interactions.size();

    std::vector<std::string> ids;
    for (auto const& i : r.open_interactions) ids.push_back(i.interaction_id);
    for (std::string const& id : ids) {
        std::printf("  approve %s (%s)\n", id.c_str(), text_of(ask_of(sup, id)).c_str());
        r = drive(sup.resume_workflow(ResumeWorkflow{id, ask_of(sup, id), {}}));
    }
    std::printf("result: %s\n", text_of(r.output).c_str());
    return opened == 2 && r.status == workflow_status::completed;
}

}  // namespace

int main() {
    bool const ok = run_sequential() && run_concurrent();
    std::puts(ok ? "\nOK" : "\nFAILED");
    return ok ? 0 : 1;
}
