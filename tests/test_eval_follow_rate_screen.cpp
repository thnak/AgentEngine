// Implements decisions/ADR-195-evaluation-harness.md §3.0 item 2: the first real end-to-end proof
// that multi-trial orchestration is wired to tier1_statistics.hpp for real -- N+N real run_trial
// calls, structurally graded, aggregated into follow_rate_screen_passes/clopper_pearson_lower_bound.
// The model is scripted (deterministic) throughout; this file proves the SCREEN's own new logic
// (arm shuffling, ITT ungraded accounting, invalidity detection), not run_trial itself (already
// proven end to end, including live, by test_eval_trial_driver{,_live_e2e}.cpp).

#include <iostream>
#include <memory>
#include <memory_resource>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentengine/eval/eval_follow_rate_screen.hpp"

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
        // is exhausted) is deliberate: Scenario 4b (a lesson that stops the agent finishing) needs a client that
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
        upd.usage        = ae::Usage{tokens_each, tokens_each, 0, 0, 0.0};
        (void)pair.producer.push(upd);
        pair.producer.close();
        return std::move(pair.consumer);
    }
    std::uint64_t tokens_each = 1;  // input and output tokens each summarizer call reports
};
static_assert(ae::ChatClient<MockSummarizerClient>);

auto make_summarizer_factory() {
    return [](ae::eval::TrialSlot const&) { return MockSummarizerClient{}; };
}

// A fixed script for baseline, a fixed script for treatment -- picked by the arm the driver tells
// the factory it's building a client for. This is what a `ScriptedChatClient`, which has no context
// to read, needs in place of what a real model naturally does (behave differently per arm because
// the CONTEXT it reads differs, not because it's a different client).
auto make_arm_scripted_factory(std::vector<ScriptStep> baseline_script,
                                std::vector<ScriptStep> treatment_script) {
    return [baseline_script = std::move(baseline_script),
            treatment_script = std::move(treatment_script)](ae::eval::TrialSlot const& slot) {
        return ScriptedChatClient(slot.arm == ae::eval::trial_arm::treatment ? treatment_script
                                                                               : baseline_script);
    };
}

// Like make_arm_scripted_factory, but the BASELINE arm's script alternates: every `every_nth` call
// (starting at call 0) it uses `baseline_follow_script` instead of `baseline_other_script` -- the
// shape Scenario 3 (baseline too easy) needs. A fresh instance must be constructed per screen run
// so its internal counter starts at 0 -- Scenario 5's determinism check relies on this.
class PeriodicBaselineScriptFactory {
public:
    PeriodicBaselineScriptFactory(std::vector<ScriptStep> baseline_other_script,
                                   std::vector<ScriptStep> baseline_follow_script, int every_nth,
                                   std::vector<ScriptStep> treatment_script)
        : baseline_other_(std::move(baseline_other_script)),
          baseline_follow_(std::move(baseline_follow_script)), every_nth_(every_nth),
          treatment_(std::move(treatment_script)), baseline_counter_(std::make_shared<int>(0)) {}

    ScriptedChatClient operator()(ae::eval::TrialSlot const& slot) {
        if (slot.arm == ae::eval::trial_arm::treatment) return ScriptedChatClient(treatment_);
        int const idx = (*baseline_counter_)++;
        if (every_nth_ > 0 && idx % every_nth_ == 0) return ScriptedChatClient(baseline_follow_);
        return ScriptedChatClient(baseline_other_);
    }

private:
    std::vector<ScriptStep> baseline_other_;
    std::vector<ScriptStep> baseline_follow_;
    int every_nth_;
    std::vector<ScriptStep> treatment_;
    std::shared_ptr<int> baseline_counter_;
};

