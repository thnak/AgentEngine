// docpipeline -- a document-processing pipeline built on AgentEngine.
//
// Requirement coverage:
//   1. fan-out to three parallel worker nodes, fan-in to one aggregator.
//   2. a real termination bound, validated before running.
//   3. run it, print the final output, branch on completed / bounded / failed.
//   4. a second variant of the SAME graph where an edge is a switch_case chosen by a classifier.
//   5. on validation or run failure: print the engine's own error/status and exit non-zero.
//
// Constructs are cited in REPORT.md with file and line. Anything the docs did not supply is marked
// "FALLBACK n" and listed in REPORT.md's fallback log.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ---- includes -------------------------------------------------------------------------------
// The complete-program page (ApiFirstAppReference.tsx:11-17) fixes the include spelling for core
// types. The workflow pages name the two workflow headers only in prose, in CodePanel titles
// ("workflow/graph.hpp", "rt/workflow_supervisor.hpp") and in a citation
// ("include/agentengine/workflow/graph.hpp:431") -- never as an #include line in a compiling file.
// FALLBACK 1 (include paths for the workflow headers).
#include "agentengine/core/content.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

using namespace agentengine;
using agentengine::rt::block_on;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::rt::workflow_status_tag;
using agentengine::workflow::edge_kind;
using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;
using agentengine::workflow::Workflow;

namespace {

// `text_message` is listed in the docs' shared "How to read the code on this page" note
// (ApiSnippetConventions.tsx:32) as EXAMPLE-LOCAL, not engine API -- so this file defines its own.
[[nodiscard]] Message text_message(std::string text) {
    Message m{};
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// Also example-local (same note). The docs state a fan_in delivery arrives as "one ContentItem per
// contributing branch, in graph-declared order" (apiContent.ts:2815).
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

// node_desc is listed as example-local (ApiSnippetConventions.tsx:33 cites
// examples/23_handoff_mesh.cpp:103); the pizza-order page prints it verbatim
// (ApiPizzaOrderReference.tsx:253-256).
[[nodiscard]] Executor node_desc(std::string id, executor_kind kind = executor_kind::function) {
    return Executor{.id                 = std::move(id),
                    .kind               = kind,
                    .input_type         = "Text",
                    .output_type        = "Text",
                    .worktree_mode      = sharing_mode::branch,
                    .capability_ceiling = {}};
}

[[nodiscard]] std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------
// Executor bodies. The docs pin the shape twice: "rt::ExecutorBody is exactly
// std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>" (apiContent.ts:3116),
// with concrete bodies at apiContent.ts:3104-3115.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] result<Message> source(Message const& in, EffectContext&) {
    return text_message(text_of(in));
}

// A source that stamps which routed branch it is, so a run's transcript says which case fired.
[[nodiscard]] ExecutorBody tagged_source(std::string tag) {
    return [tag](Message const& in, EffectContext&) -> result<Message> {
        return text_message(tag + ":" + text_of(in));
    };
}

[[nodiscard]] ExecutorBody worker(std::string name) {
    return [name](Message const& in, EffectContext&) -> result<Message> {
        return text_message(name + ":" + upper(text_of(in)));
    };
}

// The aggregator reads the merged delivery: one Text item per contributing branch.
[[nodiscard]] result<Message> aggregate(Message const& in, EffectContext&) {
    return text_message(all_text_of(in));
}

// `counted` is listed as example-local (ApiSnippetConventions.tsx:33, "wraps a body with an
// invocation counter"). The docs' own version is at apiContent.ts:3162; this is that idea.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> n) {
    return [body = std::move(body), n](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
        n->fetch_add(1, std::memory_order_relaxed);
        return body(in, ctx);
    };
}

// A classifier body, exactly the shape the Router pattern is proven with (apiContent.ts:3055-3058).
[[nodiscard]] ExecutorBody classifier(std::string label) {
    return [label](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + " -> " + label), {label}};
    };
}

