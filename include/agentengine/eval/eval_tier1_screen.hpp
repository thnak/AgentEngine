#pragma once
// Implements ADR-195 §3.0's "Tier-1 pre-registration and attempt accounting" (E32, the round-4 fix for
// R4-Stat3): a harmful lesson that fails a screen, retried until one run comes out clean, used to reach
// the approver as that one clean `ScreenResult` -- retrying lifts the chance some attempt passes a -10 pp
// lesson from 30% (one try) to 66% (three) to 97% (ten), and nothing showed the approver the retries.
//
// Three pieces, run in this order by `run_tier1_screen`:
//   1. Pre-registration. Before any trial runs, the screen's design -- the rendered lesson's digest and
//      template version, every probe, every regression task, every statistical parameter -- is rendered
//      as canonical JSON (`tier1_preregistration_json`) and hashed (`tier1_preregistration_digest`).
//   2. Attempt accounting. Also before any trial runs, a `started` record carrying that design, its digest,
//      a fresh random attempt id, and who started it and when (I4) is appended to the attempt log of the
//      candidate's LINEAGE (the run it came from; host-supplied), then READ BACK. An attempt whose `started` record cannot be written, or does not
//      read back, does not run: an uncounted attempt is exactly what E32 exists to prevent. The figures are
//      appended as a `completed` record naming the same attempt id; an attempt that crashes stays in the log
//      as started-but-not-completed, still counted.
//   3. The `Tier1ScreenResult` names this attempt's ordinal, the lineage's attempt count, and EVERY
//      attempt's design and figures read back from the log -- not only this one.
//
// The follow-rate screen (§3.0 item 2) runs first; the gross-harm screen (§3.0 item 3, ~300 runs) runs
// only if every probe passed (§3.0: "do this first", and k probes must ALL pass, §6 G4).
//
// Red team round 1 (three reviewers; one FATAL found by all three). The log's durable store,
// `rt::FileAppendLogStore`, lost and duplicated records under concurrent appends, so running several
// screens of a harmful lesson at once could hide the harmful attempts and credit a clean one's figures
// to them. The store is fixed (append_log_store.hpp). This file no longer trusts any store for identity
// either: an attempt is its random `attempt_id`, not the sequence number the store hands back, and its
// `started` record must read back before any trial runs. Also fixed from that round: a one-character
// change to `subject` (case, a space, `_` for `-`) started a fresh family, so the family key was made to
// normalise it (removed in the proportionality review below, with the family view); only the design's DIGEST was stored, so an approver saw
// that a retry changed the design but not how -- the design itself is stored now; a withheld outcome still
// left the verdict readable in `probes`/`gross_harm`; attempt records named no actor or time (I4); and
// doubles were written in a form that could not carry NaN/inf.
//
// Red team round 2: normalising `subject` was not enough -- `render_lesson` writes "For <subject> (<key>):
// <value>", so moving a word between subject and key, reordering the words, or `dep1oy` for `deploy` still
// opened a fresh family with no prior attempts and the same harmful value. The log is therefore kept per
// LINEAGE (the run the candidate came from), and every result shows every attempt from that lineage whatever
// subject it used (`lineage_attempts`), beside the narrower family view (since removed). Retrying a lesson under a new
// subject can still start a new family, but it can no longer hide the earlier attempts from the approver.
//
// Red team round 3: the HEADLINE counts (`attempt_ordinal`, `attempt_count`, `distinct_preregistrations`) were
// still the family's, so a reworded retry read "attempt 1 of 1, 1 design" and only a field beside them showed
// otherwise. They now count the whole lineage -- the unit a model cannot choose. That over-counts when one
// run produced several lessons (every lesson's attempts are in it), which is the safe direction; the
// family view (removed later, see below) narrowed it to one subject, and said it could be
// dodged. Both views now come from ONE read of the log, so the family view can never list an attempt its
// superset does not (round 3 made a concurrent attempt land between two reads).
//
// Not claimed (ADR-195 §8): the counter is a statistical correction (it adjusts no alpha -- it makes
// retries visible); the log is tamper-resistant (a host that can write the store can rewrite it -- the
// counter is honest bookkeeping, not a ledger); running the two screens DIRECTLY is prevented (they stay
// public; only `run_tier1_screen` counts); graders are hashed (a `GraderFn` is code, so the host names its
// version in `suite_version`, on trust); `extra_capabilities` are hashed (host capabilities, not design
// data); every prompt field is hashed (the prompt is hashed as `message_to_json` renders it, which omits
// `Message::attribution` and tool-call provenance). Arm S (§3.0 item 4 / §3.7) is not built, so a `cleared`
// outcome covers items 2 and 3 only and says so (`steering_manifest_run`).

// ADR-191 proportionality review (2026-09-24): the output of this screen is advice to a human approver, and the log
// can be rewritten by anyone who can write its store (§8) -- so machinery that only hardened the log further was
// cut: the per-subject family view and its subject normalisation (which refused any non-ASCII subject) are gone --
// the lineage count is the count; the lesson is declared ONCE on the spec (so the screens cannot disagree about
// it, and the mismatch check that compared them is gone); and a history that cannot be read back no longer
// withholds the verdict -- the outcome is returned with `history_complete = false` and the error beside it.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
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
#include "agentengine/eval/lesson_screen_record.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/trust/secure_random.hpp"

