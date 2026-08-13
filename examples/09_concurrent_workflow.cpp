// AgentEngine "get started" examples, 9 -- fan-out / fan-in (014 §3's "Concurrent" pattern).
//
// Mirrors MAF's samples/03-workflows/Concurrent: one source hands off to several workers that run
// in the SAME superstep round, and a single aggregator merges their results once ALL of them have
// replied. In AgentEngine this needs no separate "concurrent executor" type -- it's the same
// `Workflow` data 04_first_workflow.cpp used, just with `edge_kind::fan_out` (one source, many
// targets, all fired) and `edge_kind::fan_in` (many sources, one target, merged into ONE delivery,
// not one call per inbound edge -- 014 §2's "the superstep model makes fan-in well-defined").
//
// The aggregator receiving exactly one call, not three, is the property this example measures, not
// just infers from correct-looking output -- a fan-in that silently degraded into three separate
// calls would still produce a plausible answer. ADR-037's Quark-free `rt::WorkflowSupervisor`
// (agentengine/rt/workflow_supervisor.hpp) drives the graph via `rt::ThreadPool` fan-out instead of
// separate Quark actors, so there is no `FunctionExecutor::invocations()` counter to read anymore --
// the aggregator's own body is wrapped with a small invocation-counting lambda instead, wiring-only,
// the body function itself (`aggregate`) is unchanged.
//
// Run: ./agentengine_example_09_concurrent_workflow

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

// Fan-in delivers one ContentItem per contributing branch, in graph-declared source order -- an
// aggregator reads them by joining every Text item it was handed.
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

// A worker: uppercases whatever text it's handed and tags it with its own name.
[[nodiscard]] ExecutorBody worker(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        std::string text = text_of(in);
        std::transform(text.begin(), text.end(), text.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text_message(name + ":" + text);
    };
}

[[nodiscard]] agentengine::result<Message> source(Message const& in, EffectContext&) {
    return text_message(text_of(in));
}

[[nodiscard]] agentengine::result<Message> aggregate(Message const& in, EffectContext&) {
    return text_message(all_text_of(in));
}

// Wraps `body` with an invocation counter -- the wiring-level replacement for the old
// `FunctionExecutor::invocations()` actor state, so this example can still measure (not just infer)
// that the aggregator ran exactly once.
[[nodiscard]] ExecutorBody counted(ExecutorBody body, std::shared_ptr<std::atomic<std::uint32_t>> count) {
    return [body = std::move(body), count](Message const& in,
                                           EffectContext& ctx) -> agentengine::result<ExecutorOutcome> {
        count->fetch_add(1, std::memory_order_relaxed);
        return body(in, ctx);
    };
}

[[nodiscard]] Executor node_desc(char const* id) { return Executor{id, executor_kind::function, "T", "T"}; }

// Drives an agentengine::rt::task<T> to completion. Safe here: nothing in a WorkflowSupervisor
// round genuinely suspends on an external wake (fan-out concurrency happens through
// std::future::get(), an ordinary blocking call, not a coroutine suspension) -- the same "safe
// because nothing here genuinely suspends" reasoning every rt:: test file's own drive<T>() relies
// on.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    Workflow wf;
    wf.id        = "fan-out-fan-in";
    wf.executors = {node_desc("src"), node_desc("upper"), node_desc("lower"), node_desc("title"),
                     node_desc("agg")};
    for (char const* w : {"upper", "lower", "title"}) {
        wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "src";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    check(validate_workflow(wf).has_value(), "the graph validates");

    auto agg_invocations = std::make_shared<std::atomic<std::uint32_t>>(0);

    // `bodies` is parallel to `wf.executors` by index: src, upper, lower, title, agg.
    std::vector<ExecutorBody> bodies = {
        source,
        worker("upper"),
        worker("lower"),  // still uppercases -- the NAME is what distinguishes it
        worker("title"),
        counted(aggregate, agg_invocations),
    };
    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
    check(r.status == workflow_status::completed, "the graph terminates by running dry (014 §2)");
    check(r.rounds == 3, "src, workers, aggregator = 3 rounds -- the fan-out round runs ONCE, "
                          "not once per worker");
    check(agg_invocations->load(std::memory_order_relaxed) == 1,
          "the aggregator ran EXACTLY ONCE -- three inbound fan_in edges merged into one "
          "delivery, not three separate calls (014 §2's own claim: 'makes fan-in well-defined')");
    std::string const output = all_text_of(r.output);
    std::printf("%s\n", output.c_str());
    check(output == "upper:HI + lower:HI + title:HI",
          "the aggregator saw all three branches, in graph-declared order");

    std::fprintf(stderr, g_failures == 0 ? "example_09_concurrent_workflow: OK\n"
                                          : "example_09_concurrent_workflow: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
