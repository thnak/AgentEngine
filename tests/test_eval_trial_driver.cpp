// Implements decisions/ADR-181-evaluation-harness.md §3.0 items 2-4 / §3.2 / §3.4: the first real
// end-to-end trial-running driver, `run_trial`. Proves both delivery routes (context injection and
// recall), the baseline arm's negative control, the arm/candidate contract check, and that the
// harness seeds a lesson at exactly the caller-supplied salience -- against a REAL AgentSession,
// ComposedContextProvider, MemoryProvider and stub tools, not a model of any of them. The model is
// scripted (deterministic), and the summarizer is a fixed mock -- no live call in this test; see
// test_eval_trial_driver_live_e2e.cpp for the real-DeepSeek sanity check.

#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/eval/eval_trial.hpp"

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

// One scripted round: either a tool call or a final text answer.
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
        ScriptStep const& step = state_->script.at(state_->next++);
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

// A fixed, deterministic mock summarizer -- no live call in this test (an explicit scoping choice:
// the summarizer's own fidelity is orthogonal to what this slice proves).
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

}  // namespace

int main() {
    namespace ev = ae::eval;

    // ---- Scenario 1: context-injection delivery ----------------------------------------------------
    {
        ev::TrialSpec spec;
        spec.arm              = ev::trial_arm::treatment;
        spec.candidate        = deploy_region_candidate();
        spec.template_version = "v1";
        spec.lesson_salience  = 0.3f;
        spec.task_prompt      = user_message("please set up the deploy region");
        spec.stub_tools       = {set_deploy_region_fixture()};
        spec.trial_id         = "t1-context-injection";
        spec.max_turns        = 4;

        ScriptedChatClient client(
            {tool_step("set_deploy_region", R"({"region":"eu-west-1"})"), text_step("done")});
        auto result = drive(ev::run_trial(client, MockSummarizerClient{}, spec));

        AE_CHECK(!result.setup_error.has_value(), "S1: no setup error");
        AE_CHECK(result.outcome.has_value(), "S1: the trial converges");
        AE_CHECK(result.delivered, "S1: the lesson is delivered via context injection");
        AE_CHECK(!result.delivered_via_recall, "S1: not delivered via recall (recall never called)");
        AE_CHECK(!result.recall_invoked, "S1: recall was never invoked");
        AE_CHECK(result.tool_calls.size() == 1 && result.tool_calls[0].tool_name == "set_deploy_region",
                 "S1: exactly one stub tool call was captured, naming the right tool");
        AE_CHECK(result.seeded_item.has_value() && result.seeded_item->salience == 0.3f,
                 "S1: the seeded item's salience is EXACTLY the caller-supplied constant (§3.2)");
    }

    // ---- Scenario 2: recall-route delivery (context injection disabled) -----------------------------
    {
        ev::TrialSpec spec;
        spec.arm              = ev::trial_arm::treatment;
        spec.candidate        = deploy_region_candidate();
        spec.template_version = "v1";
        spec.lesson_salience  = 0.3f;
        spec.task_prompt      = user_message("please set up the deploy region");
        spec.stub_tools       = {set_deploy_region_fixture()};
        spec.trial_id         = "t2-recall-route";
        spec.max_turns        = 6;
        spec.max_injected     = 0;  // nothing is ever injected via on_context

        ScriptedChatClient client({tool_step("recall", R"({"query":"deploy region default"})"),
                                    tool_step("set_deploy_region", R"({"region":"eu-west-1"})"),
                                    text_step("done")});
        auto result = drive(ev::run_trial(client, MockSummarizerClient{}, spec));

        AE_CHECK(!result.setup_error.has_value(), "S2: no setup error");
        AE_CHECK(result.outcome.has_value(), "S2: the trial converges");
        AE_CHECK(!result.delivered, "S2: NOT delivered via context injection (max_injected=0)");
        AE_CHECK(result.recall_invoked, "S2: recall was invoked");
        AE_CHECK(result.delivered_via_recall,
                 "S2: delivered via recall's own result surfacing in a later round's request");
    }

    // ---- Scenario 3: baseline arm -- negative control -----------------------------------------------
    {
        ev::TrialSpec spec;
        spec.arm         = ev::trial_arm::baseline;
        spec.candidate   = std::nullopt;
        spec.task_prompt = user_message("please set up the deploy region");
        spec.stub_tools  = {set_deploy_region_fixture()};
        spec.trial_id    = "t3-baseline";
        spec.max_turns   = 4;

        ScriptedChatClient client(
            {tool_step("set_deploy_region", R"({"region":"us-east-1"})"), text_step("done")});
        auto result = drive(ev::run_trial(client, MockSummarizerClient{}, spec));

        AE_CHECK(!result.setup_error.has_value(), "S3: no setup error");
        AE_CHECK(result.outcome.has_value(), "S3: the trial converges");
        AE_CHECK(!result.delivered && !result.delivered_via_recall,
                 "S3: baseline never delivers a lesson (there isn't one)");
        AE_CHECK(!result.seeded_item.has_value(), "S3: baseline seeds nothing");
        bool leaked = false;
        for (auto const& call : result.tool_calls) {
            if (call.arguments.find("region") != nullptr &&
                call.arguments.find("region")->as_string().find("eu-west-1") != std::string::npos) {
                leaked = true;
            }
        }
        AE_CHECK(!leaked, "S3: negative control -- the candidate's value text never appears in "
                           "baseline tool-call arguments (there is no candidate)");
    }

    // ---- Scenario 4: arm/candidate contract violation -----------------------------------------------
    {
        ev::TrialSpec mismatch_a;
        mismatch_a.arm       = ev::trial_arm::treatment;
        mismatch_a.candidate = std::nullopt;  // treatment with no candidate
        mismatch_a.trial_id  = "t4a";
        auto result_a = drive(ev::run_trial(ScriptedChatClient({text_step("unused")}),
                                             MockSummarizerClient{}, mismatch_a));
        AE_CHECK(result_a.setup_error.has_value(),
                 "S4a: treatment with no candidate is a setup_error, no session ever built");

        ev::TrialSpec mismatch_b;
        mismatch_b.arm       = ev::trial_arm::baseline;
        mismatch_b.candidate = deploy_region_candidate();  // baseline WITH a candidate
        mismatch_b.trial_id  = "t4b";
        auto result_b = drive(ev::run_trial(ScriptedChatClient({text_step("unused")}),
                                             MockSummarizerClient{}, mismatch_b));
        AE_CHECK(result_b.setup_error.has_value(),
                 "S4b: baseline WITH a candidate is also a setup_error");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_trial_driver: all checks passed\n";
    return 0;
}
