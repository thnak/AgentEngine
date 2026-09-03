// A CHARACTERIZATION test pinning a real, currently-unfixed gap: an ordinary `fan_in` target does NOT
// wait for every declared incoming edge to deliver before dispatching -- it merges whatever has
// arrived THIS round and dispatches as soon as that's non-empty. When two fan_in sources genuinely
// resolve at DIFFERENT round counts (one immediate, one only after several more rounds -- e.g. a
// cyclic/multi-turn branch, 014 §3's "Reflection" shape, running alongside an ordinary one-shot
// branch under the SAME "Concurrent" fan_out/fan_in pattern), the target dispatches TWICE: once
// early, with only the fast source's content, and again later, with only the slow source's -- and
// `WorkflowResult::partial`/`RunState::selected_output`'s "last write wins" semantics mean the SECOND
// invocation's output silently OVERWRITES the first. The fast source's real content is gone from the
// final result, with no error, no trace.
//
// Found live 2026-09-03 via tests/test_workflow_research_pipeline_large_context_live_e2e.cpp -- a
// production-shaped pipeline where `market`/`technical` were rebuilt as genuinely multi-turn (real,
// sequential AgentSession turns, ~11 rounds each) while `competitive` still resolves in ~2 rounds via
// its `EdgeFailurePolicy::fallback` (GitHub issue #52's own fix, confirmed working correctly in
// isolation). The shared `aggregate` fan_in target dispatched after ONLY 2-3 rounds, once
// `competitive_fallback` delivered -- running (and then driving `writer`, then opening the
// `publish_review` request_port, suspending the ENTIRE run) with `market`/`technical` still 5 rounds
// into their own 11-round loop, never delivered at all. The writer's own real output that run
// discussed ONLY the missing competitive data -- direct evidence `aggregate` never saw market/
// technical's content. This file reproduces the SAME root cause offline, fast, deterministically,
// isolated from live-network nondeterminism and from GitHub issue #52's own (different, already-
// fixed) failure-policy composition bug -- G1 below uses ONLY plain successful executors, no
// `EdgeFailurePolicy` at all, to prove this is a distinct defect, not a residual of #52.
//
// GitHub issue #52's fix (`register_fan_in_holds()`/`deliver_to_fan_in()`/`RunState::held_fan_in`,
// workflow_supervisor.hpp) only registers a hold for the ONE shape it was scoped to: a `fallback`
// edge whose named recovery executor has a DIRECT `fan_in` edge back to the SAME target. It does
// nothing for the general case here -- two ORDINARY (non-failing, non-fallback) fan_in sources that
// simply take different numbers of rounds to resolve.
//
// A SEPARATE, COMPOUNDING characteristic (not pinned by its own scenario here, but present in the
// live reproduction above and worth naming for whoever picks this up): `execute()`'s round loop --
//   if (!ports_.empty() || !pending_sub_workflows_.empty()) { status = suspended; break; }
// -- breaks the ENTIRE round loop the instant ANY request_port opens ANYWHERE in the graph, abandoning
// every OTHER still-pending branch (a cyclic node mid-loop included) regardless of whether it has
// anything to do with that port. Resuming later DOES correctly continue the abandoned branch from its
// checkpointed state (a real, if perhaps unintended, form of partial-resume) -- but the window between
// "port opens" and "resume" is exactly when this file's core bug (an early, incomplete fan_in
// dispatch) becomes irreversible: the target has ALREADY run with partial content by the time the
// slow branch would otherwise have delivered.
//
// NOT FIXED HERE -- this is hot-path/correctness-critical routing logic used by every workflow
// pattern in this engine (014 §3's "Concurrent" pattern generally, not one narrow composition); a real
// fix needs `fan_in` to know how many sources it expects (or an explicit barrier declaration) before
// it can decide "wait" vs "dispatch", which is a genuine design question, not a local patch. CLAUDE.md
// reserves changes like that for a real design→red-team→prove→judge pass. This test exists to PIN the
// current, disclosed behavior so it cannot silently regress further, and so a future fix has a
// concrete, reproducible target: when `fan_in` correctly waits for every declared source, G1's
// "dispatched twice, first overwritten" assertions below will start FAILING -- that is the fix
// working. Update them then to assert the correct single-dispatch, fully-merged behavior instead.

