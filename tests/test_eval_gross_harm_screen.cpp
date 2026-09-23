// Implements decisions/ADR-181-evaluation-harness.md §3.0 item 3: the gross-harm regression screen,
// end to end -- 2*K*tasks real run_trial calls (arms and tasks interleaved by one seeded shuffle),
// each graded for TASK SUCCESS by its own task's grader, aggregated intention-to-treat into
// per-task diffs, and fed to tier1_statistics.hpp's sign_flip_sum_lower_tail_pvalue and
// hypergeometric_min_task_lower_tail_pvalue for real. The model is scripted (deterministic); this file
// proves the screen's own logic, not run_trial (proven end to end, including live, elsewhere).

#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <memory>
#include <memory_resource>
#include <optional>
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
    std::optional<ae::error> fail;  // the provider call itself fails with this error
};

ScriptStep tool_step(std::string name, std::string args_json) {
    return ScriptStep{true, std::move(name), std::move(args_json), "", std::nullopt};
}
ScriptStep text_step(std::string text) { return ScriptStep{false, "", "", std::move(text), std::nullopt}; }
ScriptStep fail_step(ae::error e) { return ScriptStep{false, "", "", "", std::move(e)}; }

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
        // is exhausted) is deliberate: Scenario 5 (a never-finishing treatment arm) needs a client that
        // keeps producing tool calls past `max_turns` -- a real, structural non-convergence
        // (AgentSession's own budget cuts it off), not a test-harness crash.
        std::size_t const i = std::min(state_->next, state_->script.size() - 1);
        ScriptStep const& step = state_->script[i];
        ++state_->next;
        if (step.fail.has_value()) co_return std::unexpected(*step.fail);
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

// What one (arm, task) cell's scripted model does:
//   ok / wrong    -- do the task right / wrong;
//   never         -- a tool call repeated past max_turns, so the agent never finishes (an OUTCOME: failure);
//   skip          -- answer in text without ever calling the tool (an OUTCOME: failure);
//   unmeasurable  -- a call the grader cannot judge; it throws, so the MEASUREMENT failed (`ungraded`);
//   transient     -- the provider call fails with a `failure_class::transient` error (e.g. a 503);
//   canceled      -- the run ends `run.canceled`, as a host cancel does.
// The last three are the measurement faults `grade_trial` maps to `ungraded`.
enum class cell { ok, wrong, never, skip, unmeasurable, transient, canceled };

std::vector<ScriptStep> script_for(cell c) {
    switch (c) {
        case cell::ok:           return {tool_step("do_task", R"({"result":"ok"})"), text_step("done")};
        case cell::wrong:        return {tool_step("do_task", R"({"result":"wrong"})"), text_step("done")};
        case cell::never:        return {tool_step("do_task", R"({"result":"ok"})")};
        case cell::skip:         return {text_step("I'd rather not use the tool for this.")};
        case cell::unmeasurable: return {tool_step("do_task", R"({"result":"boom"})"), text_step("done")};
        case cell::transient:
            return {fail_step(ae::error{ae::failure_class::transient, "HTTP 503", "provider.http_503"})};
        case cell::canceled:
            return {fail_step(ae::error{ae::failure_class::fatal, "canceled by the host", "run.canceled"})};
    }
    return {};
}

// The task grader, except that it throws on the "boom" sentinel -- the one remaining way to produce a
// failed measurement (§3.5: a grader error is `ungraded`).
ae::eval::GraderFn task_grader() {
    auto inner = ae::eval::make_tool_argument_grader("do_task", "result", "ok");
    return [inner](ae::eval::TrialResult const& t) {
        for (auto const& c : t.tool_calls) {
            auto const* r = c.arguments.find("result");
            if (r != nullptr && r->is_string() && r->as_string() == "boom") throw std::runtime_error("unmeasurable");
        }
        return inner(t);
    };
}

// A context-blind scripted client cannot tell arms or tasks apart on its own, so the factory is told
// both -- exactly the reason run_gross_harm_screen's factories take (arm, task_index).
template <class Plan>
auto make_cell_factory(Plan plan) {
    return [plan](ae::eval::trial_arm arm, std::size_t task_index) {
        return ScriptedChatClient(script_for(plan(arm, task_index)));
    };
}

