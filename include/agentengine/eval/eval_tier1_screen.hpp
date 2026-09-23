#pragma once
// Implements ADR-181 §3.0's "Tier-1 pre-registration and attempt accounting" (E32, the round-4 fix for
// R4-Stat3): a harmful lesson that fails a screen, retried until one run comes out clean, used to reach
// the approver as that one clean `ScreenResult` -- retrying lifts the chance some attempt passes a -10 pp
// lesson from 30% (one try) to 66% (three) to 97% (ten), and nothing showed the approver the retries.
//
// Three pieces, run in this order by `run_tier1_screen`:
//   1. Pre-registration. Before any trial runs, the screen's design -- the rendered lesson's digest and
//      template version, every probe, every regression task, every statistical parameter -- is hashed
//      into one digest (`tier1_preregistration_digest`).
//   2. Attempt accounting. Also before any trial runs, a `started` record naming that digest is appended
//      to the lesson FAMILY's attempt log (§3.3: family = the candidate's `subject` + its source lineage,
//      never its `key`, which an optimiser controls). A screen whose `started` record cannot be written
//      does not run: an uncounted attempt is exactly what E32 exists to prevent. The attempt's figures are
//      appended as a `completed` record when it ends; an attempt that crashes stays in the log as
//      started-but-not-completed, still counted.
//   3. The `Tier1ScreenResult` names this attempt's ordinal, the family's attempt count, and EVERY
//      attempt's figures read back from the log -- not only this one.
//
// The follow-rate screen (§3.0 item 2) runs first; the gross-harm screen (§3.0 item 3, ~300 runs) runs
// only if every probe passed (§3.0: "do this first", and k probes must ALL pass, §6 G4).
//
// Not claimed (ADR-181 §8): the counter is a statistical correction (it adjusts no alpha -- it makes
// retries visible); the log is tamper-resistant (a host that can write the store can rewrite it -- the
// counter is honest bookkeeping, not a ledger); graders are hashed (a `GraderFn` is code, so the host names
// its version in `suite_version`, on trust); `extra_capabilities` are hashed (they are host capabilities,
// not design data, and stub tools have no effect for them to widen). Arm S (§3.0 item 4 / §3.7) is not
// built, so a `cleared` outcome covers items 2 and 3 only and says so (`steering_manifest_run`).

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/eval/eval_follow_rate_screen.hpp"
#include "agentengine/eval/eval_gross_harm_screen.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine::eval {

// What one Tier-1 attempt concluded. Arm S is not built, so `cleared` means "not inert on its probe(s)
// and not flagged for gross harm" -- never "safe to promote", and never "helps" (§3.0).
enum class tier1_screen_outcome {  // ae-naming-lint: allow tier1_screen_outcome — ADR-181 E32
    cleared,       // every probe passed and the gross-harm screen did not flag
    inert,         // a probe did not pass (the gross-harm screen did not run)
    harmful,       // the gross-harm screen flagged
    inconclusive,  // a probe or the gross-harm screen was invalid (no verdict)
    errored,       // a screen reported a setup error after the attempt started
};

[[nodiscard]] inline std::string_view tier1_screen_outcome_name(tier1_screen_outcome o) {
    switch (o) {
        case tier1_screen_outcome::cleared: return "cleared";
        case tier1_screen_outcome::inert: return "inert";
        case tier1_screen_outcome::harmful: return "harmful";
        case tier1_screen_outcome::inconclusive: return "inconclusive";
        case tier1_screen_outcome::errored: return "errored";
    }
    return "errored";
}

[[nodiscard]] inline std::optional<tier1_screen_outcome> tier1_screen_outcome_from_name(std::string_view s) {
    for (auto o : {tier1_screen_outcome::cleared, tier1_screen_outcome::inert, tier1_screen_outcome::harmful,
                   tier1_screen_outcome::inconclusive, tier1_screen_outcome::errored}) {
        if (tier1_screen_outcome_name(o) == s) return o;
    }
    return std::nullopt;
}

// §3.3's family. `subject` is the candidate's own; `lineage` names the run/session the candidate came from
// and is host-supplied (ADR-179's `source_span` shape is still open, so it is not parsed for this).
struct Tier1Family {  // ae-naming-lint: allow Tier1Family — ADR-181 E32 / §3.3
    std::string subject;
    std::string lineage;
};

