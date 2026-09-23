// Implements decisions/ADR-181-evaluation-harness.md §3.0 item 3: the gross-harm regression screen,
// end to end -- 2*K*tasks real run_trial calls (arms and tasks interleaved by one seeded shuffle),
// each graded for TASK SUCCESS by its own task's grader, aggregated intention-to-treat into
// per-task diffs, and fed to tier1_statistics.hpp's sign_flip_sum_lower_tail_pvalue and
// hypergeometric_min_task_lower_tail_pvalue for real. The model is scripted (deterministic); this file
// proves the screen's own logic, not run_trial (proven end to end, including live, elsewhere).

#include <algorithm>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/eval/eval_gross_harm_screen.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

template <class T>
T drive(ae::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// ---- The same ScriptedChatClient/ScriptStep shape test_eval_trial_driver.cpp uses, adapted, not
// shared (this suite's own convention: each test file is self-contained). ------------------------
struct ScriptStep {
    bool is_tool_call = false;
    std::string tool_name;
    std::string arguments_json;
    std::string text;
};

ScriptStep tool_step(std::string name, std::string args_json) {
    return ScriptStep{true, std::move(name), std::move(args_json), ""};
}
ScriptStep text_step(std::string text) { return ScriptStep{false, "", "", std::move(text)}; }

class ScriptedChatClient {
public:
    explicit ScriptedChatClient(std::vector<ScriptStep> script)
        : state_(std::make_shared<State>(std::move(script))) {}
    struct State {
        explicit State(std::vector<ScriptStep> s) : script(std::move(s)) {}
        std::vector<ScriptStep> script;
        std::size_t next = 0;
    };

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext&) {
        // Repeating the LAST step forever (rather than throwing std::out_of_range once the script
        // is exhausted) is deliberate: Scenario 4 (differential missingness) needs a client that
        // keeps producing tool calls past `max_turns` -- a real, structural non-convergence
        // (AgentSession's own budget cuts it off), not a test-harness crash.
        std::size_t const i = std::min(state_->next, state_->script.size() - 1);
        ScriptStep const& step = state_->script[i];
        ++state_->next;
        ae::Message m{};
        m.role = ae::role::assistant;
        ae::ContentItem item{};
        item.origin = ae::content_origin::assistant;
        if (step.is_tool_call) {
            item.value = ae::ToolCall{"call-" + std::to_string(state_->next), step.tool_name, step.arguments_json};
        } else {
            item.value = ae::Text{step.text};
        }
        m.content.push_back(item);
        co_return ae::ChatResponse{m, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(ae::LegacyChatClient<ScriptedChatClient>);

class MockSummarizerClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value  = ae::Text{"summary: nothing notable"};
        item.origin = ae::content_origin::assistant;
        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-summary";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"summary: nothing notable"};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        (void)pair.producer.push(upd);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<MockSummarizerClient>);

auto make_summarizer_factory() {
    return [](ae::eval::trial_arm, std::size_t) { return MockSummarizerClient{}; };
}

// What one (arm, task) cell's scripted model does: complete the task correctly, complete it wrongly,
// or never converge (a tool call repeated past max_turns -> the trial fails -> graded `ungraded`).
enum class cell { ok, wrong, never };

std::vector<ScriptStep> script_for(cell c) {
    switch (c) {
        case cell::ok:    return {tool_step("do_task", R"({"result":"ok"})"), text_step("done")};
        case cell::wrong: return {tool_step("do_task", R"({"result":"wrong"})"), text_step("done")};
        case cell::never: return {tool_step("do_task", R"({"result":"ok"})")};
    }
    return {};
}

// A context-blind scripted client cannot tell arms or tasks apart on its own, so the factory is told
// both -- exactly the reason run_gross_harm_screen's factories take (arm, task_index).
template <class Plan>
auto make_cell_factory(Plan plan) {
    return [plan](ae::eval::trial_arm arm, std::size_t task_index) {
        return ScriptedChatClient(script_for(plan(arm, task_index)));
    };
}

ae::Message user_message(std::string text) {
    ae::Message m{};
    m.role = ae::role::user;
    ae::ContentItem item{};
    item.origin = ae::content_origin::user;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

ae::eval::StubToolFixture do_task_fixture() {
    ae::eval::StubToolFixture fixture;
    fixture.name              = "do_task";
    fixture.description       = "Performs the task.";
    fixture.args_schema_json  = R"({"type":"object","properties":{"result":{"type":"string"}}})";
    fixture.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    fixture.canned_reply = ae::json::Value::make_object({{"ok", ae::json::Value::make_bool(true)}});
    return fixture;
}

ae::eval::GrossHarmScreenSpec base_spec(std::size_t n_tasks) {
    ae::eval::GrossHarmScreenSpec spec;
    spec.suite_id         = "dev-suite";
    spec.candidate        = ae::eval::LessonCandidate{"deploy-region", "default",
                                                      "the default region is eu-west-1", "run-0/turn-0"};
    spec.template_version = "v1";
    spec.lesson_salience  = 0.3f;
    for (std::size_t t = 0; t < n_tasks; ++t) {
        ae::eval::RegressionTask task;
        task.task_id     = "task-" + std::to_string(t);
        task.task_prompt = user_message("please do task " + std::to_string(t));
        task.stub_tools  = {do_task_fixture()};
        task.grader      = ae::eval::make_tool_argument_grader("do_task", "result", "ok");
        spec.tasks.push_back(std::move(task));
    }
    spec.k_per_arm = 5;
    spec.max_turns = 4;
    spec.seed      = 42;
    return spec;
}

}  // namespace

int main() {
    namespace ev = ae::eval;
    using ev::trial_arm;

    // ---- Scenario 1: no effect -- both arms succeed on every task -----------------------------------
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::ok; });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value(), "S1: no setup error");
        AE_CHECK(!r.invalid, "S1: valid");
        AE_CHECK(r.trials.size() == 2u * 5u * 6u, "S1: exactly 2*K*tasks trials ran, none dropped");
        bool each_cell_k = true;
        for (std::size_t t = 0; t < 6; ++t) {
            for (trial_arm arm : {trial_arm::baseline, trial_arm::treatment}) {
                auto n = std::count_if(r.trials.begin(), r.trials.end(), [&](auto const& d) {
                    return d.task_index == t && d.arm == arm;
                });
                if (n != 5) each_cell_k = false;
            }
        }
        AE_CHECK(each_cell_k, "S1: every (task, arm) cell ran exactly K times");
        bool interleaved = false;
        for (std::size_t i = 0; i + 1 < r.trials.size(); ++i) {
            if (r.trials[i].task_index > r.trials[i + 1].task_index) interleaved = true;
        }
        AE_CHECK(interleaved, "S1: tasks are interleaved in run order, not run task-by-task (§3.4)");
        AE_CHECK(r.treatment_delivered == 5u * 6u,
                 "S1: the lesson reached the model in every treatment trial (informational count)");
        AE_CHECK(r.flagged.has_value() && !*r.flagged, "S1: not flagged");
    }

    // ---- Scenario 2: uniform harm -- treatment fails every task baseline passes ---------------------
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::wrong : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value() && !r.invalid, "S2: valid run");
        AE_CHECK(r.per_task[0].diff == -1.0, "S2: per-task diff is (T - B)/K = -1 (harm is negative)");
        AE_CHECK(r.flagged_by_sum, "S2: the sum statistic flags uniform harm");
        AE_CHECK(r.flagged.has_value() && *r.flagged, "S2: the screen flags");
    }

    // ---- Scenario 3: concentrated harm -- ONE of 10 tasks fully broken in treatment -----------------
    // The case round 4 added the min-task statistic for: the sum barely moves (one -1 among nine 0s,
    // a sign-flip p near 0.5), but the min-task statistic sees one task at 0/5 vs 5/5.
    {
        auto spec = base_spec(10);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t t) {
            return (arm == trial_arm::treatment && t == 0) ? cell::wrong : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value() && !r.invalid, "S3: valid run");
        AE_CHECK(!r.flagged_by_sum, "S3: the sum statistic alone misses concentrated harm");
        AE_CHECK(r.flagged_by_min_task, "S3: the min-task statistic catches it");
        AE_CHECK(r.flagged.has_value() && *r.flagged, "S3: the screen flags (either statistic suffices)");
    }

    // ---- Scenario 4: a benefit is never flagged as harm (one-sided) ---------------------------------
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::ok : cell::wrong;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value() && !r.invalid, "S4: valid run");
        AE_CHECK(r.per_task[0].diff == 1.0, "S4: per-task diff is +1 (benefit)");
        AE_CHECK(r.flagged.has_value() && !*r.flagged, "S4: a clear benefit is not flagged as harm");
    }

    // ---- Scenario 5: differential missingness -- treatment never converges --------------------------
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::never : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value(), "S5: no setup error");
        AE_CHECK(r.treatment_ungraded == 30u && r.baseline_ungraded == 0u,
                 "S5: every treatment trial is ungraded, no baseline trial is");
        AE_CHECK(r.invalid_differential_missingness && r.invalid, "S5: flagged invalid");
        AE_CHECK(!r.flagged.has_value() && !r.sum_pvalue.has_value() && !r.min_task_pvalue.has_value(),
                 "S5: no statistic is computed and no verdict is claimed for an invalid run");
    }

    // ---- Scenario 6: same-seed determinism (I5) ------------------------------------------------------
    {
        auto plan = [](trial_arm arm, std::size_t t) {
            return (arm == trial_arm::treatment && t % 3 == 0) ? cell::wrong : cell::ok;
        };
        auto r1 = drive(ev::run_gross_harm_screen(make_cell_factory(plan), make_summarizer_factory(), base_spec(8)));
        auto r2 = drive(ev::run_gross_harm_screen(make_cell_factory(plan), make_summarizer_factory(), base_spec(8)));

        bool same_order = r1.trials.size() == r2.trials.size();
        for (std::size_t i = 0; same_order && i < r1.trials.size(); ++i) {
            same_order = r1.trials[i].task_index == r2.trials[i].task_index &&
                         r1.trials[i].arm == r2.trials[i].arm && r1.trials[i].trial_id == r2.trials[i].trial_id;
        }
        AE_CHECK(same_order, "S6: identical seed -> identical run order");
        bool same_counts = true;
        for (std::size_t t = 0; t < r1.per_task.size(); ++t) {
            same_counts = same_counts && r1.per_task[t].baseline_successes == r2.per_task[t].baseline_successes &&
                          r1.per_task[t].treatment_successes == r2.per_task[t].treatment_successes;
        }
        AE_CHECK(same_counts, "S6: identical per-task counts");
        AE_CHECK(r1.sum_pvalue.has_value() && r1.sum_pvalue == r2.sum_pvalue &&
                     r1.min_task_pvalue == r2.min_task_pvalue,
                 "S6: identical p-values (seeded permutation tests)");

        auto spec_other = base_spec(8);
        spec_other.seed = 43;
        auto r3 = drive(ev::run_gross_harm_screen(make_cell_factory(plan), make_summarizer_factory(), spec_other));
        bool order_differs = false;
        for (std::size_t i = 0; i < r1.trials.size() && i < r3.trials.size(); ++i) {
            if (r1.trials[i].task_index != r3.trials[i].task_index || r1.trials[i].arm != r3.trials[i].arm) {
                order_differs = true;
            }
        }
        AE_CHECK(order_differs, "S6: a different seed gives a different run order (the seed is really used)");
    }

    // ---- Scenario 7: pre-flight rejects a malformed or over-budget spec before any trial runs -------
    {
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::ok; });
        auto expect_rejected = [&](ev::GrossHarmScreenSpec spec, char const* code, char const* label) {
            auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), std::move(spec)));
            AE_CHECK(r.setup_error.has_value() && r.setup_error->code == code && r.trials.empty(), label);
        };

        expect_rejected(base_spec(0), "eval.gross_harm_no_tasks", "S7a: no tasks -> rejected, zero trials");

        auto dup = base_spec(3);
        dup.tasks[2].task_id = dup.tasks[0].task_id;
        expect_rejected(dup, "eval.gross_harm_task_id_duplicate", "S7b: duplicate task_id -> rejected, zero trials");

        auto no_grader = base_spec(3);
        no_grader.tasks[1].grader = nullptr;
        expect_rejected(no_grader, "eval.gross_harm_grader_missing", "S7c: a task without a grader -> rejected");

        auto too_many_trials = base_spec(6);  // 2*5*6 = 60 trials
        too_many_trials.max_trials = 59;
        expect_rejected(too_many_trials, "eval.gross_harm_trial_budget",
                        "S7d: 2*K*tasks over max_trials -> rejected before any model call (I8)");

        auto too_much_work = base_spec(6);  // 2000 * 60 = 120,000 permutation units
        too_much_work.max_permutation_work = 119'999;
        expect_rejected(too_much_work, "eval.gross_harm_permutation_budget",
                        "S7e: permutation work over max_permutation_work -> rejected (closes R6-Num3)");

        auto at_budget = base_spec(6);
        at_budget.max_trials = 60;
        at_budget.max_permutation_work = 120'000;
        auto ok = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), at_budget));
        AE_CHECK(!ok.setup_error.has_value() && ok.trials.size() == 60u,
                 "S7f: positive control -- exactly at both budgets still runs");

        auto bad_alpha = base_spec(3);
        bad_alpha.alpha = 1.0;
        expect_rejected(bad_alpha, "eval.gross_harm_alpha_range", "S7g: alpha outside (0,1) -> rejected");

        auto huge_k = base_spec(1);
        huge_k.k_per_arm = ev::kMaxHypergeometricK + 1;
        expect_rejected(huge_k, "eval.gross_harm_k_too_large", "S7h: K beyond kMaxHypergeometricK -> rejected");
    }

    // ---- Scenario 8: every trial gets its own derived seed ------------------------------------------
    {
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::ok; });
        auto spec = base_spec(4);
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));
        std::vector<std::uint64_t> seeds;
        bool any_equals_spec_seed = false;
        for (auto const& d : r.trials) {
            seeds.push_back(d.trial_seed);
            if (d.trial_seed == spec.seed) any_equals_spec_seed = true;
        }
        std::sort(seeds.begin(), seeds.end());
        AE_CHECK(!seeds.empty() && std::adjacent_find(seeds.begin(), seeds.end()) == seeds.end(),
                 "S8: every trial's derived seed is distinct (no correlated trials once a seed is consumed)");
        AE_CHECK(!any_equals_spec_seed, "S8: no trial is handed spec.seed verbatim");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_gross_harm_screen: all checks passed\n";
    return 0;
}