namespace agentengine::eval {

// `tier1_screen_outcome` (what one attempt concluded) and `lesson_delivery` live in lesson_screen_record.hpp, so the
// approval path (promotion_ack.hpp) can name them without pulling in the trial driver.

// Who the attempt is for: the candidate's own `subject` (a label, as written -- ADR-191 dropped its normalisation)
// and the host-supplied `lineage` (the run the candidate came from), which is what attempts are counted by.
struct Tier1Family {  // ae-naming-lint: allow Tier1Family — ADR-195 E32 / §3.3
    std::string subject;
    std::string lineage;
};

struct Tier1ScreenSpec {  // ae-naming-lint: allow Tier1ScreenSpec — ADR-195 E32
    // The candidate's source lineage (§3.3). The family's `subject` is taken from the candidate itself,
    // so a caller cannot file an attempt under some other lesson's family.
    std::string lineage;
    // Names the version of the graders' code (a `GraderFn` cannot be hashed). Host-authored, on trust.
    std::string suite_version;
    // Who started this attempt and when (I4) -- host-supplied, recorded in the attempt log, never derived
    // from model output. `started_at` is a host timestamp string (ISO-8601), as `PromotionAck` records one.
    std::string operator_id;
    std::string started_at;
    // The lesson under test, declared ONCE. `run_tier1_screen` (and `tier1_preregistration_json`) copy it into
    // every probe and into the gross-harm spec, overwriting whatever those carry, so the screens cannot disagree
    // about the lesson or how it is delivered (ADR-191 proportionality review: the spec used to repeat it N+1 times
    // and a validator compared the copies).
    LessonCandidate candidate;
    std::string template_version;
    float lesson_salience = 0.0f;
    lesson_delivery delivery = lesson_delivery::fenced;  // ADR-191: the route a host that opts in ships
    // One by default; more than one must ALL pass (§3.0 item 2, §6 G4).
    std::vector<FollowRateProbeSpec> probes;
    GrossHarmScreenSpec gross_harm;
    // I8: every screen's own `max_model_calls`, summed, must fit -- one bound for the whole attempt.
    std::uint64_t max_model_calls = 50'000;
};

// The figures an approver needs from one probe, as recorded in the attempt log. The thresholds it was
// judged against are in the attempt's stored design (`Tier1AttemptRecord::preregistration_json`).
struct Tier1ProbeFigures {  // ae-naming-lint: allow Tier1ProbeFigures — ADR-195 E32
    std::string probe_id;
    std::uint64_t seed = 0;
    std::optional<bool> pass;
    bool invalid = false;
    bool invalid_baseline_too_easy = false;
    bool invalid_differential_missingness = false;
    bool invalid_insufficient_grading = false;
    std::uint64_t n_per_arm = 0;
    std::uint64_t baseline_followed = 0, treatment_followed = 0;
    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::uint64_t baseline_faulted = 0, treatment_faulted = 0;
    std::optional<double> treatment_lower_bound;
    std::string error_code;  // empty unless the probe reported a setup error
};

struct Tier1HarmFigures {  // ae-naming-lint: allow Tier1HarmFigures — ADR-195 E32
    std::uint64_t seed = 0;
    std::optional<bool> flagged;
    bool flagged_by_sum = false, flagged_by_min_task = false;
    bool invalid = false;
    bool invalid_differential_missingness = false;
    bool invalid_insufficient_grading = false;
    bool invalid_uninformative_baseline = false;
    bool worst_case_imputation = false;
    std::optional<double> sum_pvalue, min_task_pvalue;
    double baseline_success_rate = 0.0;
    std::uint64_t baseline_ungraded = 0, treatment_ungraded = 0;
    std::uint64_t baseline_faulted = 0, treatment_faulted = 0;
    std::string error_code;
};

// One attempt as read back from the lineage's attempt log.
struct Tier1AttemptRecord {  // ae-naming-lint: allow Tier1AttemptRecord — ADR-195 E32
    std::size_t ordinal = 0;               // 1-based, in the order attempts STARTED
    rt::SeqNo started_seq = 0;             // the log position of its `started` record
    std::string attempt_id;                // its identity: random, written by the attempt itself
    std::string subject;                   // the normalised subject key it was filed under (empty if unreadable)
    std::string operator_id;
    std::string started_at;
    Digest preregistration;
    std::string preregistration_json;      // the design itself, so a changed design can be compared
    // False if the attempt never wrote its figures (it crashed, or is still running). It is still counted.
    bool completed = false;
    // True if a log record could not be decoded. Counted as an attempt of its own -- over-counting is the
    // safe direction for a record whose purpose is to stop retries being hidden.
    bool unreadable = false;
    std::optional<tier1_screen_outcome> outcome;
    std::vector<Tier1ProbeFigures> probes;
    std::optional<Tier1HarmFigures> gross_harm;  // nullopt when the gross-harm screen did not run
};

struct Tier1ScreenResult {  // ae-naming-lint: allow Tier1ScreenResult — ADR-195 E32 (the ADR's `ScreenResult`)
    Tier1Family family;
    Digest preregistration_digest;
    std::string attempt_id;

    // Set iff the attempt ran. If the lineage's history could not be read back afterwards, the outcome is still
    // returned, with `history_complete = false` and the reason in `attempt_log_error`: the approver must then
    // treat the attempt count as unknown (it used to be withheld -- the whole run's figures thrown away).
    std::optional<tier1_screen_outcome> outcome;
    bool history_complete = false;
    bool steering_manifest_run = false;    // arm S (§3.7) is not built; a `cleared` outcome excludes it

    // The headline counts are over the whole LINEAGE, whatever subject each attempt was filed under (red team
    // rounds 2-3: the subject is model-chosen, so a retry under a reworded subject opens a fresh family, and
    // a family-only count read "1 of 1"). With several lessons from one run they over-count -- the safe way.
    std::size_t attempt_ordinal = 0;       // this attempt's position among the lineage's attempts
    std::size_t attempt_count = 0;         // every started attempt in this lineage, this one included
    std::size_t distinct_preregistrations = 0;  // > 1: more than one design was tried in this lineage
    std::vector<Tier1AttemptRecord> lineage_attempts;  // every attempt, in start order, this one included