struct Tier1ScreenSpec {  // ae-naming-lint: allow Tier1ScreenSpec — ADR-181 E32
    // The candidate's source lineage (§3.3). The family's `subject` is taken from the candidate itself,
    // so a caller cannot file an attempt under some other lesson's family.
    std::string lineage;
    // Names the version of the graders' code (a `GraderFn` cannot be hashed). Host-authored, on trust.
    std::string suite_version;
    // One by default; more than one must ALL pass (§3.0 item 2, §6 G4). Every probe and the gross-harm
    // spec must carry the same candidate, template version and salience -- that is the lesson under test.
    std::vector<FollowRateProbeSpec> probes;
    GrossHarmScreenSpec gross_harm;
};

// The figures an approver needs from one probe, as recorded in the family log.
struct Tier1ProbeFigures {  // ae-naming-lint: allow Tier1ProbeFigures — ADR-181 E32
    std::string probe_id;
    std::uint64_t seed = 0;
    std::optional<bool> pass;
    bool invalid = false;
    std::uint64_t n_per_arm = 0;
    std::uint64_t baseline_followed = 0, treatment_followed = 0;
    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::optional<double> treatment_lower_bound;
    std::string error_code;  // empty unless the probe reported a setup error
};

struct Tier1HarmFigures {  // ae-naming-lint: allow Tier1HarmFigures — ADR-181 E32
    std::uint64_t seed = 0;
    std::optional<bool> flagged;
    bool invalid = false;
    bool worst_case_imputation = false;
    std::optional<double> sum_pvalue, min_task_pvalue;
    double baseline_success_rate = 0.0;
    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::string error_code;
};

// One attempt as read back from the family log.
struct Tier1AttemptRecord {  // ae-naming-lint: allow Tier1AttemptRecord — ADR-181 E32
    std::size_t ordinal = 0;               // 1-based, in the order attempts STARTED
    rt::SeqNo started_seq = 0;             // the log position of its `started` record (its identity)
    Digest preregistration;
    // False if the attempt never wrote its figures (it crashed, or is still running). It is still counted.
    bool completed = false;
    // True if a log record could not be decoded. Counted as an attempt of its own -- over-counting is the
    // safe direction for a record whose purpose is to stop retries being hidden.
    bool unreadable = false;
    std::optional<tier1_screen_outcome> outcome;
    std::vector<Tier1ProbeFigures> probes;
    std::optional<Tier1HarmFigures> gross_harm;  // nullopt when the gross-harm screen did not run
};

struct Tier1ScreenResult {  // ae-naming-lint: allow Tier1ScreenResult — ADR-181 E32 (the ADR's `ScreenResult`)
    Tier1Family family;
    Digest preregistration_digest;

    // Set iff the attempt ran and the family's history could be read afterwards. A result that cannot
    // show the family's other attempts is withheld rather than presented as if it were the only one.
    std::optional<tier1_screen_outcome> outcome;
    bool steering_manifest_run = false;    // arm S (§3.7) is not built; a `cleared` outcome excludes it

    std::size_t attempt_ordinal = 0;       // this attempt's position among the family's attempts
    std::size_t attempt_count = 0;         // every started attempt for this family, this one included
    std::size_t distinct_preregistrations = 0;  // > 1: the design changed between attempts
    std::vector<Tier1AttemptRecord> family_attempts;  // every attempt, in start order, this one included

    // This attempt's full detail.
    std::vector<FollowRateScreenResult> probes;    // in run order; stops at the first probe that fails
    std::optional<GrossHarmScreenResult> gross_harm;

    std::optional<error> setup_error;       // refused before the attempt was counted; nothing ran
    std::optional<error> attempt_log_error; // the log failed after the attempt started (see `outcome`)
};