// A body that always fails. The docs state a throwing executor cannot take the process with it:
// "rt::ThreadPool::submit() returns a JobOutcome{faulted, fault_ptr} rather than letting the
// exception propagate raw" (ApiWorkflowReference.tsx:325-327) -- so throwing is the documented way
// to produce a faulting node without needing an error-construction API.
[[nodiscard]] result<Message> explode(Message const&, EffectContext&) {
    throw std::runtime_error("simulated worker failure");
}

// ---- bodies for the bounded-cycle variant ----------------------------------------------------
[[nodiscard]] result<Message> writer_body(Message const& in, EffectContext&) {
    return text_message(all_text_of(in) + ">writer");
}

// Never approves -- the docs' own description of the bounded case (apiContent.ts:3078: "a critic
// that never approves stops CLEANLY at bound.max_rounds").
[[nodiscard]] ExecutorBody critic_never_approves() {
    return [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">critic"), {"revise"}};
    };
}

[[nodiscard]] result<Message> done_body(Message const& in, EffectContext&) {
    return text_message(all_text_of(in));
}

// ---------------------------------------------------------------------------------------------
// The graphs
// ---------------------------------------------------------------------------------------------

// Variant A -- Concurrent: fan_out to three workers, fan_in to one aggregator.
// Shape from examples/09_concurrent_workflow.cpp as printed at apiContent.ts:3027-3042.
[[nodiscard]] Workflow make_concurrent_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline";
    wf.executors = {node_desc("src"), node_desc("upper"), node_desc("lower"), node_desc("title"),
                    node_desc("agg")};
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "src";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}

// Variant B -- the SAME graph with the fan_out edges replaced by switch_case chosen by a
// classifier. This is the combination the docs warn about: routes INTO a fan-in join.
[[nodiscard]] Workflow make_trap_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-routed-into-fanin";
    wf.executors = {node_desc("classifier"), node_desc("upper"), node_desc("lower"),
                    node_desc("title"), node_desc("agg")};
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"classifier", w, edge_kind::switch_case, w});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "classifier";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}

// Variant C -- the documented-correct combination (ApiWorkflowReference.tsx:90-91, "keep an
// aggregator's declared sources to ones that always fire, and route BEFORE the fan-in rather than
// inside it"). The classifier routes to one of two sources; whichever fires fans out to all three
// workers, so the aggregator's three declared sources always resolve.
[[nodiscard]] Workflow make_routed_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-routed-before-fanin";
    wf.executors = {node_desc("classifier"), node_desc("detailed_src"), node_desc("brief_src"),
                    node_desc("upper"), node_desc("lower"), node_desc("title"), node_desc("agg")};
    wf.edges.push_back(Edge{"classifier", "detailed_src", edge_kind::switch_case, "detailed"});
    wf.edges.push_back(Edge{"classifier", "brief_src", edge_kind::switch_case, "brief"});
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"detailed_src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"brief_src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "classifier";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 10;
    return wf;
}

// Variant D -- the docs' bounded-cycle shape: CY-1 from
// tests/workflow/test_rt_workflow_supervisor_patterns.cpp, printed at apiContent.ts:3061-3079. Used
// here with a critic that never approves, so the run stops at bound.max_rounds.
[[nodiscard]] Workflow make_bounded_graph() {
    Workflow wf;
    wf.id        = "writer-critic";
    wf.executors = {node_desc("writer"), node_desc("critic"), node_desc("done")};
    wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"critic", "writer", edge_kind::switch_case, "revise"});
    wf.edges.push_back(Edge{"critic", "done", edge_kind::switch_case, "approve"});
    wf.start = "writer";
    wf.output_selection.push_back("done");
    wf.bound.max_rounds = 6;
    return wf;
}

// Variant E -- switch_default (ADR-215) in front of a fan-in. The classifier's reply matches no
// declared case label, so the question this variant exists to answer is whether the default fires
// or the run fails with routing_failed: the docs assert BOTH. apiContent.ts:2818 says
// switch_default "fires iff ZERO of the same source's switch_case edges matched the reply's
// routes", while the Router pattern's own proof (ApiWorkflowPatternDetail.tsx:305) says "A route
// naming zero or more than one declared label fails the run with routing_failed".
[[nodiscard]] Workflow make_switch_default_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-switch-default";
    wf.executors = {node_desc("classifier"), node_desc("detailed_src"), node_desc("brief_src"),
                    node_desc("upper"), node_desc("lower"), node_desc("title"), node_desc("agg")};
    wf.edges.push_back(Edge{"classifier", "detailed_src", edge_kind::switch_case, "detailed"});
    wf.edges.push_back(Edge{"classifier", "brief_src", edge_kind::switch_default, {}});
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"detailed_src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"brief_src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "classifier";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 10;
    return wf;
}

