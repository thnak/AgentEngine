// devsim/rerun7/main.cpp -- a document-processing pipeline built on AgentEngine's workflow graph.
//
// A developer-facing rebuild of: fan-out to three workers, fan-in to one aggregator, a termination
// bound validated before the run, the result read out and the run's status branched on, a second
// variant of the SAME graph whose routing edge is a switch_case chosen by a classifier, and a
// failure path that prints the engine's own error text and exits non-zero.
//
// Built from the docs (web/marketing/src/{data/apiContent.ts,components/ApiWorkflowReference.tsx,
// components/ApiFirstAppReference.tsx}) alone; see REPORT.md for every place that failed and had to
// be reverse-engineered from include/.
//
// Implements 014-Workflow-and-Orchestration.md §2 (termination bound) / §3 (Concurrent, Router).

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// -- The five headers the Workflow & Orchestration page names as "what to include" --------------
// web/marketing/src/components/ApiWorkflowReference.tsx:23-29.
#include "agentengine/core/content.hpp"                    // Message, ContentItem, Text
#include "agentengine/rt/block_on.hpp"                     // rt::block_on -- drive the coroutine from main()
#include "agentengine/rt/workflow_supervisor.hpp"          // WorkflowSupervisor, WorkflowResult, ExecutorBody
#include "agentengine/workflow/graph.hpp"                  // Workflow, Executor, Edge, validate_workflow
#include "agentengine/workflow/workflow_event.hpp"         // live-view vocabulary (unused here, but named)

// The engine's own text reader (core/tool_call_extraction.hpp:83). The docs name it as real engine
// API -- ApiSnippetConventions.tsx:29 -- and as NOT living in any of the five headers above.
#include "agentengine/core/tool_call_extraction.hpp"

// -- Namespaces. Three of them, and the docs are explicit that all three are needed -------------
// ApiWorkflowReference.tsx:122-137: the DATA is agentengine::workflow, the RUNNING types are
// agentengine::rt, and agentengine::sharing_mode is agentengine -- a third namespace a
// `using namespace agentengine::workflow;` does not reach.
using namespace agentengine;             // Message, ContentItem, Text, result, error, sharing_mode
using namespace agentengine::workflow;   // Workflow, Executor, Edge, edge_kind, executor_kind
using namespace agentengine::rt;         // WorkflowSupervisor, ExecutorBody, RunWorkflow, workflow_status

