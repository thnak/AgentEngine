// A CHARACTERIZATION test pinning a real, currently-unfixed gap: `edge_failure_policy::propagate`
// and `edge_failure_policy::fallback` (014 §6) do not compose correctly with a `fan_in` target that a
// SIBLING executor also delivers to normally in the SAME round -- exactly the shape a concurrent
// research fan-out with one degraded specialist needs (014 §3's "Concurrent" pattern with one branch
// recovering from failure).
//
// Found live 2026-09-03 via tests/test_workflow_research_pipeline_live_e2e.cpp -- a production-shaped
// workflow against REAL OpenRouter calls, where one specialist's edge used `fallback` and the final
// report silently lost its two successful specialists' findings. This file reproduces the SAME root
// cause offline, fast, and deterministically, isolated from live-network nondeterminism.
//
// ROOT CAUSE (include/agentengine/rt/workflow_supervisor.hpp's `route_from()`, the `propagate`/
// `fallback` switch): the normal-path merge loop APPENDS onto an existing `Delivery` for a `fan_in`
// target if one already exists this round. The failure-path's `deliver_once()` does NOT -- it inserts
// only if the target is ABSENT and otherwise silently no-ops. Concretely:
//
//   - `propagate`: if a succeeding sibling's normal delivery already created the fan_in target's
//     `next` entry this round, the failure marker is DROPPED with no trace -- the target runs once,
//     with no indication a third branch ever existed. (This is ORDER-DEPENDENT: if the failing
//     executor happens to be processed first instead, the marker becomes the initial entry and the
//     later siblings correctly merge onto it -- see F2 below.)
//   - `fallback`: the named recovery executor is a DIFFERENT node with its own edge back to the
//     shared target, which can only fire in a LATER round. The fan_in target therefore runs TWICE,
//     and `WorkflowResult::partial`'s "at most one entry per executor_id" rule means the SECOND
//     invocation's output silently OVERWRITES the first -- the succeeding siblings' real content is
//     gone from the final result.
//
// `tests/test_rt_workflow_supervisor_failure_policies.cpp`'s own D4 only ever exercises `fallback` on
// a single-source (non-fan_in-shared) edge, so this combination was never gate-proven despite 014 §8
// G1's "each pattern... under injected executor failures" language.
//
// NOT FIXED HERE -- `route_from()` is hot-path/correctness-critical routing logic shared by every
// workflow pattern in this engine; CLAUDE.md reserves changes there for a real
// design→red-team→prove→judge pass, not an inline patch discovered mid-task. This test exists to PIN
// the current, disclosed behavior so it cannot silently regress further, and so a future fix has a
// concrete, reproducible target: when `route_from()` is fixed to correctly merge a failure marker (or
// a recovery executor's output) into an ALREADY-populated fan_in delivery within the same round, F1's
// "dropped" and F2's "order-dependent" and F3's "overwritten" assertions below will start FAILING --
// that is the fix working. Update them then to assert the correct merged-content behavior instead of
// loosening or deleting them.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
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

// Renders Text verbatim and Error as "ERR:<message>" -- a failure marker (failure_marker(),
// workflow_supervisor.hpp) arrives as an Error content item, not Text.
[[nodiscard]] std::string render(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (!out.empty()) out += " | ";
        if (auto const* t = std::get_if<Text>(&item.value)) {
            out += t->text;
        } else if (auto const* e = std::get_if<Error>(&item.value)) {
            out += "ERR:" + e->message;
        } else {
            out += "?";
        }
    }
    return out;
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] ExecutorBody ok_body(std::string tag) {
    return [tag](Message const&, EffectContext&) -> result<Message> { return text_message(tag); };
}

[[nodiscard]] result<Message> always_fails(Message const&, EffectContext&) {
    return std::unexpected(error{failure_class::contract, "bad always fails", "probe.bad"});
}

}  // namespace

