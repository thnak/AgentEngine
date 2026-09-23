// Implements decisions/ADR-181-evaluation-harness.md §3.0's "Tier-1 pre-registration and attempt
// accounting" (E32): the pre-registration digest is recorded before any trial runs; every started attempt
// for a lesson family is counted; the `Tier1ScreenResult` names the count and shows every attempt's figures,
// not only the latest. The model is scripted throughout; the two screens themselves are proven by
// test_eval_follow_rate_screen.cpp and test_eval_gross_harm_screen.cpp.

#include <atomic>
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
    [[nodiscard]] ae::result<ae::rt::SeqNo> append(ae::rt::LogId const& id, std::vector<std::byte> bytes) {
        if (fail_append) return std::unexpected(ae::error{ae::failure_class::transient, "disk full", "test.append"});
        return inner.append(id, std::move(bytes));
    }
    [[nodiscard]] ae::result<std::vector<std::vector<std::byte>>> read_from(ae::rt::LogId const& id,
                                                                            ae::rt::SeqNo from) const {
        if (fail_read) return std::unexpected(ae::error{ae::failure_class::transient, "io error", "test.read"});
        return inner.read_from(id, from);
    }
    [[nodiscard]] ae::rt::SeqNo last_seq(ae::rt::LogId const& id) const { return inner.last_seq(id); }
};
static_assert(ae::rt::AppendLogStore<FlakyStore>);

std::vector<std::byte> bytes_of(std::string_view s) {
    auto b = std::as_bytes(std::span{s.data(), s.size()});
    return {b.begin(), b.end()};
}

}  // namespace

int main() {
    namespace ev = ae::eval;
    using Store = ae::rt::InMemoryAppendLogStore;
    ev::Tier1Family const family{"deploy-region", "session-7"};

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
        AE_CHECK(r.family.subject == "deploy-region" && r.family.lineage == "session-7",
                 "T1: the family is the candidate's subject plus the host's lineage");
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

    // ---- T5: what the pre-registration digest covers, and what it deliberately does not -----------------
    {
        auto const base = ev::tier1_preregistration_digest(spec());
        auto differs = [&](auto mutate) {
            auto s = spec();
            mutate(s);
            auto d = ev::tier1_preregistration_digest(s);
            return base.has_value() && d.has_value() && *d != *base;
        };
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks[3].task_prompt = user_message("other"); }),
                 "T5: a regression task's prompt is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks.pop_back(); }), "T5: the task list is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks[0].stub_tools[0].canned_reply =
                                                             ae::json::Value::make_string("x"); }),
                 "T5: a stub tool's canned reply is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.k_per_arm = 6; }), "T5: K is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.alpha = 0.05; }), "T5: the harm alpha is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.gross_harm.max_turns = 5; }), "T5: the turn cap is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.probes[0].n_per_arm = 20; }), "T5: the probe's N is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.probes[0].task_prompt = user_message("other"); }),
                 "T5: the probe's prompt is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.probes[0].target_lower_bound = 0.6; }),
                 "T5: the probe's pass threshold is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) { s.suite_version = "graders-2"; }), "T5: suite_version is covered");
        AE_CHECK(differs([](ev::Tier1ScreenSpec& s) {
                     s.probes[0].lesson_salience = 0.4f;
                     s.gross_harm.lesson_salience = 0.4f;
                 }),
                 "T5: the lesson's salience is covered (through the rendered-lesson digest)");
        AE_CHECK(!differs([](ev::Tier1ScreenSpec& s) {
                     s.gross_harm.seed = 1;
                     s.probes[0].seed = 1;
                     s.gross_harm.max_model_calls = 1'000'000;
                     s.gross_harm.max_permutation_work = 1;
                     s.gross_harm.retain_recordings = true;
                 }),
                 "T5: seeds and pure resource caps are not part of the design");
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
            {"T6: the probe's candidate has another source span only", "",  // same lesson: allowed
             [](ev::Tier1ScreenSpec& s) { s.probes[0].candidate.source_span = "run-7/turn-3"; }},
            {"T6: a gross-harm spec its own screen would refuse", "eval.gross_harm_no_tasks",
             [](ev::Tier1ScreenSpec& s) { s.gross_harm.tasks.clear(); }},
            {"T6: a probe spec its own screen would refuse", "eval.follow_rate_n_zero",
             [](ev::Tier1ScreenSpec& s) { s.probes[0].n_per_arm = 0; }},
        };
        for (Case const& c : cases) {
            Store store;
            ev::Tier1AttemptLog<Store> log(store);
            auto calls = std::make_shared<CallLog>();
            auto s = spec();
            c.mutate(s);
            auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), s));
            auto const attempts = log.attempts(ev::Tier1Family{s.gross_harm.candidate.subject, s.lineage});
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
        auto crashed = log.begin_attempt(family, "deadbeef");  // started, never completed (the process died)
        auto const id = ev::detail::tier1_family_log_id(family);
        AE_CHECK(crashed.has_value() && id.has_value(), "T10: setup");
        (void)store.append(*id, bytes_of("not json"));
        (void)store.append(*id, bytes_of(R"({"schema":"adr181.tier1.attempt.v1","event":"completed","started_seq":"99",)"
                                         R"("outcome":"cleared","probes":[],"gross_harm":null})"));
        auto calls = std::make_shared<CallLog>();
        auto r = drive(ev::run_tier1_screen(log, make_factory<Store>(Behaviour{}, calls), summarizer_factory(), spec()));
        AE_CHECK(r.outcome == ev::tier1_screen_outcome::cleared && r.attempt_count == 4 && r.attempt_ordinal == 4,
                 "T10: a crashed attempt, a corrupt record and an orphan completion each count -> this is attempt 4 of 4");
        AE_CHECK(r.family_attempts.size() == 4 && !r.family_attempts[0].completed &&
                     !r.family_attempts[0].unreadable && r.family_attempts[0].preregistration == "deadbeef" &&
                     r.family_attempts[1].unreadable && r.family_attempts[2].unreadable &&
                     r.family_attempts[3].completed,
                 "T10: each is shown for what it is (incomplete / unreadable / unreadable / completed)");
        AE_CHECK(r.distinct_preregistrations == 2, "T10: the crashed attempt ran a different design");
    }

    // ---- T11: concurrent starts for one family are all counted, each with its own attempt ------------
    {
        Store store;
        ev::Tier1AttemptLog<Store> log(store);
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 50; ++i) {
                    if (!log.begin_attempt(family, "d").has_value()) ++failures;
                }
            });
        }
        for (auto& th : threads) th.join();
        auto a = log.attempts(family);
        std::set<ae::rt::SeqNo> seqs;
        if (a.has_value()) {
            for (auto const& r : *a) seqs.insert(r.started_seq);
        }
        AE_CHECK(failures == 0 && a.has_value() && a->size() == 200 && seqs.size() == 200 && a->back().ordinal == 200,
                 "T11: 4 threads x 50 starts -> 200 distinct attempts, ordinals 1..200 (no counter to race on)");
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

    std::cout << (g_failures == 0 ? "test_eval_tier1_screen: OK\n" : "test_eval_tier1_screen: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
