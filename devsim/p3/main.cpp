// devsim/p3/main.cpp -- a consumer program built on the vendored AgentEngine.
//
// PRODUCT: a document-processing pipeline that fans work out and produces one answer.
//
//   scenario "fanout"  -- the base graph:   ingest --fan_out--> {summarize, keywords, sentiment}
//                                            each --fan_in--> aggregate   (one answer)
//   scenario "router"  -- the SAME graph, one class of edge swapped for switch_case: the classifier
//                         node `ingest` returns ExecutorOutcome::routes naming exactly one label,
//                         so exactly ONE worker runs and the unselected branches' invoke() is never
//                         called. Same executors, same aggregator, same bound.
//   scenario "bounded" -- a cycle with an explicit iteration bound that never converges on its own:
//                         it stops CLEANLY at bound.max_rounds, status bound_max_rounds.
//   failure scenarios  -- "invalid-graph" (bound omitted -> validate_workflow refuses the graph),
//                         "unroutable" (body names a route label the graph never declared ->
//                         workflow_status::routing_failed), "body-error" (a worker returns an
//                         engine error -> workflow_status::executor_failed). Each prints the
//                         engine's own message/code/status and exits non-zero.
//
// Everything below is written against the AgentEngine API reference docs; identifiers the docs
// leave undefined (text_message, drive, check, node_desc, counted, appender, ...) are supplied here
// and each one is marked with where its definition was actually found. See REPORT.md.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

namespace ae  = agentengine;
namespace wfg = agentengine::workflow;

using ae::rt::ExecutorBody;
using ae::rt::ExecutorOutcome;
using ae::rt::RunWorkflow;
using ae::rt::WorkflowResult;
using ae::rt::WorkflowSupervisor;
using ae::rt::workflow_status;
using wfg::Edge;
using wfg::Executor;
using wfg::Workflow;
using wfg::edge_kind;
using wfg::executor_kind;
// NOT agentengine::workflow::sharing_mode -- it is agentengine::sharing_mode
// (include/agentengine/core/sharing_mode.hpp:18). The docs' snippets use it unqualified from inside
// the engine's own namespace and never say which one that is.
using ae::sharing_mode;

namespace {

// ================================================================================================
// Helpers the docs use but never define. Each cites where the real definition actually lives.
// ================================================================================================

// NOT part of the engine. Copied verbatim from examples/09_concurrent_workflow.cpp:52-60 (the docs'
// Concurrent snippet is trimmed from that file but leaves this out).
[[nodiscard]] ae::Message text_message(std::string text) {
    ae::ContentItem item{};
    item.origin = ae::content_origin::user;
    item.value  = ae::Text{std::move(text)};
    ae::Message m{};
    m.role = ae::role::user;
    m.content.push_back(std::move(item));
    return m;
}

// The docs DO define this one: apiContent.ts:3047 (workflowConcurrentMergeSnippet), transcribed from
// examples/09_concurrent_workflow.cpp:64-73. A fan-in delivery is one ContentItem per contributing
// branch, in graph-declared order, so the aggregator joins every Text item it was handed.
[[nodiscard]] std::string all_text_of(ae::Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<ae::Text>(&item.value)) {
            if (!out.empty()) out += " | ";
            out += t->text;
        }
    }
    return out;
}

// The docs use node_desc()/node() everywhere but only ever define it on the pizza-order page
// (ApiPizzaOrderReference.tsx:198-201, "transcribed from examples/23_handoff_mesh.cpp"). Same shape,
// with the docs' "Text" port names rather than examples/09's "T".
[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id                 = id,
                    .kind               = kind,
                    .input_type         = "Text",
                    .output_type        = "Text",
                    .worktree_mode      = sharing_mode::branch,
                    .capability_ceiling = {}};
}

// The docs use check() in almost every test-derived snippet and never define it. This is the same
// shape examples/*.cpp use.
int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// The docs' workflow snippets all say `drive(sup.run_workflow(...))`; drive() is defined nowhere in
// the docs. The engine does ship a correct synchronous driver -- rt::block_on() (documented on the
// Builder page, builderContent.ts:328 and ApiBuilderReference.tsx:133) -- and it is what this
// program uses instead of the examples' hand-rolled resume loop.
template <class T>
[[nodiscard]] T drive(ae::rt::task<T> t) {
    return ae::rt::block_on(std::move(t));
}