namespace {

// ------------------------------------------------------------------------------------------------
// Helpers the docs describe but do not ship. ApiSnippetConventions.tsx:32-33 lists text_message /
// all_text_of / node_desc / counted / handler as "example-local helpers on the workflow pages ...
// none is engine API". text_of IS engine API, and is included above rather than re-implemented.
// ------------------------------------------------------------------------------------------------

// A user-role Message carrying one text part (the example-local `text_message`).
[[nodiscard]] Message user_text(std::string text) {
    Message m{};
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// An executor that needs no per-node data: an id, the default function kind, and a Text->Text port
// pair (the example-local `node_desc`). Executor's other two members keep their documented defaults
// -- worktree_mode == branch, batch == false (apiContent.ts:3007-3022).
[[nodiscard]] Executor node_desc(std::string id) {
    return Executor{std::move(id), executor_kind::function, "Text", "Text"};
}

// Counts invocations of another body, so the run's own claims can be checked rather than trusted.
// A workflow body is a copy that can outlive the scope that built it, so the counter is shared by
// value, not captured by reference (apiContent.ts:3119-3121).
[[nodiscard]] ExecutorBody counted(ExecutorBody inner, std::shared_ptr<std::atomic<std::uint32_t>> n) {
    return [inner = std::move(inner), n](Message const& in, EffectContext& ctx) -> result<ExecutorOutcome> {
        n->fetch_add(1, std::memory_order_relaxed);
        return inner(in, ctx);
    };
}

[[nodiscard]] std::shared_ptr<std::atomic<std::uint32_t>> counter() {
    return std::make_shared<std::atomic<std::uint32_t>>(0);
}

// Prints the engine's own error, not a paraphrase of it.
void print_error(char const* what, error const& e) {
    std::fprintf(stderr, "%s: %s (%s)\n", what, e.message.c_str(), e.code.c_str());
}

// ------------------------------------------------------------------------------------------------
// The pipeline's own vocabulary. A document arrives as one line; every stage passes a growing
// "key=value;key=value" record along, which is what makes the fan-in merge observable by eye.
// ------------------------------------------------------------------------------------------------
constexpr std::string_view kDoc =
    "Invoice 1042 disputed. Ana Reyes reported on 2026-03-04 that the charge did not match "
    "the signed contract.";

[[nodiscard]] std::string field(std::string_view text, std::string_view key) {
    std::string const needle = std::string(key) + "=";
    auto const        at     = text.find(needle);
    if (at == std::string_view::npos) return {};
    auto const begin = at + needle.size();
    auto const end   = text.find(';', begin);
    return std::string{text.substr(begin, end == std::string_view::npos ? end : end - begin)};
}

// Splits a comma-separated field into sorted, duplicate-free tokens.
[[nodiscard]] std::vector<std::string> tokens_of(std::string_view field_value) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (start <= field_value.size() && !field_value.empty()) {
        auto const comma = field_value.find(',', start);
        auto const piece = field_value.substr(start, comma == std::string_view::npos ? comma : comma - start);
        if (!piece.empty()) out.emplace_back(piece);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// The three fan-out workers and the fan-in aggregator.
// ------------------------------------------------------------------------------------------------

// ingest -> the fan-out source. Normalizes the document into the pipeline's own record format.
[[nodiscard]] result<Message> ingest(Message const& in, EffectContext&) {
    return user_text("doc=" + text_of(in));
}

// Worker 1: the capitalized words, as a stand-in for named-entity extraction.
[[nodiscard]] result<Message> entities(Message const& in, EffectContext&) {
    std::string const doc  = field(text_of(in), "doc");
    std::string       hits;
    bool              in_word = false;
    std::string       word;
    auto              flush   = [&] {
        if (word.size() > 1 && std::isupper(static_cast<unsigned char>(word.front())) != 0) {
            if (!hits.empty()) hits += ',';
            hits += word;
        }
        word.clear();
    };
    for (char const c : doc) {
        bool const alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
        if (alnum) {
            in_word = true;
            word.push_back(c);
        } else if (in_word) {
            in_word = false;
            flush();
        }
    }
    if (in_word) flush();
    return user_text(text_of(in) + ";entities=" + hits);
}

// Worker 2: the dates.
[[nodiscard]] result<Message> dates(Message const& in, EffectContext&) {
    std::string const doc = field(text_of(in), "doc");
    std::string       hits;
    for (std::size_t i = 0; i + 10 <= doc.size(); ++i) {
        bool const shaped = std::isdigit(static_cast<unsigned char>(doc[i])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 1])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 2])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 3])) != 0 && doc[i + 4] == '-' &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 5])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 6])) != 0 && doc[i + 7] == '-' &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 8])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(doc[i + 9])) != 0;
        if (!shaped) continue;
        if (!hits.empty()) hits += ',';
        hits += doc.substr(i, 10);
    }
    return user_text(text_of(in) + ";dates=" + hits);
}

// Worker 3: the tokens that carry the dispute, as a stand-in for a keyword pass.
[[nodiscard]] result<Message> keywords(Message const& in, EffectContext&) {
    std::string const doc = field(text_of(in), "doc");
    std::string       hits;
    for (std::string_view const k : {"disputed", "charge", "contract", "invoice"}) {
        if (doc.find(k) != std::string::npos) {
            if (!hits.empty()) hits += ',';
            hits += k;
        }
    }
    return user_text(text_of(in) + ";keywords=" + hits);
}