// --- deliberately-broken graphs, for requirement 5 -------------------------------------------

// A switch_case edge with no case label. The docs name the expected code
// (ApiPizzaOrderReference.tsx:264-267: "workflow.missing_case_label").
[[nodiscard]] Workflow make_missing_label_graph() {
    Workflow wf = make_concurrent_graph();
    wf.edges.push_back(Edge{"src", "upper", edge_kind::switch_case, ""});
    return wf;
}

// A graph with no termination bound. The docs name "workflow.unbounded"
// (ApiBuilderReference.tsx:186).
[[nodiscard]] Workflow make_unbounded_graph() {
    Workflow wf    = make_concurrent_graph();
    wf.bound       = {};
    return wf;
}

// ---------------------------------------------------------------------------------------------
// Requirement 3 + 5: read the run's status, print the engine's own string for it, and decide.
// The three kinds are the docs' own taxonomy (ApiWorkflowReference.tsx:335-369).
// ---------------------------------------------------------------------------------------------
enum class outcome_kind { success, waiting, bound, fault, refusal };

[[nodiscard]] outcome_kind kind_of(workflow_status s) {
    switch (s) {
        case workflow_status::completed:        return outcome_kind::success;
        case workflow_status::suspended:        return outcome_kind::waiting;
        case workflow_status::bound_max_rounds:
        case workflow_status::bound_deadline:
        case workflow_status::bound_max_stalls:
        case workflow_status::bound_max_resets:
        case workflow_status::cancelled:        return outcome_kind::bound;
        case workflow_status::executor_failed:
        case workflow_status::routing_failed:
        case workflow_status::merge_conflict:   return outcome_kind::fault;
        case workflow_status::invalid:
        case workflow_status::admission_denied:
        case workflow_status::invalid_routes:   return outcome_kind::refusal;
    }
    return outcome_kind::fault;
}

[[nodiscard]] char const* kind_name(outcome_kind k) {
    switch (k) {
        case outcome_kind::success: return "success";
        case outcome_kind::waiting: return "waiting";
        case outcome_kind::bound:   return "bound";
        case outcome_kind::fault:   return "fault";
        case outcome_kind::refusal: return "refusal";
    }
    return "unknown";
}