// ================================================================================================
// The three worker bodies. Deterministic, offline, no model call -- this is a pipeline, not an agent.
// ================================================================================================

[[nodiscard]] ExecutorBody worker_summarize() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        std::string const text = ae::text_of(in);
        std::string       body = text.size() > 56 ? text.substr(0, 56) + "..." : text;
        return text_message("summary{" + body + "}");
    };
}

[[nodiscard]] ExecutorBody worker_keywords() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        std::string const text = ae::text_of(in);
        std::string       out;
        std::string       word;
        auto              flush = [&] {
            if (word.size() >= 7) {
                if (!out.empty()) out += ",";
                out += word;
            }
            word.clear();
        };
        for (char const c : text) {
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            } else {
                flush();
            }
        }
        flush();
        if (out.empty()) out = "<none>";
        return text_message("keywords{" + out + "}");
    };
}

[[nodiscard]] ExecutorBody worker_sentiment() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        std::string  text = ae::text_of(in);
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t positive = 0;
        std::size_t negative = 0;
        for (std::string_view const w : {"good", "great", "excellent", "approved", "clear"}) {
            for (std::size_t p = text.find(w); p != std::string::npos; p = text.find(w, p + 1)) ++positive;
        }
        for (std::string_view const w : {"bad", "poor", "failed", "blocked", "unclear"}) {
            for (std::size_t p = text.find(w); p != std::string::npos; p = text.find(w, p + 1)) ++negative;
        }
        char const* label = positive > negative ? "positive" : (negative > positive ? "negative" : "neutral");
        return text_message(std::string("sentiment{") + label + "}");
    };
}

// The fan-out source: a plain pass-through, like examples/09_concurrent_workflow.cpp:85-87.
[[nodiscard]] ExecutorBody source() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        return text_message(ae::text_of(in));
    };
}

// The router source: a CLASSIFIER. Same signature, but it returns ExecutorOutcome::routes naming the
// one case it selects -- exactly the shape the docs' Router snippet shows (apiContent.ts:2978-2981).
// This body is the graph's single point of nondeterminism-free routing.
[[nodiscard]] std::string classify_document(std::string const& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("budget") != std::string::npos || lower.find("cost") != std::string::npos) {
        return "summary";
    }
    if (lower.find("risk") != std::string::npos || lower.find("incident") != std::string::npos) {
        return "keywords";
    }
    return "sentiment";
}

[[nodiscard]] ExecutorBody classifier() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ExecutorOutcome> {
        std::string const text  = ae::text_of(in);
        std::string const label = classify_document(text);
        return ExecutorOutcome{text_message(text), {label}};
    };
}

// A classifier that names a label no edge in the graph carries -- the routing_failed demonstration.
[[nodiscard]] ExecutorBody classifier_naming_an_undeclared_route() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(ae::text_of(in)), {"legal_review"}};  // never declared
    };
}

// The aggregator: fan-in delivers ONE message carrying one ContentItem per contributing branch, and
// the aggregator runs exactly ONCE for the round. This is the single answer the pipeline produces.
[[nodiscard]] ExecutorBody aggregate() {
    return [](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        return text_message(all_text_of(in));
    };
}

// Wraps a body with an invocation counter so the "aggregator ran exactly once" property is MEASURED,
// not inferred from plausible-looking output -- examples/09_concurrent_workflow.cpp:96-106.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> n) {
    return [body = std::move(body), n](ae::Message const& in,
                                       ae::EffectContext& ctx) -> ae::result<ExecutorOutcome> {
        n->fetch_add(1, std::memory_order_relaxed);
        return body(in, ctx);
    };
}

// A body that fails with an ENGINE error, not an exception.
[[nodiscard]] ExecutorBody worker_that_fails() {
    return [](ae::Message const&, ae::EffectContext&) -> ae::result<ExecutorOutcome> {
        return std::unexpected(ae::error{ae::failure_class::fatal,
                                         "summarize: the document could not be parsed as text",
                                         "pipeline.document_unparseable"});
    };
}

// Appends a tag to the message. Used by the bounded cycle, where the message must grow every round
// so the run cannot converge on its own.
[[nodiscard]] ExecutorBody appender(std::string tag) {
    return [tag = std::move(tag)](ae::Message const& in, ae::EffectContext&) -> ae::result<ae::Message> {
        return text_message(ae::text_of(in) + ">" + tag);
    };
}