// Per-TRIAL control: `plan(arm, task, k)` where k counts that (arm, task) cell's trials so far, so a
// scenario can make exactly one or three trials of a cell behave differently.
template <class Plan>
auto make_trial_factory(Plan plan) {
    auto counts = std::make_shared<std::map<std::pair<int, std::size_t>, int>>();
    return [plan, counts](ae::eval::trial_arm arm, std::size_t task_index) {
        int const k = (*counts)[{static_cast<int>(arm), task_index}]++;
        return ScriptedChatClient(script_for(plan(arm, task_index, k)));
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
        task.grader      = task_grader();
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
        // Baseline succeeds on even tasks only (50% -- above the uninformative-baseline floor); the
        // treatment succeeds everywhere, so the odd tasks show a +1 benefit.
        auto factory = make_cell_factory([](trial_arm arm, std::size_t t) {
            return (arm == trial_arm::treatment || t % 2 == 0) ? cell::ok : cell::wrong;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value() && !r.invalid, "S4: valid run");
        AE_CHECK(r.per_task[1].diff == 1.0 && r.per_task[0].diff == 0.0, "S4: per-task diff is +1 (benefit) where it differs");
        AE_CHECK(r.flagged.has_value() && !*r.flagged, "S4: a clear benefit is not flagged as harm");
    }

    // ---- Scenario 5: a lesson that stops the agent finishing is HARM, not missing data ------------
    // Red-team finding (MAJOR): this scenario used to assert `invalid` -- a non-converging treatment
    // arm counted as ungraded, tripped differential missingness, and hid a total harm behind "no
    // verdict". Not finishing is an outcome (ITT), so it is now flagged.
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::never : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.setup_error.has_value() && !r.invalid, "S5: a never-finishing treatment arm is a valid run");
        AE_CHECK(r.treatment_ungraded == 0u, "S5: non-finishing trials are failures, not ungraded");
        AE_CHECK(r.flagged.has_value() && *r.flagged, "S5: and the screen flags the harm");
    }

    // ---- Scenario 5b: a lesson that makes the agent skip the tool is harm too ---------------------
    // The same finding's second shape: the built-in grader used to return `ungraded` when the tool
    // was never called, so a lesson that suppressed the tool in enough tasks produced `invalid`.
    {
        auto spec = base_spec(30);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::skip : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.invalid && r.treatment_ungraded == 0u, "S5b: skipping the tool is a failure, not missing data");
        AE_CHECK(r.flagged.has_value() && *r.flagged, "S5b: 30 of 30 tasks skipped is flagged, not 'invalid'");
    }

    // ---- Scenario 5c: the treatment arm is unmeasurable -- harm cannot be ruled out -----------------
    // Round-2 red-team finding (MAJOR): this used to assert `invalid`. A lesson can make its own trials
    // unmeasurable (here: a model-chosen argument the grader throws on), so "invalid" was the lesson's
    // escape from a flag. Harm-favouring imputation now decides such a run.
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::unmeasurable : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(r.treatment_ungraded == 30u && r.baseline_ungraded == 0u,
                 "S5c: only a failed measurement is ungraded");
        AE_CHECK(r.invalid_differential_missingness && r.worst_case_imputation && !r.invalid,
                 "S5c: the missingness rule trips, and the harm-favouring analysis decides instead of 'invalid'");
        AE_CHECK(r.flagged.has_value() && *r.flagged, "S5c: flagged -- an unmeasurable treatment arm cannot pass");
    }

    // ---- Scenario 5c2: the round-2 attack -- lesson-induced provider faults hiding a real harm ------
    // The red team's case: the lesson breaks 12 of 30 tasks, 10 visibly (wrong answer) and 2 by making
    // the provider fail (a 503 or read timeout is `transient` whatever caused it). 10 ungraded trials
    // out of 150 tripped the missingness rule and the screen said `invalid` -- no verdict, rerun it.
    {
        auto spec = base_spec(30);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t t) {
            if (arm == trial_arm::baseline) return cell::ok;
            if (t < 10) return cell::wrong;
            return t < 12 ? cell::transient : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(r.treatment_ungraded == 10u && r.baseline_ungraded == 0u,
                 "S5c2: a transient provider error is a failed measurement (ungraded)");
        AE_CHECK(r.invalid_differential_missingness && r.worst_case_imputation,
                 "S5c2: the missingness rule trips");
        AE_CHECK(r.flagged.has_value() && *r.flagged && !r.invalid,
                 "S5c2: and the harm is flagged anyway, not hidden behind 'invalid'");
    }

    // ---- Scenario 5d: no data -- never 'no harm' ------------------------------------------------------
    // Round-1 finding (MAJOR): every trial in BOTH arms ungraded gave equal ungraded rates, so the run
    // counted as valid and reported "no harm" from zero data. Under harm-favouring imputation a run with
    // nothing measured cannot rule harm out, so it is flagged (`worst_case_imputation` says why).
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::unmeasurable; });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!r.invalid_differential_missingness && r.invalid_insufficient_grading,
                 "S5d: both arms unmeasurable -> insufficient grading");
        AE_CHECK(r.worst_case_imputation && r.flagged.has_value() && *r.flagged,
                 "S5d: harm cannot be ruled out from no data, so the screen does not pass the lesson");
    }

    // ---- Scenario 5f: `invalid` survives when the missing data could not hide a harm ----------------
    // The baseline, which the lesson never touches, is unmeasurable in one task. Even counting those
    // baseline trials as successes, the treatment matches the baseline everywhere -- no harm can be
    // shown, and too little was measured to rule it out: `invalid`, no verdict.
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm arm, std::size_t t) {
            return (arm == trial_arm::baseline && t == 0) ? cell::transient : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(r.baseline_ungraded == 5u && r.treatment_ungraded == 0u,
                 "S5f: transient provider errors are ungraded (a failed measurement, not a failure)");
        AE_CHECK(r.invalid && r.worst_case_imputation && !r.flagged.has_value(),
                 "S5f: invalid, no verdict -- the harm-favouring analysis does not flag");
    }

    // ---- Scenario 5g: the other measurement faults are ungraded too ---------------------------------
    {
        auto spec = base_spec(6);
        auto canceled = make_cell_factory([](trial_arm arm, std::size_t t) {
            return (arm == trial_arm::baseline && t == 0) ? cell::canceled : cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(canceled, make_summarizer_factory(), spec));
        AE_CHECK(r.baseline_ungraded == 5u, "S5g: a run that ends `run.canceled` is ungraded");

        // A factory that throws: that trial's setup error, not the loss of the whole screen.
        auto throws = [](trial_arm arm, std::size_t t) {
            if (arm == trial_arm::baseline && t == 0) throw std::runtime_error("factory failed");
            return ScriptedChatClient(script_for(cell::ok));
        };
        auto r2 = drive(ev::run_gross_harm_screen(throws, make_summarizer_factory(), spec));
        bool setup_errors = true;
        for (auto const& d : r2.trials) {
            if (d.arm == trial_arm::baseline && d.task_index == 0) {
                setup_errors = setup_errors && d.trial_result.setup_error.has_value() &&
                               d.trial_result.setup_error->code == "eval.screen_trial_threw" &&
                               d.grade == ev::grade_outcome::ungraded;
            }
        }
        AE_CHECK(r2.trials.size() == 60u && r2.baseline_ungraded == 5u && setup_errors,
                 "S5g: a throwing factory costs only its own trials (setup error -> ungraded); all 60 ran");
    }

    // ---- Scenario 5h: ITT counts and the validity thresholds, trial by trial ------------------------
    {
        // One ungraded trial in each arm, in different tasks: the gap is 0 (valid), the ungraded trials
        // are not successes, and the baseline rate is ITT over all 30 trials.
        auto spec = base_spec(6);
        auto factory = make_trial_factory([](trial_arm arm, std::size_t t, int k) {
            if (k == 0 && arm == trial_arm::baseline && t == 0) return cell::transient;
            if (k == 0 && arm == trial_arm::treatment && t == 1) return cell::transient;
            return cell::ok;
        });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));
        AE_CHECK(!r.worst_case_imputation && r.flagged.has_value() && !*r.flagged, "S5h: valid, not flagged");
        AE_CHECK(r.per_task[0].baseline_successes == 4u && r.per_task[1].treatment_successes == 4u,
                 "S5h: an ungraded trial is never a success, in either arm (ITT)");
        AE_CHECK(r.baseline_success_rate == 29.0 / 30.0,
                 "S5h: the baseline rate is ITT over all K * tasks trials, not over graded trials only");

        // Differential missingness at its boundary: n = 30, bound 0.05 -> at most floor(1.5) = 1 trial.
        auto gap = [&](int ungraded) {
            auto f = make_trial_factory([ungraded](trial_arm arm, std::size_t t, int k) {
                return (arm == trial_arm::treatment && t == 0 && k < ungraded) ? cell::transient : cell::ok;
            });
            auto spec_gap = base_spec(6);
            spec_gap.min_graded_fraction = 0.0;  // isolate the missingness rule
            return drive(ev::run_gross_harm_screen(f, make_summarizer_factory(), spec_gap));
        };
        AE_CHECK(!gap(1).invalid_differential_missingness, "S5h: a gap of exactly the allowed 1 trial is valid");
        AE_CHECK(gap(2).invalid_differential_missingness, "S5h: a gap of 2 trials trips the rule");

        // Insufficient grading at its boundary, in each arm on its own: 3 of 30 ungraded is exactly the
        // 0.9 floor (fine); 4 is below it.
        auto grading = [&](trial_arm which, int ungraded) {
            auto f = make_trial_factory([which, ungraded](trial_arm arm, std::size_t t, int k) {
                return (arm == which && static_cast<int>(t) * 5 + k < ungraded) ? cell::transient : cell::ok;
            });
            auto spec_g = base_spec(6);
            spec_g.max_differential_missingness = 1.0;  // isolate the grading rule
            return drive(ev::run_gross_harm_screen(f, make_summarizer_factory(), spec_g));
        };
        AE_CHECK(!grading(trial_arm::baseline, 3).invalid_insufficient_grading,
                 "S5h: exactly the minimum graded fraction is enough");
        AE_CHECK(grading(trial_arm::baseline, 4).invalid_insufficient_grading,
                 "S5h: below it in the BASELINE arm alone is insufficient");
        AE_CHECK(grading(trial_arm::treatment, 4).invalid_insufficient_grading,
                 "S5h: below it in the TREATMENT arm alone is insufficient");

        // The baseline floor at its boundary: 8 tasks, baseline succeeding in exactly 2 -> 0.25.
        auto floor_plan = make_cell_factory([](trial_arm, std::size_t t) { return t < 2 ? cell::ok : cell::wrong; });
        auto r_floor = drive(ev::run_gross_harm_screen(floor_plan, make_summarizer_factory(), base_spec(8)));
        AE_CHECK(r_floor.baseline_success_rate == 0.25 && !r_floor.invalid_uninformative_baseline &&
                     r_floor.flagged.has_value() && !*r_floor.flagged,
                 "S5h: a baseline exactly at min_baseline_success_rate is informative");
    }

    // ---- Scenario 5e: a baseline floor carries no information --------------------------------------
    {
        auto spec = base_spec(6);
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::wrong; });
        auto r = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(r.baseline_success_rate == 0.0 && r.invalid_uninformative_baseline && r.invalid,
                 "S5e: a suite the baseline fails everywhere is invalid -- there is no success left to harm");
        AE_CHECK(!r.flagged.has_value(), "S5e: no verdict is claimed");
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

        // 2*5*6 = 60 trials * max_turns 4 * 2 calls per turn (agent + summarizer) = 480 calls
        auto too_many_calls = base_spec(6);
        too_many_calls.max_model_calls = 479;
        expect_rejected(too_many_calls, "eval.screen_model_call_budget",
                        "S7d: trials * max_turns * 2 over max_model_calls -> rejected before any model call (I8)");

        // 60 trials * 2 * max_turns wraps to 16 in 64 bits; the per-trial guard must catch it first.
        auto wrapping = base_spec(6);
        wrapping.max_turns = 153722867280912931ull;
        expect_rejected(wrapping, "eval.screen_model_call_budget",
                        "S7d3: a max_turns whose call product wraps around 2^64 is still rejected");

        auto zero_turns = base_spec(6);
        zero_turns.max_turns = 0;
        expect_rejected(zero_turns, "eval.screen_max_turns_required", "S7d4: max_turns = 0 is rejected");

        auto no_turns = base_spec(6);
        no_turns.max_turns = std::nullopt;
        expect_rejected(no_turns, "eval.screen_max_turns_required",
                        "S7d2: an unset max_turns is rejected (it left each trial's calls unbounded)");

        auto too_much_work = base_spec(6);  // 2000 * 6 * (2*5 + 1) = 132,000 permutation units
        too_much_work.max_permutation_work = 131'999;
        expect_rejected(too_much_work, "eval.gross_harm_permutation_budget",
                        "S7e: permutation work over max_permutation_work -> rejected (closes R6-Num3)");

        auto at_budget = base_spec(6);
        at_budget.max_model_calls = 480;
        at_budget.max_permutation_work = 132'000;
        auto ok = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), at_budget));
        AE_CHECK(!ok.setup_error.has_value() && ok.trials.size() == 60u,
                 "S7f: positive control -- exactly at both budgets still runs");
        std::size_t agent_calls = 0, summarizer_calls = 0;  // counted from a run that keeps transcripts
        auto at_budget_kept = at_budget;
        at_budget_kept.retain_recordings = true;
        auto kept = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), at_budget_kept));
        for (auto const& d : kept.trials) {
            agent_calls += d.trial_result.recordings.size();
            summarizer_calls += d.trial_result.summarizer_recordings.size();
        }
        AE_CHECK(summarizer_calls > 0 && agent_calls + summarizer_calls <= at_budget.max_model_calls,
                 "S7f: every summarizer call is recorded, and agent + summarizer calls fit the budget");

        auto bad_alpha = base_spec(3);
        bad_alpha.alpha = 1.0;
        expect_rejected(bad_alpha, "eval.gross_harm_alpha_range", "S7g: alpha outside (0,1) -> rejected");

        auto huge_k = base_spec(1);
        huge_k.k_per_arm = ev::kMaxHypergeometricK + 1;
        expect_rejected(huge_k, "eval.gross_harm_k_too_large", "S7h: K beyond kMaxHypergeometricK -> rejected");

        auto nan_alpha = base_spec(6);
        nan_alpha.alpha = std::numeric_limits<double>::quiet_NaN();
        expect_rejected(nan_alpha, "eval.gross_harm_alpha_range",
                        "S7i: a NaN alpha is rejected (it used to pass and silence every flag)");

        auto nan_graded = base_spec(6);
        nan_graded.min_graded_fraction = std::numeric_limits<double>::quiet_NaN();
        expect_rejected(nan_graded, "eval.gross_harm_min_graded_fraction_range",
                        "S7i2: a NaN min_graded_fraction is rejected");

        auto nan_floor = base_spec(6);
        nan_floor.min_baseline_success_rate = std::numeric_limits<double>::quiet_NaN();
        expect_rejected(nan_floor, "eval.gross_harm_min_baseline_success_rate_range",
                        "S7i3: a NaN min_baseline_success_rate is rejected");

        auto nan_missing = base_spec(6);
        nan_missing.max_differential_missingness = std::numeric_limits<double>::quiet_NaN();
        expect_rejected(nan_missing, "eval.gross_harm_differential_missingness_range",
                        "S7j: a NaN missingness bound is rejected (it used to disable the check)");

        expect_rejected(base_spec(4), "eval.gross_harm_sum_test_unreachable",
                        "S7k: 4 tasks -> the sum test can never reach alpha/2 -> rejected, not run blind");

        auto small_k = base_spec(6);
        small_k.k_per_arm = 3;  // 1 / C(6,3) = 1/20 -> never below alpha/2 = 0.05
        expect_rejected(small_k, "eval.gross_harm_min_task_test_unreachable",
                        "S7l: K=3 -> the min-task test can never reach alpha/2 -> rejected");

        auto few_perms = base_spec(6);
        few_perms.num_permutations = 9;
        expect_rejected(few_perms, "eval.gross_harm_sum_test_unreachable",
                        "S7m: too few permutations for either test to reach alpha/2 -> rejected");

        // Round-2 finding: round 1 accepted this -- the MEAN p at the most extreme data (1/32, smoothed)
        // is under alpha/2 = 0.032, but the realised p is under it only ~55% of the time.
        auto marginal = base_spec(5);
        marginal.alpha = 0.064;
        expect_rejected(marginal, "eval.gross_harm_sum_test_unreachable",
                        "S7n: a test that would catch total harm only about half the time -> rejected");
        auto five_tasks = base_spec(5);  // the same 5 tasks at alpha 0.10 flag reliably
        auto five_ok = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), five_tasks));
        AE_CHECK(!five_ok.setup_error.has_value(), "S7n: positive control -- 5 tasks at the default alpha run");

        auto zero_k = base_spec(3);
        zero_k.k_per_arm = 0;
        expect_rejected(zero_k, "eval.gross_harm_k_zero", "S7o: K = 0 -> rejected");

        auto empty_suite = base_spec(6);
        empty_suite.suite_id = "";
        expect_rejected(empty_suite, "eval.gross_harm_suite_id_invalid", "S7p: an empty suite_id -> rejected");

        auto colon_suite = base_spec(6);
        colon_suite.suite_id = "dev:suite";
        expect_rejected(colon_suite, "eval.gross_harm_suite_id_invalid",
                        "S7p: a ':' in suite_id -> rejected (trial_ids could not mint a principal)");

        auto colon_task = base_spec(6);
        colon_task.tasks[3].task_id = "task:3";
        expect_rejected(colon_task, "eval.gross_harm_task_id_invalid",
                        "S7q: a ':' in a task_id -> rejected, not every one of its trials silently ungraded");

        auto empty_task = base_spec(6);
        empty_task.tasks[1].task_id = "";
        expect_rejected(empty_task, "eval.gross_harm_task_id_invalid", "S7q: an empty task_id -> rejected");

        auto bad_lesson = base_spec(6);
        bad_lesson.candidate.value = "";
        expect_rejected(bad_lesson, "eval.value_length",
                        "S7r: a candidate the lesson template rejects -> refused before any baseline trial runs");
    }

    // ---- Scenario 8: every trial gets its own derived seed ------------------------------------------
    {
        auto factory = make_cell_factory([](trial_arm, std::size_t) { return cell::ok; });
        auto spec = base_spec(6);
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

    // ---- Scenario 9: transcripts dropped by default; per-test alpha is alpha/2 ---------------------
    {
        auto factory = make_cell_factory([](trial_arm arm, std::size_t) {
            return arm == trial_arm::treatment ? cell::wrong : cell::ok;
        });
        auto dropped = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), base_spec(6)));
        bool none_kept = !dropped.trials.empty();
        for (auto const& d : dropped.trials) {
            none_kept = none_kept && d.trial_result.recordings.empty() && d.trial_result.summarizer_recordings.empty();
        }
        AE_CHECK(none_kept, "S9: by default no trial keeps its full transcripts (memory grows trials x turns^2)");

        auto keep = base_spec(6);
        keep.retain_recordings = true;
        auto kept = drive(ev::run_gross_harm_screen(factory, make_summarizer_factory(), keep));
        bool all_kept = true;
        for (auto const& d : kept.trials) {
            all_kept = all_kept && !d.trial_result.recordings.empty() && !d.trial_result.summarizer_recordings.empty();
        }
        AE_CHECK(all_kept && kept.flagged == dropped.flagged && kept.sum_pvalue == dropped.sum_pvalue,
                 "S9: retain_recordings keeps them, and the verdict does not depend on them");
        AE_CHECK(dropped.per_test_alpha == 0.05,
                 "S9: each test is compared against alpha/2 (Bonferroni), so `alpha` bounds the screen");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_gross_harm_screen: all checks passed\n";
    return 0;
}
