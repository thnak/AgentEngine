// GitHub issue #62 FIX proof: an ordinary `fan_in` target now WAITS for every one of its declared
// sources before dispatching, even when they resolve at genuinely DIFFERENT round counts (one
// immediate, one only after several more rounds -- e.g. a cyclic/multi-turn branch, 014 §3's
// "Reflection" shape, running alongside an ordinary one-shot branch under the SAME "Concurrent"
// fan_out/fan_in pattern). Before this fix, the target dispatched TWICE: once early, with only the
// fast source's content, and again later, with only the slow source's -- and
// `WorkflowResult::partial`/`RunState::selected_output`'s "last write wins" semantics meant the SECOND
// invocation's output silently OVERWRITES the first, with no error, no trace.
//
// Originally a CHARACTERIZATION test (test_workflow_fanin_uneven_round_sources_gap.cpp) pinning the
// gap, found live 2026-09-03 via tests/test_workflow_research_pipeline_large_context_live_e2e.cpp -- a
// production-shaped pipeline where `market`/`technical` were rebuilt as genuinely multi-turn (real,
// sequential AgentSession turns, ~11 rounds each) while `competitive` still resolves in ~2 rounds via
// its `EdgeFailurePolicy::fallback` (GitHub issue #52's own fix, confirmed working correctly in
// isolation). The shared `aggregate` fan_in target dispatched after ONLY 2-3 rounds, once
// `competitive_fallback` delivered -- running (and then driving `writer`, then opening the
// `publish_review` request_port, suspending the ENTIRE run) with `market`/`technical` still 5 rounds
// into their own 11-round loop, never delivered at all. Renamed and promoted, same day, into this
// positive-proof test once the engine fix landed -- that live test's own top comment records the fix
// too.
//
// THE FIX (include/agentengine/rt/workflow_supervisor.hpp): GitHub issue #52's own fix
// (`deliver_to_fan_in()`/`RunState::held_fan_in`) only ever registered a hold for the ONE narrow shape
// it was scoped to: a `fallback` edge whose named recovery executor has a DIRECT `fan_in` edge back to
// the SAME target. Issue #62 generalizes it: `seed_fan_in_holds()`, called once per `run_workflow()`
// before round 1 ever runs, pre-registers a `HeldFanIn` barrier for EVERY fan_in target with 2 or more
// DISTINCT declared sources (any edges of kind `fan_in` sharing the same `to`), awaiting ALL of them --
// not just a fallback's recovery. That barrier now persists across as many rounds as it takes (it
// lives in `state_`, not round-local scratch state), so `deliver_to_fan_in()`'s existing merge-and-
// release logic (unchanged) naturally becomes a genuine cross-round join for U1 below. A failing
// source under `propagate`/`fallback` policy resolves its own barrier slot too (`route_from()`'s
// updated switch, `resolve_fan_in_await()`), so a failed source never leaves its target waiting
// forever -- see test_workflow_fanin_concurrent_failure_policy_fix.cpp's F1-F4 for that composition.
//
// A SEPARATE, COMPOUNDING characteristic NOT addressed by this fix (still real, still worth naming for
// whoever picks it up next): `execute()`'s round loop --
//   if (!ports_.empty() || !pending_sub_workflows_.empty()) { status = suspended; break; }
// -- still breaks the ENTIRE round loop the instant ANY request_port opens ANYWHERE in the graph,
// abandoning every OTHER still-pending branch (a cyclic node mid-loop included) regardless of whether
// it has anything to do with that port. This fix's own barrier means a fan_in target downstream of a
// slow cyclic branch no longer dispatches EARLY with incomplete content, so for the shape that found
// this gap (a request_port strictly downstream of the shared fan_in target) the practical symptom is
// gone -- but a graph with a port that can open from a DIFFERENT, unrelated branch while a fan_in
// target is still genuinely waiting would still see that unrelated branch's progress paused mid-loop
// until resume. Resuming later DOES correctly continue the abandoned branch from its checkpointed
// state (a real, if perhaps unintended, form of partial-resume). Left as a disclosed residual, not
// fixed here.

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
    // ==== U1: `fast` resolves in round 1; `slow` only resolves after kSlowRounds more rounds (an ====
    //      ordinary cyclic self-loop, no failure/fallback involved at all). Both have a plain =========
    //      `fan_in` edge into the SAME `agg` target. ====================================================
    {
        Workflow wf;
        wf.id        = "u1-fanin-waits-for-uneven-sources";
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
        check(validate_workflow(wf).has_value(), "U1: the graph validates");

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

        check(r.status == workflow_status::completed, "U1: the workflow completes");
        std::string const out = render(r.output);
        std::printf("U1 output: %s (agg ran %d time(s))\n", out.c_str(), *agg_calls);
        check(*agg_calls == 1,
              "U1 (FIXED): `agg` now runs EXACTLY ONCE -- held back from round 1 (when `fast` first "
              "delivers) all the way until `slow_done` finally delivers several rounds later, instead "
              "of dispatching once for `fast` alone and again, overwriting the first, for `slow_done` "
              "alone.");
        check(out.find("FAST") != std::string::npos && out.find("SLOW-DONE") != std::string::npos,
              "U1 (FIXED): the FINAL output carries BOTH contributions merged -- `fast`'s real content "
              "is no longer silently overwritten and lost, closing the exact mechanism that dropped "
              "market/technical's real findings in "
              "tests/test_workflow_research_pipeline_large_context_live_e2e.cpp's live run "
              "(2026-09-03).");
    }

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_fanin_uneven_round_sources_fix: OK (GitHub issue #62's "
                                "fix verified)\n"
                              : "test_workflow_fanin_uneven_round_sources_fix: FAIL (read the failing "
                                "check's own comment)\n");
    return g_failures == 0 ? 0 : 1;
}