// ================================================================================================
// Graph builders. The graph is DATA -- no actors, no scheduler, no runtime decision (014 §1).
// ================================================================================================

// The base product graph: fan_out to three workers, fan_in to one aggregator.
[[nodiscard]] Workflow make_fanout_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-fanout";
    wf.executors = {node_desc("ingest"), node_desc("summarize"), node_desc("keywords"),
                    node_desc("sentiment"), node_desc("aggregate")};
    for (char const* w : {"summarize", "keywords", "sentiment"}) {
        wf.edges.push_back(Edge{"ingest", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "aggregate", edge_kind::fan_in, {}});
    }
    wf.start = "ingest";
    wf.output_selection.push_back("aggregate");
    wf.bound.max_rounds = 8;  // validate_workflow REQUIRES at least one bound -- 014 §2
    return wf;
}

// The SAME graph, but the fan_out edge set is replaced by switch_case edges carrying case labels the
// classifier selects. Same executors, same start, same aggregator, same bound -- only the edge kind
// and the classifier body differ.
//
// MEASURED, and not stated anywhere in the docs: the worker->aggregate edges must ALSO stop being
// `fan_in` here. A `fan_in` target with 2+ declared sources is held until EVERY declared source has
// resolved (rt/workflow_supervisor.hpp:1031-1040, "GitHub issue #52 ... GENERALIZED by issue #62"),
// and `switch_case` fires exactly one source. Leaving them as `fan_in` and firing only one route
// starves the join: the aggregator is never dispatched, the graph runs dry, and the run reports
// `workflow_status::completed` with an EMPTY output. See scenario "mixed-fanin" below, which pins
// that behaviour down rather than asserting it.
[[nodiscard]] Workflow make_router_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-router";
    wf.executors = {node_desc("ingest"), node_desc("summarize"), node_desc("keywords"),
                    node_desc("sentiment"), node_desc("aggregate")};
    struct Route {
        char const* worker;
        char const* label;
    };
    for (Route const& r : {Route{"summarize", "summary"}, Route{"keywords", "keywords"},
                           Route{"sentiment", "sentiment"}}) {
        wf.edges.push_back(Edge{"ingest", r.worker, edge_kind::switch_case, r.label});
        wf.edges.push_back(Edge{r.worker, "aggregate", edge_kind::direct, {}});
    }
    wf.start = "ingest";
    wf.output_selection.push_back("aggregate");
    wf.bound.max_rounds = 8;
    return wf;
}

// The trap, kept deliberately: the same switch_case routing but with the worker->aggregate edges
// left as `fan_in`, exactly as the fan-out variant declares them. Nothing in the docs warns that this
// combination cannot produce an answer.
[[nodiscard]] Workflow make_router_graph_with_fanin_kept() {
    Workflow wf = make_router_graph();
    wf.id       = "doc-pipeline-router-fanin-starved";
    wf.edges.clear();
    struct Route {
        char const* worker;
        char const* label;
    };
    for (Route const& r : {Route{"summarize", "summary"}, Route{"keywords", "keywords"},
                           Route{"sentiment", "sentiment"}}) {
        wf.edges.push_back(Edge{"ingest", r.worker, edge_kind::switch_case, r.label});
        wf.edges.push_back(Edge{r.worker, "aggregate", edge_kind::fan_in, {}});
    }
    return wf;
}

// A bounded cycle that never converges: writer <-> critic, six rounds, then the bound stops it.
[[nodiscard]] Workflow make_bounded_graph() {
    Workflow wf;
    wf.id        = "doc-pipeline-bounded-cycle";
    wf.executors = {node_desc("writer"), node_desc("critic")};
    wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}});
    wf.start = "writer";
    wf.output_selection.push_back("critic");
    wf.bound.max_rounds = 6;
    return wf;
}

// ================================================================================================
// Status handling -- the run's status is a first-class branch, never an assumption.
// ================================================================================================

[[nodiscard]] bool status_is_bounded(workflow_status s) {
    switch (s) {
        case workflow_status::bound_max_rounds:
        case workflow_status::bound_deadline:
        case workflow_status::bound_max_stalls:
        case workflow_status::bound_max_resets:
        case workflow_status::cancelled:
            return true;
        default:
            return false;
    }
}