namespace detail {

[[nodiscard]] inline json::Value tier1_u64(std::uint64_t v) {
    // Integers as decimal strings: a JSON number is a double here, exact only to 2^53.
    return json::Value::make_string(std::to_string(v));
}

[[nodiscard]] inline json::Value tier1_opt_u64(std::optional<std::uint64_t> v) {
    return v.has_value() ? tier1_u64(*v) : json::Value::make_null();
}

[[nodiscard]] inline json::Value tier1_opt_double(std::optional<double> v) {
    return v.has_value() ? json::Value::make_number(*v) : json::Value::make_null();
}

[[nodiscard]] inline json::Value tier1_opt_bool(std::optional<bool> v) {
    return v.has_value() ? json::Value::make_bool(*v) : json::Value::make_null();
}

[[nodiscard]] inline json::Value tier1_stub_tools_json(std::vector<StubToolFixture> const& tools) {
    std::vector<json::Value> out;
    out.reserve(tools.size());
    for (StubToolFixture const& t : tools) {
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("name", json::Value::make_string(t.name));
        o.emplace_back("description", json::Value::make_string(t.description));
        o.emplace_back("args_schema_json", json::Value::make_string(t.args_schema_json));
        o.emplace_back("reply_schema_json", json::Value::make_string(t.reply_schema_json));
        o.emplace_back("canned_reply", t.canned_reply);
        out.push_back(json::Value::make_object(std::move(o)));
    }
    return json::Value::make_array(std::move(out));
}

[[nodiscard]] inline result<Digest> tier1_digest_of(std::string_view text) {
    return compute_digest(std::as_bytes(std::span{text.data(), text.size()}));
}

// The rendered lesson's digest for one spec's (candidate, template_version, salience).
[[nodiscard]] inline result<Digest> tier1_lesson_digest(LessonCandidate const& candidate,
                                                         std::string const& template_version, float salience) {
    auto rendered = render_lesson(candidate, template_version, salience);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered_lesson_digest(*rendered, template_version);
}

[[nodiscard]] inline error tier1_contract(char const* msg, char const* code) {
    return error{failure_class::contract, msg, code};
}

// Everything that must hold before an attempt is counted. Each screen's own pre-flight runs here too, so
// a spec that screen would refuse is refused before its family is charged an attempt.
[[nodiscard]] inline std::optional<error> validate_tier1_spec(Tier1ScreenSpec const& spec) {
    if (spec.lineage.empty()) {
        return tier1_contract("lineage must be set: it is half of the family key", "eval.tier1_lineage_missing");
    }
    if (spec.suite_version.empty()) {
        return tier1_contract("suite_version must name the graders' version", "eval.tier1_suite_version_missing");
    }
    if (spec.probes.empty()) {
        return tier1_contract("at least one follow-rate probe is required", "eval.tier1_no_probes");
    }
    auto const lesson = tier1_lesson_digest(spec.gross_harm.candidate, spec.gross_harm.template_version,
                                            spec.gross_harm.lesson_salience);
    if (!lesson) return lesson.error();
    std::set<std::string> probe_ids;
    for (FollowRateProbeSpec const& probe : spec.probes) {
        if (auto bad = validate_follow_rate_probe_spec(probe); bad.has_value()) return bad;
        if (!probe_ids.insert(probe.probe_id).second) {
            return tier1_contract("probe_ids must be unique", "eval.tier1_probe_id_duplicate");
        }
        // The rendered digest covers content, tags, salience and template version -- the lesson as the model
        // reads it -- so two specs agreeing on it are testing the same lesson.
        auto const probe_lesson = tier1_lesson_digest(probe.candidate, probe.template_version, probe.lesson_salience);
        if (!probe_lesson) return probe_lesson.error();
        if (*probe_lesson != *lesson || probe.candidate.subject != spec.gross_harm.candidate.subject ||
            probe.candidate.key != spec.gross_harm.candidate.key ||
            probe.candidate.value != spec.gross_harm.candidate.value) {
            return tier1_contract("every probe and the gross-harm spec must screen the same lesson",
                                  "eval.tier1_lesson_mismatch");
        }
    }
    if (auto bad = validate_gross_harm_spec(spec.gross_harm); bad.has_value()) return bad;
    return std::nullopt;
}

[[nodiscard]] inline Tier1Family tier1_family_of(Tier1ScreenSpec const& spec) {
    return Tier1Family{spec.gross_harm.candidate.subject, spec.lineage};
}

// The family's log id: a digest, so any subject/lineage text maps to a store-safe name.
[[nodiscard]] inline result<rt::LogId> tier1_family_log_id(Tier1Family const& family) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("subject", json::Value::make_string(family.subject));
    o.emplace_back("lineage", json::Value::make_string(family.lineage));
    auto digest = tier1_digest_of(json::dump(json::Value::make_object(std::move(o))));
    if (!digest) return std::unexpected(digest.error());
    return rt::LogId("tier1-family-" + *digest);
}

}  // namespace detail