int main() {
    // ==== F1: `propagate` on a fan_in edge whose target ALREADY has a sibling's delivery this round
    //      (siblings dispatched/processed BEFORE the failing one) -- the marker is silently dropped ===
    {
        Workflow wf;
        wf.id        = "f1-propagate-marker-dropped";
        wf.executors = {node_desc("src"), node_desc("okA"), node_desc("okB"), node_desc("bad"),
                         node_desc("agg")};
        wf.edges     = {
            Edge{"src", "okA", edge_kind::fan_out, {}},
            Edge{"src", "okB", edge_kind::fan_out, {}},
            Edge{"src", "bad", edge_kind::fan_out, {}},
            Edge{"okA", "agg", edge_kind::fan_in, {}},
            Edge{"okB", "agg", edge_kind::fan_in, {}},
            // Declared LAST, after okA/okB -- exec_deliveries processes edges/executors in
            // declaration order, so okA/okB's normal merge creates agg's `next` entry first.
            Edge{"bad", "agg", edge_kind::fan_in, {},
                 EdgeFailurePolicy{edge_failure_policy::propagate, 0, {}}},
        };
        wf.start            = "src";
        wf.output_selection = {"agg"};
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "F1: the graph validates");

        auto agg_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> result<Message> { return in; },
            ok_body("A-ok"),
            ok_body("B-ok"),
            always_fails,
            [agg_calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                ++*agg_calls;
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));

        check(r.status == workflow_status::completed,
              "F1: the workflow completes (propagate keeps it running, unlike fail)");
        check(*agg_calls == 1, "F1: the fan_in target still runs exactly once");
        std::string const out = render(r.output);
        std::printf("F1 output: %s\n", out.c_str());
        // THE GAP: the failure marker never arrives -- `agg` cannot tell "bad" ever existed.
        check(out.find("ERR:") == std::string::npos,
              "F1 (KNOWN GAP): the propagated failure marker is SILENTLY DROPPED -- `agg`'s input "
              "carries only the two successful siblings, with no trace 'bad' ever ran or failed. If "
              "this starts failing, route_from()'s propagate path now correctly merges into an "
              "already-populated fan_in delivery -- update this assertion to expect the marker present.");
        check(out.find("A-ok") != std::string::npos && out.find("B-ok") != std::string::npos,
              "F1: at least the two successful siblings' content survives");
    }

    // ==== F2: the SAME propagate policy, but the failing executor is declared/processed FIRST -- the
    //      marker becomes the initial `next` entry and later siblings correctly merge onto it. This
    //      is what makes F1 a genuine ORDER-DEPENDENCE bug, not a clean "always drops" rule ===========
    {
        Workflow wf;
        wf.id        = "f2-propagate-order-dependent";
        wf.executors = {node_desc("src"), node_desc("bad"), node_desc("okA"), node_desc("okB"),
                         node_desc("agg")};
        wf.edges     = {
            Edge{"src", "bad", edge_kind::fan_out, {}},
            Edge{"src", "okA", edge_kind::fan_out, {}},
            Edge{"src", "okB", edge_kind::fan_out, {}},
            // Declared FIRST this time.
            Edge{"bad", "agg", edge_kind::fan_in, {},
                 EdgeFailurePolicy{edge_failure_policy::propagate, 0, {}}},
            Edge{"okA", "agg", edge_kind::fan_in, {}},
            Edge{"okB", "agg", edge_kind::fan_in, {}},
        };
        wf.start            = "src";
        wf.output_selection = {"agg"};
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "F2: the graph validates");

        auto agg_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> result<Message> { return in; },
            always_fails,
            ok_body("A-ok"),
            ok_body("B-ok"),
            [agg_calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                ++*agg_calls;
                return ExecutorOutcome{in};
            },
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));

        check(*agg_calls == 1, "F2: the fan_in target still runs exactly once");
        std::string const out = render(r.output);
        std::printf("F2 output: %s\n", out.c_str());
        check(out.find("ERR:") != std::string::npos,
              "F2 (SAME GAP, OPPOSITE SYMPTOM): when the failing sibling happens to be processed "
              "FIRST, its marker DOES survive and later siblings merge onto it -- the exact opposite "
              "of F1 with no graph-level difference except declaration order, proving this is order-"
              "dependence, not a deliberate policy");
    }

    // ==== F3: `fallback` on a fan_in edge whose target siblings ALSO deliver normally this round --
    //      the target runs TWICE and the later (recovery-only) invocation overwrites the first =======
    {
        Workflow wf;
        wf.id        = "f3-fallback-overwrites-siblings";
        wf.executors = {node_desc("src"), node_desc("okA"), node_desc("okB"), node_desc("bad"),
                         node_desc("agg"), node_desc("recovery")};
        wf.edges     = {
            Edge{"src", "okA", edge_kind::fan_out, {}},
            Edge{"src", "okB", edge_kind::fan_out, {}},
            Edge{"src", "bad", edge_kind::fan_out, {}},
            Edge{"okA", "agg", edge_kind::fan_in, {}},
            Edge{"okB", "agg", edge_kind::fan_in, {}},
            Edge{"bad", "agg", edge_kind::fan_in, {},
                 EdgeFailurePolicy{edge_failure_policy::fallback, 0, "recovery"}},
            Edge{"recovery", "agg", edge_kind::fan_in, {}},
        };
        wf.start            = "src";
        wf.output_selection = {"agg"};
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "F3: the graph validates");

        auto agg_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> result<Message> { return in; },
            ok_body("A-ok"),
            ok_body("B-ok"),
            always_fails,
            [agg_calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                ++*agg_calls;
                return ExecutorOutcome{in};
            },
            ok_body("RECOVERED"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));

        check(r.status == workflow_status::completed, "F3: the workflow completes via the recovery branch");
        check(*agg_calls == 2,
              "F3 (KNOWN GAP): the fan_in target runs TWICE -- once for the two successful siblings, "
              "once more for the recovery executor's own later-round delivery. A correct single-round "
              "join would run it exactly once with all three contributions merged. If this starts "
              "failing with *agg_calls == 1, the fallback path now correctly joins within one round -- "
              "update this test to assert the merged three-way content instead.");
        std::string const out = render(r.output);
        std::printf("F3 output: %s\n", out.c_str());
        check(out == "RECOVERED",
              "F3 (KNOWN GAP): the FINAL workflow output is ONLY the recovery branch's text -- the two "
              "successful siblings' real content ('A-ok', 'B-ok') is silently discarded, overwritten by "
              "WorkflowResult::partial's own 'at most one entry per executor_id' rule. This is the "
              "exact mechanism that dropped a production report's real findings in "
              "tests/test_workflow_research_pipeline_live_e2e.cpp's first live run (2026-09-03).");
    }

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_fanin_concurrent_failure_policy_gap: OK (gap still "
                                "present and correctly pinned)\n"
                              : "test_workflow_fanin_concurrent_failure_policy_gap: FAIL (behavior "
                                "changed from what's pinned -- read the failing check's own comment)\n");
    return g_failures == 0 ? 0 : 1;
}
