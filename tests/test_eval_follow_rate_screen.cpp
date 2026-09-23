// Implements decisions/ADR-181-evaluation-harness.md §3.0 item 2: the first real end-to-end proof
// that multi-trial orchestration is wired to tier1_statistics.hpp for real -- N+N real run_trial
// calls, structurally graded, aggregated into follow_rate_screen_passes/clopper_pearson_lower_bound.
// The model is scripted (deterministic) throughout; this file proves the SCREEN's own new logic
// (arm shuffling, ITT ungraded accounting, invalidity detection), not run_trial itself (already
// proven end to end, including live, by test_eval_trial_driver{,_live_e2e}.cpp).

#include <iostream>
#include <memory>
#include <memory_resource>
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
        // is exhausted) is deliberate: Scenario 4b (a lesson that stops the agent finishing) needs a client that
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
    return [](ae::eval::trial_arm) { return MockSummarizerClient{}; };
}

// A fixed script for baseline, a fixed script for treatment -- picked by the arm the driver tells
// the factory it's building a client for. This is what a `ScriptedChatClient`, which has no context
// to read, needs in place of what a real model naturally does (behave differently per arm because
// the CONTEXT it reads differs, not because it's a different client).
auto make_arm_scripted_factory(std::vector<ScriptStep> baseline_script,
                                std::vector<ScriptStep> treatment_script) {
    return [baseline_script = std::move(baseline_script),
            treatment_script = std::move(treatment_script)](ae::eval::trial_arm arm) {
        return ScriptedChatClient(arm == ae::eval::trial_arm::treatment ? treatment_script
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

    ScriptedChatClient operator()(ae::eval::trial_arm arm) {
        if (arm == ae::eval::trial_arm::treatment) return ScriptedChatClient(treatment_);
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
    return spec;
}

std::vector<ScriptStep> followed_script() {
    return {tool_step("set_deploy_region", R"({"region":"eu-west-1"})"), text_step("done")};
}
std::vector<ScriptStep> not_followed_script() {
    return {tool_step("set_deploy_region", R"({"region":"us-east-1"})"), text_step("done")};
}
// Repeats a tool call forever -- with spec.max_turns == 4 (base_probe_spec), this never reaches a
// final text answer, so `outcome` fails (max_turns exhausted) and the trial grades `ungraded`.
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

std::vector<ScriptStep> never_converges_script() {
    return {tool_step("set_deploy_region", R"({"region":"eu-west-1"})")};
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

        ev::FollowRateProbeSpec over_budget = base_probe_spec();  // 2 * 10 trials * 4 turns = 80
        over_budget.max_model_calls = 79;
        auto r_budget = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), over_budget));
        AE_CHECK(r_budget.setup_error.has_value() && r_budget.setup_error->code == "eval.screen_model_call_budget",
                 "S6e: trials * max_turns over max_model_calls is rejected before any model call (I8)");
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
        for (auto const& d : dropped.trials) none_kept = none_kept && d.trial_result.recordings.empty();
        AE_CHECK(none_kept && !dropped.trials.empty(), "S8: by default no trial keeps its full transcripts");

        ev::FollowRateProbeSpec keep = base_probe_spec();
        keep.retain_recordings = true;
        auto kept = drive(ev::run_follow_rate_screen(factory, make_summarizer_factory(), keep));
        bool all_kept = true;
        for (auto const& d : kept.trials) all_kept = all_kept && !d.trial_result.recordings.empty();
        AE_CHECK(all_kept && kept.pass == dropped.pass,
                 "S8: retain_recordings keeps them, and the verdict does not depend on them");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_follow_rate_screen: all checks passed\n";
    return 0;
}
