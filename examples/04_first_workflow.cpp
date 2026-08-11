// AgentEngine "get started" examples, 4 of 4 -- a two-node workflow.
//
// Mirrors Microsoft Agent Framework's samples/01-get-started/05_first_workflow: a small text
// pipeline of two executors connected by an edge -- uppercase the input, then reverse it. For
// input "Hello, World!" the workflow produces "!DLROW ,OLLEH", exactly like the MAF sample it
// mirrors.
//
// AgentEngine's workflow model (014-Workflow-and-Orchestration.md) is its own thing, not a
// `WorkflowBuilder` fluent API: a `Workflow` is DATA (`executors`/`edges`/`start`/
// `output_selection`/`bound`, agentengine/workflow/graph.hpp), executed round-by-round (a
// "superstep" per round, 014 §2) by a `WorkflowSupervisor` actor that `co_await`s each round's
// executors and only starts round N+1 once every round-N executor has replied.
//
// Unlike 01-03, this needs a REAL `quark::Engine`, not `quark::TestKit`: the supervisor's whole job
// is `co_await`-ing cross-actor asks across a superstep barrier, which TestKit (no async carrier)
// cannot host. This is the same construction shape `tests/test_workflow_superstep.cpp` uses,
// narrated down to the two-node minimum. 4 workers / 4 shards per CLAUDE.md's machine-safety rule
// for anything that spins up a real engine.
//
// Run: ./agentengine_example_04_first_workflow

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/workflow/executor.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/placement.hpp"
#include "agentengine/workflow/supervisor.hpp"

using namespace quark;
using namespace agentengine;
using namespace agentengine::workflow;

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

// Two executor bodies -- plain functions over a `Message`, no agent, no model call. `ExecutorBody`
// (workflow/executor.hpp) is exactly this: `std::function<result<ExecutorOutcome>(Message const&,
// EffectContext&)>`, and a `Message` converts implicitly into an `ExecutorOutcome` for the common
// "just pass a message downstream" case.
// `agentengine::result`, qualified: `quark::result` is also in scope from `using namespace quark;`
// above, and the two are different templates -- the same disambiguation
// `tests/test_workflow_superstep.cpp`'s own executor bodies use.
[[nodiscard]] agentengine::result<Message> uppercase(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::transform(text.begin(), text.end(), text.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text_message(std::move(text));
}

[[nodiscard]] agentengine::result<Message> reverse_text(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::reverse(text.begin(), text.end());
    return text_message(std::move(text));
}

}  // namespace

int main() {
    auto const config = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(config.has_value(), "engine config builds (4 workers / 4 shards)");
    if (!config) return 1;

    Engine<>            engine(*config);
    detail::MessagePool pool{64};
    LocalRouter          router(engine.post_courier(), pool);

    // Both executors are the SAME actor type (`FunctionExecutor` -- a graph's shape stays data, not
    // a compile-time property of the host program), so their placement on the engine's shards is
    // decided entirely by instance key. Sequential keys (1, 2, ...) do NOT spread evenly across
    // shards (placement.hpp's own file banner measured this for real) -- `spread_executor_keys` is
    // the workflow layer's fix, used here even for a 2-node graph so this example demonstrates the
    // pattern a larger graph actually needs.
    auto const keys = spread_executor_keys(
        2, engine.shard_count(),
        [&](std::uint64_t k) { return engine.shard_of(actor_id_of<FunctionExecutor>(k)); });

    FunctionExecutor uppercase_node;
    uppercase_node.initialize("Uppercase", uppercase, EffectContext{});
    auto uppercase_activation = make_workflow_activation(uppercase_node, pool.sink());
    engine.register_activation(actor_id_of<FunctionExecutor>(keys[0]), *uppercase_activation);

    FunctionExecutor reverse_node;
    reverse_node.initialize("Reverse", reverse_text, EffectContext{});
    auto reverse_activation = make_workflow_activation(reverse_node, pool.sink());
    engine.register_activation(actor_id_of<FunctionExecutor>(keys[1]), *reverse_activation);

    // The graph itself: two executors, one edge, "Uppercase" first, "Reverse"'s output selected.
    Workflow wf;
    wf.id        = "uppercase-then-reverse";
    wf.executors = {Executor{"Uppercase", executor_kind::function, "Text", "Text"},
                     Executor{"Reverse", executor_kind::function, "Text", "Text"}};
    wf.edges.push_back(Edge{"Uppercase", "Reverse", edge_kind::chain, {}});
    wf.start = "Uppercase";
    wf.output_selection.push_back("Reverse");
    wf.bound.max_rounds = 4;
    check(validate_workflow(wf).has_value(), "the graph validates");

    std::vector<ActorRef<FunctionExecutor>> refs = {router.get<FunctionExecutor>(keys[0]),
                                                       router.get<FunctionExecutor>(keys[1])};
    WorkflowSupervisor supervisor;
    supervisor.initialize(wf, refs);
    auto sup_activation = make_workflow_activation(supervisor, pool.sink());
    constexpr std::uint64_t kSupervisorKey = 100;
    engine.register_activation(actor_id_of<WorkflowSupervisor>(kSupervisorKey), *sup_activation);
    ActorRef<WorkflowSupervisor> sup = router.get<WorkflowSupervisor>(kSupervisorKey);

    engine.start();
    quark::result<WorkflowResult> r =
        block_on(sup.ask<WorkflowResult>(RunWorkflow{text_message("Hello, World!")}));
    check(r.has_value(), "the workflow run completes");
    if (r.has_value()) {
        check(r->status == workflow_status::completed, "the chain terminates by running dry (014 §2)");
        check(r->rounds == 2, "exactly one round per edge in the chain");
        std::string const output = text_of(r->output);
        std::printf("%s\n", output.c_str());
        check(output == "!DLROW ,OLLEH", "input threaded through both nodes in graph order");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_04_first_workflow: OK\n"
                                          : "example_04_first_workflow: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
