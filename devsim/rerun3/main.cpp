// devsim/rerun3/main.cpp
//
// A document-processing pipeline built on AgentEngine, written ONLY from the docs site
// (web/marketing/src/data/*.ts and web/marketing/src/components/Api*Reference.tsx).
//
// Shape:
//   1. declare a Workflow graph as DATA with a fan_out to three workers and a fan_in
//      to one aggregator, plus a real TerminationBound;
//   2. validate_workflow() it before running;
//   3. run it, print the final output, and branch on the run's own workflow_status;
//   4. a second variant of the SAME graph where one edge is a switch_case chosen by a
//      classifier;
//   5. the failure path: print the engine's own error/status and exit non-zero.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using namespace agentengine::rt;

namespace {

// ---------------------------------------------------------------------------------------
// Small local helpers. `text_of` is documented engine API (core/tool_call_extraction.hpp).
//
// `text_message` is NOT: it is used by ~15 doc snippets verbatim but is never defined on the
// docs site and is not engine API -- every example defines its own (examples/09_concurrent_
// workflow.cpp:52). I had to reconstruct it from the documented Message/ContentItem/Text shape
// in workflowConcurrentMergeSnippet. See REPORT.md fallback #1.
//
// `all_text_of` is the example-local helper the Concurrent page shows verbatim
// (apiContent.ts workflowConcurrentMergeSnippet).
// ---------------------------------------------------------------------------------------

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += " + ";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------
// Executor bodies. `ExecutorBody` is documented as
// std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>, and the docs say a
// Message converts implicitly into an ExecutorOutcome for the "just pass it downstream" case.
// ---------------------------------------------------------------------------------------

[[nodiscard]] agentengine::task<agentengine::result<ExecutorOutcome>> source(
    Message const& in, EffectContext&) {
    co_return ExecutorOutcome{text_message(text_of(in))};
}

// A worker: a document-processing stage, three of them in the fan-out round.
[[nodiscard]] ExecutorBody worker(std::string name) {
    return [name](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        co_return ExecutorOutcome{text_message(name + ":" + upper(text_of(in)))};
    };
}

// The fan-in aggregator. Documented to be invoked EXACTLY ONCE per round, receiving one
// ContentItem per contributing branch in graph-declared order -- never once per inbound edge.
[[nodiscard]] agentengine::task<agentengine::result<ExecutorOutcome>> aggregate(
    Message const& in, EffectContext&) {
    co_return ExecutorOutcome{text_message(all_text_of(in))};
}

// A classifier: returns ExecutorOutcome{message, routes}, where `routes` names the ONE case
// it selects. This is the switch_case variant's decision point.
[[nodiscard]] ExecutorBody classifier(std::string const& needle) {
    return [needle](Message const& in, EffectContext&)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        std::string const text  = text_of(in);
        std::string const label = text.find(needle) != std::string::npos ? "upper" : "lower";
        co_return ExecutorOutcome{text_message(text + " -> " + label), {label}};
    };
}

[[nodiscard]] agentengine::task<agentengine::result<ExecutorOutcome>> identity_sink(
    Message const& in, EffectContext&) {
    co_return ExecutorOutcome{text_message(text_of(in))};
}

// Wraps a body with an invocation counter. The docs' Concurrent page uses the example-local
// `counted` helper for exactly this, to check that a fan-in aggregator ran EXACTLY ONCE rather
// than once per inbound edge; the docs also say a starved join finishes with output empty and
// the aggregator invoked ZERO times, so this is how that is measured rather than assumed.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> n) {
    return [body = std::move(body), n](Message const& in, EffectContext& ctx)
        -> agentengine::task<agentengine::result<ExecutorOutcome>> {
        n->fetch_add(1, std::memory_order_relaxed);
        co_return co_await body(in, ctx);
    };
}

// ---------------------------------------------------------------------------------------
// Status handling. Every workflow_status value the docs table names, grouped into the three
// kinds the docs describe: bound (expected, non-error), fault (a real failure), refusal
// (the run never started/resumed, so nothing changed and the same call can be made again).
// ---------------------------------------------------------------------------------------

