// 014 §8 G3 (determinism): "shuffling intra-round executor scheduling across 10^3 seeds produces
// identical workflow output; an intentionally order-dependent executor is detected by the same
// test." Gap-audit finding 22 (2026-08-14, decisions/ADR-051-*.md): this test never existed in this
// codebase before this file -- confirmed directly (no file under tests/ matched "shuffle" for the
// workflow-scheduling sense before this one; the milestone-6 breakdown doc's own decisions 5/8 only
// ever DESIGNED this test, never claimed it built).
//
// Reuses the same graph/helper shape test_rt_workflow_supervisor_patterns.cpp's FI-1/FI-2 already
// established and already proved a NARROWER version of this exact claim (fan-in merges in fixed
// SOURCE order, not completion order, for ONE hand-picked delay assignment). This file broadens that
// to a real 10^3-seed sweep -- each seed assigns a different, genuinely random per-branch delay, so
// real executor completion order actually varies run to run, not just in principle -- and adds the
// gate's OTHER half: a deliberately order-dependent executor (one that bakes its own real completion
// position into its output via a shared counter) is proven to actually produce DIFFERENT output
// across the same seed sweep, so this test is a real, non-vacuous check, not something that would
// pass regardless of whether shuffling ever really happened.
//
// MACHINE SAFETY (CLAUDE.md; 014 §8's own decision 8): ONE WorkflowSupervisor (one ThreadPool) is
// built ONCE per graph and re-run sequentially across all 1000 seeds -- "the failure mode to avoid
// is spawning an Engine per seed," never done here. Per-branch delays are capped at 3ms, so the
// whole 1000-seed sweep (four concurrent branches per round) completes in low single-digit seconds,
// not a machine-taxing duration.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

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
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::role;
using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "+";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}

constexpr int kBranchCount = 4;
constexpr int kSeedCount   = 1000;  // 014 §8 G3's own literal "10^3 seeds" -- the positive claim's
                                     // own count, kept exact.
// The negative control's own sweep size is deliberately smaller than kSeedCount: its only job is to
// prove the seed-varied delays actually produce real completion-order variety (so the positive
// result above isn't vacuous), not to independently satisfy "10^3" itself -- a few hundred
// independent random delay assignments make a divergence from a genuinely order-dependent executor
// overwhelmingly likely, at a fraction of the wall-clock cost of a second full 1000-seed sweep.
constexpr int kNegativeControlSeedCount = 200;

// Sleeps the CURRENT value of `*delay_slot` (mutated by the test driver between runs, never while a
// run is in flight -- run_workflow()'s own AsyncMutex serializes calls, and each `drive()` below
// resolves fully before the next seed's delays are assigned) then appends its own name -- output
// text never itself depends on timing, matching test_rt_workflow_supervisor_patterns.cpp's own
// delayed_appender() shape.
[[nodiscard]] ExecutorBody shuffled_appender(std::string name, std::chrono::microseconds const* delay_slot) {
    return [name = std::move(name), delay_slot](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (delay_slot->count() > 0) std::this_thread::sleep_for(*delay_slot);
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

// The deliberately-broken counterpart: identical delay behavior, but bakes the branch's own REAL
// completion position (a shared atomic counter, reset once per run by the driver) into its output --
// this is what "an executor whose output depends on intra-round ordering" (014 §8's own wording)
// looks like concretely. The fan-in MERGE position stays fixed by source order regardless (that
// invariant is unrelated and already proven elsewhere) -- what varies here is the VALUE each branch
// itself observes and reports, which is exactly the class of bug this gate exists to catch.
[[nodiscard]] ExecutorBody order_leaking_appender(std::string name, std::chrono::microseconds const* delay_slot,
                                                    std::atomic<int>* completion_counter) {
    return [name = std::move(name), delay_slot, completion_counter](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        if (delay_slot->count() > 0) std::this_thread::sleep_for(*delay_slot);
        int const real_completion_position = completion_counter->fetch_add(1);
        return ExecutorOutcome{
            text_message(all_text_of(in) + ">" + name + "#" + std::to_string(real_completion_position))};
    };
}

[[nodiscard]] Workflow fan_out_in_graph() {
    Workflow wf;
    wf.id        = "shuffleg3";
    wf.executors = {node_desc("src"), node_desc("w1"), node_desc("w2"), node_desc("w3"),
                    node_desc("w4"), node_desc("agg")};
    for (char const* w : {"w1", "w2", "w3", "w4"}) {
        wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "src";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] ExecutorBody aggregator() {
    return [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in))};
    };
}

}  // namespace