// The fan-in aggregator. It runs ONCE per round and receives ONE ContentItem per contributing
// branch, in graph-declared order (apiContent.ts:2862). It prints exactly what it was handed, so
// the merge is visible in the program's own output -- and it reads the delivery per ITEM, because
// the engine's own text_of() concatenates every text part with no separator.
[[nodiscard]] result<Message> aggregate(Message const& in, EffectContext&) {
    std::vector<std::string> branches;  // one entry per contributing branch, in delivery order
    for (auto const& item : in.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) branches.push_back(t->text);
    }
    std::printf("[aggregate] one delivery, %zu ContentItem(s), in graph-declared order:\n",
                branches.size());
    for (std::size_t i = 0; i < branches.size(); ++i) {
        std::printf("            [%zu] %s\n", i + 1, branches[i].c_str());
    }
    // Merge: union the three per-worker token sets, in graph-declared order.
    std::string merged;
    for (std::string_view const key : {"entities", "dates", "keywords"}) {
        for (std::string const& branch : branches) {
            for (std::string const& v : tokens_of(field(branch, key))) {
                if (merged.find(v) == std::string::npos) {
                    if (!merged.empty()) merged += ',';
                    merged += v;
                }
            }
        }
    }
    // A canonical record: the document once, then the merged field. Everything downstream reads
    // fields off a single text part instead of off a concatenation.
    std::string const doc = branches.empty() ? std::string{} : field(branches.front(), "doc");
    std::string       entity_hits;
    for (std::string_view const key : {"entities"}) {
        for (std::string const& branch : branches) {
            for (std::string const& v : tokens_of(field(branch, key))) {
                if (entity_hits.find(v) == std::string::npos) {
                    if (!entity_hits.empty()) entity_hits += ',';
                    entity_hits += v;
                }
            }
        }
    }
    return user_text("doc=" + doc + ";entities=" + entity_hits + ";merged=" + merged);
}

// ------------------------------------------------------------------------------------------------
// Variant A: ingest -(fan_out)-> {entities, dates, keywords} -(fan_in)-> aggregate -> summarize.
// A second direct edge out of the aggregator, so both output selections exist in one run.
// ------------------------------------------------------------------------------------------------
[[nodiscard]] result<Message> summarize(Message const& in, EffectContext&) {
    return user_text(text_of(in) + ";summary=3 branches merged, " +
                     std::to_string(tokens_of(field(text_of(in), "merged")).size()) + " distinct tokens");
}

// ------------------------------------------------------------------------------------------------
// Variant B: the SAME graph, plus a classifier whose typed decision selects exactly one of two
// switch_case edges. The classifier's label is the ONLY thing that decides which sink runs
// (apiContent.ts:3091-3106; ApiWorkflowReference.tsx:388 on routing_failed).
// ------------------------------------------------------------------------------------------------
// A body that ROUTES must name its result type as ExecutorOutcome, not Message: the Message ->
// ExecutorOutcome conversion the docs describe (apiContent.ts:3172-3175) does not carry through
// std::expected, so `result<Message>` will not convert to `result<ExecutorOutcome>`.
[[nodiscard]] result<ExecutorOutcome> classify(Message const& in, EffectContext&) {
    std::string const text = text_of(in);
    bool const        has_entities = !field(text, "entities").empty();
    std::string const label        = has_entities ? "structured" : "plain";
    return ExecutorOutcome{user_text(text + ";route=" + label), {label}};
}
[[nodiscard]] result<Message> structured_sink(Message const& in, EffectContext&) {
    return user_text(text_of(in) + ";structured=entities present, table form");
}

[[nodiscard]] result<Message> plain_sink(Message const& in, EffectContext&) {
    return user_text(text_of(in) + ";plain=no entities, prose form");
}

// ------------------------------------------------------------------------------------------------
// The graphs. Both are pure data (apiContent.ts:3000-3050).
// ------------------------------------------------------------------------------------------------
[[nodiscard]] Workflow make_variant_a() {
    Workflow wf;
    wf.id        = "docpipeline-a";
    wf.executors = {node_desc("ingest"), node_desc("entities"), node_desc("dates"),
                    node_desc("keywords"), node_desc("aggregate"), node_desc("summarize")};
    wf.edges.push_back(Edge{"ingest", "entities", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"ingest", "dates", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"ingest", "keywords", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"entities", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"dates", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"keywords", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"aggregate", "summarize"});
    wf.start = "ingest";
    wf.output_selection.push_back("summarize");
    wf.bound.max_rounds = 8;  // REQUIRED: an unbounded graph does not run (apiContent.ts:3040)
    return wf;
}