// The engine's own status string (rt::workflow_status_tag, workflow_supervisor.hpp:492) so nothing
// here paraphrases what the engine said.
[[nodiscard]] char const* engine_status_name(workflow_status s) { return ae::rt::workflow_status_tag(s); }

// Runs a graph and prints the engine's own result. Returns true when the run completed.
bool run_and_report(Workflow const& wf, std::vector<ExecutorBody> bodies, ae::Message const& input,
                    std::shared_ptr<std::atomic<std::uint32_t>> aggregate_calls) {
    std::printf("  graph      : %s -- %zu executors, %zu edges, bound.max_rounds=%u\n", wf.id.c_str(),
                wf.executors.size(), wf.edges.size(), wf.bound.max_rounds.value_or(0));

    // Requirement: validate BEFORE running.
    ae::result<void> const valid = wfg::validate_workflow(wf);
    if (!valid.has_value()) {
        std::fprintf(stderr, "  validation : REFUSED -- %s [code=%s, class=%.*s]\n",
                     valid.error().message.c_str(), valid.error().code.c_str(),
                     static_cast<int>(ae::failure_class_to_string(valid.error().klass).size()),
                     ae::failure_class_to_string(valid.error().klass).data());
        return false;
    }
    std::printf("  validation : accepted\n");

    WorkflowSupervisor sup;
    sup.initialize(wf, std::move(bodies));  // bodies are parallel to wf.executors BY INDEX

    WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{input}));

    std::printf("  status     : %s (rounds=%u)\n", engine_status_name(r.status), r.rounds);
    if (!r.failed_executor.empty()) {
        std::printf("  failed at  : %s\n", r.failed_executor.c_str());
    }
    if (aggregate_calls) {
        std::printf("  fan-in     : aggregator invoked %u time(s) -- exactly once per round\n",
                    aggregate_calls->load(std::memory_order_relaxed));
    }

    if (r.status == workflow_status::completed) {
        std::printf("  answer     : %s\n", all_text_of(r.output).c_str());
        return true;
    }
    if (status_is_bounded(r.status)) {
        std::printf("  partial    : %s\n", all_text_of(r.output).c_str());
        return false;
    }
    std::fprintf(stderr, "  run failed : engine status '%s'\n", engine_status_name(r.status));
    return false;
}

[[nodiscard]] bool same_executor_set(Workflow const& a, Workflow const& b) {
    if (a.executors.size() != b.executors.size()) return false;
    for (std::size_t i = 0; i < a.executors.size(); ++i) {
        if (a.executors[i].id != b.executors[i].id) return false;
    }
    return true;
}

// ================================================================================================
// Scenarios
// ================================================================================================

constexpr char const* kDocument =
    "Q3 budget review: the cost model looks good and the rollout plan is clear. "
    "One risk remains around the incident backlog.";

int scenario_fanout() {
    std::printf("=== scenario: fanout -- 014 §3 Concurrent (fan_out x3, fan_in x1) ===\n");
    auto agg_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    Workflow const wf = make_fanout_graph();
    std::vector<ExecutorBody> bodies = {source(), worker_summarize(), worker_keywords(),
                                        worker_sentiment(), counted(aggregate(), agg_calls)};
    bool const ok = run_and_report(wf, std::move(bodies), text_message(kDocument), agg_calls);
    check(agg_calls->load(std::memory_order_relaxed) == 1,
          "the aggregator ran EXACTLY once -- three inbound fan_in edges merged into one delivery");
    std::printf("  => %s\n\n", ok ? "completed" : "did not complete");
    return ok ? 0 : 1;
}

int scenario_router(std::string const& document) {
    std::printf("=== scenario: router -- the SAME graph, the ingest->worker edge is switch_case ===\n");
    Workflow const fanout = make_fanout_graph();
    Workflow const router = make_router_graph();
    check(same_executor_set(fanout, router), "both variants declare the SAME executors");
    std::printf("  edge kind  : fan_out -> switch_case (label chosen by the classifier body)\n");
    std::printf("  classifier : '%s' -> route '%s'\n", document.c_str(),
                classify_document(document).c_str());

    auto agg_calls = std::make_shared<std::atomic<std::uint32_t>>(0);
    std::vector<ExecutorBody> bodies = {classifier(), worker_summarize(), worker_keywords(),
                                        worker_sentiment(), counted(aggregate(), agg_calls)};
    bool const ok = run_and_report(router, std::move(bodies), text_message(document), agg_calls);
    check(agg_calls->load(std::memory_order_relaxed) == 1,
          "exactly ONE route fired, and the aggregator ran exactly once for it");
    std::printf("  => %s\n\n", ok ? "completed" : "did not complete");
    return ok ? 0 : 1;
}