int main() {
    std::mt19937 seed_rng(12345);  // deterministic seed-of-seeds -- this test's OWN randomness must
                                    // itself be reproducible, or a real failure couldn't be replayed.
    std::uniform_int_distribution<int> seed_dist(0, 1'000'000);
    std::vector<int> seeds;
    seeds.reserve(kSeedCount);
    for (int i = 0; i < kSeedCount; ++i) seeds.push_back(seed_dist(seed_rng));

    // ---- G3 positive claim: identical output across 10^3 real, seed-varied completion orders -----
    {
        std::array<std::chrono::microseconds, kBranchCount> delays{};
        Workflow wf = fan_out_in_graph();
        check(validate_workflow(wf).has_value(), "G3 setup: the fan-out/fan-in graph validates");

        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(all_text_of(in) + "in")};
            },
            shuffled_appender("w1", &delays[0]),
            shuffled_appender("w2", &delays[1]),
            shuffled_appender("w3", &delays[2]),
            shuffled_appender("w4", &delays[3]),
            aggregator(),
        };

        WorkflowSupervisor sup;  // ONE instance, ONE ThreadPool, reused across every seed below.
        sup.initialize(wf, bodies);

        std::string reference_output;
        bool all_identical = true;
        for (int i = 0; i < kSeedCount; ++i) {
            std::mt19937 delay_rng(static_cast<unsigned>(seeds[static_cast<std::size_t>(i)]));
            std::uniform_int_distribution<int> delay_us(0, 3000);
            for (auto& d : delays) d = std::chrono::microseconds(delay_us(delay_rng));

            WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("")}));
            if (r.status != workflow_status::completed) {
                check(false, "G3-POS: every seed's run completes");
                all_identical = false;
                break;
            }
            std::string const out = all_text_of(r.output);
            if (i == 0) {
                reference_output = out;
                note("G3 reference output (seed 0)", reference_output);
            } else if (out != reference_output) {
                all_identical = false;
                std::fprintf(stderr, "  .. seed index %d diverged: %s\n", i, out.c_str());
            }
        }
        check(all_identical,
              "G3-POS: shuffling intra-round executor completion order across 1000 real, randomly-"
              "delayed runs produces BYTE-IDENTICAL workflow output every time -- the round's result "
              "assembly does not depend on completion order, by construction (issue-all-then-collect-"
              "in-fixed-index-order, workflow_supervisor.hpp's own file banner), not by luck");
    }

    // ---- G3's other half: an intentionally order-dependent executor IS caught, same seed sweep,
    // same graph shape -- proving the positive claim above is a real check, not vacuous. -------------
    {
        std::array<std::chrono::microseconds, kBranchCount> delays{};
        std::atomic<int> completion_counter{0};
        Workflow wf = fan_out_in_graph();

        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{text_message(all_text_of(in) + "in")};
            },
            order_leaking_appender("w1", &delays[0], &completion_counter),
            order_leaking_appender("w2", &delays[1], &completion_counter),
            order_leaking_appender("w3", &delays[2], &completion_counter),
            order_leaking_appender("w4", &delays[3], &completion_counter),
            aggregator(),
        };

        WorkflowSupervisor sup;  // a SEPARATE instance -- this graph's executors are deliberately
                                  // different bodies, not merely a different config of the same one.
        sup.initialize(wf, bodies);

        std::string reference_output;
        bool any_divergence = false;
        for (int i = 0; i < kNegativeControlSeedCount; ++i) {
            std::mt19937 delay_rng(static_cast<unsigned>(seeds[static_cast<std::size_t>(i)]));
            std::uniform_int_distribution<int> delay_us(0, 3000);
            for (auto& d : delays) d = std::chrono::microseconds(delay_us(delay_rng));
            completion_counter.store(0);

            WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("")}));
            if (r.status != workflow_status::completed) continue;
            std::string const out = all_text_of(r.output);
            if (i == 0) {
                reference_output = out;
            } else if (out != reference_output) {
                any_divergence = true;
            }
        }
        check(any_divergence,
              "G3-NEG (the gate's other half): the SAME seed-derived delay sweep (a 200-seed subset "
              "of the 1000 used above), against a DELIBERATELY order-dependent executor (one that "
              "leaks its own real completion position into its output), actually produces different "
              "output across seeds -- this test can tell the difference between 'the invariant "
              "holds' and 'nothing ever varied,' so G3-POS's clean pass above is a real result, not "
              "a test that would pass no matter what");
    }

    std::printf(g_failures == 0 ? "test_rt_workflow_supervisor_scheduling_shuffle: OK\n"
                                 : "test_rt_workflow_supervisor_scheduling_shuffle: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