[[nodiscard]] Workflow make_variant_b() {
    Workflow wf;
    wf.id        = "docpipeline-b";
    wf.executors = {node_desc("ingest"), node_desc("entities"), node_desc("dates"),
                    node_desc("keywords"), node_desc("aggregate"), node_desc("classify"),
                    node_desc("structured"), node_desc("plain")};
    wf.edges.push_back(Edge{"ingest", "entities", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"ingest", "dates", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"ingest", "keywords", edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{"entities", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"dates", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"keywords", "aggregate", edge_kind::fan_in, {}});
    wf.edges.push_back(Edge{"aggregate", "classify"});
    // The one edge this variant changes: a switch_case whose label is the classifier's decision.
    wf.edges.push_back(Edge{"classify", "structured", edge_kind::switch_case, "structured"});
    wf.edges.push_back(Edge{"classify", "plain", edge_kind::switch_case, "plain"});
    wf.start = "ingest";
    wf.output_selection = {"structured", "plain"};
    wf.bound.max_rounds = 10;
    return wf;
}

// A graph that must be REFUSED: same shape, no bound at all.
[[nodiscard]] Workflow make_unbounded() {
    Workflow wf     = make_variant_a();
    wf.id           = "docpipeline-unbounded";
    wf.bound        = TerminationBound{};  // no max_rounds, no deadline: the author forgot
    return wf;
}

// A graph that must be REFUSED: a switch_case edge carrying no case label is unroutable.
[[nodiscard]] Workflow make_case_label_missing() {
    Workflow wf = make_variant_b();
    wf.id       = "docpipeline-unlabelled-case";
    for (auto& e : wf.edges) {
        if (e.kind == edge_kind::switch_case && e.to == "structured") e.case_label.clear();
    }
    return wf;
}

// A graph that must be REFUSED: a case label on an edge kind that carries none.
[[nodiscard]] Workflow make_phoney_case_label() {
    Workflow wf = make_variant_a();
    wf.id       = "docpipeline-phoney-label";
    for (auto& e : wf.edges) {
        if (e.from == "aggregate") e.case_label = "structured";  // a plain direct edge
    }
    return wf;
}

// ------------------------------------------------------------------------------------------------
// The failure path. Validation first, exactly as the docs prescribe: "check it before
// initialize(), and treat invalid from a run as the backstop" (ApiWorkflowReference.tsx:414-426).
// Returns false and prints the engine's own error when the graph is unacceptable.
// ------------------------------------------------------------------------------------------------
[[nodiscard]] bool validate_or_report(Workflow const& wf) {
    auto valid = validate_workflow(wf);
    if (!valid) {
        std::fprintf(stderr, "REFUSED graph '%s'\n", wf.id.c_str());
        print_error("  validate_workflow", valid.error());
        return false;
    }
    auto runnable = check_workflow_executable(wf);
    if (!runnable) {
        std::fprintf(stderr, "REFUSED graph '%s'\n", wf.id.c_str());
        print_error("  check_workflow_executable", runnable.error());
        return false;
    }
    return true;
}

// Branches on the run's status rather than assuming success. Returns the process exit code.
[[nodiscard]] int report(WorkflowResult const& r, std::uint32_t aggregate_calls, bool expect_aggregate_once) {
    std::printf("[status] %s after %u round(s)", workflow_status_tag(r.status), r.rounds);
    if (!r.failed_executor.empty()) std::printf(" -- failed_executor='%s'", r.failed_executor.c_str());
    std::printf("\n");
    if (expect_aggregate_once) {
        std::printf("[check] aggregator invoked %u time(s) -- fan_in is one delivery, not one per edge\n",
                    aggregate_calls);
        if (aggregate_calls != 1) {
            std::fprintf(stderr, "FAILED: aggregator ran %u times, expected exactly 1\n", aggregate_calls);
            return 1;
        }
    }
    switch (r.status) {
        case workflow_status::completed:
            break;
        case workflow_status::bound_max_rounds:
        case workflow_status::bound_deadline:
        case workflow_status::bound_max_stalls:
        case workflow_status::bound_max_resets:
        case workflow_status::cancelled:
            std::fprintf(stderr, "run ended on a bound, not a fault: %s\n", workflow_status_tag(r.status));
            return 3;
        case workflow_status::suspended:
            std::fprintf(stderr, "run suspended on %zu open interaction(s)\n", r.open_interactions.size());
            return 3;
        default:
            std::fprintf(stderr, "run failed: %s", workflow_status_tag(r.status));
            if (!r.failed_executor.empty()) std::fprintf(stderr, " (node '%s')", r.failed_executor.c_str());
            std::fprintf(stderr, "\n");
            for (auto const& p : r.partial) {
                std::fprintf(stderr, "  partial: %s round %u -> %.80s\n", p.executor_id.c_str(), p.round,
                             text_of(p.payload).c_str());
            }
            return 4;
    }

    // "completed" is not the same as "there is an answer" (ApiWorkflowReference.tsx:380).
    std::string const answer = text_of(r.output);
    if (answer.empty()) {
        std::fprintf(stderr, "completed with an EMPTY output -- the starved-join caveat; do not trust this\n");
        return 5;
    }
    std::printf("[result] %s\n", answer.c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------
// One full variant: validate, initialize, run, report.
// ------------------------------------------------------------------------------------------------
int run_variant(char const* which, std::string doc = std::string{kDoc}) {
    bool const     variant_b = std::string_view{which} == "b";
    Workflow       wf        = variant_b ? make_variant_b() : make_variant_a();
    auto           agg_calls = counter();
    auto           sink_hits = counter();

    std::printf("== variant %s: %s ==\n", which, wf.id.c_str());

    // 1. Validate BEFORE running anything.
    if (!validate_or_report(wf)) return 2;

    // 2. Bodies, in the SAME ORDER as wf.executors -- that positional match is the entire contract
    //    between a body and its node (apiContent.ts:3086-3087, 3364-3365).
    std::string       ran_sink;
    std::vector<ExecutorBody> bodies;
    if (variant_b) {
        bodies = {ingest, entities, dates, keywords, counted(aggregate, agg_calls), classify,
                  [&](Message const& in, EffectContext& ctx) -> result<Message> {
                      *sink_hits += 1;
                      ran_sink += "structured";
                      return structured_sink(in, ctx);
                  },
                  [&](Message const& in, EffectContext& ctx) -> result<Message> {
                      *sink_hits += 1;
                      ran_sink += "plain";
                      return plain_sink(in, ctx);
                  }};
    } else {
        bodies = {ingest, entities, dates, keywords, counted(aggregate, agg_calls), summarize};
    }

    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);

    // 3. Run it, synchronously, from main().
    WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{user_text(std::move(doc))}));

    // 4. Get the result out and branch on the status.
    int const rc = report(r, agg_calls->load(std::memory_order_relaxed), true);
    if (rc != 0) return rc;

    // The classifier's own label chose the edge, and the engine is what turned it into a route:
    // exactly one sink ran, and the unselected sink's body was never invoked (apiContent.ts:3106).
    if (variant_b) {
        std::string const   route = field(text_of(r.output), "route");
        std::uint32_t const hits  = sink_hits->load(std::memory_order_relaxed);
        std::printf("[check] classifier chose '%s'; sink bodies invoked: %u (%s)\n", route.c_str(), hits,
                    ran_sink.c_str());
        if (hits != 1 || ran_sink != route) {
            std::fprintf(stderr, "FAILED: switch_case fired %u sink(s) for label '%s'\n", hits, route.c_str());
            return 1;
        }
    }
    return 0;
}

