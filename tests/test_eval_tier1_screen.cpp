// Implements decisions/ADR-181-evaluation-harness.md §3.0's "Tier-1 pre-registration and attempt
// accounting" (E32): the pre-registration digest is recorded before any trial runs; every started attempt
// for a lesson family is counted; the `Tier1ScreenResult` names the count and shows every attempt's figures,
// not only the latest. The model is scripted throughout; the two screens themselves are proven by
// test_eval_follow_rate_screen.cpp and test_eval_gross_harm_screen.cpp.

#include <atomic>
#include <cmath>
#include <map>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/eval/eval_tier1_screen.hpp"

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

// ---- Scripted clients, the same shape test_eval_follow_rate_screen.cpp uses (each file self-contained).
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
        item.value  = ae::Text{"NONE"};
        item.origin = ae::content_origin::assistant;
        ae::Message reply{};
        reply.role = ae::role::assistant;
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"NONE"};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        (void)pair.producer.push(upd);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<MockSummarizerClient>);

auto summarizer_factory() {
    return [](ae::eval::TrialSlot const&) { return MockSummarizerClient{}; };
}

// How a scenario's scripted agent behaves: on the follow-rate probe (does the treatment arm follow the
// lesson?), and on the regression suite (does the treatment arm still do its tasks?).
struct Behaviour {
    bool treatment_follows = true;
    bool treatment_harmed = false;
};

// Counts the calls it gets, per screen, and on the very first call records whether the attempt had already
// been counted with the expected pre-registration -- E32's "before the screen runs".
struct CallLog {
    int probe_calls = 0;
    int harm_calls = 0;
    std::optional<bool> counted_before_first_trial;
};