ae::Message user_message(std::string text) {
    ae::Message m{};
    m.role = ae::role::user;
    ae::ContentItem item{};
    item.origin = ae::content_origin::user;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

ae::eval::LessonCandidate deploy_region_candidate() {
    return ae::eval::LessonCandidate{"deploy-region", "default", "the default region is eu-west-1",
                                      "run-0/turn-0"};
}

ae::eval::StubToolFixture set_deploy_region_fixture() {
    ae::eval::StubToolFixture fixture;
    fixture.name              = "set_deploy_region";
    fixture.description       = "Sets the deployment region.";
    fixture.args_schema_json  = R"({"type":"object","properties":{"region":{"type":"string"}}})";
    fixture.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    fixture.canned_reply = ae::json::Value::make_object({{"ok", ae::json::Value::make_bool(true)}});
    return fixture;
}

ae::eval::FollowRateProbeSpec base_probe_spec() {
    ae::eval::FollowRateProbeSpec spec;
    spec.probe_id         = "deploy-region-probe";
    spec.candidate        = deploy_region_candidate();
    spec.template_version = "v1";
    spec.lesson_salience  = 0.3f;
    spec.task_prompt      = user_message("please set up the deploy region");
    spec.stub_tools       = {set_deploy_region_fixture()};
    spec.grader           = ae::eval::make_tool_argument_grader("set_deploy_region", "region", "eu-west-1");
    spec.n_per_arm        = 10;
    spec.max_turns        = 4;
    spec.seed             = 42;
    spec.max_retried_trials = 0;  // retries are exercised in their own scenario (S10)
    return spec;
}

std::vector<ScriptStep> followed_script() {
    return {tool_step("set_deploy_region", R"({"region":"eu-west-1"})"), text_step("done")};
}
std::vector<ScriptStep> not_followed_script() {
    return {tool_step("set_deploy_region", R"({"region":"us-east-1"})"), text_step("done")};
}
// The grader cannot judge a trial whose tool call carried the sentinel "boom" -- it throws, which the
// screen maps to `ungraded` (a failed MEASUREMENT, §3.5), the one way left to produce missing data.
std::vector<ScriptStep> unmeasurable_script() {
    return {tool_step("set_deploy_region", R"({"region":"boom"})"), text_step("done")};
}
ae::eval::GraderFn throwing_on_boom_grader() {
    auto inner = ae::eval::make_tool_argument_grader("set_deploy_region", "region", "eu-west-1");
    return [inner](ae::eval::TrialResult const& t) {
        for (auto const& c : t.tool_calls) {
            auto const* r = c.arguments.find("region");
            if (r != nullptr && r->is_string() && r->as_string() == "boom") throw std::runtime_error("unmeasurable");
        }
        return inner(t);
    };
}

// Repeats a tool call forever -- with spec.max_turns == 4 (base_probe_spec), this never reaches a
// final text answer, so `outcome` fails (max_turns exhausted): an OUTCOME, graded `failure`.
std::vector<ScriptStep> never_converges_script() {
    return {tool_step("set_deploy_region", R"({"region":"eu-west-1"})")};
}

// A provider fault -- `failure_class::transient`, a failed measurement (`ungraded`).
std::vector<ScriptStep> transient_script() {
    return {fail_step(ae::error{ae::failure_class::transient, "HTTP 503", "provider.http_503"})};
}

// Per-trial control: `plan(arm, k)` where k counts that arm's trials so far.
template <class Plan>
auto make_trial_factory(Plan plan) {
    auto counts = std::make_shared<std::array<int, 2>>();
    return [plan, counts](ae::eval::TrialSlot const& slot) {
        int const k = (*counts)[slot.arm == ae::eval::trial_arm::treatment ? 1 : 0]++;
        return ScriptedChatClient(plan(slot.arm, k));
    };
}

}  // namespace