// A run-level fault, on a graph that VALIDATES: the classifier names a route label the graph never
// declared. The engine reports routing_failed and names the offending node
// (ApiWorkflowReference.tsx:305,388; apiContent.ts:2844-2847).
int run_routing_fault() {
    std::printf("== failure path: a classifier naming an undeclared route ==\n");
    Workflow wf = make_variant_b();
    wf.id       = "docpipeline-bad-route";
    if (!validate_or_report(wf)) return 2;  // the graph itself is fine -- this is a RUN failure

    std::vector<ExecutorBody> bodies = {
        ingest, entities, dates, keywords, counted(aggregate, counter()),
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{user_text(text_of(in)), {"html"}};  // neither label is declared
        },
        structured_sink, plain_sink};
    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);
    WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{user_text(std::string{kDoc})}));
    int const rc = report(r, 0, false);
    if (rc == 0) {
        std::fprintf(stderr, "FAILED: an undeclared route was expected to fail the run\n");
        return 1;
    }
    return rc;
}

// The failure path, as its own mode so its non-zero exit is observable.
int run_broken() {
    std::printf("== failure path: an unbounded graph ==\n");
    Workflow wf = make_unbounded();
    if (!validate_or_report(wf)) {
        std::fprintf(stderr, "exit 2: refused before any run started -- nothing executed\n");
        return 2;
    }
    // The backstop the docs describe: a graph that slipped past validate_workflow still surfaces as
    // workflow_status::invalid at the first run entry point (ApiWorkflowReference.tsx:416-421).
    WorkflowSupervisor        sup;
    std::vector<ExecutorBody> bodies = {ingest, entities, dates, keywords,
                                        counted(aggregate, counter()), summarize};
    sup.initialize(wf, bodies);
    WorkflowResult r = block_on(sup.run_workflow(RunWorkflow{user_text(std::string{kDoc})}));
    if (r.status != workflow_status::completed) {
        std::fprintf(stderr, "run refused: status=%s rounds=%u\n", workflow_status_tag(r.status), r.rounds);
        return 4;
    }
    return 0;
}