    // This attempt's full detail.
    std::vector<FollowRateScreenResult> probes;    // in run order; stops at the first probe that fails
    std::optional<GrossHarmScreenResult> gross_harm;

    std::optional<error> setup_error;       // refused before any trial ran
    std::optional<error> attempt_log_error; // the log failed after the attempt started (see `outcome`)
};

namespace detail {

[[nodiscard]] inline json::Value tier1_u64(std::uint64_t v) {
    // Integers as decimal strings: a JSON number is a double here, exact only to 2^53.
    return json::Value::make_string(std::to_string(v));
}

// Doubles as shortest round-trip strings: exact, and able to carry NaN and infinities, which a JSON number
// cannot (red team round 1: a NaN figure made its whole record unreadable).
[[nodiscard]] inline std::string tier1_double_text(double v) {
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc{} ? std::string(buf, ptr) : std::string("nan");
}

[[nodiscard]] inline json::Value tier1_double(double v) { return json::Value::make_string(tier1_double_text(v)); }

[[nodiscard]] inline json::Value tier1_opt_u64(std::optional<std::uint64_t> v) {
    return v.has_value() ? tier1_u64(*v) : json::Value::make_null();
}

[[nodiscard]] inline json::Value tier1_opt_double(std::optional<double> v) {
    return v.has_value() ? tier1_double(*v) : json::Value::make_null();
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

}  // namespace detail

namespace detail {

[[nodiscard]] inline std::uint64_t tier1_saturating_add(std::uint64_t a, std::uint64_t b) {
    return a > (std::numeric_limits<std::uint64_t>::max)() - b ? (std::numeric_limits<std::uint64_t>::max)() : a + b;
}

// The spec with its one lesson copied into every screen (see `Tier1ScreenSpec::candidate`).
[[nodiscard]] inline Tier1ScreenSpec tier1_with_lesson(Tier1ScreenSpec spec) {
    for (FollowRateProbeSpec& probe : spec.probes) {
        probe.candidate = spec.candidate;
        probe.template_version = spec.template_version;
        probe.lesson_salience = spec.lesson_salience;
        probe.delivery = spec.delivery;
    }
    spec.gross_harm.candidate = spec.candidate;
    spec.gross_harm.template_version = spec.template_version;
    spec.gross_harm.lesson_salience = spec.lesson_salience;
    spec.gross_harm.delivery = spec.delivery;
    return spec;
}

// Everything that must hold before an attempt is counted, on a spec whose lesson has already been copied into its
// screens. Each screen's own pre-flight runs here too, so a spec a screen would refuse costs no attempt.
[[nodiscard]] inline std::optional<error> validate_tier1_spec(Tier1ScreenSpec const& spec) {
    if (spec.lineage.empty()) {
        return tier1_contract("lineage must be set: it is half of the family key", "eval.tier1_lineage_missing");
    }
    if (spec.suite_version.empty()) {
        return tier1_contract("suite_version must name the graders' version", "eval.tier1_suite_version_missing");
    }
    if (spec.operator_id.empty() || spec.started_at.empty()) {
        return tier1_contract("operator_id and started_at must be set (I4)", "eval.tier1_actor_missing");
    }
    if (spec.probes.empty()) {
        return tier1_contract("at least one follow-rate probe is required", "eval.tier1_no_probes");
    }
    if (spec.candidate.subject.empty() && spec.candidate.key.empty() && spec.candidate.value.empty()) {
        // The lesson is declared once, on the spec, and copied over every screen's: a lesson set only on a probe
        // would be overwritten by this empty one, so say so rather than fail later on an empty render.
        return tier1_contract("the lesson must be set on Tier1ScreenSpec::candidate", "eval.tier1_lesson_unset");
    }
    auto const lesson = tier1_lesson_digest(spec.candidate, spec.template_version, spec.lesson_salience);
    if (!lesson) return lesson.error();
    std::set<std::string> probe_ids;
    std::uint64_t total_calls = spec.gross_harm.max_model_calls;
    for (FollowRateProbeSpec const& probe : spec.probes) {
        if (auto bad = validate_follow_rate_probe_spec(probe); bad.has_value()) return bad;
        if (!probe_ids.insert(probe.probe_id).second) {
            return tier1_contract("probe_ids must be unique", "eval.tier1_probe_id_duplicate");
        }
        total_calls = tier1_saturating_add(total_calls, probe.max_model_calls);
    }
    if (auto bad = validate_gross_harm_spec(spec.gross_harm); bad.has_value()) return bad;
    if (total_calls > spec.max_model_calls) {
        return error{failure_class::resource,
                     "the probes' and gross-harm screen's max_model_calls, summed, exceed the attempt's max_model_calls",
                     "eval.tier1_model_call_budget"};
    }
    return std::nullopt;
}

[[nodiscard]] inline Tier1Family tier1_family_of(Tier1ScreenSpec const& spec) {
    return Tier1Family{spec.candidate.subject, spec.lineage};
}

// The log id: one log per LINEAGE (red team round 2), named by a digest so any lineage text maps to a
// store-safe name. Families are views over it, filtered by the subject key each record carries.
[[nodiscard]] inline result<rt::LogId> tier1_lineage_log_id(std::string const& lineage) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("lineage", json::Value::make_string(lineage));
    auto digest = tier1_digest_of(json::dump(json::Value::make_object(std::move(o))));
    if (!digest) return std::unexpected(digest.error());
    return rt::LogId("tier1-lineage-" + *digest);
}

}  // namespace detail

// E32's pre-registration record: canonical JSON of the screen's design. Covered: the rendered lesson's
// digest, template version and exact salience, `suite_version`, and for every probe and regression task its
// id, prompt, stub tools and statistical parameters (K, N, alpha, thresholds, permutations, turn and token
// caps, retry pool). NOT covered, deliberately: the seeds (a new seed is a new attempt of the same design,
// and each attempt's seeds are recorded with its figures), and the pure resource caps `max_model_calls`,
// `max_permutation_work` and `retain_recordings`, which bound cost but not what is measured.
[[nodiscard]] inline result<std::string> tier1_preregistration_json(Tier1ScreenSpec const& declared) {
    Tier1ScreenSpec const spec = detail::tier1_with_lesson(declared);
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
        o.emplace_back("baseline_invalid_threshold", detail::tier1_double(p.baseline_invalid_threshold));
        o.emplace_back("target_lower_bound", detail::tier1_double(p.target_lower_bound));
        o.emplace_back("alpha", detail::tier1_double(p.alpha));
        o.emplace_back("max_differential_missingness", detail::tier1_double(p.max_differential_missingness));
        o.emplace_back("min_graded_fraction", detail::tier1_double(p.min_graded_fraction));
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
    harm.emplace_back("alpha", detail::tier1_double(g.alpha));
    harm.emplace_back("num_permutations", detail::tier1_u64(g.num_permutations));
    harm.emplace_back("max_retried_trials", detail::tier1_u64(g.max_retried_trials));
    harm.emplace_back("max_differential_missingness", detail::tier1_double(g.max_differential_missingness));
    harm.emplace_back("min_graded_fraction", detail::tier1_double(g.min_graded_fraction));
    harm.emplace_back("min_baseline_success_rate", detail::tier1_double(g.min_baseline_success_rate));
    harm.emplace_back("token_budget", detail::tier1_opt_u64(g.token_budget));
    harm.emplace_back("summarizer_token_budget", detail::tier1_opt_u64(g.summarizer_token_budget));
    harm.emplace_back("max_turns", detail::tier1_opt_u64(g.max_turns));
    harm.emplace_back("max_injected", detail::tier1_u64(g.max_injected));

    Obj root;
    root.emplace_back("schema", Value::make_string("adr181.tier1.preregistration.v2"));  // v2: lesson_delivery
    root.emplace_back("suite_version", Value::make_string(spec.suite_version));
    root.emplace_back("template_version", Value::make_string(g.template_version));
    root.emplace_back("rendered_lesson_digest", Value::make_string(*lesson));
    root.emplace_back("lesson_salience", detail::tier1_double(static_cast<double>(g.lesson_salience)));
    // ADR-191: how the lesson reaches the model changes what the screen measures, so it is part of the design. Round 4:
    // every wire form ADR-192 can ship has its own name here (lesson_screen_record.hpp), so a screen at one form hashes
    // differently from the same screen at any other, and `tier1_screen_record` reads the form back from this field.
    root.emplace_back("lesson_delivery", Value::make_string(std::string(lesson_delivery_name(g.delivery))));
    root.emplace_back("probes", Value::make_array(std::move(probes)));
    root.emplace_back("gross_harm", Value::make_object(std::move(harm)));
    root.emplace_back("arm_s", Value::make_null());  // not built (§3.7); a SlotTable will be recorded here
    return json::dump(Value::make_object(std::move(root)));
}

// SHA-256 of `tier1_preregistration_json`.
[[nodiscard]] inline result<Digest> tier1_preregistration_digest(Tier1ScreenSpec const& spec) {
    auto design = tier1_preregistration_json(spec);
    if (!design) return std::unexpected(design.error());
    return detail::tier1_digest_of(*design);
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
    o.emplace_back("invalid_baseline_too_easy", json::Value::make_bool(f.invalid_baseline_too_easy));
    o.emplace_back("invalid_differential_missingness", json::Value::make_bool(f.invalid_differential_missingness));
    o.emplace_back("invalid_insufficient_grading", json::Value::make_bool(f.invalid_insufficient_grading));
    o.emplace_back("n_per_arm", tier1_u64(f.n_per_arm));
    o.emplace_back("baseline_followed", tier1_u64(f.baseline_followed));
    o.emplace_back("treatment_followed", tier1_u64(f.treatment_followed));
    o.emplace_back("baseline_ungraded", tier1_u64(f.baseline_ungraded));
    o.emplace_back("treatment_ungraded", tier1_u64(f.treatment_ungraded));
    o.emplace_back("baseline_faulted", tier1_u64(f.baseline_faulted));
    o.emplace_back("treatment_faulted", tier1_u64(f.treatment_faulted));
    o.emplace_back("treatment_lower_bound", tier1_opt_double(f.treatment_lower_bound));
    o.emplace_back("error_code", json::Value::make_string(f.error_code));
    return json::Value::make_object(std::move(o));
}

[[nodiscard]] inline json::Value tier1_harm_figures_json(Tier1HarmFigures const& f) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("seed", tier1_u64(f.seed));
    o.emplace_back("flagged", tier1_opt_bool(f.flagged));
    o.emplace_back("flagged_by_sum", json::Value::make_bool(f.flagged_by_sum));
    o.emplace_back("flagged_by_min_task", json::Value::make_bool(f.flagged_by_min_task));
    o.emplace_back("invalid", json::Value::make_bool(f.invalid));
    o.emplace_back("invalid_differential_missingness", json::Value::make_bool(f.invalid_differential_missingness));
    o.emplace_back("invalid_insufficient_grading", json::Value::make_bool(f.invalid_insufficient_grading));
    o.emplace_back("invalid_uninformative_baseline", json::Value::make_bool(f.invalid_uninformative_baseline));
    o.emplace_back("worst_case_imputation", json::Value::make_bool(f.worst_case_imputation));
    o.emplace_back("sum_pvalue", tier1_opt_double(f.sum_pvalue));
    o.emplace_back("min_task_pvalue", tier1_opt_double(f.min_task_pvalue));
    o.emplace_back("baseline_success_rate", tier1_double(f.baseline_success_rate));
    o.emplace_back("baseline_ungraded", tier1_u64(f.baseline_ungraded));
    o.emplace_back("treatment_ungraded", tier1_u64(f.treatment_ungraded));
    o.emplace_back("baseline_faulted", tier1_u64(f.baseline_faulted));
    o.emplace_back("treatment_faulted", tier1_u64(f.treatment_faulted));
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

[[nodiscard]] inline std::optional<double> tier1_parse_double(std::string const& s) {
    if (s.empty()) return std::nullopt;
    double out = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return out;
}

[[nodiscard]] inline std::optional<double> tier1_read_double(json::Value const& o, std::string_view key) {
    auto s = tier1_read_string(o, key);
    return s.has_value() ? tier1_parse_double(*s) : std::nullopt;
}

// For an optional field: outer nullopt = malformed or missing, inner nullopt = JSON null.
[[nodiscard]] inline std::optional<std::optional<double>> tier1_read_opt_double(json::Value const& o,
                                                                                std::string_view key) {
    json::Value const* v = o.find(key);
    if (v == nullptr) return std::nullopt;
    if (v->is_null()) return std::optional<double>{};
    if (!v->is_string()) return std::nullopt;
    auto d = tier1_parse_double(v->as_string());
    if (!d.has_value()) return std::nullopt;
    return std::optional<double>{*d};
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

// Reads every listed field, or fails as a whole.
class Tier1FieldReader {
public:
    explicit Tier1FieldReader(json::Value const& o) : o_(o), ok_(o.is_object()) {}
    void str(std::string_view k, std::string& out) { take(tier1_read_string(o_, k), out); }
    void u64(std::string_view k, std::uint64_t& out) { take(tier1_read_u64(o_, k), out); }
    void dbl(std::string_view k, double& out) { take(tier1_read_double(o_, k), out); }
    void flag(std::string_view k, bool& out) { take(tier1_read_bool(o_, k), out); }
    void opt_dbl(std::string_view k, std::optional<double>& out) { take(tier1_read_opt_double(o_, k), out); }
    void opt_flag(std::string_view k, std::optional<bool>& out) { take(tier1_read_opt_bool(o_, k), out); }
    [[nodiscard]] bool ok() const { return ok_; }

private:
    template <class T, class U>
    void take(std::optional<T> v, U& out) {
        if (!ok_) return;
        if (!v.has_value()) {
            ok_ = false;
            return;
        }
        out = std::move(*v);
    }
    json::Value const& o_;
    bool ok_;
};

[[nodiscard]] inline std::optional<Tier1ProbeFigures> tier1_probe_figures_from_json(json::Value const& o) {
    Tier1ProbeFigures f;
    Tier1FieldReader r(o);
    r.str("probe_id", f.probe_id);
    r.u64("seed", f.seed);
    r.opt_flag("pass", f.pass);
    r.flag("invalid", f.invalid);
    r.flag("invalid_baseline_too_easy", f.invalid_baseline_too_easy);
    r.flag("invalid_differential_missingness", f.invalid_differential_missingness);
    r.flag("invalid_insufficient_grading", f.invalid_insufficient_grading);
    r.u64("n_per_arm", f.n_per_arm);
    r.u64("baseline_followed", f.baseline_followed);
    r.u64("treatment_followed", f.treatment_followed);
    r.u64("baseline_ungraded", f.baseline_ungraded);
    r.u64("treatment_ungraded", f.treatment_ungraded);
    r.u64("baseline_faulted", f.baseline_faulted);
    r.u64("treatment_faulted", f.treatment_faulted);
    r.opt_dbl("treatment_lower_bound", f.treatment_lower_bound);
    r.str("error_code", f.error_code);
    if (!r.ok()) return std::nullopt;
    return f;
}

[[nodiscard]] inline std::optional<Tier1HarmFigures> tier1_harm_figures_from_json(json::Value const& o) {
    Tier1HarmFigures f;
    Tier1FieldReader r(o);
    r.u64("seed", f.seed);
    r.opt_flag("flagged", f.flagged);
    r.flag("flagged_by_sum", f.flagged_by_sum);
    r.flag("flagged_by_min_task", f.flagged_by_min_task);
    r.flag("invalid", f.invalid);
    r.flag("invalid_differential_missingness", f.invalid_differential_missingness);
    r.flag("invalid_insufficient_grading", f.invalid_insufficient_grading);
    r.flag("invalid_uninformative_baseline", f.invalid_uninformative_baseline);
    r.flag("worst_case_imputation", f.worst_case_imputation);
    r.opt_dbl("sum_pvalue", f.sum_pvalue);
    r.opt_dbl("min_task_pvalue", f.min_task_pvalue);
    r.dbl("baseline_success_rate", f.baseline_success_rate);
    r.u64("baseline_ungraded", f.baseline_ungraded);
    r.u64("treatment_ungraded", f.treatment_ungraded);
    r.u64("baseline_faulted", f.baseline_faulted);
    r.u64("treatment_faulted", f.treatment_faulted);
    r.str("error_code", f.error_code);
    if (!r.ok()) return std::nullopt;
    return f;
}

inline constexpr std::string_view kTier1LogSchema = "adr181.tier1.attempt.v2";

}  // namespace detail

// A lineage's attempt log over any `rt::AppendLogStore` -- in memory for tests, `FileAppendLogStore` (or a
// host's own durable store) in production, so the count survives a restart.
//
// Identity does not depend on the store. Each attempt names itself with a random `attempt_id` written into
// its own records; its `completed` record attaches only to the `started` record with that id. Red team
// round 1 found a store that hands two appends the same sequence number (the old `FileAppendLogStore`)
// credited one attempt's figures to another; with ids that cannot happen, and `run_tier1_screen` reads its
// `started` record back before any trial runs, so an attempt the store lost never runs.
template <rt::AppendLogStore Store>
class Tier1AttemptLog {  // ae-naming-lint: allow Tier1AttemptLog — ADR-195 E32
public:
    struct Started {
        rt::SeqNo seq = 0;
        std::string attempt_id;
    };