enum class status_kind { success, waiting, bound, fault, refusal, unknown };

[[nodiscard]] status_kind kind_of(workflow_status s) {
    switch (s) {
        case workflow_status::completed:        return status_kind::success;
        case workflow_status::suspended:        return status_kind::waiting;
        case workflow_status::bound_max_rounds: return status_kind::bound;
        case workflow_status::bound_deadline:   return status_kind::bound;
        case workflow_status::bound_max_stalls: return status_kind::bound;
        case workflow_status::bound_max_resets: return status_kind::bound;
        case workflow_status::cancelled:        return status_kind::bound;
        case workflow_status::executor_failed:  return status_kind::fault;
        case workflow_status::routing_failed:   return status_kind::fault;
        case workflow_status::merge_conflict:   return status_kind::fault;
        case workflow_status::invalid:          return status_kind::refusal;
        case workflow_status::admission_denied: return status_kind::refusal;
        case workflow_status::invalid_routes:   return status_kind::refusal;
    }
    return status_kind::unknown;
}

[[nodiscard]] char const* kind_name(status_kind k) {
    switch (k) {
        case status_kind::success: return "success";
        case status_kind::waiting: return "waiting";
        case status_kind::bound:   return "bound";
        case status_kind::fault:   return "fault";
        case status_kind::refusal: return "refusal";
        case status_kind::unknown: return "unknown";
    }
    return "unknown";
}

// Prints the engine's OWN status tag (agentengine::rt::workflow_status_tag) plus the
// classification, then returns the process exit code this outcome deserves: 0 only for a
// completed run that actually produced an answer. A bound is reported honestly as a bounded
// run, and -- per the docs' own advice ("When you get an empty answer with a successful
// status, count the aggregator's invocations before you trust the status") -- a `completed`
// run whose output is empty is NOT treated as success.
[[nodiscard]] int report_run(char const* label, WorkflowResult const& r) {
    status_kind const k       = kind_of(r.status);
    std::string const answer = all_text_of(r.output);
    std::printf("[%s] status=%-16s kind=%-8s rounds=%u output=\"%s\"\n",
                label,
                agentengine::rt::workflow_status_tag(r.status),
                kind_name(k),
                static_cast<unsigned>(r.rounds),
                answer.c_str());

    switch (k) {
        case status_kind::success:
            if (answer.empty()) {
                std::fprintf(stderr,
                             "[%s] status says completed but the answer is EMPTY -- the engine's "
                             "status is not a promise that output was produced\n",
                             label);
                return 7;
            }
            std::printf("[%s] FINAL ANSWER: %s\n", label, answer.c_str());
            return 0;
        case status_kind::bound:
            std::fprintf(stderr, "[%s] run ended at its termination bound (expected, non-error)\n", label);
            return 2;
        case status_kind::waiting:
            std::fprintf(stderr, "[%s] run is suspended, holding no resources\n", label);
            return 3;
        case status_kind::fault:
            std::fprintf(stderr, "[%s] run faulted: %s\n", label,
                         agentengine::rt::workflow_status_tag(r.status));
            return 4;
        case status_kind::refusal:
            std::fprintf(stderr, "[%s] run was refused: %s\n", label,
                         agentengine::rt::workflow_status_tag(r.status));
            return 5;
        case status_kind::unknown:
            break;
    }
    std::fprintf(stderr, "[%s] unrecognised workflow_status\n", label);
    return 6;
}

void print_engine_error(char const* label, agentengine::error const& e) {
    std::fprintf(stderr, "[%s] engine error: code=%s message=%s\n", label, e.code.c_str(),
                 e.message.c_str());
}

// ---------------------------------------------------------------------------------------
// Graph 1 -- the product's real pipeline: fan_out to three workers, fan_in to one aggregator.
// ---------------------------------------------------------------------------------------