int main() {
    namespace ev = ae::eval;

    // ---- Scenario 1: probe clearly followed -----------------------------------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        auto factory = make_arm_scripted_factory(/*baseline=*/not_followed_script(),
                                                   /*treatment=*/followed_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!result.setup_error.has_value(), "S1: no setup error");
        AE_CHECK(!result.invalid, "S1: the screen is valid");
        AE_CHECK(result.baseline_followed == 0, "S1: baseline never follows (negative control)");
        AE_CHECK(result.treatment_followed == spec.n_per_arm, "S1: every treatment trial follows");
        AE_CHECK(result.pass.has_value() && *result.pass, "S1: the screen passes");
        AE_CHECK(result.treatment_lower_bound.has_value() && *result.treatment_lower_bound >= 0.5,
                 "S1: the reported CP lower bound clears the target");
        AE_CHECK(result.trials.size() == 2 * spec.n_per_arm, "S1: every trial is recorded, none dropped");
    }

    // ---- Scenario 2: probe never followed ---------------------------------------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        auto factory = make_arm_scripted_factory(/*baseline=*/not_followed_script(),
                                                   /*treatment=*/not_followed_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!result.setup_error.has_value(), "S2: no setup error");
        AE_CHECK(!result.invalid, "S2: the screen is valid (baseline never follows either)");
        AE_CHECK(result.treatment_followed == 0, "S2: treatment never follows");
        AE_CHECK(result.pass.has_value() && !*result.pass, "S2: the screen fails");
        AE_CHECK(result.treatment_lower_bound.has_value() && *result.treatment_lower_bound < 0.5,
                 "S2: the reported CP lower bound is below the target");
    }

    // ---- Scenario 3: baseline follow rate too high (probe is guessable) ---------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        // Every 2nd baseline trial (50%) follows -- well above the 10% threshold.
        PeriodicBaselineScriptFactory factory(/*baseline_other=*/not_followed_script(),
                                                /*baseline_follow=*/followed_script(),
                                                /*every_nth=*/2,
                                                /*treatment=*/followed_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!result.setup_error.has_value(), "S3: no setup error");
        AE_CHECK(result.invalid_baseline_too_easy, "S3: flagged as baseline-too-easy");
        AE_CHECK(result.invalid, "S3: the overall screen is invalid");
        AE_CHECK(!result.pass.has_value(), "S3: no pass/fail claim is made for an invalid screen");
    }

    // ---- Scenario 4: differential missingness -- the MEASUREMENT fails in one arm only -------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        spec.grader = throwing_on_boom_grader();
        auto factory = make_arm_scripted_factory(/*baseline=*/not_followed_script(),
                                                   /*treatment=*/unmeasurable_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!result.setup_error.has_value(), "S4: no setup error");
        AE_CHECK(result.treatment_ungraded == spec.n_per_arm, "S4: every treatment trial is ungraded");
        AE_CHECK(result.baseline_ungraded == 0, "S4: no baseline trial is ungraded");
        AE_CHECK(result.invalid_differential_missingness, "S4: flagged as differential missingness");
        AE_CHECK(result.invalid, "S4: the overall screen is invalid");
        AE_CHECK(!result.pass.has_value(), "S4: no pass/fail claim is made for an invalid screen");
    }

    // ---- Scenario 4b: gross-harm red-team fix -- an agent that never finishes did not follow ------
    // Before the fix this was Scenario 4: a treatment arm that never converged counted as MISSING, and
    // the screen reported `invalid` instead of a verdict. Not finishing is an outcome (ITT): it is
    // "not followed", so the screen runs and fails.
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        auto factory = make_arm_scripted_factory(/*baseline=*/not_followed_script(),
                                                   /*treatment=*/never_converges_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(!result.setup_error.has_value() && !result.invalid,
                 "S4b: a never-finishing treatment arm is a valid run, not missing data");
        AE_CHECK(result.treatment_ungraded == 0 && result.treatment_followed == 0,
                 "S4b: every non-finishing trial counts as not followed");
        AE_CHECK(result.pass.has_value() && !*result.pass, "S4b: the screen fails");
    }

    // ---- Scenario 4c: gross-harm red-team fix -- no verdict from no data --------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        spec.grader = throwing_on_boom_grader();
        auto factory = make_arm_scripted_factory(unmeasurable_script(), unmeasurable_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(result.invalid_insufficient_grading && !result.invalid_differential_missingness,
                 "S4c: both arms unmeasurable -> invalid for insufficient grading (the arms' EQUAL "
                 "ungraded rates no longer let it pass as valid)");
        AE_CHECK(!result.pass.has_value(), "S4c: no pass/fail claim is made from zero data");
    }

    // ---- Scenario 4d: missingness on counts, at its boundary --------------------------------------
    // Round-2 red-team finding (MINOR): the rule compared floating-point RATES, so at n = 20 a one-trial
    // gap was valid for B=0/T=1 but invalid for B=3/T=4. It now compares counts: at most
    // floor(0.05 * 20) = 1 trial apart. Also proves a transient provider fault is `ungraded`.
    {
        auto run_gap = [&](int b_ungraded, int t_ungraded) {
            auto f = make_trial_factory([=](ev::trial_arm arm, int k) {
                int const limit = arm == ev::trial_arm::treatment ? t_ungraded : b_ungraded;
                return k < limit ? transient_script() : not_followed_script();
            });
            ev::FollowRateProbeSpec spec = base_probe_spec();
            spec.n_per_arm = 20;
            spec.min_graded_fraction = 0.0;  // isolate the missingness rule
            return drive(ev::run_follow_rate_screen(f, make_summarizer_factory(), spec));
        };
        auto r34 = run_gap(3, 4);
        AE_CHECK(r34.baseline_ungraded == 3 && r34.treatment_ungraded == 4,
                 "S4d: transient provider faults are ungraded (failed measurements)");
        AE_CHECK(!r34.invalid_differential_missingness, "S4d: B=3, T=4 -- a one-trial gap is within the bound");
        AE_CHECK(!run_gap(0, 1).invalid_differential_missingness, "S4d: B=0, T=1 -- the same gap, the same verdict");
        AE_CHECK(run_gap(0, 2).invalid_differential_missingness, "S4d: a two-trial gap trips the rule");
    }

    // ---- Scenario 4e: insufficient grading at its boundary, in each arm alone ---------------------
    {
        auto run_grading = [&](ev::trial_arm which, int ungraded) {
            auto f = make_trial_factory([=](ev::trial_arm arm, int k) {
                return (arm == which && k < ungraded) ? transient_script() : not_followed_script();
            });
            ev::FollowRateProbeSpec spec = base_probe_spec();  // n = 10, floor 0.9
            spec.max_differential_missingness = 1.0;          // isolate the grading rule
            return drive(ev::run_follow_rate_screen(f, make_summarizer_factory(), spec));
        };
        AE_CHECK(!run_grading(ev::trial_arm::baseline, 1).invalid_insufficient_grading,
                 "S4e: exactly the minimum graded fraction (9 of 10) is enough");
        AE_CHECK(run_grading(ev::trial_arm::baseline, 2).invalid_insufficient_grading,
                 "S4e: below it in the BASELINE arm alone is insufficient");
        AE_CHECK(run_grading(ev::trial_arm::treatment, 2).invalid_insufficient_grading,
                 "S4e: below it in the TREATMENT arm alone is insufficient");
    }

    // ---- Scenario 3b: baseline-too-easy at its boundary -------------------------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();  // n = 10, threshold 0.10
        PeriodicBaselineScriptFactory one(not_followed_script(), followed_script(), 10, followed_script());
        auto r1 = drive(ev::run_follow_rate_screen(one, make_summarizer_factory(), spec));
        AE_CHECK(r1.baseline_followed == 1 && !r1.invalid_baseline_too_easy,
                 "S3b: a baseline following exactly at the threshold (1 of 10) is not too easy");
        PeriodicBaselineScriptFactory two(not_followed_script(), followed_script(), 5, followed_script());
        auto r2 = drive(ev::run_follow_rate_screen(two, make_summarizer_factory(), spec));
        AE_CHECK(r2.baseline_followed == 2 && r2.invalid_baseline_too_easy, "S3b: 2 of 10 is too easy");
    }

    // ---- Scenario 5: same-seed determinism (I5) ---------------------------------------------------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        PeriodicBaselineScriptFactory factory1(not_followed_script(), followed_script(), 3,
                                                 followed_script());
        PeriodicBaselineScriptFactory factory2(not_followed_script(), followed_script(), 3,
                                                 followed_script());
        auto result1 = drive(ev::run_follow_rate_screen(factory1, make_summarizer_factory(), spec));
        auto result2 = drive(ev::run_follow_rate_screen(factory2, make_summarizer_factory(), spec));

        AE_CHECK(result1.arm_order == result2.arm_order,
                 "S5: identical seed produces an identical arm order");
        AE_CHECK(result1.baseline_followed == result2.baseline_followed &&
                     result1.treatment_followed == result2.treatment_followed,
                 "S5: identical seed + identical scripts produce identical follow counts");
        AE_CHECK(result1.pass == result2.pass && result1.invalid == result2.invalid,
                 "S5: identical seed + identical scripts produce an identical verdict");
        AE_CHECK(result1.treatment_lower_bound == result2.treatment_lower_bound,
                 "S5: identical seed + identical scripts produce an identical CP lower bound");
    }

    // ---- Scenario 6: pre-flight validation rejects a malformed spec before running any trial ------
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        spec.n_per_arm = 0;
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        AE_CHECK(result.setup_error.has_value(), "S6a: n_per_arm==0 is rejected before running anything");
        AE_CHECK(result.trials.empty(), "S6a: zero trials were run -- no model calls spent on a bad spec");

        ev::FollowRateProbeSpec bad_alpha = base_probe_spec();
        bad_alpha.alpha = 1.5;
        auto result_alpha =
            drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), bad_alpha));
        AE_CHECK(result_alpha.setup_error.has_value(), "S6b: an out-of-range alpha is rejected");
        AE_CHECK(result_alpha.trials.empty(), "S6b: zero trials were run");

        ev::FollowRateProbeSpec nan_alpha = base_probe_spec();
        nan_alpha.alpha = std::numeric_limits<double>::quiet_NaN();
        auto r_nan = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), nan_alpha));
        AE_CHECK(r_nan.setup_error.has_value() && r_nan.trials.empty(),
                 "S6c: a NaN alpha is rejected (it used to pass validation)");

        ev::FollowRateProbeSpec no_turns = base_probe_spec();
        no_turns.max_turns = std::nullopt;
        auto r_turns = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), no_turns));
        AE_CHECK(r_turns.setup_error.has_value() && r_turns.setup_error->code == "eval.screen_max_turns_required",
                 "S6d: an unset max_turns is rejected (it left every trial's model calls unbounded)");

        // 2 * 10 trials * 4 turns * 2 calls per turn (agent + summarizer) = 160
        ev::FollowRateProbeSpec over_budget = base_probe_spec();
        over_budget.max_model_calls = 159;
        auto r_budget = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), over_budget));
        AE_CHECK(r_budget.setup_error.has_value() && r_budget.setup_error->code == "eval.screen_model_call_budget",
                 "S6e: trials * max_turns * 2 over max_model_calls is rejected before any model call (I8)");
        ev::FollowRateProbeSpec at_budget = base_probe_spec();
        at_budget.max_model_calls = 160;
        auto r_at = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), at_budget));
        AE_CHECK(!r_at.setup_error.has_value() && r_at.trials.size() == 20u,
                 "S6e: positive control -- exactly at the budget still runs");

        auto rejected = [&](ev::FollowRateProbeSpec bad, char const* code) {
            auto r = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), std::move(bad)));
            return r.setup_error.has_value() && r.setup_error->code == code && r.trials.empty();
        };
        double const nan = std::numeric_limits<double>::quiet_NaN();
        ev::FollowRateProbeSpec s1 = base_probe_spec();
        s1.min_graded_fraction = nan;
        ev::FollowRateProbeSpec s2 = base_probe_spec();
        s2.baseline_invalid_threshold = nan;
        ev::FollowRateProbeSpec s3 = base_probe_spec();
        s3.target_lower_bound = nan;
        ev::FollowRateProbeSpec s4 = base_probe_spec();
        s4.max_differential_missingness = nan;
        AE_CHECK(rejected(s1, "eval.follow_rate_min_graded_fraction_range") &&
                     rejected(s2, "eval.follow_rate_baseline_threshold_range") &&
                     rejected(s3, "eval.follow_rate_target_lower_bound_range") &&
                     rejected(s4, "eval.follow_rate_differential_missingness_range"),
                 "S6f: a NaN in any threshold is rejected, not left to silently disable its rule");

        ev::FollowRateProbeSpec zero_target = base_probe_spec();
        zero_target.target_lower_bound = 0.0;
        AE_CHECK(rejected(zero_target, "eval.follow_rate_target_lower_bound_range"),
                 "S6g: target_lower_bound = 0 is rejected (a never-followed probe would pass)");

        ev::FollowRateProbeSpec colon = base_probe_spec();
        colon.probe_id = "deploy:region";
        AE_CHECK(rejected(colon, "eval.follow_rate_probe_id_invalid"),
                 "S6h: a ':' in probe_id is rejected (trial_ids could not mint a principal)");

        ev::FollowRateProbeSpec bad_lesson = base_probe_spec();
        bad_lesson.candidate.value = "";
        AE_CHECK(rejected(bad_lesson, "eval.value_length"),
                 "S6i: a candidate the lesson template rejects is refused before any baseline trial runs");
    }

    // ---- Scenario 7: round-3 red-team fix -- each trial gets its OWN derived seed, not spec.seed
    // forwarded unchanged to all 2*n_per_arm trials (the fix for a disclosed latent design gap: once
    // something consumes TrialSpec::seed stochastically, forwarding one shared value would silently
    // correlate every trial in an arm, breaking the independent-Bernoulli-trials assumption the
    // statistics require). Also re-proves determinism now covers the derived seeds themselves. -----
    {
        ev::FollowRateProbeSpec spec = base_probe_spec();
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto result = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));

        bool any_equals_spec_seed = false;
        bool all_distinct = true;
        for (std::size_t i = 0; i < result.trials.size(); ++i) {
            if (result.trials[i].trial_seed == spec.seed) any_equals_spec_seed = true;
            for (std::size_t j = i + 1; j < result.trials.size(); ++j) {
                if (result.trials[i].trial_seed == result.trials[j].trial_seed) all_distinct = false;
            }
        }
        AE_CHECK(!any_equals_spec_seed,
                 "S7: no trial's derived seed is literally spec.seed forwarded unchanged");
        AE_CHECK(all_distinct, "S7: every trial in the screen gets its own distinct derived seed");

        auto result_again = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));
        bool seeds_match = result.trials.size() == result_again.trials.size();
        for (std::size_t i = 0; seeds_match && i < result.trials.size(); ++i) {
            seeds_match = result.trials[i].trial_seed == result_again.trials[i].trial_seed;
        }
        AE_CHECK(seeds_match, "S7: derived per-trial seeds are themselves deterministic given the "
                               "same spec.seed (I5)");
    }

    // ---- Scenario 8: transcripts are dropped after grading unless the host opts in ----------------
    {
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto dropped = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), base_probe_spec()));
        bool none_kept = true;
        for (auto const& d : dropped.trials) {
            none_kept = none_kept && d.trial_result.recordings.empty() && d.trial_result.summarizer_recordings.empty();
        }
        AE_CHECK(none_kept && !dropped.trials.empty(), "S8: by default no trial keeps its full transcripts");

        ev::FollowRateProbeSpec keep = base_probe_spec();
        keep.retain_recordings = true;
        auto kept = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), keep));
        bool all_kept = true;
        for (auto const& d : kept.trials) {
            all_kept = all_kept && !d.trial_result.recordings.empty() && !d.trial_result.summarizer_recordings.empty();
        }
        AE_CHECK(all_kept && kept.pass == dropped.pass,
                 "S8: retain_recordings keeps them, and the verdict does not depend on them");
    }

    // ---- Scenario 9: the arms really are interleaved -------------------------------------------------
    {
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto r = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), base_probe_spec()));
        bool treatment_then_baseline = false;
        for (std::size_t i = 0; i + 1 < r.arm_order.size(); ++i) {
            if (r.arm_order[i] == ev::trial_arm::treatment && r.arm_order[i + 1] == ev::trial_arm::baseline) {
                treatment_then_baseline = true;
            }
        }
        AE_CHECK(treatment_then_baseline, "S9: run order interleaves the arms, not all baseline then all treatment");
    }

    // ---- Scenario 10: retries and the seed reach the factory, as in the gross-harm screen -------------
    {
        auto seen = std::make_shared<std::vector<std::pair<std::string, std::uint64_t>>>();
        // Every trial, in BOTH arms, faults on its first attempt and succeeds on its retry: provider noise.
        auto flaky = [seen](ev::TrialSlot const& slot) {
            seen->emplace_back(slot.trial_id, slot.trial_seed);
            if (slot.attempt == 0) return ScriptedChatClient(transient_script());
            return ScriptedChatClient(slot.arm == ev::trial_arm::treatment ? followed_script() : not_followed_script());
        };
        ev::FollowRateProbeSpec spec = base_probe_spec();  // 10 per arm
        spec.max_retried_trials = 20;
        auto r = drive(ev::run_follow_rate_screen(flaky, make_summarizer_factory(), spec));
        std::size_t retried = 0;
        for (auto const& d : r.trials) retried += d.attempts == 2 ? 1 : 0;
        AE_CHECK(retried == 20u && r.treatment_ungraded == 0 && r.treatment_faulted == 10 &&
                     r.baseline_faulted == 10 && r.treatment_followed == 10 && r.pass == true,
                 "S10: symmetric faults are retried once each, the retries count, and the screen passes");
        bool retry_ids = true, seeds_match = true;
        for (auto const& d : r.trials) {
            bool first = false, second = false;
            for (auto const& [id, seed] : *seen) {
                if (id == d.trial_id) first = seed == d.trial_seed;
                if (id == d.trial_id + "-retry1") second = seed == d.trial_seed;
            }
            retry_ids = retry_ids && second && d.counted_trial_id == d.trial_id + "-retry1";
            seeds_match = seeds_match && first;
        }
        AE_CHECK(seeds_match && retry_ids,
                 "S10: the factory sees each trial's recorded seed, and a retry re-runs it under its own id "
                 "with the SAME seed");

        // Treatment-only faults are the lesson's doing: retries recover the data, but the first-attempt
        // counts (10 vs 0) still trip the missingness rule, so no pass (PR #100 red team, MAJOR).
        auto lesson = [](ev::TrialSlot const& slot) {
            if (slot.arm == ev::trial_arm::treatment && slot.attempt == 0) return ScriptedChatClient(transient_script());
            return ScriptedChatClient(slot.arm == ev::trial_arm::treatment ? followed_script() : not_followed_script());
        };
        auto r2 = drive(ev::run_follow_rate_screen(lesson, make_summarizer_factory(), spec));
        AE_CHECK(r2.treatment_ungraded == 0 && r2.treatment_faulted == 10 && r2.invalid_differential_missingness &&
                     !r2.pass.has_value(),
                 "S10: lesson-only faults are not retried away -- the run is invalid, never a pass");

        // The pool is shared across the whole screen, not per trial.
        ev::FollowRateProbeSpec small = base_probe_spec();
        small.max_retried_trials = 2;
        auto r3 = drive(ev::run_follow_rate_screen(lesson, make_summarizer_factory(), small));
        std::size_t retried3 = 0;
        bool unretried_clean = true;
        for (auto const& d : r3.trials) {
            retried3 += d.attempts == 2 ? 1 : 0;
            if (d.attempts == 1) {
                unretried_clean = unretried_clean && !d.retried_error.has_value() && !d.first_attempt.has_value();
            }
        }
        AE_CHECK(retried3 == 2u && r3.treatment_ungraded == 8 && unretried_clean,
                 "S10: a pool of 2 retries exactly 2 trials; the rest record no retry, even when ungraded");
    }

    // ---- Scenario 11: the retry pool is charged to the call budget, and cannot overflow it ----------
    {
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto budget_run = [&](std::uint64_t calls) {
            ev::FollowRateProbeSpec spec = base_probe_spec();
            spec.max_retried_trials = 4;  // (20 trials + 4) * 4 turns * 2 = 192
            spec.max_model_calls = calls;
            return drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), spec));
        };
        auto over = budget_run(191);
        auto at = budget_run(192);
        AE_CHECK(over.setup_error.has_value() && over.setup_error->code == "eval.screen_model_call_budget" &&
                     !at.setup_error.has_value(),
                 "S11: (2n + max_retried_trials) * max_turns * 2 is the budget, exactly");

        ev::FollowRateProbeSpec huge = base_probe_spec();
        huge.n_per_arm = std::numeric_limits<std::uint64_t>::max() / 2;
        huge.max_retried_trials = 2;  // 2n + 2 wraps
        auto r = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), huge));
        AE_CHECK(r.setup_error.has_value() && r.setup_error->code == "eval.screen_model_call_budget" && r.trials.empty(),
                 "S11: trials + retried trials that would wrap is rejected");

        // The per-trial cap must be taken over trials + retries: with 2 trials and a pool of 2^32 - 1, a
        // max_turns of 2^31 fits `max_model_calls / 2 trials` easily, but (2 + 2^32 - 1) * 2 * 2^31 wraps
        // 2^64 to a small number.
        ev::FollowRateProbeSpec wide = base_probe_spec();
        wide.n_per_arm = 1;
        wide.max_retried_trials = std::numeric_limits<std::uint32_t>::max();
        wide.max_model_calls = std::numeric_limits<std::uint64_t>::max();
        wide.max_turns = 2147483648ull;
        auto rw = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), wide));
        AE_CHECK(rw.setup_error.has_value() && rw.setup_error->code == "eval.screen_model_call_budget" && rw.trials.empty(),
                 "S11: a call product that wraps only once the retry pool is counted is still rejected");
    }

    // ---- Scenario 12: the summarizer budget defaults to token_budget here too -------------------------
    {
        auto factory = make_arm_scripted_factory(not_followed_script(), followed_script());
        auto heavy = [](ev::TrialSlot const&) {
            MockSummarizerClient c;
            c.tokens_each = 50;  // 100 tokens a call
            return c;
        };
        ev::FollowRateProbeSpec spec = base_probe_spec();
        spec.retain_recordings = true;
        spec.token_budget = 100;
        auto r = drive(ev::run_follow_rate_screen(factory, heavy, spec));
        bool capped = !r.trials.empty();
        for (auto const& d : r.trials) {
            capped = capped && d.trial_result.summarizer_recordings.size() == 1u && d.trial_result.summarizer_budget_exhausted;
        }
        AE_CHECK(capped, "S12: an unset summarizer budget inherits token_budget (one 100-token call, then refused)");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_follow_rate_screen: all checks passed\n";
    return 0;
}