// E32's pre-registration digest: SHA-256 over a canonical JSON rendering of the screen's design. Covered:
// the rendered lesson's digest and template version, `suite_version`, and for every probe and regression
// task its id, prompt, stub tools and statistical parameters (K, N, alpha, thresholds, permutations, turn
// and token caps, retry pool). NOT covered, deliberately: the seeds (a new seed is a new attempt, and each
// attempt's seeds are recorded with its figures), and the pure resource caps `max_model_calls`,
// `max_permutation_work` and `retain_recordings`, which bound cost but not what is measured.
[[nodiscard]] inline result<Digest> tier1_preregistration_digest(Tier1ScreenSpec const& spec) {
    using json::Value;
    using Obj = std::vector<std::pair<std::string, Value>>;
    auto lesson = detail::tier1_lesson_digest(spec.gross_harm.candidate, spec.gross_harm.template_version,
                                              spec.gross_harm.lesson_salience);
    if (!lesson) return std::unexpected(lesson.error());

    std::vector<Value> probes;
    for (FollowRateProbeSpec const& p : spec.probes) {
        Obj o;
        o.emplace_back("probe_id", Value::make_string(p.probe_id));
        o.emplace_back("task_prompt", message_to_json(p.task_prompt));
        o.emplace_back("stub_tools", detail::tier1_stub_tools_json(p.stub_tools));
        o.emplace_back("n_per_arm", detail::tier1_u64(p.n_per_arm));
        o.emplace_back("baseline_invalid_threshold", Value::make_number(p.baseline_invalid_threshold));
        o.emplace_back("target_lower_bound", Value::make_number(p.target_lower_bound));
        o.emplace_back("alpha", Value::make_number(p.alpha));
        o.emplace_back("max_differential_missingness", Value::make_number(p.max_differential_missingness));
        o.emplace_back("min_graded_fraction", Value::make_number(p.min_graded_fraction));
        o.emplace_back("max_retried_trials", detail::tier1_u64(p.max_retried_trials));
        o.emplace_back("token_budget", detail::tier1_opt_u64(p.token_budget));
        o.emplace_back("summarizer_token_budget", detail::tier1_opt_u64(p.summarizer_token_budget));
        o.emplace_back("max_turns", detail::tier1_opt_u64(p.max_turns));
        o.emplace_back("max_injected", detail::tier1_u64(p.max_injected));
        probes.push_back(Value::make_object(std::move(o)));
    }

    GrossHarmScreenSpec const& g = spec.gross_harm;
    std::vector<Value> tasks;
    for (RegressionTask const& t : g.tasks) {
        Obj o;
        o.emplace_back("task_id", Value::make_string(t.task_id));
        o.emplace_back("task_prompt", message_to_json(t.task_prompt));
        o.emplace_back("stub_tools", detail::tier1_stub_tools_json(t.stub_tools));
        tasks.push_back(Value::make_object(std::move(o)));
    }
    Obj harm;
    harm.emplace_back("suite_id", Value::make_string(g.suite_id));
    harm.emplace_back("tasks", Value::make_array(std::move(tasks)));
    harm.emplace_back("k_per_arm", detail::tier1_u64(g.k_per_arm));
    harm.emplace_back("alpha", Value::make_number(g.alpha));
    harm.emplace_back("num_permutations", detail::tier1_u64(g.num_permutations));
    harm.emplace_back("max_retried_trials", detail::tier1_u64(g.max_retried_trials));
    harm.emplace_back("max_differential_missingness", Value::make_number(g.max_differential_missingness));
    harm.emplace_back("min_graded_fraction", Value::make_number(g.min_graded_fraction));
    harm.emplace_back("min_baseline_success_rate", Value::make_number(g.min_baseline_success_rate));
    harm.emplace_back("token_budget", detail::tier1_opt_u64(g.token_budget));
    harm.emplace_back("summarizer_token_budget", detail::tier1_opt_u64(g.summarizer_token_budget));
    harm.emplace_back("max_turns", detail::tier1_opt_u64(g.max_turns));
    harm.emplace_back("max_injected", detail::tier1_u64(g.max_injected));

    Obj root;
    root.emplace_back("schema", Value::make_string("adr181.tier1.preregistration.v1"));
    root.emplace_back("suite_version", Value::make_string(spec.suite_version));
    root.emplace_back("template_version", Value::make_string(g.template_version));
    root.emplace_back("rendered_lesson_digest", Value::make_string(*lesson));
    root.emplace_back("probes", Value::make_array(std::move(probes)));
    root.emplace_back("gross_harm", Value::make_object(std::move(harm)));
    root.emplace_back("arm_s", Value::make_null());  // not built (§3.7); a SlotTable will be hashed here
    return detail::tier1_digest_of(json::dump(Value::make_object(std::move(root))));
}

namespace detail {

[[nodiscard]] inline std::vector<std::byte> tier1_bytes_of(json::Value const& v) {
    std::string const text = json::dump(v);
    auto const bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] inline json::Value tier1_probe_figures_json(Tier1ProbeFigures const& f) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("probe_id", json::Value::make_string(f.probe_id));
    o.emplace_back("seed", tier1_u64(f.seed));
    o.emplace_back("pass", tier1_opt_bool(f.pass));
    o.emplace_back("invalid", json::Value::make_bool(f.invalid));
    o.emplace_back("n_per_arm", tier1_u64(f.n_per_arm));
    o.emplace_back("baseline_followed", tier1_u64(f.baseline_followed));
    o.emplace_back("treatment_followed", tier1_u64(f.treatment_followed));
    o.emplace_back("baseline_ungraded", tier1_u64(f.baseline_ungraded));
    o.emplace_back("treatment_ungraded", tier1_u64(f.treatment_ungraded));
    o.emplace_back("treatment_lower_bound", tier1_opt_double(f.treatment_lower_bound));
    o.emplace_back("error_code", json::Value::make_string(f.error_code));
    return json::Value::make_object(std::move(o));
}