[[nodiscard]] Workflow make_fan_out_fan_in() {
    Workflow wf;
    wf.id        = "doc-pipeline";
    wf.executors = {Executor{"src", executor_kind::function, "Text", "Text"},
                    Executor{"upper", executor_kind::function, "Text", "Text"},
                    Executor{"lower", executor_kind::function, "Text", "Text"},
                    Executor{"title", executor_kind::function, "Text", "Text"},
                    Executor{"agg", executor_kind::function, "Text", "Text"}};
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "src";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}


// ---------------------------------------------------------------------------------------
// Graph 2 -- the SAME pipeline, but the fan-out is replaced by a switch_case chosen by a
// classifier, and BOTH cases land on the aggregator that carries a fan_in edge from each.
// The docs warn this starves (ApiWorkflowReference.tsx fanInTrap): a fan_in target with two
// or more declared sources is held until EVERY one of them has resolved, so if routing means
// only one of them ever fires, the graph can finish with workflow_status::completed and an
// EMPTY output, having invoked the aggregator zero times. Both `upper` and `lower` are
// reachable from the classifier, so validate_workflow accepts this graph.
// ---------------------------------------------------------------------------------------

[[nodiscard]] Workflow make_switch_case_into_fanin() {
    Workflow wf;
    wf.id        = "doc-pipeline-router-into-join";
    wf.executors = {Executor{"classifier", executor_kind::function, "Text", "Text"},
                    Executor{"upper", executor_kind::function, "Text", "Text"},
                    Executor{"lower", executor_kind::function, "Text", "Text"},
                    Executor{"agg", executor_kind::function, "Text", "Text"}};
    wf.edges.push_back(Edge{"classifier", "upper", edge_kind::switch_case, "upper"});
    wf.edges.push_back(Edge{"classifier", "lower", edge_kind::switch_case, "lower"});
    wf.edges.push_back(Edge{"upper", "agg", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"lower", "agg", edge_kind::fan_in, {}});
    wf.start = "classifier";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}

// ---------------------------------------------------------------------------------------
// Graph 3 -- the switch_case variant done the way the docs recommend: "route BEFORE the
// fan-in rather than inside it", keeping the aggregator's declared sources to ones that
// always fire. The classifier picks a branch; whatever branch it picks fans out to the
// always-firing workers; only those workers are declared sources of the aggregator.
// ---------------------------------------------------------------------------------------

[[nodiscard]] Workflow make_switch_case_before_fanin() {
    Workflow wf;
    wf.id        = "doc-pipeline-router-before-join";
    wf.executors = {Executor{"classifier", executor_kind::function, "Text", "Text"},
                    Executor{"upper", executor_kind::function, "Text", "Text"},
                    Executor{"lower", executor_kind::function, "Text", "Text"},
                    Executor{"join", executor_kind::function, "Text", "Text"},
                    Executor{"w1", executor_kind::function, "Text", "Text"},
                    Executor{"w2", executor_kind::function, "Text", "Text"},
                    Executor{"agg", executor_kind::function, "Text", "Text"}};
    wf.edges.push_back(Edge{"classifier", "upper", edge_kind::switch_case, "upper"});
    wf.edges.push_back(Edge{"classifier", "lower", edge_kind::switch_case, "lower"});
    wf.edges.push_back(Edge{"upper", "join", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"lower", "join", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"join", "w1", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"join", "w2", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"w1", "agg", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"w2", "agg", edge_kind::fan_in, {}});
    wf.start = "classifier";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}

// ---------------------------------------------------------------------------------------
// Graph 4 -- a bounded cycle that never converges, to exercise the `bound` branch of the
// status handler (the docs' reflection page: "a critic that never approves stops CLEANLY at
// bound.max_rounds -- not a crash, not a hang").
// ---------------------------------------------------------------------------------------

[[nodiscard]] Workflow make_never_converging_cycle() {
    Workflow wf;
    wf.id        = "reflection";
    wf.executors = {Executor{"writer", executor_kind::function, "Text", "Text"},
                    Executor{"critic", executor_kind::function, "Text", "Text"}};
    wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}});
    wf.start = "writer";
    wf.output_selection.push_back("critic");
    wf.bound.max_rounds = 6;
    return wf;
}