template <class Store>
auto make_factory(Behaviour b, std::shared_ptr<CallLog> calls, ae::eval::Tier1AttemptLog<Store>* log = nullptr,
                  ae::eval::Tier1Family family = {}, ae::Digest expected_prereg = {}) {
    return [b, calls, log, family, expected_prereg](ae::eval::TrialSlot const& slot) {
        if (!calls->counted_before_first_trial.has_value() && log != nullptr) {
            auto attempts = log->attempts(family);
            calls->counted_before_first_trial = attempts.has_value() && !attempts->empty() &&
                                                attempts->back().preregistration == expected_prereg &&
                                                !attempts->back().completed;
        }
        bool const treatment = slot.arm == ae::eval::trial_arm::treatment;
        if (slot.trial_id.starts_with("dev-suite-")) {
            ++calls->harm_calls;
            bool const ok = !(treatment && b.treatment_harmed);
            return ScriptedChatClient({tool_step("do_task", ok ? R"({"result":"ok"})" : R"({"result":"wrong"})"),
                                       text_step("done")});
        }
        ++calls->probe_calls;
        bool const follows = treatment && b.treatment_follows;
        return ScriptedChatClient(
            {tool_step("set_deploy_region", follows ? R"({"region":"eu-west-1"})" : R"({"region":"us-east-1"})"),
             text_step("done")});
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

ae::eval::StubToolFixture fixture(std::string name, std::string arg) {
    ae::eval::StubToolFixture f;
    f.name              = std::move(name);
    f.description       = "A stub tool.";
    f.args_schema_json  = R"({"type":"object","properties":{")" + arg + R"(":{"type":"string"}}})";
    f.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    f.canned_reply      = ae::json::Value::make_object({{"ok", ae::json::Value::make_bool(true)}});
    return f;
}

ae::eval::LessonCandidate candidate() {
    return ae::eval::LessonCandidate{"deploy-region", "default", "the default region is eu-west-1", "run-7/turn-2"};
}

ae::eval::FollowRateProbeSpec probe(std::string id = "deploy-region-probe") {
    ae::eval::FollowRateProbeSpec p;
    p.probe_id           = std::move(id);
    p.candidate          = candidate();
    p.template_version   = "v1";
    p.lesson_salience    = 0.3f;
    p.task_prompt        = user_message("please set up the deploy region");
    p.stub_tools         = {fixture("set_deploy_region", "region")};
    p.grader             = ae::eval::make_tool_argument_grader("set_deploy_region", "region", "eu-west-1");
    p.n_per_arm          = 10;
    p.max_turns          = 4;
    p.seed               = 42;
    p.max_retried_trials = 0;
    return p;
}

ae::eval::Tier1ScreenSpec spec() {
    ae::eval::Tier1ScreenSpec s;
    s.lineage       = "session-7";
    s.suite_version = "graders-2026-09-23";
    s.operator_id   = "host-operator-1";
    s.started_at    = "2026-09-23T10:00:00Z";
    s.probes        = {probe()};
    ae::eval::GrossHarmScreenSpec& g = s.gross_harm;
    g.suite_id         = "dev-suite";
    g.candidate        = candidate();
    g.template_version = "v1";
    g.lesson_salience  = 0.3f;
    for (int t = 0; t < 6; ++t) {
        ae::eval::RegressionTask task;
        task.task_id     = "task-" + std::to_string(t);
        task.task_prompt = user_message("please do task " + std::to_string(t));
        task.stub_tools  = {fixture("do_task", "result")};
        task.grader      = ae::eval::make_tool_argument_grader("do_task", "result", "ok");
        g.tasks.push_back(std::move(task));
    }
    g.k_per_arm          = 5;
    g.max_turns          = 4;
    g.seed               = 42;
    g.max_retried_trials = 0;
    return s;
}

// A store whose append and read each fail on demand, for the log-failure paths.
struct FlakyStore {
    ae::rt::InMemoryAppendLogStore inner;
    bool fail_append = false;
    bool fail_read = false;
    bool hide_all = false;  // reads succeed but return nothing, as if the log had been wiped
    [[nodiscard]] ae::result<ae::rt::SeqNo> append(ae::rt::LogId const& id, std::vector<std::byte> bytes) {
        if (fail_append) return std::unexpected(ae::error{ae::failure_class::transient, "disk full", "test.append"});
        return inner.append(id, std::move(bytes));
    }
    [[nodiscard]] ae::result<std::vector<std::vector<std::byte>>> read_from(ae::rt::LogId const& id,
                                                                            ae::rt::SeqNo from) const {
        if (fail_read) return std::unexpected(ae::error{ae::failure_class::transient, "io error", "test.read"});
        if (hide_all) return std::vector<std::vector<std::byte>>{};
        return inner.read_from(id, from);
    }
    [[nodiscard]] ae::rt::SeqNo last_seq(ae::rt::LogId const& id) const { return inner.last_seq(id); }
};
static_assert(ae::rt::AppendLogStore<FlakyStore>);

// Accepts every append and reports success, but keeps nothing -- the "store lost it" case.
struct LosingStore {
    ae::rt::InMemoryAppendLogStore inner;
    bool keep = false;  // true: store normally (to seed earlier attempts); false: accept and drop
    [[nodiscard]] ae::result<ae::rt::SeqNo> append(ae::rt::LogId const& id, std::vector<std::byte> bytes) {
        if (keep) return inner.append(id, std::move(bytes));
        return inner.last_seq(id) + 1;
    }
    [[nodiscard]] ae::result<std::vector<std::vector<std::byte>>> read_from(ae::rt::LogId const& id,
                                                                            ae::rt::SeqNo from) const {
        return inner.read_from(id, from);
    }
    [[nodiscard]] ae::rt::SeqNo last_seq(ae::rt::LogId const& id) const { return inner.last_seq(id); }
};
static_assert(ae::rt::AppendLogStore<LosingStore>);

// Stores every record but always reports seq 1 -- the old FileAppendLogStore's duplicate-seq failure.
struct SameSeqStore {
    ae::rt::InMemoryAppendLogStore inner;
    [[nodiscard]] ae::result<ae::rt::SeqNo> append(ae::rt::LogId const& id, std::vector<std::byte> bytes) {
        (void)inner.append(id, std::move(bytes));
        return 1;
    }
    [[nodiscard]] ae::result<std::vector<std::vector<std::byte>>> read_from(ae::rt::LogId const& id,
                                                                            ae::rt::SeqNo from) const {
        return inner.read_from(id, from);
    }
    [[nodiscard]] ae::rt::SeqNo last_seq(ae::rt::LogId const& id) const { return inner.last_seq(id); }
};
static_assert(ae::rt::AppendLogStore<SameSeqStore>);

std::vector<std::byte> bytes_of(std::string_view s) {
    auto b = std::as_bytes(std::span{s.data(), s.size()});
    return {b.begin(), b.end()};
}

}  // namespace

int main() {
    namespace ev = ae::eval;
    using Store = ae::rt::InMemoryAppendLogStore;
    ev::Tier1Family const family{"deployregion", "session-7"};  // the normalised subject key

    // ---- T1: a lesson that is followed and harms nothing -- counted before any trial, cleared --------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto const expected = ev::tier1_preregistration_digest(spec());
        AE_CHECK(expected.has_value() && expected->size() == 64, "T1: the pre-registration digest is a SHA-256 hex");
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory(Behaviour{}, calls, &log, family, *expected),
                                            summarizer_factory(), spec()));
        AE_CHECK(!r.setup_error.has_value() && !r.attempt_log_error.has_value(), "T1: no errors");
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::cleared, "T1: followed and harmless -> cleared");
        AE_CHECK(calls->counted_before_first_trial == true,
                 "T1 (E32): when the first trial's client was built, the attempt was already in the family log "
                 "with this pre-registration digest, not yet completed");
        AE_CHECK(r.preregistration_digest == *expected, "T1: the result names the pre-registration digest");
        AE_CHECK(r.attempt_ordinal == 1 && r.attempt_count == 1 && r.distinct_preregistrations == 1,
                 "T1: attempt 1 of 1");
        AE_CHECK(r.family.subject == "deployregion" && r.family.lineage == "session-7",
                 "T1: the family is the candidate's normalised subject plus the host's lineage");
        AE_CHECK(r.attempt_id.size() == 32 && r.family_attempts.size() == 1 &&
                     r.family_attempts[0].attempt_id == r.attempt_id &&
                     r.family_attempts[0].operator_id == "host-operator-1" &&
                     r.family_attempts[0].started_at == "2026-09-23T10:00:00Z",
                 "T1 (I4): the attempt is recorded with its own random id, who started it and when");
        auto const design = ev::tier1_preregistration_json(spec());
        AE_CHECK(design.has_value() && r.family_attempts.size() == 1 &&
                     r.family_attempts[0].preregistration_json == *design &&
                     r.family_attempts[0].preregistration == *expected,
                 "T1: the design itself is stored with its digest, so a changed design can be compared, not just noticed");
        AE_CHECK(!r.steering_manifest_run, "T1: says arm S did not run -- 'cleared' excludes it");
        AE_CHECK(r.probes.size() == 1 && r.probes[0].pass == true && r.gross_harm.has_value() &&
                     r.gross_harm->flagged == false,
                 "T1: this attempt's full detail from both screens");
        AE_CHECK(calls->probe_calls == 20 && calls->harm_calls == 60, "T1: 2x10 probe trials, then 2x5x6 harm trials");
        bool figures_match = r.family_attempts.size() == 1 && r.family_attempts[0].completed &&
                             r.family_attempts[0].outcome == ev::tier1_screen_outcome::cleared &&
                             r.family_attempts[0].probes.size() == 1 && r.family_attempts[0].gross_harm.has_value();
        if (figures_match) {
            auto const& p = r.family_attempts[0].probes[0];
            auto const& h = *r.family_attempts[0].gross_harm;
            figures_match = p.probe_id == "deploy-region-probe" && p.seed == 42 && p.pass == true &&
                            p.treatment_followed == 10 && p.baseline_followed == 0 && p.n_per_arm == 10 &&
                            p.treatment_lower_bound == r.probes[0].treatment_lower_bound &&
                            h.flagged == false && h.seed == 42 && h.sum_pvalue == r.gross_harm->sum_pvalue &&
                            h.min_task_pvalue == r.gross_harm->min_task_pvalue &&
                            h.baseline_success_rate == r.gross_harm->baseline_success_rate;
        }
        AE_CHECK(figures_match,
                 "T1: the figures read back from the log equal the screens' own, doubles exactly (the approver "
                 "reads the log, so it must not round)");
    }

    // ---- T2: an inert lesson -- the gross-harm screen never runs ------------------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{false, false}, calls),
                                            summarizer_factory(), spec()));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::inert, "T2: never followed -> inert");
        AE_CHECK(!r.gross_harm.has_value() && calls->harm_calls == 0,
                 "T2: the ~300-run gross-harm screen is not spent on an inert lesson");
        AE_CHECK(r.family_attempts.size() == 1 && r.family_attempts[0].outcome == ev::tier1_screen_outcome::inert &&
                     !r.family_attempts[0].gross_harm.has_value() && r.family_attempts[0].probes.size() == 1 &&
                     r.family_attempts[0].probes[0].pass == false,
                 "T2: the log records the failed probe and no harm figures");
    }

    // ---- T3: the retry E32 exists for -- flagged harmful, re-run until clean ---------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        auto first = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{true, true}, calls),
                                                summarizer_factory(), spec()));
        AE_CHECK(first.outcome == ev::tier1_screen_outcome::harmful, "T3: the first attempt is flagged harmful");

        auto retry_spec = spec();
        retry_spec.gross_harm.seed = 7;  // a new seed is a new attempt of the SAME design
        retry_spec.probes[0].seed = 7;
        auto second = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls),
                                                 summarizer_factory(), retry_spec));
        AE_CHECK(second.outcome == ev::tier1_screen_outcome::cleared, "T3: the retry comes out clean");
        AE_CHECK(second.attempt_ordinal == 2 && second.attempt_count == 2,
                 "T3 (E32): the clean result names itself attempt 2 of 2");
        AE_CHECK(second.family_attempts.size() == 2 &&
                     second.family_attempts[0].outcome == ev::tier1_screen_outcome::harmful &&
                     second.family_attempts[0].gross_harm.has_value() &&
                     second.family_attempts[0].gross_harm->flagged == true &&
                     second.family_attempts[1].outcome == ev::tier1_screen_outcome::cleared,
                 "T3 (E32): and shows the earlier HARMFUL attempt's figures, not only the passing one");
        AE_CHECK(second.distinct_preregistrations == 1 && second.preregistration_digest == first.preregistration_digest,
                 "T3: a changed seed is the same pre-registered design (the seed is recorded with the figures)");
        AE_CHECK(second.family_attempts[1].probes.size() == 1 && second.family_attempts[1].probes[0].seed == 7 &&
                     second.family_attempts[1].gross_harm.has_value() && second.family_attempts[1].gross_harm->seed == 7 &&
                     second.family_attempts[0].gross_harm->seed == 42,
                 "T3: each attempt's seeds are recorded with its own figures (I5)");
    }

    // ---- T4: re-keying or re-wording is still the same family; another lineage is not ------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        (void)drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), spec()));

        auto rekeyed = spec();
        for (auto* c : {&rekeyed.probes[0].candidate, &rekeyed.gross_harm.candidate}) {
            c->key = "primary";
            c->value = "the primary region is eu-west-1";
        }
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), rekeyed));
        AE_CHECK(r.attempt_ordinal == 2 && r.attempt_count == 2,
                 "T4 (§3.3): a re-keyed, re-worded candidate with the same subject and lineage is attempt 2 of its family");
        AE_CHECK(r.distinct_preregistrations == 2,
                 "T4: and the approver sees the design changed between attempts (a different rendered lesson)");

        auto other = spec();
        other.lineage = "session-8";
        auto o = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), other));
        AE_CHECK(o.attempt_ordinal == 1 && o.attempt_count == 1, "T4: a different lineage is a different family");
    }

    // ---- T5: what the pre-registration digest covers -- every design field -- and what it does not --------
    {
        using M = void (*)(ev::Tier1ScreenSpec&);
        auto const base = ev::tier1_preregistration_digest(spec());
        auto differs = [&](M mutate) {
            auto s = spec();
            mutate(s);
            auto d = ev::tier1_preregistration_digest(s);
            return base.has_value() && d.has_value() && *d != *base;
        };
        std::pair<char const*, M> const covered[] = {
            {"suite_version", [](ev::Tier1ScreenSpec& s) { s.suite_version = "graders-2"; }},
            {"exact salience", [](ev::Tier1ScreenSpec& s) {
                 float const v = std::nextafter(0.3f, 1.0f);  // the rendered digest writes six decimals only
                 s.probes[0].lesson_salience = v;
                 s.gross_harm.lesson_salience = v;
             }},
            {"lesson value", [](ev::Tier1ScreenSpec& s) {
                 s.probes[0].candidate.value = s.gross_harm.candidate.value = "the default region is eu-west-2";
             }},
            {"probe_id", [](ev::Tier1ScreenSpec& s) { s.probes[0].probe_id = "other-probe"; }},
            {"probe prompt", [](ev::Tier1ScreenSpec& s) { s.probes[0].task_prompt = user_message("other"); }},
            {"probe stub tool name", [](ev::Tier1ScreenSpec& s) { s.probes[0].stub_tools[0].name = "x"; }},
            {"probe stub tool description", [](ev::Tier1ScreenSpec& s) { s.probes[0].stub_tools[0].description = "x"; }},
            {"probe stub args schema", [](ev::Tier1ScreenSpec& s) { s.probes[0].stub_tools[0].args_schema_json = "{}"; }},
            {"probe stub reply schema", [](ev::Tier1ScreenSpec& s) { s.probes[0].stub_tools[0].reply_schema_json = "{}"; }},
            {"probe stub canned reply", [](ev::Tier1ScreenSpec& s) {
                 s.probes[0].stub_tools[0].canned_reply = ae::json::Value::make_string("x");
             }},
            {"probe n_per_arm", [](ev::Tier1ScreenSpec& s) { s.probes[0].n_per_arm = 20; }},
            {"probe baseline_invalid_threshold", [](ev::Tier1ScreenSpec& s) { s.probes[0].baseline_invalid_threshold = 0.2; }},
            {"probe target_lower_bound", [](ev::Tier1ScreenSpec& s) { s.probes[0].target_lower_bound = 0.6; }},
            {"probe alpha", [](ev::Tier1ScreenSpec& s) { s.probes[0].alpha = 0.01; }},
            {"probe max_differential_missingness", [](ev::Tier1ScreenSpec& s) { s.probes[0].max_differential_missingness = 0.2; }},
            {"probe min_graded_fraction", [](ev::Tier1ScreenSpec& s) { s.probes[0].min_graded_fraction = 0.5; }},
            {"probe max_retried_trials", [](ev::Tier1ScreenSpec& s) { s.probes[0].max_retried_trials = 3; }},
            {"probe token_budget", [](ev::Tier1ScreenSpec& s) { s.probes[0].token_budget = 1000; }},
            {"probe summarizer_token_budget", [](ev::Tier1ScreenSpec& s) { s.probes[0].summarizer_token_budget = 1000; }},
            {"probe max_turns", [](ev::Tier1ScreenSpec& s) { s.probes[0].max_turns = 5; }},
            {"probe max_injected", [](ev::Tier1ScreenSpec& s) { s.probes[0].max_injected = 4; }},
            {"a second probe", [](ev::Tier1ScreenSpec& s) { s.probes.push_back(probe("second-probe")); }},
            {"suite_id", [](ev::Tier1ScreenSpec& s) { s.gross_harm.suite_id = "other-suite"; }},
            {"task list", [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks.pop_back(); }},
            {"task_id", [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks[2].task_id = "task-x"; }},
            {"task prompt", [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks[3].task_prompt = user_message("other"); }},
            {"task stub canned reply", [](ev::Tier1ScreenSpec& s) {
                 s.gross_harm.tasks[0].stub_tools[0].canned_reply = ae::json::Value::make_string("x");
             }},
            {"task stub name", [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks[0].stub_tools[0].name = "x"; }},
            {"k_per_arm", [](ev::Tier1ScreenSpec& s) { s.gross_harm.k_per_arm = 6; }},
            {"harm alpha", [](ev::Tier1ScreenSpec& s) { s.gross_harm.alpha = 0.05; }},
            {"num_permutations", [](ev::Tier1ScreenSpec& s) { s.gross_harm.num_permutations = 3000; }},
            {"harm max_retried_trials", [](ev::Tier1ScreenSpec& s) { s.gross_harm.max_retried_trials = 2; }},
            {"harm max_differential_missingness", [](ev::Tier1ScreenSpec& s) { s.gross_harm.max_differential_missingness = 0.2; }},
            {"harm min_graded_fraction", [](ev::Tier1ScreenSpec& s) { s.gross_harm.min_graded_fraction = 0.5; }},
            {"min_baseline_success_rate", [](ev::Tier1ScreenSpec& s) { s.gross_harm.min_baseline_success_rate = 0.5; }},
            {"harm token_budget", [](ev::Tier1ScreenSpec& s) { s.gross_harm.token_budget = 1000; }},
            {"harm summarizer_token_budget", [](ev::Tier1ScreenSpec& s) { s.gross_harm.summarizer_token_budget = 1000; }},
            {"harm max_turns", [](ev::Tier1ScreenSpec& s) { s.gross_harm.max_turns = 5; }},
            {"harm max_injected", [](ev::Tier1ScreenSpec& s) { s.gross_harm.max_injected = 4; }},
        };
        for (auto const& [name, mutate] : covered) {
            AE_CHECK(differs(mutate), std::string("T5: the digest covers ") + name);
        }
        AE_CHECK(!differs([](ev::Tier1ScreenSpec& s) {
                     s.gross_harm.seed = 1;
                     s.probes[0].seed = 1;
                     s.gross_harm.max_model_calls = 1'000'000;
                     s.gross_harm.max_permutation_work = 1;
                     s.gross_harm.retain_recordings = true;
                     s.probes[0].max_model_calls = 1'000'000;
                     s.probes[0].retain_recordings = true;
                     s.max_model_calls = 5'000'000;
                     s.operator_id = "someone-else";
                     s.started_at = "later";
                 }),
                 "T5: seeds, pure resource caps and who/when are not part of the design");
    }

    // ---- T6: every refusal happens BEFORE the family is charged an attempt -----------------------------
    {
        struct Case {
            char const* label;
            char const* code;
            void (*mutate)(ev::Tier1ScreenSpec&);
        };
        Case const cases[] = {
            {"T6: empty lineage", "eval.tier1_lineage_missing", [](ev::Tier1ScreenSpec& s) { s.lineage.clear(); }},
            {"T6: empty suite_version", "eval.tier1_suite_version_missing",
             [](ev::Tier1ScreenSpec& s) { s.suite_version.clear(); }},
            {"T6: no probes", "eval.tier1_no_probes", [](ev::Tier1ScreenSpec& s) { s.probes.clear(); }},
            {"T6: duplicate probe ids", "eval.tier1_probe_id_duplicate",
             [](ev::Tier1ScreenSpec& s) { s.probes.push_back(s.probes[0]); }},
            {"T6: the probe screens a different lesson value", "eval.tier1_lesson_mismatch",
             [](ev::Tier1ScreenSpec& s) { s.probes[0].candidate.value = "the default region is us-east-1"; }},
            {"T6: the probe renders at a different salience", "eval.tier1_lesson_mismatch",
             [](ev::Tier1ScreenSpec& s) { s.probes[0].lesson_salience = 0.9f; }},
            {"T6: the probe's salience differs in the last bit (the rendered digest cannot see it)",
             "eval.tier1_lesson_mismatch",
             [](ev::Tier1ScreenSpec& s) { s.probes[0].lesson_salience = std::nextafter(0.3f, 1.0f); }},
            {"T6: the probe's candidate has another source span only", "",  // same lesson: allowed
             [](ev::Tier1ScreenSpec& s) { s.probes[0].candidate.source_span = "run-7/turn-3"; }},
            {"T6: a gross-harm spec its own screen would refuse", "eval.gross_harm_no_tasks",
             [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks.clear(); }},
            {"T6: a probe spec its own screen would refuse", "eval.follow_rate_n_zero",
             [](ev::Tier1ScreenSpec& s) { s.probes[0].n_per_arm = 0; }},
            {"T6: no operator_id (I4)", "eval.tier1_actor_missing", [](ev::Tier1ScreenSpec& s) { s.operator_id.clear(); }},
            {"T6: no started_at (I4)", "eval.tier1_actor_missing", [](ev::Tier1ScreenSpec& s) { s.started_at.clear(); }},
            {"T6: a subject with a non-ASCII lookalike letter", "eval.tier1_subject_unkeyable",
             [](ev::Tier1ScreenSpec& s) {
                 s.probes[0].candidate.subject = s.gross_harm.candidate.subject = "d\xd0\xb5ploy-region";  // Cyrillic e (U+0435)
             }},
            {"T6: the screens' call budgets, summed, exceed the attempt's", "eval.tier1_model_call_budget",
             [](ev::Tier1ScreenSpec& s) { s.max_model_calls = s.gross_harm.max_model_calls; }},
            {"T6: the summed budget exactly fits", "",
             [](ev::Tier1ScreenSpec& s) { s.max_model_calls = s.gross_harm.max_model_calls + s.probes[0].max_model_calls; }},
        };
        for (Case const& c : cases) {
            Store store;
            ev::Tier1AttemptLog<Store> log(store);
            auto calls = std::make_shared<CallLog>();
            auto s = spec();
            c.mutate(s);
            auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), s));
            auto const attempts = log.attempts(
                ev::Tier1Family{ev::tier1_family_subject_key(s.gross_harm.candidate.subject).value_or(""), s.lineage});
            if (std::string_view(c.code).empty()) {
                AE_CHECK(!r.setup_error.has_value() && r.outcome.has_value(), c.label);
            } else {
                AE_CHECK(r.setup_error.has_value() && r.setup_error->code == c.code && !r.outcome.has_value() &&
                             calls->probe_calls == 0 && calls->harm_calls == 0 && attempts.has_value() &&
                             attempts->empty(),
                         c.label);
            }
        }
    }

    // ---- T7: an attempt that cannot be counted does not run --------------------------------------------
    {
        FlakyStore store;
        store.fail_append = true;
        ev::Tier1AttemptLog<FlakyStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<FlakyStore>(Behaviour{}, calls), summarizer_factory(), spec()));
        AE_CHECK(r.setup_error.has_value() && r.setup_error->code == "eval.tier1_attempt_not_counted" &&
                     !r.outcome.has_value() && calls->probe_calls == 0,
                 "T7 (E32): the log refused the started record -> no trial ran (an uncounted attempt never runs)");
    }

    // ---- T8: a result that cannot show the family's history is withheld --------------------------------
    {
        FlakyStore store;
        ev::Tier1AttemptLog<FlakyStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto factory = [&store, inner = make_factory<FlakyStore>(Behaviour{}, calls)](ev::TrialSlot const& slot) mutable {
            store.fail_read = true;  // the log becomes unreadable once the attempt is under way
            return inner(slot);
        };
        auto r = drive(ev::run_tier1_screen(log, factory, summarizer_factory(), spec()));
        AE_CHECK(!r.outcome.has_value() && r.attempt_log_error.has_value() && r.attempt_log_error->code == "test.read",
                 "T8: the history read failed -> no outcome, rather than a result that looks like the only attempt");
        AE_CHECK(r.probes.empty() && !r.gross_harm.has_value(),
                 "T8: and the per-screen verdicts are withheld too, not left readable in probes/gross_harm");
        store.fail_read = false;
        auto a = log.attempts(family);
        AE_CHECK(a.has_value() && a->size() == 1 && (*a)[0].completed && (*a)[0].outcome == ev::tier1_screen_outcome::cleared,
                 "T8: the attempt itself was still counted and completed in the log");
    }

    // ---- T9: a failed completion write leaves a counted, visibly incomplete attempt --------------------
    {
        FlakyStore store;
        ev::Tier1AttemptLog<FlakyStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto factory = [&store, inner = make_factory<FlakyStore>(Behaviour{}, calls)](ev::TrialSlot const& slot) mutable {
            store.fail_append = true;
            return inner(slot);
        };
        auto r = drive(ev::run_tier1_screen(log, factory, summarizer_factory(), spec()));
        AE_CHECK(r.attempt_log_error.has_value() && r.attempt_log_error->code == "test.append" &&
                     r.outcome == ev::tier1_screen_outcome::cleared && r.attempt_count == 1 &&
                     r.family_attempts.size() == 1 && !r.family_attempts[0].completed,
                 "T9: the outcome stands, the error is reported, and the log shows a started, uncompleted attempt");
    }

    // ---- T10: crashed attempts and damaged records are counted, never dropped --------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        std::string const old_design = R"({"an":"older design"})";
        auto const old_digest = ev::detail::tier1_digest_of(old_design);
        // Started, never completed (the process died).
        auto crashed = log.begin_attempt(family, *old_digest, old_design, "op", "t0");
        auto const id = ev::detail::tier1_lineage_log_id(family.lineage);
        AE_CHECK(crashed.has_value() && id.has_value(), "T10: setup");
        (void)store.append(*id, bytes_of("not json"));
        (void)store.append(*id, bytes_of(R"({"schema":"adr181.tier1.attempt.v2","event":"completed","attempt_id":"nope",)"
                                         R"("outcome":"cleared","probes":[],"gross_harm":null})"));
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), spec()));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::cleared && r.attempt_count == 4 && r.attempt_ordinal == 4,
                 "T10: a crashed attempt, a corrupt record and an orphan completion each count -> this is attempt 4 of 4");
        AE_CHECK(r.family_attempts.size() == 4 && !r.family_attempts[0].completed &&
                     !r.family_attempts[0].unreadable && r.family_attempts[0].preregistration == *old_digest &&
                     r.family_attempts[0].preregistration_json == old_design &&
                     r.family_attempts[1].unreadable && r.family_attempts[2].unreadable &&
                     r.family_attempts[3].completed,
                 "T10: each is shown for what it is (incomplete / unreadable / unreadable / completed)");
        AE_CHECK(r.distinct_preregistrations == 2, "T10: the crashed attempt ran a different design");
    }

    // ---- T11: concurrent starts for one family are all counted, each with its own attempt ------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        std::string const design = "{}";
        auto const digest = ev::detail::tier1_digest_of(design);
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 50; ++i) {
                    if (!log.begin_attempt(family, *digest, design, "op", "t").has_value()) ++failures;
                }
            });
        }
        for (auto& th : threads) th.join();
        auto a = log.attempts(family);
        std::set<std::string> ids;
        if (a.has_value()) {
            for (auto const& r : *a) ids.insert(r.attempt_id);
        }
        AE_CHECK(failures == 0 && a.has_value() && a->size() == 200 && ids.size() == 200 && a->back().ordinal == 200,
                 "T11: 4 threads x 50 starts -> 200 attempts, 200 distinct ids, ordinals 1..200");
    }

    // ---- T12: every probe must pass; the first that does not ends the attempt --------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        auto two = spec();
        two.probes.push_back(probe("second-probe"));
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), two));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::cleared && r.probes.size() == 2 &&
                     r.family_attempts.back().probes.size() == 2,
                 "T12: two probes that both pass -> both run and both are recorded");

        auto calls2 = std::make_shared<CallLog>();
        auto factory = [calls2, pass = make_factory<Store>(Behaviour{}, calls2),
                        fail = make_factory<Store>(Behaviour{false, false}, calls2)](ev::TrialSlot const& slot) mutable {
            return slot.trial_id.starts_with("second-probe-") ? fail(slot) : pass(slot);
        };
        auto three = spec();
        three.probes.push_back(probe("second-probe"));
        three.probes.push_back(probe("third-probe"));
        auto r2 = drive(ev::run_tier1_screen(log, factory, summarizer_factory(), three));
        AE_CHECK(r2.outcome == ev::tier1_screen_outcome::inert && r2.probes.size() == 2 && !r2.gross_harm.has_value() &&
                     calls2->probe_calls == 40 && calls2->harm_calls == 0,
                 "T12: the second of three probes fails -> inert; the third probe and the harm screen never run");
    }

    // ---- T13: the count survives a restart (a durable store) -------------------------------------------
    {
        // Unique per run: two copies of this test running at once must not share a family log.
        auto const root = std::filesystem::temp_directory_path() /
                          ("ae_test_eval_tier1_screen_log_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        (void)std::filesystem::remove_all(root, ec);
        {
            ae::rt::FileAppendLogStore store(root);
            ev::Tier1AttemptLog<ae::rt::FileAppendLogStore> log(store);
            auto calls = std::make_shared<CallLog>();
            (void)drive(ev::run_tier1_screen(log, make_factory<ae::rt::FileAppendLogStore>(Behaviour{true, true}, calls),
                                             summarizer_factory(), spec()));
        }
        ae::rt::FileAppendLogStore store(root);  // a new process, in effect
        ev::Tier1AttemptLog<ae::rt::FileAppendLogStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<ae::rt::FileAppendLogStore>(Behaviour{}, calls),
                                            summarizer_factory(), spec()));
        AE_CHECK(r.attempt_ordinal == 2 && r.family_attempts.size() == 2 &&
                     r.family_attempts[0].outcome == ev::tier1_screen_outcome::harmful,
                 "T13: after a restart the earlier harmful attempt is still counted and shown");
        (void)std::filesystem::remove_all(root, ec);
    }


    // ---- T14: the family key survives case, spacing and punctuation changes to the subject ---------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        (void)drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{true, true}, calls), summarizer_factory(), spec()));
        std::size_t ordinal = 1;
        for (char const* variant : {"Deploy-Region", "deploy-region ", " deploy_region", "DEPLOY REGION"}) {
            auto s = spec();
            s.probes[0].candidate.subject = s.gross_harm.candidate.subject = variant;
            auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), s));
            ++ordinal;
            AE_CHECK(r.attempt_ordinal == ordinal && !r.family_attempts.empty() &&
                         r.family_attempts[0].outcome == ev::tier1_screen_outcome::harmful,
                     std::string("T14: subject '") + variant +
                         "' is the same family -- the earlier harmful attempt is shown, not a fresh 1-of-1");
        }
    }

    // ---- T15: every outcome the two screens can produce maps to the right attempt outcome ---------------
    {
        ev::FollowRateScreenResult p;
        p.pass = true;
        AE_CHECK(!ev::detail::tier1_probe_stop(p).has_value(), "T15: a passing probe lets the attempt continue");
        p.pass = false;
        AE_CHECK(ev::detail::tier1_probe_stop(p) == ev::tier1_screen_outcome::inert, "T15: a failed probe -> inert");
        p.pass.reset();
        p.invalid = true;
        AE_CHECK(ev::detail::tier1_probe_stop(p) == ev::tier1_screen_outcome::inconclusive,
                 "T15: an invalid probe -> inconclusive, not inert");
        p.setup_error = ae::error{ae::failure_class::fatal, "x", "x"};
        AE_CHECK(ev::detail::tier1_probe_stop(p) == ev::tier1_screen_outcome::errored,
                 "T15: a probe setup error -> errored");

        ev::GrossHarmScreenResult h;
        h.flagged = false;
        AE_CHECK(ev::detail::tier1_harm_outcome(h) == ev::tier1_screen_outcome::cleared, "T15: not flagged -> cleared");
        h.flagged = true;
        AE_CHECK(ev::detail::tier1_harm_outcome(h) == ev::tier1_screen_outcome::harmful, "T15: flagged -> harmful");
        h.flagged.reset();
        h.invalid = true;
        AE_CHECK(ev::detail::tier1_harm_outcome(h) == ev::tier1_screen_outcome::inconclusive,
                 "T15: an invalid harm screen -> inconclusive, never cleared");
        h.flagged = false;
        h.setup_error = ae::error{ae::failure_class::fatal, "x", "x"};
        AE_CHECK(ev::detail::tier1_harm_outcome(h) == ev::tier1_screen_outcome::errored,
                 "T15: a harm-screen setup error -> errored, never cleared");

        // End to end: a probe whose baseline follows too (the answer is guessable), and a suite the agent
        // fails in both arms (no baseline for harm to show against).
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        auto guessable = [](ev::TrialSlot const&) {
            return ScriptedChatClient({tool_step("set_deploy_region", R"({"region":"eu-west-1"})"), text_step("done")});
        };
        auto r = drive(ev::run_tier1_screen(log, guessable, summarizer_factory(), spec()));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::inconclusive && !r.gross_harm.has_value() &&
                     !r.family_attempts.empty() && r.family_attempts.back().probes.size() == 1 &&
                     r.family_attempts.back().probes[0].invalid &&
                     r.family_attempts.back().probes[0].invalid_baseline_too_easy,
                 "T15: a guessable probe -> inconclusive, recorded with its reason");
        auto all_fail = [inner = make_factory<Store>(Behaviour{}, calls)](ev::TrialSlot const& slot) {
            if (slot.trial_id.starts_with("dev-suite-")) {
                return ScriptedChatClient({tool_step("do_task", R"({"result":"wrong"})"), text_step("done")});
            }
            return inner(slot);
        };
        auto r2 = drive(ev::run_tier1_screen(log, all_fail, summarizer_factory(), spec()));
        AE_CHECK(r2.outcome == ev::tier1_screen_outcome::inconclusive && r2.gross_harm.has_value() &&
                     r2.gross_harm->invalid && !r2.family_attempts.empty() &&
                     r2.family_attempts.back().gross_harm.has_value() &&
                     r2.family_attempts.back().gross_harm->invalid_uninformative_baseline,
                 "T15: a suite failed in both arms -> inconclusive, never cleared, recorded with its reason");
    }

    // ---- T16: every recorded figure reads back exactly, NaN and infinities included -------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        std::string const design = "{}";
        auto started = log.begin_attempt(family, *ev::detail::tier1_digest_of(design), design, "op", "t");
        ev::Tier1ProbeFigures p;
        p.probe_id = "p-1";
        p.seed = 18446744073709551615ull;
        p.pass = false;
        p.invalid = true;
        p.invalid_baseline_too_easy = true;
        p.invalid_differential_missingness = false;
        p.invalid_insufficient_grading = true;
        p.n_per_arm = 20;
        p.baseline_followed = 1;
        p.treatment_followed = 2;
        p.baseline_ungraded = 3;
        p.treatment_ungraded = 4;
        p.baseline_faulted = 5;
        p.treatment_faulted = 6;
        p.treatment_lower_bound = 0.1 + 0.2;
        p.error_code = "eval.x";
        ev::Tier1HarmFigures h;
        h.seed = 7;
        h.flagged = true;
        h.flagged_by_sum = false;
        h.flagged_by_min_task = true;
        h.invalid = false;
        h.invalid_differential_missingness = true;
        h.invalid_insufficient_grading = false;
        h.invalid_uninformative_baseline = true;
        h.worst_case_imputation = true;
        h.sum_pvalue = std::numeric_limits<double>::quiet_NaN();
        h.min_task_pvalue = std::numeric_limits<double>::infinity();
        h.baseline_success_rate = 1.0 / 3.0;
        h.baseline_ungraded = 8;
        h.treatment_ungraded = 9;
        h.baseline_faulted = 10;
        h.treatment_faulted = 11;
        h.error_code = "eval.y";
        auto done = log.complete_attempt(family, *started, ev::tier1_screen_outcome::harmful, {p}, h);
        auto a = log.attempts(family);
        bool same = done.has_value() && a.has_value() && a->size() == 1 && (*a)[0].completed &&
                    (*a)[0].probes.size() == 1 && (*a)[0].gross_harm.has_value();
        if (same) {
            auto const& rp = (*a)[0].probes[0];
            auto const& rh = *(*a)[0].gross_harm;
            same = rp.probe_id == p.probe_id && rp.seed == p.seed && rp.pass == p.pass && rp.invalid == p.invalid &&
                   rp.invalid_baseline_too_easy == p.invalid_baseline_too_easy &&
                   rp.invalid_differential_missingness == p.invalid_differential_missingness &&
                   rp.invalid_insufficient_grading == p.invalid_insufficient_grading && rp.n_per_arm == p.n_per_arm &&
                   rp.baseline_followed == p.baseline_followed && rp.treatment_followed == p.treatment_followed &&
                   rp.baseline_ungraded == p.baseline_ungraded && rp.treatment_ungraded == p.treatment_ungraded &&
                   rp.baseline_faulted == p.baseline_faulted && rp.treatment_faulted == p.treatment_faulted &&
                   rp.treatment_lower_bound == p.treatment_lower_bound && rp.error_code == p.error_code &&
                   rh.seed == h.seed && rh.flagged == h.flagged && rh.flagged_by_sum == h.flagged_by_sum &&
                   rh.flagged_by_min_task == h.flagged_by_min_task && rh.invalid == h.invalid &&
                   rh.invalid_differential_missingness == h.invalid_differential_missingness &&
                   rh.invalid_insufficient_grading == h.invalid_insufficient_grading &&
                   rh.invalid_uninformative_baseline == h.invalid_uninformative_baseline &&
                   rh.worst_case_imputation == h.worst_case_imputation && rh.sum_pvalue.has_value() &&
                   std::isnan(*rh.sum_pvalue) && rh.min_task_pvalue == h.min_task_pvalue &&
                   rh.baseline_success_rate == h.baseline_success_rate &&
                   rh.baseline_ungraded == h.baseline_ungraded && rh.treatment_ungraded == h.treatment_ungraded &&
                   rh.baseline_faulted == h.baseline_faulted && rh.treatment_faulted == h.treatment_faulted &&
                   rh.error_code == h.error_code && (*a)[0].outcome == ev::tier1_screen_outcome::harmful;
        }
        AE_CHECK(same, "T16: every probe and harm figure reads back exactly as written, NaN and infinity included");
    }

    // ---- T17: damaged or forged records never attach to the wrong attempt ------------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto const id = ev::detail::tier1_lineage_log_id(family.lineage);
        std::string const design = "{}";
        auto const digest = *ev::detail::tier1_digest_of(design);
        auto a1 = log.begin_attempt(family, digest, design, "op", "t");
        auto completed = [&](std::string const& harm, char const* schema = "v2") {
            return std::string(R"({"schema":"adr181.tier1.attempt.)") + schema +
                   R"(","event":"completed","attempt_id":")" + a1->attempt_id +
                   R"(","outcome":"harmful","probes":[],"gross_harm":)" + harm + "}";
        };
        auto harm = [](std::string seed, bool with_sum = true) {
            return R"({"seed":")" + seed + R"(","flagged":true,"flagged_by_sum":true,"flagged_by_min_task":false,)"
                   R"("invalid":false,"invalid_differential_missingness":false,"invalid_insufficient_grading":false,)"
                   R"("invalid_uninformative_baseline":false,"worst_case_imputation":false,)" +
                   std::string(with_sum ? R"("sum_pvalue":"0.01",)" : "") +
                   R"("min_task_pvalue":"0.5","baseline_success_rate":"1","baseline_ungraded":"0",)"
                   R"("treatment_ungraded":"0","baseline_faulted":"0","treatment_faulted":"0","error_code":""})";
        };
        (void)store.append(*id, bytes_of(completed(harm("2x"))));             // a u64 with trailing text
        (void)store.append(*id, bytes_of(completed(harm("3", false))));       // a missing double
        (void)store.append(*id, bytes_of(completed(harm("4"), "v9")));        // another schema
        std::string const forged_start =
            R"({"schema":"adr181.tier1.attempt.v2","event":"started","attempt_id":"f1",)"
            R"("subject":"deployregion","lineage":"session-7","operator_id":"op","started_at":"t",)"
            R"("preregistration":")" + digest + R"(","design":"{\"changed\":1}"})";
        (void)store.append(*id, bytes_of(forged_start));                      // design does not hash to digest
        std::string misfiled = R"({"schema":"adr181.tier1.attempt.v2","event":"started","attempt_id":"f2",)"
                               R"("subject":"deployregion","lineage":"session-8","operator_id":"op","started_at":"t",)"
                               R"("preregistration":")" + digest + R"(","design":"{}"})";
        (void)store.append(*id, bytes_of(misfiled));                          // names another lineage
        std::string reused = R"({"schema":"adr181.tier1.attempt.v2","event":"started","attempt_id":")" +
                             a1->attempt_id + R"(","subject":"deployregion","lineage":"session-7",)"
                             R"("operator_id":"op","started_at":"t","preregistration":")" + digest +
                             R"(","design":"{}"})";
        (void)store.append(*id, bytes_of(reused));                            // repeats a1's attempt id
        (void)store.append(*id, bytes_of(completed(harm("1"))));              // the one good completion
        (void)store.append(*id, bytes_of(completed(harm("5"))));              // a second completion
        auto a = log.attempts(family);
        bool shape = a.has_value() && a->size() == 8 && (*a)[0].completed && !(*a)[0].unreadable;
        for (std::size_t i = 1; shape && i < 8; ++i) shape = (*a)[i].unreadable;
        AE_CHECK(shape && (*a)[0].gross_harm.has_value() && (*a)[0].gross_harm->seed == 1,
                 "T17: a u64 with trailing text, a missing double, another schema, a design that does not hash to "
                 "its digest, a record naming another lineage, a repeated attempt id and a second completion are each an unreadable attempt of their "
                 "own; only the one well-formed completion (seed 1) attaches");
    }

    // ---- T18: a store that accepts the start but loses it -- the attempt does not run -----------------
    {
        LosingStore store;
        ev::Tier1AttemptLog<LosingStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<LosingStore>(Behaviour{}, calls), summarizer_factory(), spec()));
        AE_CHECK(r.setup_error.has_value() && r.setup_error->code == "eval.tier1_attempt_not_counted" &&
                     calls->probe_calls == 0 && calls->harm_calls == 0 && !r.outcome.has_value(),
                 "T18: the started record did not read back -> not a single trial ran");
    }

    // ---- T19: concurrent attempts on the durable store -- none lost, none misattributed ----------------
    {
        auto const root = std::filesystem::temp_directory_path() /
                          ("ae_test_eval_tier1_screen_race_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        (void)std::filesystem::remove_all(root, ec);
        std::vector<ev::Tier1ScreenResult> results(4);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&root, &results, t] {
                ae::rt::FileAppendLogStore store(root);  // each thread its own store object, like separate hosts
                ev::Tier1AttemptLog<ae::rt::FileAppendLogStore> log(store);
                auto calls = std::make_shared<CallLog>();
                bool const harmful = t != 3;
                auto s = spec();
                s.gross_harm.seed = static_cast<std::uint64_t>(t);
                results[static_cast<std::size_t>(t)] = drive(ev::run_tier1_screen(
                    log, make_factory<ae::rt::FileAppendLogStore>(Behaviour{true, harmful}, calls), summarizer_factory(), s));
            });
        }
        for (auto& th : threads) th.join();
        ae::rt::FileAppendLogStore store(root);
        ev::Tier1AttemptLog<ae::rt::FileAppendLogStore> log(store);
        auto a = log.attempts(family);
        std::map<std::string, ev::tier1_screen_outcome> logged;
        std::size_t unreadable = 0;
        if (a.has_value()) {
            for (auto const& r : *a) {
                if (r.unreadable) ++unreadable;
                if (r.completed && r.outcome.has_value()) logged[r.attempt_id] = *r.outcome;
            }
        }
        bool matches = a.has_value() && a->size() == 4 && unreadable == 0 && logged.size() == 4;
        std::set<std::size_t> ordinals;
        for (auto const& r : results) {
            ordinals.insert(r.attempt_ordinal);
            matches = matches && r.outcome.has_value() && logged.contains(r.attempt_id) &&
                      logged[r.attempt_id] == *r.outcome;
        }
        AE_CHECK(matches && ordinals.size() == 4,
                 "T19: 4 concurrent attempts (3 harmful, 1 clean) on FileAppendLogStore -> 4 attempts logged, each "
                 "with its own outcome, 4 distinct ordinals");
        AE_CHECK(results[3].outcome == ev::tier1_screen_outcome::cleared,
                 "T19: the clean attempt reports cleared");
        (void)std::filesystem::remove_all(root, ec);
    }


    // ---- T20: a store that repeats sequence numbers cannot mix attempts up -----------------------------
    {
        SameSeqStore store;
        ev::Tier1AttemptLog<SameSeqStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto first = drive(ev::run_tier1_screen(log, make_factory<SameSeqStore>(Behaviour{true, true}, calls),
                                                summarizer_factory(), spec()));
        auto second = drive(ev::run_tier1_screen(log, make_factory<SameSeqStore>(Behaviour{}, calls),
                                                 summarizer_factory(), spec()));
        AE_CHECK(first.attempt_ordinal == 1 && second.attempt_ordinal == 2 && second.family_attempts.size() == 2 &&
                     second.family_attempts[0].outcome == ev::tier1_screen_outcome::harmful &&
                     second.family_attempts[1].outcome == ev::tier1_screen_outcome::cleared,
                 "T20: both attempts got seq 1 from the store, yet each is identified by its own id -- the clean "
                 "retry is attempt 2, and the harmful attempt keeps its own figures");
    }

    // ---- T21: the attempt's own record vanishes during the run -> withheld -----------------------------
    {
        FlakyStore store;
        ev::Tier1AttemptLog<FlakyStore> log(store);
        auto calls = std::make_shared<CallLog>();
        auto factory = [&store, inner = make_factory<FlakyStore>(Behaviour{}, calls)](ev::TrialSlot const& slot) mutable {
            store.hide_all = true;  // after the read-back, before the final history read
            return inner(slot);
        };
        auto r = drive(ev::run_tier1_screen(log, factory, summarizer_factory(), spec()));
        AE_CHECK(!r.outcome.has_value() && r.attempt_log_error.has_value() &&
                     r.attempt_log_error->code == "eval.tier1_attempt_missing" && r.probes.empty() &&
                     !r.gross_harm.has_value(),
                 "T21: the history reads back without this attempt -> the verdict is withheld, not reported as attempt 0");
    }


    // ---- T22: a retry under a reworded subject cannot hide the lineage's earlier attempts ---------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        (void)drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{true, true}, calls), summarizer_factory(), spec()));
        auto swapped = spec();
        for (auto* c : {&swapped.probes[0].candidate, &swapped.gross_harm.candidate}) {
            c->subject = "default";         // subject and key swapped: the same words, a new subject key
            c->key = "deploy-region";
        }
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), swapped));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::cleared && r.attempt_count == 1 && r.lineage_attempt_count == 2 &&
                     r.lineage_attempts.size() == 2 &&
                     r.lineage_attempts[0].outcome == ev::tier1_screen_outcome::harmful &&
                     r.lineage_attempts[0].subject == "deployregion" && r.lineage_attempts[1].subject == "default",
                 "T22: subject and key swapped is a new family, but the result still shows the lineage's earlier "
                 "HARMFUL attempt under its old subject");
    }

    // ---- T23: every screen field reaches the recorded figures --------------------------------------------
    {
        ev::FollowRateProbeSpec ps = probe("mapped");
        ps.n_per_arm = 17;
        ev::FollowRateScreenResult pr;
        pr.seed = 11;
        pr.pass = false;
        pr.invalid = true;
        pr.invalid_baseline_too_easy = true;
        pr.invalid_differential_missingness = true;
        pr.invalid_insufficient_grading = true;
        pr.baseline_followed = 1;
        pr.treatment_followed = 2;
        pr.baseline_ungraded = 3;
        pr.treatment_ungraded = 4;
        pr.baseline_faulted = 5;
        pr.treatment_faulted = 6;
        pr.treatment_lower_bound = 0.25;
        pr.setup_error = ae::error{ae::failure_class::fatal, "x", "eval.probe_code"};
        auto const pf = ev::detail::tier1_figures_of(ps, pr);
        AE_CHECK(pf.probe_id == "mapped" && pf.seed == 11 && pf.pass == false && pf.invalid &&
                     pf.invalid_baseline_too_easy && pf.invalid_differential_missingness &&
                     pf.invalid_insufficient_grading && pf.n_per_arm == 17 && pf.baseline_followed == 1 &&
                     pf.treatment_followed == 2 && pf.baseline_ungraded == 3 && pf.treatment_ungraded == 4 &&
                     pf.baseline_faulted == 5 && pf.treatment_faulted == 6 && pf.treatment_lower_bound == 0.25 &&
                     pf.error_code == "eval.probe_code",
                 "T23: every probe result field is copied into its recorded figures, each to the right field");

        ev::GrossHarmScreenResult hr;
        hr.seed = 21;
        hr.flagged = true;
        hr.flagged_by_sum = true;
        hr.flagged_by_min_task = true;
        hr.invalid = true;
        hr.invalid_differential_missingness = true;
        hr.invalid_insufficient_grading = true;
        hr.invalid_uninformative_baseline = true;
        hr.worst_case_imputation = true;
        hr.sum_pvalue = 0.125;
        hr.min_task_pvalue = 0.375;
        hr.baseline_success_rate = 0.625;
        hr.baseline_ungraded = 7;
        hr.treatment_ungraded = 8;
        hr.baseline_faulted = 9;
        hr.treatment_faulted = 10;
        hr.setup_error = ae::error{ae::failure_class::fatal, "x", "eval.harm_code"};
        auto const hf = ev::detail::tier1_figures_of(hr);
        AE_CHECK(hf.seed == 21 && hf.flagged == true && hf.flagged_by_sum && hf.flagged_by_min_task && hf.invalid &&
                     hf.invalid_differential_missingness && hf.invalid_insufficient_grading &&
                     hf.invalid_uninformative_baseline && hf.worst_case_imputation && hf.sum_pvalue == 0.125 &&
                     hf.min_task_pvalue == 0.375 && hf.baseline_success_rate == 0.625 && hf.baseline_ungraded == 7 &&
                     hf.treatment_ungraded == 8 && hf.baseline_faulted == 9 && hf.treatment_faulted == 10 &&
                     hf.error_code == "eval.harm_code",
                 "T23: every gross-harm result field is copied into its recorded figures, each to the right field");
        ev::GrossHarmScreenResult off;  // and the all-false/empty case maps to all-false/empty, not a constant
        auto const of = ev::detail::tier1_figures_of(off);
        AE_CHECK(!of.flagged_by_sum && !of.flagged_by_min_task && !of.invalid && !of.worst_case_imputation &&
                     !of.invalid_uninformative_baseline && of.error_code.empty() && !of.flagged.has_value(),
                 "T23: an all-clear harm result records all-clear figures");
    }

    // ---- T24: every malformed record kind counts as an unreadable attempt --------------------------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto const id = ev::detail::tier1_lineage_log_id(family.lineage);
        std::string const design = "{}";
        auto const digest = *ev::detail::tier1_digest_of(design);
        auto a1 = log.begin_attempt(family, digest, design, "op", "t");
        std::string const probe_ok =
            R"({"probe_id":"p","seed":"1","pass":true,"invalid":false,"invalid_baseline_too_easy":false,)"
            R"("invalid_differential_missingness":false,"invalid_insufficient_grading":false,"n_per_arm":"10",)"
            R"("baseline_followed":"0","treatment_followed":"10","baseline_ungraded":"0","treatment_ungraded":"0",)"
            R"("baseline_faulted":"0","treatment_faulted":"0","treatment_lower_bound":"0.7","error_code":""})";
        auto harm = [](std::string seed) {
            return R"({"seed":")" + seed + R"(","flagged":true,"flagged_by_sum":true,"flagged_by_min_task":false,)"
                   R"("invalid":false,"invalid_differential_missingness":false,"invalid_insufficient_grading":false,)"
                   R"("invalid_uninformative_baseline":false,"worst_case_imputation":false,"sum_pvalue":"0.01",)"
                   R"("min_task_pvalue":"0.5","baseline_success_rate":"1","baseline_ungraded":"0",)"
                   R"("treatment_ungraded":"0","baseline_faulted":"0","treatment_faulted":"0","error_code":""})";
        };
        auto record = [&](std::string event, std::string attempt_field, std::string outcome, std::string probes,
                          std::string gross) {
            return R"({"schema":"adr181.tier1.attempt.v2","event":")" + event + "\"" + attempt_field +
                   R"(,"outcome":")" + outcome + R"(","probes":)" + probes + gross + "}";
        };
        std::string const aid = R"(,"attempt_id":")" + a1->attempt_id + "\"";
        auto with_gross = [](std::string h) { return R"(,"gross_harm":)" + h; };
        auto sub = [](std::string text, std::string from, std::string to) {
            text.replace(text.find(from), from.size(), to);
            return text;
        };
        std::vector<std::pair<char const*, std::string>> const bad = {
            {"an unknown event", record("paused", aid, "harmful", "[]", with_gross(harm("2")))},
            {"no attempt_id", record("completed", "", "harmful", "[]", with_gross(harm("3")))},
            {"an unknown outcome name", record("completed", aid, "great", "[]", with_gross(harm("4")))},
            {"a malformed probe figure", record("completed", aid, "harmful",
                                                "[" + sub(probe_ok, R"("seed":"1")", R"("seed":1)") + "]",
                                                with_gross(harm("5")))},
            {"a missing bool", record("completed", aid, "harmful", "[]",
                                      with_gross(sub(harm("6"), R"("invalid":false,)", "")))},
            {"a missing string", record("completed", aid, "harmful", "[]",
                                        with_gross(sub(harm("7"), R"(,"error_code":"")", "")))},
            {"a double with trailing text", record("completed", aid, "harmful", "[]",
                                                   with_gross(sub(harm("8"), R"("0.01")", R"("0.01x")")))},
            {"a double as a JSON number", record("completed", aid, "harmful", "[]",
                                                 with_gross(sub(harm("9"), R"("0.01")", "0.01")))},
            {"no gross_harm key", record("completed", aid, "harmful", "[]", "")},
            {"probes not an array", record("completed", aid, "harmful", "{}", with_gross(harm("10")))},
        };
        for (auto const& [name, text] : bad) (void)store.append(*id, bytes_of(text));
        (void)store.append(*id, bytes_of(record("completed", aid, "harmful", "[" + probe_ok + "]", with_gross(harm("1")))));
        auto a = log.attempts(family);
        bool ok = a.has_value() && a->size() == 1 + bad.size() && (*a)[0].completed && (*a)[0].gross_harm.has_value() &&
                  (*a)[0].gross_harm->seed == 1 && (*a)[0].probes.size() == 1;
        for (std::size_t i = 1; ok && i < a->size(); ++i) ok = (*a)[i].unreadable;
        AE_CHECK(ok, "T24: an unknown event, no attempt_id, an unknown outcome, a malformed probe figure, a missing bool, "
                     "a missing string, a double with trailing text, a double as a JSON number, no gross_harm key and "
                     "a non-array probes each count as an unreadable attempt; only the well-formed completion attaches");
    }

    // ---- T25: the read-back must find THIS attempt, and a read error stops the run ----------------------
    {
        LosingStore store;
        store.keep = true;
        ev::Tier1AttemptLog<LosingStore> log(store);
        auto calls = std::make_shared<CallLog>();
        (void)drive(ev::run_tier1_screen(log, make_factory<LosingStore>(Behaviour{true, true}, calls), summarizer_factory(), spec()));
        store.keep = false;  // earlier attempts are in the log; this one's start is accepted and lost
        auto calls2 = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<LosingStore>(Behaviour{}, calls2), summarizer_factory(), spec()));
        AE_CHECK(r.setup_error.has_value() && r.setup_error->code == "eval.tier1_attempt_not_counted" &&
                     calls2->probe_calls == 0,
                 "T25: the log holds an earlier attempt but not this one -> this attempt does not run");

        FlakyStore flaky;
        ev::Tier1AttemptLog<FlakyStore> flaky_log(flaky);
        flaky.fail_read = true;  // the start is written, but nothing can be read back
        auto calls3 = std::make_shared<CallLog>();
        auto r2 = drive(ev::run_tier1_screen(flaky_log, make_factory<FlakyStore>(Behaviour{}, calls3), summarizer_factory(), spec()));
        AE_CHECK(r2.setup_error.has_value() && r2.setup_error->code == "eval.tier1_attempt_not_counted" &&
                     calls3->probe_calls == 0,
                 "T25: the read-back itself fails -> this attempt does not run");
    }

    // ---- T26: edge cases of the family key and the call budget -------------------------------------------
    {
        AE_CHECK(!ev::tier1_family_subject_key("--- __").has_value() && ev::tier1_family_subject_key("A-b_1") == "ab1",
                 "T26: a subject with no letter or digit has no key; otherwise the key is its lower-cased letters and digits");
        auto s = spec();
        s.probes[0].max_model_calls = std::numeric_limits<std::uint64_t>::max();
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), s));
        AE_CHECK(r.setup_error.has_value() && r.setup_error->code == "eval.tier1_model_call_budget" && calls->probe_calls == 0,
                 "T26: summed budgets that would overflow saturate and are refused, not wrapped to a small number");
    }

    std::cout << (g_failures == 0 ? "test_eval_tier1_screen: OK\n" : "test_eval_tier1_screen: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