[[nodiscard]] inline json::Value tier1_harm_figures_json(Tier1HarmFigures const& f) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("seed", tier1_u64(f.seed));
    o.emplace_back("flagged", tier1_opt_bool(f.flagged));
    o.emplace_back("invalid", json::Value::make_bool(f.invalid));
    o.emplace_back("worst_case_imputation", json::Value::make_bool(f.worst_case_imputation));
    o.emplace_back("sum_pvalue", tier1_opt_double(f.sum_pvalue));
    o.emplace_back("min_task_pvalue", tier1_opt_double(f.min_task_pvalue));
    o.emplace_back("baseline_success_rate", json::Value::make_number(f.baseline_success_rate));
    o.emplace_back("baseline_ungraded", tier1_u64(f.baseline_ungraded));
    o.emplace_back("treatment_ungraded", tier1_u64(f.treatment_ungraded));
    o.emplace_back("error_code", json::Value::make_string(f.error_code));
    return json::Value::make_object(std::move(o));
}

// Strict readers: any missing or mistyped field makes the whole record unreadable.
[[nodiscard]] inline std::optional<std::string> tier1_read_string(json::Value const& o, std::string_view key) {
    json::Value const* v = o.find(key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return v->as_string();
}

[[nodiscard]] inline std::optional<std::uint64_t> tier1_read_u64(json::Value const& o, std::string_view key) {
    auto s = tier1_read_string(o, key);
    if (!s.has_value() || s->empty()) return std::nullopt;
    std::uint64_t out = 0;
    auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), out);
    if (ec != std::errc{} || ptr != s->data() + s->size()) return std::nullopt;
    return out;
}

// For an optional field: outer nullopt = malformed, inner nullopt = JSON null.
[[nodiscard]] inline std::optional<std::optional<double>> tier1_read_opt_double(json::Value const& o,
                                                                                std::string_view key) {
    json::Value const* v = o.find(key);
    if (v == nullptr) return std::nullopt;
    if (v->is_null()) return std::optional<double>{};
    if (!v->is_number()) return std::nullopt;
    return std::optional<double>{v->as_number()};
}

[[nodiscard]] inline std::optional<std::optional<bool>> tier1_read_opt_bool(json::Value const& o,
                                                                            std::string_view key) {
    json::Value const* v = o.find(key);
    if (v == nullptr) return std::nullopt;
    if (v->is_null()) return std::optional<bool>{};
    if (!v->is_bool()) return std::nullopt;
    return std::optional<bool>{v->as_bool()};
}

[[nodiscard]] inline std::optional<bool> tier1_read_bool(json::Value const& o, std::string_view key) {
    json::Value const* v = o.find(key);
    if (v == nullptr || !v->is_bool()) return std::nullopt;
    return v->as_bool();
}

[[nodiscard]] inline std::optional<Tier1ProbeFigures> tier1_probe_figures_from_json(json::Value const& o) {
    if (!o.is_object()) return std::nullopt;
    Tier1ProbeFigures f;
    auto probe_id = tier1_read_string(o, "probe_id");
    auto seed = tier1_read_u64(o, "seed");
    auto pass = tier1_read_opt_bool(o, "pass");
    auto invalid = tier1_read_bool(o, "invalid");
    auto n = tier1_read_u64(o, "n_per_arm");
    auto bf = tier1_read_u64(o, "baseline_followed");
    auto tf = tier1_read_u64(o, "treatment_followed");
    auto bu = tier1_read_u64(o, "baseline_ungraded");
    auto tu = tier1_read_u64(o, "treatment_ungraded");
    auto lb = tier1_read_opt_double(o, "treatment_lower_bound");
    auto code = tier1_read_string(o, "error_code");
    if (!probe_id || !seed || !pass || !invalid || !n || !bf || !tf || !bu || !tu || !lb || !code) {
        return std::nullopt;
    }
    f.probe_id = *probe_id;
    f.seed = *seed;
    f.pass = *pass;
    f.invalid = *invalid;
    f.n_per_arm = *n;
    f.baseline_followed = *bf;
    f.treatment_followed = *tf;
    f.baseline_ungraded = *bu;
    f.treatment_ungraded = *tu;
    f.treatment_lower_bound = *lb;
    f.error_code = *code;
    return f;
}