// The trap above, measured. This is the one scenario whose "success" is that it does NOT mislead:
// the run reports `completed` while producing no answer at all.
int scenario_mixed_fanin() {
    std::printf("=== scenario: mixed-fanin -- switch_case feeding a fan_in join (the trap) ===\n");
    Workflow const wf     = make_router_graph_with_fanin_kept();
    auto           agg    = std::make_shared<std::atomic<std::uint32_t>>(0);
    std::vector<ExecutorBody> bodies = {classifier(), worker_summarize(), worker_keywords(),
                                        worker_sentiment(), counted(aggregate(), agg)};

    ae::result<void> const valid = wfg::validate_workflow(wf);
    check(valid.has_value(), "the graph VALIDATES -- nothing here is an authoring error");
    if (!valid.has_value()) return 1;

    WorkflowSupervisor sup;
    sup.initialize(wf, std::move(bodies));
    WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{text_message(kDocument)}));

    std::printf("  status     : %s (rounds=%u)\n", engine_status_name(r.status), r.rounds);
    std::printf("  fan-in     : aggregator invoked %u time(s)\n",
                agg->load(std::memory_order_relaxed));
    std::printf("  answer     : '%s'\n", all_text_of(r.output).c_str());
    check(r.status == workflow_status::completed && agg->load(std::memory_order_relaxed) == 0 &&
              all_text_of(r.output).empty(),
          "MEASURED: a starved fan_in join yields status=completed with an EMPTY answer -- "
          "validate_workflow accepts it, and no doc warns about it");
    std::printf("  => this graph validates and reports success, yet produces no answer at all\n\n");
    return 0;
}

int scenario_bounded() {
    std::printf("=== scenario: bounded -- a cycle that never converges, stopped by max_rounds ===\n");
    Workflow const wf = make_bounded_graph();
    std::printf("  graph      : %s -- %zu executors, %zu edges, bound.max_rounds=%u\n", wf.id.c_str(),
                wf.executors.size(), wf.edges.size(), wf.bound.max_rounds.value_or(0));
    ae::result<void> const valid = wfg::validate_workflow(wf);
    check(valid.has_value(), "the bounded cycle validates");
    if (!valid.has_value()) return 1;

    WorkflowSupervisor sup;
    sup.initialize(wf, {appender("write"), appender("critique")});
    WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{text_message("draft")}));

    std::printf("  status     : %s (rounds=%u)\n", engine_status_name(r.status), r.rounds);
    bool const bounded_cleanly = status_is_bounded(r.status);
    check(bounded_cleanly, "the bound stopped the loop CLEANLY -- not a crash, not a hang");
    std::printf("  partial    : %s\n", all_text_of(r.output).c_str());
    std::printf("  => %s\n\n", bounded_cleanly ? "bounded (an expected, non-error termination)"
                                               : "unexpected status");
    return bounded_cleanly ? 0 : 1;
}

int scenario_invalid_graph() {
    std::printf("=== scenario: invalid-graph -- validation failure path ===\n");
    Workflow wf = make_fanout_graph();
    wf.bound = {};  // drop the bound: an unbounded workflow does not run (014 §2)

    std::printf("  graph      : %s -- bound deliberately removed\n", wf.id.c_str());
    ae::result<void> const valid = wfg::validate_workflow(wf);
    if (valid.has_value()) {
        std::fprintf(stderr, "  validation : unexpectedly accepted\n");
        return 1;
    }
    std::fprintf(stderr, "  validation : REFUSED -- %s [code=%s, class=%.*s]\n",
                 valid.error().message.c_str(), valid.error().code.c_str(),
                 static_cast<int>(ae::failure_class_to_string(valid.error().klass).size()),
                 ae::failure_class_to_string(valid.error().klass).data());
    std::printf("  => failed before any run started (exit non-zero)\n\n");
    return 1;
}