// Every malformed graph must be refused by the engine's own validator, with the engine's own code.
int run_refusals() {
    std::printf("== failure path: three malformed graphs, each refused by the engine ==\n");
    int refused = 0;
    for (Workflow const& wf : {make_unbounded(), make_case_label_missing(), make_phoney_case_label()}) {
        if (!validate_or_report(wf)) ++refused;
        std::printf("\n");
    }
    std::printf("(refused %d of 3; each printed error is the engine's own message and code)\n", refused);
    return refused == 3 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "a";
    std::string doc  = argc > 2 ? argv[2] : std::string{kDoc};
    if (mode == "--help") {
        std::printf("usage: docpipeline [a|b|broken|badroute|refuse|all] [document]\n"
                    "  a        fan_out + fan_in, output_selection = summarize\n"
                    "  b        the SAME graph, aggregator -> classifier switch_case -> one of two sinks\n"
                    "  broken   a deliberately unbounded graph: proves the failure path exits non-zero\n"
                    "  badroute a validating graph whose classifier names an undeclared route\n"
                    "  refuse   three malformed graphs, each refused by validate_workflow\n"
                    "  all      a, b twice with two documents, then every failure path (default: a)\n");
        return 0;
    }
    if (mode == "all") {
        int const a = run_variant("a");
        std::printf("\n");
        int const b = run_variant("b");
        std::printf("\n");
        // The second document has no capitalized words, so the classifier's own decision changes --
        // the proof that one branch is not hardcoded (apiContent.ts:3208-3210).
        int const b2 = run_variant("b", "the refund arrived late, so we filed a complaint.");
        std::printf("\n");
        int const broken = run_broken();  // expected non-zero; `all` still succeeds overall
        std::printf("\n");
        int const refusals = run_refusals();
        std::printf("\n");
        int const badroute = run_routing_fault();  // expected non-zero
        std::printf("\n(variant a exit %d, variant b exit %d, variant b/2 exit %d, broken-graph exit %d, "
                    "refusals exit %d, bad-route exit %d)\n",
                    a, b, b2, broken, refusals, badroute);
        return (a == 0 && b == 0 && b2 == 0 && broken != 0 && refusals == 0 && badroute != 0) ? 0 : 1;
    }
    if (mode == "a" || mode == "b") return run_variant(mode.c_str(), std::move(doc));
    if (mode == "broken") return run_broken();
    if (mode == "badroute") return run_routing_fault();
    if (mode == "refuse") return run_refusals();
    std::fprintf(stderr, "unknown mode '%s' (try --help)\n", mode.c_str());
    return 64;
}