#include <cstdio>
#include <memory>
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

[[nodiscard]] std::string render(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (!out.empty()) out += " | ";
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
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

}  // namespace

int main() {
    // ==== G1: `fast` resolves in round 1; `slow` only resolves after kSlowRounds more rounds (an ====
    //      ordinary cyclic self-loop, no failure/fallback involved at all). Both have a plain =========
    //      `fan_in` edge into the SAME `agg` target. ====================================================
    {
        Workflow wf;
        wf.id        = "g1-fanin-does-not-wait-for-uneven-sources";
        wf.executors = {node_desc("src"), node_desc("fast"), node_desc("slow"), node_desc("slow_done"),
                         node_desc("agg")};
        wf.edges     = {
            Edge{"src", "fast", edge_kind::fan_out, {}},
            Edge{"src", "slow", edge_kind::fan_out, {}},
            Edge{"fast", "agg", edge_kind::fan_in, {}},
            // A plain cyclic self-loop (014 §9 Q2) -- no EdgeFailurePolicy anywhere in this graph,
            // deliberately, so this cannot be mistaken for a residual of GitHub issue #52's own
            // (different, already-fixed) propagate/fallback composition bug.
            Edge{"slow", "slow", edge_kind::switch_case, "continue"},
            Edge{"slow", "slow_done", edge_kind::switch_case, "finish"},
            Edge{"slow_done", "agg", edge_kind::fan_in, {}},
        };
        wf.start            = "src";
        wf.output_selection = {"agg"};
        wf.bound.max_rounds = 20;
        check(validate_workflow(wf).has_value(), "G1: the graph validates");

        constexpr int kSlowRounds = 4;  // slow resolves 4 rounds after fast already has
        auto slow_round = std::make_shared<int>(0);
        auto agg_calls  = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, EffectContext&) -> result<ExecutorOutcome> { return ExecutorOutcome{in}; },
            [](Message const&, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message("FAST")};
            },
            [slow_round](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                int const i = (*slow_round)++;
                ExecutorOutcome out{};
                out.payload = in;
                out.routes  = {i < kSlowRounds ? "continue" : "finish"};
                return out;
            },
            [](Message const&, EffectContext&) -> result<ExecutorOutcome> {
                return ExecutorOutcome{text_message("SLOW-DONE")};
            },
            [agg_calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                ++*agg_calls;
                return ExecutorOutcome{in};
            },
        };

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));

        check(r.status == workflow_status::completed, "G1: the workflow completes");
        std::string const out = render(r.output);
        std::printf("G1 output: %s (agg ran %d time(s))\n", out.c_str(), *agg_calls);
        check(*agg_calls == 2,
              "G1 (KNOWN GAP): `agg` runs TWICE -- once as soon as `fast` delivers (round 2), a "
              "second time once `slow_done` finally delivers several rounds later. A correct barrier "
              "would run it exactly ONCE, after both. If this starts failing with *agg_calls == 1, "
              "fan_in now correctly waits for every declared source -- update this test to assert the "
              "single merged dispatch instead.");
        check(out == "SLOW-DONE",
              "G1 (KNOWN GAP): the FINAL output is ONLY `slow`'s late contribution -- `fast`'s real "
              "content ('FAST') is silently OVERWRITTEN and gone, exactly the mechanism that dropped "
              "market/technical's real findings in "
              "tests/test_workflow_research_pipeline_large_context_live_e2e.cpp's live run "
              "(2026-09-03), except reached here with zero failures/fallbacks anywhere in the graph.");
    }

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_fanin_uneven_round_sources_gap: OK (gap still present "
                                "and correctly pinned)\n"
                              : "test_workflow_fanin_uneven_round_sources_gap: FAIL (behavior changed "
                                "from what's pinned -- read the failing check's own comment)\n");
    return g_failures == 0 ? 0 : 1;
}