    explicit Tier1AttemptLog(Store& store) : store_(&store) {}

    // Counts an attempt. Called BEFORE the attempt's first trial; a failure here means it must not run.
    [[nodiscard]] result<Started> begin_attempt(Tier1Family const& family, Digest const& preregistration,
                                                std::string const& preregistration_json,
                                                std::string const& operator_id, std::string const& started_at) {
        auto id = detail::tier1_lineage_log_id(family.lineage);
        if (!id) return std::unexpected(id.error());
        auto attempt_id = trust::secure_random_hex(16);
        if (!attempt_id) return std::unexpected(attempt_id.error());
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("schema", json::Value::make_string(std::string(detail::kTier1LogSchema)));
        o.emplace_back("event", json::Value::make_string("started"));
        o.emplace_back("attempt_id", json::Value::make_string(*attempt_id));
        o.emplace_back("subject", json::Value::make_string(family.subject));
        o.emplace_back("lineage", json::Value::make_string(family.lineage));
        o.emplace_back("operator_id", json::Value::make_string(operator_id));
        o.emplace_back("started_at", json::Value::make_string(started_at));
        o.emplace_back("preregistration", json::Value::make_string(preregistration));
        o.emplace_back("design", json::Value::make_string(preregistration_json));
        auto seq = store_->append(*id, detail::tier1_bytes_of(json::Value::make_object(std::move(o))));
        if (!seq) return std::unexpected(seq.error());
        return Started{*seq, std::move(*attempt_id)};
    }