// A deliberately unbounded graph: validate_workflow must refuse it (014 Â§2, "an unbounded
// workflow does not run").
[[nodiscard]] Workflow make_unbounded() {
    Workflow wf;
    wf.id        = "unbounded";
    wf.executors = {Executor{"A", executor_kind::function, "Text", "Text"},
                    Executor{"B", executor_kind::function, "Text", "Text"}};
    wf.edges     = {Edge{"A", "B"}};
    wf.start     = "A";
    wf.output_selection.push_back("B");
    // no bound set on purpose
    return wf;
}

// A graph with a node no edge reaches. The docs never mention this check or its error code.
[[nodiscard]] Workflow make_unreachable() {
    Workflow wf;
    wf.id        = "unreachable";
    wf.executors = {Executor{"A", executor_kind::function, "Text", "Text"},
                    Executor{"B", executor_kind::function, "Text", "Text"},
                    Executor{"orphan", executor_kind::function, "Text", "Text"}};
    wf.edges     = {Edge{"A", "B"}};
    wf.start     = "A";
    wf.output_selection.push_back("B");
    wf.bound.max_rounds = 4;
    return wf;
}

}  // namespace

int main() {
    int exit_code = 0;

    // ===================================================================================
    // 1 + 2. Declare the graph as data, give it a termination bound, validate it.
    // ===================================================================================
    Workflow pipeline = make_fan_out_fan_in();
    std::printf("graph=%s executors=%zu edges=%zu max_rounds=%u\n", pipeline.id.c_str(),
                pipeline.executors.size(), pipeline.edges.size(),
                static_cast<unsigned>(pipeline.bound.max_rounds.value_or(0)));

    auto valid = validate_workflow(pipeline);
    if (!valid.has_value()) {
        print_engine_error("validate", valid.error());
        return 1;
    }
    std::printf("[validate] graph %s is well-formed\n", pipeline.id.c_str());

    // ===================================================================================
    // 3. Run it and get the result out.
    // ===================================================================================
    {
        std::vector<ExecutorBody> bodies = {source, worker("upper"), worker("lower"),
                                            worker("title"), aggregate};
        WorkflowSupervisor sup;
        sup.initialize(pipeline, bodies);
        WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{text_message("quarterly report")}));
        exit_code |= report_run("fan-out/fan-in", r);
    }

    // ===================================================================================
    // 4. The switch_case variant of the SAME graph -- routing feeding a fan-in join.
    // ===================================================================================
    {
        Workflow router = make_switch_case_into_fanin();
        auto rv = validate_workflow(router);
        if (!rv.has_value()) {
            print_engine_error("validate-router-into-join", rv.error());
            exit_code |= 1;
        } else {
            std::printf("[validate] graph %s is well-formed (the graph the docs' trap describes)\n",
                        router.id.c_str());
            // Run it twice, with both classifier outcomes, so the silence cannot be blamed on
            // one hardcoded branch.
            for (char const* const needle : std::initializer_list<char const*>{"invoice", "crash"}) {
                auto agg_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
                std::vector<ExecutorBody> bodies = {classifier(needle), worker("upper"),
                                                    worker("lower"), counted(aggregate, agg_calls)};
                WorkflowSupervisor sup;
                sup.initialize(router, bodies);
                WorkflowResult r = block_on(sup.run_workflow(
                    RunWorkflow{text_message(std::string("please process my ") + needle)}));
                std::printf("[router-into-fanin] classifier needle=%s aggregator_invocations=%u\n",
                            needle, agg_calls->load(std::memory_order_relaxed));
                // The run's own status is `completed` while producing no answer at all -- the
                // documented trap. report_run() refuses to treat that as success, so the exit
                // code reflects the product-level failure even though the engine said "success".
                exit_code |= report_run("router-into-fanin", r);
                std::printf("[router-into-fanin] had a successful status, empty answer, aggregator "
                            "invoked %u times\n",
                            agg_calls->load(std::memory_order_relaxed));
            }
        }
    }

    // Same switch_case decision, wired the way the docs recommend (route BEFORE the join).
    {
        Workflow router = make_switch_case_before_fanin();
        auto rv = validate_workflow(router);
        if (!rv.has_value()) {
            print_engine_error("validate-router-before-join", rv.error());
            exit_code |= 1;
        } else {
            std::printf("[validate] graph %s is well-formed\n", router.id.c_str());
            auto agg_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
            std::vector<ExecutorBody> bodies = {classifier("invoice"), worker("upper"),
                                                worker("lower"), identity_sink, worker("w1"),
                                                worker("w2"), counted(aggregate, agg_calls)};
            WorkflowSupervisor sup;
            sup.initialize(router, bodies);
            WorkflowResult r =
                block_on(sup.run_workflow(RunWorkflow{text_message("please process my invoice")}));
            std::printf("[router-before-fanin] aggregator_invocations=%u\n",
                        agg_calls->load(std::memory_order_relaxed));
            exit_code |= report_run("router-before-fanin", r);
        }
    }

    // ===================================================================================
    // 4b. A bounded run that the bound stops -- the "bounded" branch of the status handler.
    // ===================================================================================
    {
        Workflow cycle = make_never_converging_cycle();
        auto rv = validate_workflow(cycle);
        if (!rv.has_value()) {
            print_engine_error("validate-cycle", rv.error());
            exit_code |= 1;
        } else {
            std::vector<ExecutorBody> bodies = {identity_sink, identity_sink};
            WorkflowSupervisor sup;
            sup.initialize(cycle, bodies);
            WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{text_message("draft")}));
            // A bound is an expected, non-error termination: it does not make the process fail.
            (void)report_run("bounded-cycle", r);
        }
    }

    // ===================================================================================
    // 5. Failure path A -- validation refuses the graph.
    // ===================================================================================
    {
        Workflow unbounded = make_unbounded();
        auto rv = validate_workflow(unbounded);
        if (!rv.has_value()) {
            std::printf("[validate-unbounded] refused, as the docs say it would\n");
            print_engine_error("validate-unbounded", rv.error());
            exit_code |= 1;
        } else {
            std::fprintf(stderr, "[validate-unbounded] UNEXPECTED: unbounded graph validated\n");
            exit_code |= 1;
        }
    }

    // ===================================================================================
    // 5. Failure path A2 -- a graph check the docs never name.
    // ===================================================================================
    {
        Workflow bad = make_unreachable();
        auto rv = validate_workflow(bad);
        if (!rv.has_value()) {
            std::printf("[validate-unreachable] refused by a check the docs never mention\n");
            print_engine_error("validate-unreachable", rv.error());
        } else {
            std::printf("[validate-unreachable] validated -- no reachability check after all\n");
        }
    }

    // ===================================================================================
    // 5. Failure path B -- the run itself faults (a route label the graph never declared).
    // ===================================================================================
    {
        Workflow wf;
        wf.id        = "routing-failure";
        wf.executors = {Executor{"triage", executor_kind::function, "Text", "Text"},
                        Executor{"billing", executor_kind::function, "Text", "Text"},
                        Executor{"tech", executor_kind::function, "Text", "Text"}};
        wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
        wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
        wf.start = "triage";
        wf.output_selection = {"billing", "tech"};
        wf.bound.max_rounds = 4;

        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&)
                -> agentengine::task<agentengine::result<ExecutorOutcome>> {
                // "nobody" is a label this graph never declared.
                co_return ExecutorOutcome{text_message(text_of(in)), {"nobody"}};
            },
            identity_sink, identity_sink};

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{text_message("my invoice is wrong")}));
        exit_code |= report_run("routing-failure", r);
    }

    std::printf("exit=%d\n", exit_code);
    return exit_code;
}