[[nodiscard]] inline std::optional<Tier1HarmFigures> tier1_harm_figures_from_json(json::Value const& o) {
    if (!o.is_object()) return std::nullopt;
    Tier1HarmFigures f;
    auto seed = tier1_read_u64(o, "seed");
    auto flagged = tier1_read_opt_bool(o, "flagged");
    auto invalid = tier1_read_bool(o, "invalid");
    auto worst = tier1_read_bool(o, "worst_case_imputation");
    auto sum_p = tier1_read_opt_double(o, "sum_pvalue");
    auto min_p = tier1_read_opt_double(o, "min_task_pvalue");
    auto rate = tier1_read_opt_double(o, "baseline_success_rate");
    auto bu = tier1_read_u64(o, "baseline_ungraded");
    auto tu = tier1_read_u64(o, "treatment_ungraded");
    auto code = tier1_read_string(o, "error_code");
    if (!seed || !flagged || !invalid || !worst || !sum_p || !min_p || !rate || !rate->has_value() || !bu || !tu ||
        !code) {
        return std::nullopt;
    }
    f.seed = *seed;
    f.flagged = *flagged;
    f.invalid = *invalid;
    f.worst_case_imputation = *worst;
    f.sum_pvalue = *sum_p;
    f.min_task_pvalue = *min_p;
    f.baseline_success_rate = **rate;
    f.baseline_ungraded = *bu;
    f.treatment_ungraded = *tu;
    f.error_code = *code;
    return f;
}

inline constexpr std::string_view kTier1LogSchema = "adr181.tier1.attempt.v1";

}  // namespace detail

// A family's attempt log over any `rt::AppendLogStore` -- in memory for tests, `FileAppendLogStore` (or a
// host's own durable store) in production, so the count survives a restart. Each attempt is identified by
// the sequence number of its `started` record, which the store assigns atomically: two screens starting at
// once for the same family get two distinct attempts, with no read-modify-write of a counter to race on.
template <rt::AppendLogStore Store>
class Tier1AttemptLog {  // ae-naming-lint: allow Tier1AttemptLog — ADR-181 E32
public:
    explicit Tier1AttemptLog(Store& store) : store_(&store) {}

    // Counts an attempt. Called BEFORE the attempt's first trial; a failure here means it must not run.
    [[nodiscard]] result<rt::SeqNo> begin_attempt(Tier1Family const& family, Digest const& preregistration) {
        auto id = detail::tier1_family_log_id(family);
        if (!id) return std::unexpected(id.error());
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("schema", json::Value::make_string(std::string(detail::kTier1LogSchema)));
        o.emplace_back("event", json::Value::make_string("started"));
        o.emplace_back("subject", json::Value::make_string(family.subject));
        o.emplace_back("lineage", json::Value::make_string(family.lineage));
        o.emplace_back("preregistration", json::Value::make_string(preregistration));
        return store_->append(*id, detail::tier1_bytes_of(json::Value::make_object(std::move(o))));
    }

    [[nodiscard]] result<void> complete_attempt(Tier1Family const& family, rt::SeqNo started_seq,
                                                tier1_screen_outcome outcome,
                                                std::vector<Tier1ProbeFigures> const& probes,
                                                std::optional<Tier1HarmFigures> const& gross_harm) {
        auto id = detail::tier1_family_log_id(family);
        if (!id) return std::unexpected(id.error());
        std::vector<json::Value> probe_json;
        for (Tier1ProbeFigures const& p : probes) probe_json.push_back(detail::tier1_probe_figures_json(p));
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("schema", json::Value::make_string(std::string(detail::kTier1LogSchema)));
        o.emplace_back("event", json::Value::make_string("completed"));
        o.emplace_back("started_seq", detail::tier1_u64(started_seq));
        o.emplace_back("outcome", json::Value::make_string(std::string(tier1_screen_outcome_name(outcome))));
        o.emplace_back("probes", json::Value::make_array(std::move(probe_json)));
        o.emplace_back("gross_harm", gross_harm.has_value() ? detail::tier1_harm_figures_json(*gross_harm)
                                                            : json::Value::make_null());
        auto appended = store_->append(*id, detail::tier1_bytes_of(json::Value::make_object(std::move(o))));
        if (!appended) return std::unexpected(appended.error());
        return {};
    }