    [[nodiscard]] result<void> complete_attempt(Tier1Family const& family, Started const& started,
                                                tier1_screen_outcome outcome,
                                                std::vector<Tier1ProbeFigures> const& probes,
                                                std::optional<Tier1HarmFigures> const& gross_harm) {
        auto id = detail::tier1_lineage_log_id(family.lineage);
        if (!id) return std::unexpected(id.error());
        std::vector<json::Value> probe_json;
        for (Tier1ProbeFigures const& p : probes) probe_json.push_back(detail::tier1_probe_figures_json(p));
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("schema", json::Value::make_string(std::string(detail::kTier1LogSchema)));
        o.emplace_back("event", json::Value::make_string("completed"));
        o.emplace_back("attempt_id", json::Value::make_string(started.attempt_id));
        o.emplace_back("outcome", json::Value::make_string(std::string(tier1_screen_outcome_name(outcome))));
        o.emplace_back("probes", json::Value::make_array(std::move(probe_json)));
        o.emplace_back("gross_harm", gross_harm.has_value() ? detail::tier1_harm_figures_json(*gross_harm)
                                                            : json::Value::make_null());
        auto appended = store_->append(*id, detail::tier1_bytes_of(json::Value::make_object(std::move(o))));
        if (!appended) return std::unexpected(appended.error());
        return {};
    }