int scenario_unroutable() {
    std::printf("=== scenario: unroutable -- a route label the graph never declared ===\n");
    Workflow const wf     = make_router_graph();
    auto           unused = std::make_shared<std::atomic<std::uint32_t>>(0);
    std::vector<ExecutorBody> bodies = {classifier_naming_an_undeclared_route(), worker_summarize(),
                                        worker_keywords(), worker_sentiment(),
                                        counted(aggregate(), unused)};

    WorkflowSupervisor sup;
    sup.initialize(wf, std::move(bodies));
    WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{text_message(kDocument)}));

    std::fprintf(stderr, "  status     : %s (rounds=%u)\n", engine_status_name(r.status), r.rounds);
    check(r.status == workflow_status::routing_failed,
          "an undeclared route label fails the run with routing_failed, never silently ignored");
    std::fprintf(stderr, "  => the engine refused to silently ignore an undeclared route (exit non-zero)\n\n");
    return 1;  // any non-completed status is a failure for this scenario
}

int scenario_body_error() {
    std::printf("=== scenario: body-error -- an executor body returns an engine error ===\n");
    Workflow const wf     = make_fanout_graph();
    auto           unused = std::make_shared<std::atomic<std::uint32_t>>(0);
    std::vector<ExecutorBody> bodies = {source(), worker_that_fails(), worker_keywords(),
                                        worker_sentiment(), counted(aggregate(), unused)};

    WorkflowSupervisor sup;
    sup.initialize(wf, std::move(bodies));
    WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{text_message(kDocument)}));

    std::fprintf(stderr, "  status     : %s (rounds=%u)\n", engine_status_name(r.status), r.rounds);
    if (!r.failed_executor.empty()) {
        std::fprintf(stderr, "  failed at  : %s\n", r.failed_executor.c_str());
    }
    std::fprintf(stderr, "  => a faulted worker in a fan-out round failed the run (exit non-zero)\n\n");
    return 1;
}

void usage() {
    std::printf(
        "usage: p3_doc_pipeline [scenario]\n"
        "  all (default)   fanout, router, bounded\n"
        "  fanout          the fan_out/fan_in document pipeline\n"
        "  router          the SAME graph with a switch_case edge chosen by a classifier\n"
        "  bounded         a cycle stopped cleanly by wf.bound.max_rounds\n"
        "  mixed-fanin     switch_case feeding a fan_in join -- validates, 'completes', no answer\n"
        "  invalid-graph   validation failure path (exit non-zero)\n"
        "  unroutable      workflow_status::routing_failed (exit non-zero)\n"
        "  body-error      workflow_status::executor_failed (exit non-zero)\n"
        "  demo-failures   all three failure paths (exit non-zero)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string const scenario = argc > 1 ? std::string(argv[1]) : std::string("all");

    // Three different documents, so the router's classification is what steers the route rather than
    // one branch being hardcoded (examples/10_conditional_routing.cpp:136-160 runs the same graph
    // against two inputs for exactly this reason).
    int rc = 0;
    if (scenario == "all" || scenario == "fanout") rc |= scenario_fanout();
    if (scenario == "all" || scenario == "router") {
        rc |= scenario_router("Q3 budget review: the cost model looks good.");
        rc |= scenario_router("Risk register: the incident backlog is unclear.");
        rc |= scenario_router("Release notes: the docs are great and the build is clear.");
    }
    if (scenario == "all" || scenario == "bounded") rc |= scenario_bounded();
    if (scenario == "mixed-fanin") rc |= scenario_mixed_fanin();
    if (scenario == "invalid-graph" || scenario == "demo-failures") rc |= scenario_invalid_graph();
    if (scenario == "unroutable" || scenario == "demo-failures") rc |= scenario_unroutable();
    if (scenario == "body-error" || scenario == "demo-failures") rc |= scenario_body_error();
    if (scenario == "--help" || scenario == "-h") {
        usage();
        return 0;
    }
    if (scenario != "all" && scenario != "fanout" && scenario != "router" && scenario != "bounded" &&
        scenario != "mixed-fanin" && scenario != "invalid-graph" && scenario != "unroutable" &&
        scenario != "body-error" && scenario != "demo-failures" && scenario != "--help" &&
        scenario != "-h") {
        std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
        usage();
        return 2;
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "p3_doc_pipeline: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    if (rc != 0) {
        std::fprintf(stderr,
                     "p3_doc_pipeline: at least one scenario did not complete successfully -- "
                     "see the engine's own status/error lines above\n");
        return 1;
    }
    std::printf("p3_doc_pipeline: all requested scenarios completed (status: completed)\n");
    return 0;
}