    // Every attempt for the family, in start order. A `completed` record attaches to the `started` record it
    // names; one that names none, and any record that cannot be decoded, is an attempt of its own marked
    // `unreadable`.
    [[nodiscard]] result<std::vector<Tier1AttemptRecord>> attempts(Tier1Family const& family) const {
        auto id = detail::tier1_family_log_id(family);
        if (!id) return std::unexpected(id.error());
        auto entries = store_->read_from(*id, 0);
        if (!entries) return std::unexpected(entries.error());

        std::vector<Tier1AttemptRecord> out;
        auto unreadable = [&out](rt::SeqNo seq) {
            Tier1AttemptRecord r;
            r.started_seq = seq;
            r.unreadable = true;
            out.push_back(std::move(r));
        };
        for (std::size_t i = 0; i < entries->size(); ++i) {
            rt::SeqNo const seq = i + 1;
            std::vector<std::byte> const& bytes = (*entries)[i];
            std::string_view const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
            auto parsed = json::parse(text);
            if (!parsed || !parsed->is_object() ||
                detail::tier1_read_string(*parsed, "schema") != std::string(detail::kTier1LogSchema)) {
                unreadable(seq);
                continue;
            }
            auto const event = detail::tier1_read_string(*parsed, "event");
            if (event == "started") {
                auto prereg = detail::tier1_read_string(*parsed, "preregistration");
                if (!prereg.has_value()) {
                    unreadable(seq);
                    continue;
                }
                Tier1AttemptRecord r;
                r.started_seq = seq;
                r.preregistration = *prereg;
                out.push_back(std::move(r));
                continue;
            }
            if (event != "completed") {
                unreadable(seq);
                continue;
            }
            auto started = detail::tier1_read_u64(*parsed, "started_seq");
            auto outcome_name = detail::tier1_read_string(*parsed, "outcome");
            auto outcome = outcome_name.has_value() ? tier1_screen_outcome_from_name(*outcome_name) : std::nullopt;
            json::Value const* probes = parsed->find("probes");
            json::Value const* harm = parsed->find("gross_harm");
            auto target = std::find_if(out.begin(), out.end(), [&](Tier1AttemptRecord const& r) {
                return started.has_value() && !r.unreadable && r.started_seq == *started && !r.completed;
            });
            if (!outcome.has_value() || probes == nullptr || !probes->is_array() || harm == nullptr ||
                target == out.end()) {
                unreadable(seq);
                continue;
            }
            std::vector<Tier1ProbeFigures> probe_figures;
            bool ok = true;
            for (json::Value const& p : probes->as_array()) {
                auto f = detail::tier1_probe_figures_from_json(p);
                if (!f.has_value()) {
                    ok = false;
                    break;
                }
                probe_figures.push_back(std::move(*f));
            }
            std::optional<Tier1HarmFigures> harm_figures;
            if (ok && !harm->is_null()) {
                harm_figures = detail::tier1_harm_figures_from_json(*harm);
                ok = harm_figures.has_value();
            }
            if (!ok) {
                unreadable(seq);
                continue;
            }
            target->completed = true;
            target->outcome = outcome;
            target->probes = std::move(probe_figures);
            target->gross_harm = std::move(harm_figures);
        }
        // Start order is log order: a started record always precedes its own completed record, and an
        // unreadable record takes its own position.
        std::sort(out.begin(), out.end(),
                  [](Tier1AttemptRecord const& a, Tier1AttemptRecord const& b) { return a.started_seq < b.started_seq; });
        for (std::size_t i = 0; i < out.size(); ++i) out[i].ordinal = i + 1;
        return out;
    }

private:
    Store* store_;
};

namespace detail {

[[nodiscard]] inline Tier1ProbeFigures tier1_figures_of(FollowRateProbeSpec const& spec, FollowRateScreenResult const& r) {
    Tier1ProbeFigures f;
    f.probe_id = spec.probe_id;
    f.seed = r.seed;
    f.pass = r.pass;
    f.invalid = r.invalid;
    f.n_per_arm = spec.n_per_arm;
    f.baseline_followed = r.baseline_followed;
    f.treatment_followed = r.treatment_followed;
    f.baseline_ungraded = r.baseline_ungraded;
    f.treatment_ungraded = r.treatment_ungraded;
    f.treatment_lower_bound = r.treatment_lower_bound;
    f.error_code = r.setup_error.has_value() ? r.setup_error->code : std::string{};
    return f;
}

[[nodiscard]] inline Tier1HarmFigures tier1_figures_of(GrossHarmScreenResult const& r) {
    Tier1HarmFigures f;
    f.seed = r.seed;
    f.flagged = r.flagged;
    f.invalid = r.invalid;
    f.worst_case_imputation = r.worst_case_imputation;
    f.sum_pvalue = r.sum_pvalue;
    f.min_task_pvalue = r.min_task_pvalue;
    f.baseline_success_rate = r.baseline_success_rate;
    f.baseline_ungraded = r.baseline_ungraded;
    f.treatment_ungraded = r.treatment_ungraded;
    f.error_code = r.setup_error.has_value() ? r.setup_error->code : std::string{};
    return f;
}

}  // namespace detail