    // Every attempt from the lineage, whatever its subject, in start order. A `completed` record attaches to
    // the `started` record with its `attempt_id`; one that names no such attempt, names one already
    // completed, or cannot be decoded, is an attempt of its own marked `unreadable`. So is a `started` record
    // whose stored design does not hash to its stated digest, or that repeats an earlier attempt's id.
    [[nodiscard]] result<std::vector<Tier1AttemptRecord>> lineage_attempts(std::string const& lineage) const {
        auto id = detail::tier1_lineage_log_id(lineage);
        if (!id) return std::unexpected(id.error());
        auto entries = store_->read_from(*id, 0);
        if (!entries) return std::unexpected(entries.error());

        std::vector<Tier1AttemptRecord> out;
        std::map<std::string, std::size_t> by_id;  // attempt_id -> index in `out`
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
            auto const attempt_id = detail::tier1_read_string(*parsed, "attempt_id");
            if (!attempt_id.has_value() || attempt_id->empty()) {
                unreadable(seq);
                continue;
            }
            if (event == "started") {
                Tier1AttemptRecord r;
                detail::Tier1FieldReader fields(*parsed);
                std::string filed_lineage;
                fields.str("subject", r.subject);
                fields.str("lineage", filed_lineage);
                fields.str("operator_id", r.operator_id);
                fields.str("started_at", r.started_at);
                fields.str("preregistration", r.preregistration);
                fields.str("design", r.preregistration_json);
                auto const design_digest = detail::tier1_digest_of(r.preregistration_json);
                // A record naming another lineage was misfiled (or copied in): it is not this lineage's attempt.
                if (!fields.ok() || filed_lineage != lineage || !design_digest || *design_digest != r.preregistration ||
                    by_id.contains(*attempt_id)) {
                    unreadable(seq);
                    continue;
                }
                r.started_seq = seq;
                r.attempt_id = *attempt_id;
                by_id.emplace(*attempt_id, out.size());
                out.push_back(std::move(r));
                continue;
            }
            if (event != "completed") {
                unreadable(seq);
                continue;
            }
            auto outcome_name = detail::tier1_read_string(*parsed, "outcome");
            auto outcome = outcome_name.has_value() ? tier1_screen_outcome_from_name(*outcome_name) : std::nullopt;
            json::Value const* probes = parsed->find("probes");
            json::Value const* harm = parsed->find("gross_harm");
            auto target = by_id.find(*attempt_id);
            if (!outcome.has_value() || probes == nullptr || !probes->is_array() || harm == nullptr ||
                target == by_id.end() || out[target->second].completed) {
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
            Tier1AttemptRecord& t = out[target->second];
            t.completed = true;
            t.outcome = outcome;
            t.probes = std::move(probe_figures);
            t.gross_harm = std::move(harm_figures);
        }
        // `out` is already in log order: each started or unreadable record was appended at its own position.
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
    f.invalid_baseline_too_easy = r.invalid_baseline_too_easy;
    f.invalid_differential_missingness = r.invalid_differential_missingness;
    f.invalid_insufficient_grading = r.invalid_insufficient_grading;
    f.n_per_arm = spec.n_per_arm;
    f.baseline_followed = r.baseline_followed;
    f.treatment_followed = r.treatment_followed;
    f.baseline_ungraded = r.baseline_ungraded;
    f.treatment_ungraded = r.treatment_ungraded;
    f.baseline_faulted = r.baseline_faulted;
    f.treatment_faulted = r.treatment_faulted;
    f.treatment_lower_bound = r.treatment_lower_bound;
    f.error_code = r.setup_error.has_value() ? r.setup_error->code : std::string{};
    return f;
}

[[nodiscard]] inline Tier1HarmFigures tier1_figures_of(GrossHarmScreenResult const& r) {
    Tier1HarmFigures f;
    f.seed = r.seed;
    f.flagged = r.flagged;
    f.flagged_by_sum = r.flagged_by_sum;
    f.flagged_by_min_task = r.flagged_by_min_task;
    f.invalid = r.invalid;
    f.invalid_differential_missingness = r.invalid_differential_missingness;
    f.invalid_insufficient_grading = r.invalid_insufficient_grading;
    f.invalid_uninformative_baseline = r.invalid_uninformative_baseline;
    f.worst_case_imputation = r.worst_case_imputation;
    f.sum_pvalue = r.sum_pvalue;
    f.min_task_pvalue = r.min_task_pvalue;
    f.baseline_success_rate = r.baseline_success_rate;
    f.baseline_ungraded = r.baseline_ungraded;
    f.treatment_ungraded = r.treatment_ungraded;
    f.baseline_faulted = r.baseline_faulted;
    f.treatment_faulted = r.treatment_faulted;
    f.error_code = r.setup_error.has_value() ? r.setup_error->code : std::string{};
    return f;
}

// Where a probe's result ends the attempt: nullopt means it passed and the attempt continues.
[[nodiscard]] inline std::optional<tier1_screen_outcome> tier1_probe_stop(FollowRateScreenResult const& r) {
    if (r.setup_error.has_value()) return tier1_screen_outcome::errored;
    if (r.invalid) return tier1_screen_outcome::inconclusive;
    if (r.pass != true) return tier1_screen_outcome::inert;
    return std::nullopt;
}

[[nodiscard]] inline tier1_screen_outcome tier1_harm_outcome(GrossHarmScreenResult const& r) {
    if (r.setup_error.has_value()) return tier1_screen_outcome::errored;
    if (r.flagged == true) return tier1_screen_outcome::harmful;
    if (!r.flagged.has_value()) return tier1_screen_outcome::inconclusive;
    return tier1_screen_outcome::cleared;
}

}  // namespace detail

// Runs one counted Tier-1 attempt (ADR-195 §3.0, E32) -- see the file-top comment for the order. The
// factories are shared by every screen the attempt runs (a scripted test factory can tell them apart by
// `TrialSlot::trial_id`, which each screen namespaces with its own probe or suite id).
template <rt::AppendLogStore Store, class InnerFactory, class SummarizerFactory>
[[nodiscard]] task<Tier1ScreenResult> run_tier1_screen(Tier1AttemptLog<Store>& log, InnerFactory make_inner,
                                                      SummarizerFactory make_summarizer, Tier1ScreenSpec declared) {
    Tier1ScreenResult result;
    Tier1ScreenSpec const spec = detail::tier1_with_lesson(std::move(declared));

    if (auto bad = detail::validate_tier1_spec(spec); bad.has_value()) {
        result.setup_error = std::move(bad);
        co_return result;
    }
    result.family = detail::tier1_family_of(spec);
    auto design = tier1_preregistration_json(spec);
    if (!design) {
        result.setup_error = design.error();
        co_return result;
    }
    auto prereg = detail::tier1_digest_of(*design);
    if (!prereg) {
        result.setup_error = prereg.error();
        co_return result;
    }
    result.preregistration_digest = *prereg;

    auto not_counted = [](std::string why) {
        return error{failure_class::transient, "the attempt could not be counted, so it was not run: " + why,
                     "eval.tier1_attempt_not_counted"};
    };
    auto started = log.begin_attempt(result.family, result.preregistration_digest, *design, spec.operator_id,
                                     spec.started_at);
    if (!started) {
        result.setup_error = not_counted(started.error().message);
        co_return result;
    }
    result.attempt_id = started->attempt_id;
    // Read the `started` record back before spending a single trial: a store that accepted the append but
    // lost it, or a log that has become unreadable, would otherwise run the whole attempt uncounted.
    {
        auto before = log.lineage_attempts(result.family.lineage);
        bool const counted = before.has_value() &&
                             std::any_of(before->begin(), before->end(), [&](Tier1AttemptRecord const& a) {
                                 return !a.unreadable && a.attempt_id == started->attempt_id;
                             });
        if (!counted) {
            result.setup_error = not_counted(before.has_value() ? "its started record did not read back"
                                                                : before.error().message);
            co_return result;
        }
    }

    std::vector<Tier1ProbeFigures> probe_figures;
    std::optional<Tier1HarmFigures> harm_figures;
    std::optional<tier1_screen_outcome> outcome;
    for (FollowRateProbeSpec const& probe : spec.probes) {
        FollowRateScreenResult r = co_await run_follow_rate_screen(std::ref(make_inner), std::ref(make_summarizer), probe);
        probe_figures.push_back(detail::tier1_figures_of(probe, r));
        std::optional<tier1_screen_outcome> const stop = detail::tier1_probe_stop(r);
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
        outcome = detail::tier1_harm_outcome(r);
        result.gross_harm = std::move(r);
    }

    // A failed `completed` write leaves the attempt in the log as started-but-not-completed: still
    // counted, still visible. This attempt's outcome stands; the error is reported beside it.
    if (auto done = log.complete_attempt(result.family, *started, *outcome, probe_figures, harm_figures); !done) {
        result.attempt_log_error = done.error();
    }

    // The verdict is returned either way; if the lineage's history cannot be read back, `history_complete` stays
    // false and the reason is reported, so the approver knows the attempt count is unknown.
    result.outcome = outcome;
    auto lineage = log.lineage_attempts(result.family.lineage);
    if (!lineage) {
        if (!result.attempt_log_error) result.attempt_log_error = lineage.error();  // the first failure is the cause
        co_return result;
    }
    result.lineage_attempts = std::move(*lineage);
    result.attempt_count = result.lineage_attempts.size();
    std::set<Digest> designs;
    for (Tier1AttemptRecord const& a : result.lineage_attempts) {
        if (a.unreadable) continue;
        designs.insert(a.preregistration);
        if (a.attempt_id == started->attempt_id) result.attempt_ordinal = a.ordinal;
    }
    result.distinct_preregistrations = designs.size();
    if (result.attempt_ordinal == 0) {
        // Our own `started` record no longer reads back: the log cannot be trusted to show the other attempts, so
        // no count read from it is reported either (a truncated read would otherwise show a low count).
        if (!result.attempt_log_error) {
            result.attempt_log_error = error{failure_class::fatal, "this attempt's own record is missing from the log",
                                             "eval.tier1_attempt_missing"};
        }
        result.lineage_attempts.clear();
        result.attempt_count = 0;
        result.distinct_preregistrations = 0;
        co_return result;
    }
    result.history_complete = true;
    co_return result;
}

namespace detail {

// ADR-191 §3.8 round 4: the `Tier1ScreenRecord` for `attempt_id`, from a lineage's attempts as read back from the log.
// The lesson digest, template version and delivery form come from the attempt's own stored design -- which
// `lineage_attempts` has already checked hashes to the digest its `started` record names -- so a caller cannot claim
// a form the screen did not run at. The outcome is the log's; an attempt whose figures were never written has none.
[[nodiscard]] inline Tier1ScreenRecord tier1_screen_record_of(std::vector<Tier1AttemptRecord> const& attempts,
                                                              std::string const& lineage,
                                                              std::string const& attempt_id, bool history_read) {
    Tier1ScreenRecord out;
    out.attempt_id = attempt_id;
    out.lineage = lineage;
    out.attempt_count = attempts.size();
    bool found = false;
    for (Tier1AttemptRecord const& a : attempts) {
        if (a.unreadable) {
            ++out.unreadable_records;
            continue;
        }
        if (a.attempt_id != attempt_id) {
            if (!a.completed) ++out.other_attempts_unfinished;
            if (a.outcome == tier1_screen_outcome::harmful) ++out.other_attempts_harmful;
            continue;
        }
        found = true;
        out.attempt_ordinal = a.ordinal;
        out.preregistration_digest = a.preregistration;
        out.outcome = a.completed ? a.outcome : std::nullopt;
        if (auto design = json::parse(a.preregistration_json); design && design->is_object()) {
            out.rendered_lesson_digest = tier1_read_string(*design, "rendered_lesson_digest").value_or("");
            out.template_version = tier1_read_string(*design, "template_version").value_or("");
            if (auto name = tier1_read_string(*design, "lesson_delivery"); name.has_value()) {
                out.delivery = lesson_delivery_from_name(*name);
            }
        }
    }
    out.history_complete = history_read && found && out.outcome.has_value();
    if (!found) out.attempt_count = 0;  // a read that lost this attempt cannot be trusted to count the others
    return out;
}

}  // namespace detail

// The record an approval rests on, from a finished attempt's result. Its history is the one the attempt read back
// when it finished; a host approving later should prefer `tier1_screen_record_from_log`, which also sees attempts
// started since. An attempt that never ran (a `setup_error`) yields a record with no attempt id: "no screen".
[[nodiscard]] inline Tier1ScreenRecord tier1_screen_record(Tier1ScreenResult const& r) {
    if (r.attempt_id.empty() || !r.outcome.has_value()) return {};
    Tier1ScreenRecord out = detail::tier1_screen_record_of(r.lineage_attempts, r.family.lineage, r.attempt_id,
                                                           r.history_complete && !r.attempt_log_error.has_value());
    return out;
}

// The same record, read fresh from the lineage's log -- what an approver should be shown at approval time.
template <rt::AppendLogStore Store>
[[nodiscard]] inline Tier1ScreenRecord tier1_screen_record_from_log(Tier1AttemptLog<Store> const& log,
                                                                    std::string const& lineage,
                                                                    std::string const& attempt_id) {
    if (attempt_id.empty()) return {};
    auto attempts = log.lineage_attempts(lineage);
    if (!attempts) {
        Tier1ScreenRecord out;
        out.attempt_id = attempt_id;
        out.lineage = lineage;
        return out;  // history_complete = false, no outcome, no form: every approval check objects
    }
    return detail::tier1_screen_record_of(*attempts, lineage, attempt_id, true);
}

}  // namespace agentengine::eval
