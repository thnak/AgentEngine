// GitHub issue #52 FIX proof: `edge_failure_policy::propagate` and `edge_failure_policy::fallback`
// (014 §6) now compose correctly with a `fan_in` target that a SIBLING executor also delivers to
// normally in the SAME round -- exactly the shape a concurrent research fan-out with one degraded
// specialist needs (014 §3's "Concurrent" pattern with one branch recovering from failure).
//
// Originally a CHARACTERIZATION test pinning the gap, found live 2026-09-03 via
// tests/test_workflow_research_pipeline_live_e2e.cpp -- a production-shaped workflow against REAL
// OpenRouter calls, where one specialist's edge used `fallback` and the final report silently lost
// its two successful specialists' findings. Promoted, same day, into this positive-proof test once
// the engine fix landed; that live test now also exercises the fix directly (its own top comment).
//
// THE FIX (include/agentengine/rt/workflow_supervisor.hpp's `route_from()`):
//   - `propagate`: `deliver_or_merge()` (renamed from `deliver_once()`, which used to no-op on an
//     existing `next` entry instead of merging into it) now uses the SAME append-or-insert semantics
//     the normal-path merge already used -- order-independent regardless of whether the marker or a
//     sibling's normal delivery reaches the shared target first. See F1/F2 below.
//   - `fallback`: `register_fan_in_holds()`/`deliver_to_fan_in()`/`RunState::held_fan_in` hold the
//     shared target back across the round boundary when the named recovery executor has its own
//     DIRECT `fan_in` edge back to the SAME target, releasing it -- with every contribution merged --
//     only once the recovery resolves, instead of dispatching the target twice with the second
//     invocation overwriting the first. See F3 below. F4 additionally proves the hold survives a real
//     checkpoint/resume round-trip through the JSON wire format (`HeldFanInRecord`'s own codec).
//
// A quarantine-specific carve-out (`route_from()`'s `is_quarantine_echo` parameter) had to ship
// alongside this fix: ADR-077 P9 / T7 in tests/test_rt_agent_workflow_executor.cpp relies on the
// OQ-19 same-round duplicate-delivery quarantine's synthetic failure being "silently absorbed" with
// NO effect on any downstream target -- including one the survivor's real delivery never reached --
// which a blanket "always merge" propagate/fallback fix would have broken. `deliver_or_merge()` and
// `register_fan_in_holds()` both skip a `!ok` reply that shares its executor_index with an `ok` reply
// THIS round (is_same_round_quarantine_echo()), so that guarantee still holds.
//
// `tests/test_rt_workflow_supervisor_failure_policies.cpp`'s own D4 only ever exercised `fallback` on
// a single-source (non-fan_in-shared) edge, so this combination was never gate-proven despite 014 §8
// G1's "each pattern... under injected executor failures" language until F1-F4 below.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunStateRecord;
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
    //      (siblings dispatched/processed BEFORE the failing one) -- the marker now merges instead ===
    //      of being dropped =========================================================================
    {
        Workflow wf;
        wf.id        = "f1-propagate-marker-merges";
        wf.executors = {node_desc("src"), node_desc("okA"), node_desc("okB"), node_desc("bad"),
                         node_desc("agg")};
        wf.edges     = {
            Edge{"src", "okA", edge_kind::fan_out, {}},
            Edge{"src", "okB", edge_kind::fan_out, {}},
            Edge{"src", "bad", edge_kind::fan_out, {}},
            Edge{"okA", "agg", edge_kind::fan_in, {}},
            Edge{"okB", "agg", edge_kind::fan_in, {}},
            // Declared LAST, after okA/okB -- exec_deliveries processes edges/executors in
            // declaration order, so okA/okB's normal merge creates agg's `next` entry first. Before
            // the fix, that made the marker get dropped (F1) vs. survive (F2, declared first) purely
            // by declaration order -- deliver_or_merge()'s symmetric semantics make that order
            // irrelevant now; F1 and F2 assert the SAME outcome despite the opposite declaration order.
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
        check(out.find("ERR:") != std::string::npos,
              "F1 (FIXED): the propagated failure marker survives merged into agg's input, even "
              "though the two successful siblings' normal deliveries created agg's `next` entry "
              "first this round");
        check(out.find("A-ok") != std::string::npos && out.find("B-ok") != std::string::npos,
              "F1: the two successful siblings' content survives alongside the marker");
    }

    // ==== F2: the SAME propagate policy, but the failing executor is declared/processed FIRST -- ====
    //      proves F1's merged outcome is genuinely order-INdependent, not an artifact of F1's own ====
    //      declaration order ========================================================================
    {
        Workflow wf;
        wf.id        = "f2-propagate-order-independent";
        wf.executors = {node_desc("src"), node_desc("bad"), node_desc("okA"), node_desc("okB"),
                         node_desc("agg")};
        wf.edges     = {
            Edge{"src", "bad", edge_kind::fan_out, {}},
            Edge{"src", "okA", edge_kind::fan_out, {}},
            Edge{"src", "okB", edge_kind::fan_out, {}},
            // Declared FIRST this time -- opposite of F1.
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
        check(out.find("ERR:") != std::string::npos && out.find("A-ok") != std::string::npos &&
                  out.find("B-ok") != std::string::npos,
              "F2: SAME merged outcome as F1 despite the OPPOSITE declaration order -- no graph-level "
              "difference except which edge is declared first, proving the fix is genuinely "
              "order-independent, not a declaration-order coincidence");
    }

    // ==== F3: `fallback` on a fan_in edge whose target siblings ALSO deliver normally this round -- ==
    //      the target now runs EXACTLY ONCE, held back until the recovery executor resolves, with =====
    //      every contribution merged =================================================================
    {
        Workflow wf;
        wf.id        = "f3-fallback-single-round-join";
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
        check(*agg_calls == 1,
              "F3 (FIXED): the fan_in target runs EXACTLY ONCE -- held back across the round boundary "
              "until `recovery` resolved, instead of dispatching once for the two successful siblings "
              "and again, overwriting the first, for the recovery executor alone");
        std::string const out = render(r.output);
        std::printf("F3 output: %s\n", out.c_str());
        check(out.find("A-ok") != std::string::npos && out.find("B-ok") != std::string::npos &&
                  out.find("RECOVERED") != std::string::npos,
              "F3 (FIXED): the FINAL workflow output carries ALL THREE contributions merged -- the two "
              "successful siblings' real content AND the recovery branch's output -- closing the exact "
              "mechanism that dropped a production report's real findings in "
              "tests/test_workflow_research_pipeline_live_e2e.cpp's first live run (2026-09-03)");
    }

    // ==== F4: the SAME `fallback` cross-round join, but with a checkpoint taken WHILE `agg` is held ==
    //      back (between bad's failure round and recovery's own round), round-tripped through the ====
    //      REAL JSON wire format, and resumed on a BRAND-NEW WorkflowSupervisor with fresh body =======
    //      closures -- proves HeldFanIn's own checkpoint codec (HeldFanInRecord), not just the ========
    //      in-memory mechanism F3 already covers =============================================
    {
        Workflow wf;
        wf.id        = "f4-fallback-join-survives-checkpoint";
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

        auto agg_calls1 = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies1 = {
            [](Message const& in, EffectContext&) -> result<Message> { return in; },
            ok_body("A-ok"),
            ok_body("B-ok"),
            always_fails,
            [agg_calls1](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                ++*agg_calls1;
                return ExecutorOutcome{in};
            },
            ok_body("RECOVERED"),
        };

        std::vector<RunStateRecord> records;
        WorkflowSupervisor          sup1;
        sup1.initialize(wf, bodies1);
        sup1.set_checkpoint_hook(
            [&records](std::uint32_t, RunStateRecord const& rec) { records.push_back(rec); });
        WorkflowResult r1 = drive(sup1.run_workflow(RunWorkflow{text_message("go")}));
        check(r1.status == workflow_status::completed, "F4: the first (uninterrupted) run completes");

        // Find the checkpoint taken while `agg` was genuinely held -- must exist, or this test isn't
        // exercising the cross-round hold at all.
        RunStateRecord const* held_record = nullptr;
        for (auto const& rec : records) {
            if (!rec.held_fan_in.empty()) {
                held_record = &rec;
                break;
            }
        }
        check(held_record != nullptr,
              "F4: at least one checkpoint was taken while `agg` was held back awaiting `recovery` -- "
              "otherwise this test isn't exercising the cross-round hold at all");

        if (held_record != nullptr) {
            // Round-trip through the REAL JSON wire format, exactly like WorkflowCheckpointManager
            // does through a real SessionStore -- not just an in-memory copy of the struct.
            std::vector<std::byte> const encoded = agentengine::rt::encode_run_state_record(*held_record);
            agentengine::result<RunStateRecord> decoded = agentengine::rt::decode_run_state_record(encoded);
            check(decoded.has_value(),
                  "F4: the held-back checkpoint decodes cleanly from its own JSON wire bytes");
            check(decoded.has_value() && decoded->held_fan_in.size() == 1 &&
                      decoded->held_fan_in.at(0).awaiting_recovery.size() == 1,
                  "F4: the decoded record still carries the held fan_in entry with its ONE outstanding "
                  "recovery debt -- HeldFanInRecord's own JSON codec round-trips correctly");

            auto agg_calls2 = std::make_shared<int>(0);
            std::vector<ExecutorBody> bodies2 = {
                [](Message const& in, EffectContext&) -> result<Message> { return in; },
                ok_body("A-ok"),
                ok_body("B-ok"),
                always_fails,
                [agg_calls2](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
                    ++*agg_calls2;
                    return ExecutorOutcome{in};
                },
                ok_body("RECOVERED"),
            };
            WorkflowSupervisor sup2;
            sup2.initialize(wf, bodies2);
            sup2.restore_from_record(decoded.value());
            WorkflowResult r2 = drive(sup2.continue_workflow(ContinueWorkflow{}));

            check(r2.status == workflow_status::completed,
                  "F4: the run resumed from mid-hold on a BRAND-NEW supervisor completes normally");
            check(*agg_calls2 == 1,
                  "F4: `agg` runs EXACTLY ONCE on the resumed supervisor -- the hold survived the "
                  "checkpoint round-trip and still released correctly once `recovery` resolved");
            std::string const out2 = render(r2.output);
            std::printf("F4 output: %s\n", out2.c_str());
            check(out2.find("A-ok") != std::string::npos && out2.find("B-ok") != std::string::npos &&
                      out2.find("RECOVERED") != std::string::npos,
                  "F4: the resumed run's final output still carries ALL THREE contributions -- the "
                  "pre-checkpoint siblings' content (persisted inside the held record's own accumulated "
                  "payload) merged with the post-resume recovery's own output");
        }
    }

    std::fprintf(stderr, g_failures == 0
                              ? "test_workflow_fanin_concurrent_failure_policy_fix: OK (GitHub issue "
                                "#52's fix verified)\n"
                              : "test_workflow_fanin_concurrent_failure_policy_fix: FAIL (read the "
                                "failing check's own comment)\n");
    return g_failures == 0 ? 0 : 1;
}