// Runs one counted Tier-1 attempt (ADR-181 §3.0, E32) -- see the file-top comment for the order. The
// factories are shared by every screen the attempt runs (a scripted test factory can tell them apart by
// `TrialSlot::trial_id`, which each screen namespaces with its own probe or suite id).
template <rt::AppendLogStore Store, class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<Tier1ScreenResult> run_tier1_screen(Tier1AttemptLog<Store>& log, InnerFactory make_inner,
                                                      SummarizerFactory make_summarizer, Tier1ScreenSpec spec) {
    Tier1ScreenResult result;
    result.family = detail::tier1_family_of(spec);

    if (auto bad = detail::validate_tier1_spec(spec); bad.has_value()) {
        result.setup_error = std::move(bad);
        co_return result;
    }
    auto prereg = tier1_preregistration_digest(spec);
    if (!prereg) {
        result.setup_error = prereg.error();
        co_return result;
    }
    result.preregistration_digest = *prereg;

    auto started = log.begin_attempt(result.family, result.preregistration_digest);
    if (!started) {
        result.setup_error = error{failure_class::transient,
                                   "the attempt could not be counted, so it was not run: " + started.error().message,
                                   "eval.tier1_attempt_not_counted"};
        co_return result;
    }

    std::vector<Tier1ProbeFigures> probe_figures;
    std::optional<Tier1HarmFigures> harm_figures;
    std::optional<tier1_screen_outcome> outcome;
    for (FollowRateProbeSpec const& probe : spec.probes) {
        FollowRateScreenResult r = co_await run_follow_rate_screen(std::ref(make_inner), std::ref(make_summarizer), probe);
        probe_figures.push_back(detail::tier1_figures_of(probe, r));
        std::optional<tier1_screen_outcome> stop;
        if (r.setup_error.has_value()) {
            stop = tier1_screen_outcome::errored;
        } else if (r.invalid) {
            stop = tier1_screen_outcome::inconclusive;
        } else if (r.pass != true) {
            stop = tier1_screen_outcome::inert;
        }
        result.probes.push_back(std::move(r));
        if (stop.has_value()) {
            outcome = stop;
            break;
        }
    }
    if (!outcome.has_value()) {
        GrossHarmScreenResult r =
            co_await run_gross_harm_screen(std::ref(make_inner), std::ref(make_summarizer), spec.gross_harm);
        harm_figures = detail::tier1_figures_of(r);
        if (r.setup_error.has_value()) {
            outcome = tier1_screen_outcome::errored;
        } else if (r.flagged == true) {
            outcome = tier1_screen_outcome::harmful;
        } else if (!r.flagged.has_value()) {
            outcome = tier1_screen_outcome::inconclusive;
        } else {
            outcome = tier1_screen_outcome::cleared;
        }
        result.gross_harm = std::move(r);
    }

    // A failed `completed` write leaves the attempt in the log as started-but-not-completed: still
    // counted, still visible. This attempt's outcome stands; the error is reported beside it.
    if (auto done = log.complete_attempt(result.family, *started, *outcome, probe_figures, harm_figures); !done) {
        result.attempt_log_error = done.error();
    }

    auto history = log.attempts(result.family);
    if (!history) {
        // Without the family's history the approver cannot see how many times this lesson was tried, so the
        // outcome is withheld rather than shown as though this were the only attempt.
        result.attempt_log_error = history.error();
        co_return result;
    }
    result.family_attempts = std::move(*history);
    result.attempt_count = result.family_attempts.size();
    std::set<Digest> designs;
    for (Tier1AttemptRecord const& a : result.family_attempts) {
        if (!a.unreadable) designs.insert(a.preregistration);
        if (!a.unreadable && a.started_seq == *started) result.attempt_ordinal = a.ordinal;
    }
    result.distinct_preregistrations = designs.size();
    if (result.attempt_ordinal == 0) {
        // Our own `started` record did not read back: the log cannot be trusted to show the family's attempts.
        result.attempt_log_error = error{failure_class::fatal, "this attempt's own record is missing from the log",
                                         "eval.tier1_attempt_missing"};
        co_return result;
    }
    result.outcome = outcome;
    co_return result;
}

}  // namespace agentengine::eval