// Returns the process exit code. Requirement 5: a fault or a refusal exits non-zero.
[[nodiscard]] int report_run(WorkflowResult const& r, std::uint32_t aggregator_invocations) {
    workflow_status const s = r.status;
    // FALLBACK 2: workflow_status_tag's return type is not stated in the docs; the call site below
    // is written for the type the compiler turned out to want.
    std::printf("status      : %s\n", workflow_status_tag(s));
    std::printf("status kind : %s\n", kind_name(kind_of(s)));
    std::printf("agg invoked : %u\n", aggregator_invocations);

    // The WorkflowResult reference (ApiWorkflowReference.tsx:371-380 + the struct printed at
    // apiContent.ts:2812-2847) says the status tag alone gets a fault handler wrong: it cannot say
    // WHICH node failed, and it gives no reason to look at what the other nodes produced.
    std::printf("rounds      : %u\n", r.rounds);
    if (!r.failed_executor.empty()) {
        std::printf("failed node : %s\n", r.failed_executor.c_str());
    }
    std::printf("partial     : %zu node(s) produced a message preserved after the run\n",
                r.partial.size());

    std::string const text = all_text_of(r.output);
    std::printf("output      : %s\n", text.empty() ? "<empty>" : text.c_str());

    switch (kind_of(s)) {
        case outcome_kind::success:
            // The docs' own trap: completed does NOT mean "there is a result"
            // (ApiWorkflowReference.tsx:350). Check both.
            if (text.empty()) {
                std::fprintf(stderr,
                             "FAIL: status says completed but the output is empty and the "
                             "aggregator ran %u time(s) -- a starved fan-in join.\n",
                             aggregator_invocations);
                return 4;
            }
            return 0;
        case outcome_kind::waiting:
            std::fprintf(stderr, "run is waiting on a request_port; no result yet.\n");
            return 5;
        case outcome_kind::bound:
            std::fprintf(stderr, "run stopped at a bound (expected, not a fault): %s\n",
                         workflow_status_tag(s));
            return 2;
        case outcome_kind::fault:
            std::fprintf(stderr, "run faulted: %s\n", workflow_status_tag(s));
            return 3;
        case outcome_kind::refusal:
            std::fprintf(stderr, "run was refused before it started: %s\n", workflow_status_tag(s));
            return 3;
    }
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    std::string variant = "concurrent";
    std::string route   = "detailed";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--variant=", 0) == 0) variant = a.substr(10);
        else if (a.rfind("--route=", 0) == 0) route = a.substr(8);
    }

    std::printf("== docpipeline variant=%s ==\n", variant.c_str());

    Workflow                 wf;
    std::vector<ExecutorBody> bodies;
    auto                      agg_invocations = std::make_shared<std::atomic<std::uint32_t>>(0);
    RunWorkflow               request{text_message("Quarterly Report")};

    if (variant == "concurrent") {
        wf     = make_concurrent_graph();
        bodies = {source, worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "trap") {
        // The classifier picks exactly one branch; the aggregator declares three.
        wf     = make_trap_graph();
        bodies = {classifier("upper"), worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "routed") {
        wf     = make_routed_graph();
        bodies = {classifier(route), tagged_source("DETAILED"), tagged_source("BRIEF"),
                  worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "bounded") {
        wf     = make_bounded_graph();
        bodies = {writer_body, critic_never_approves(), done_body};
    } else if (variant == "switch-default") {
        wf     = make_switch_default_graph();
        // A reply that matches no declared case label.
        bodies = {classifier("unmatched"), tagged_source("DETAILED"), tagged_source("BRIEF"),
                  worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "throw") {
        wf     = make_concurrent_graph();
        bodies = {explode, worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "bad-route") {
        wf     = make_trap_graph();
        // A label the graph never declared -- the docs' routing_failed boundary.
        bodies = {classifier("nope"), worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "missing-label") {
        wf     = make_missing_label_graph();
        bodies = {source, worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else if (variant == "unbounded") {
        wf     = make_unbounded_graph();
        bodies = {source, worker("upper"), worker("lower"), worker("title"),
                  counted(aggregate, agg_invocations)};
    } else {
        std::fprintf(stderr, "unknown variant '%s'\n", variant.c_str());
        return 1;
    }

    // Requirement 2: validate the bound before running. The docs show this call and its shape
    // (ApiPizzaOrderReference.tsx:277; error fields at ApiFirstAppReference.tsx:300-313).
    auto validated = validate_workflow(wf);
    if (!validated.has_value()) {
        std::fprintf(stderr, "validation failed: %s (%s)\n", validated.error().message.c_str(),
                     validated.error().code.c_str());
        return 1;
    }
    std::printf("validated   : ok (bound max_rounds=%u)\n",
                static_cast<unsigned>(wf.bound.max_rounds.value_or(0)));

    WorkflowSupervisor sup;
    // FALLBACK 3: initialize() returns void (verified by probe). The docs describe it as running a
    // check that can refuse a graph ("check_workflow_executable refuses both outright",
    // ApiWorkflowReference.tsx:377-388) but never say how the refusal is reported; every snippet
    // discards the call. A refusal is therefore only observable as the run's own status.
    sup.initialize(wf, bodies);

    // `drive(...)` in every docs snippet is example-local; ApiSnippetConventions.tsx:17 gives the
    // engine's real equivalent: agentengine::rt::block_on(task).
    WorkflowResult const r = block_on(sup.run_workflow(std::move(request)));

    return report_run(r, agg_invocations->load(std::memory_order_relaxed));
}
