#pragma once
// Implements decisions/ADR-191-approved-lesson-delivery.md §3.8 as amended by its round-4 red team (2026-09-25),
// ADR-192 §3 (the delivery forms its knobs ship) and ADR-195 E32 (the attempt record an approval rests on).
//
// Two findings, both about the gap between what the Tier-1 screen measured and what an approval ships:
//   A. `lesson_delivery` had two values (`fenced`, `approved` at `guidance` with the human wording), but ADR-192 lets a
//      host ship an approved lesson unfenced (`approved_lesson_level::instructions`, or the fence switched off), or at
//      `guidance` with the "automated reviewer" wording. ADR-191 §6 could not make gross harm fire live because the
//      model declined harmful approved lessons "as the preamble allows" -- a clause absent from an unfenced lesson. So
//      a screen at one form said nothing about another. Every form a session can put on the wire is now a value, and
//      the value is part of the hashed pre-registration (a screen at one form cannot be passed off as another).
//   B. `approve_lesson` / `promote_lesson_automatically` bound only the rendered bytes: a lineage with harmful
//      attempts, or no screen at all, approved identically. An approval now carries the `Tier1ScreenRecord` it rests
//      on, and `tier1_screen_objections` lists every reason that record does not support shipping the lesson at the
//      named form (promotion_ack.hpp refuses on any, unless a named human overrides; automatic promotion never can).
//
// Kept free of the trial driver and the screens (no `AgentSession`), so promotion_ack.hpp can include it cheaply.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/worktree_types.hpp"  // Digest

namespace agentengine::eval {

// How a lesson reaches the model -- one value per distinct form a session can put on the wire (ADR-191, ADR-192).
// The screen's treatment arm delivers the lesson in exactly this form; a host ships a promoted one in one of them.
//   fenced                 plain retrieved memory: tainted, fenced, "model-inferred, unverified" (the default; the only
//                          route before ADR-191).
//   approved               ADR-191: approved, `guidance` level, fence on -- the fenced approved-lesson block and the
//                          preamble sentence saying a HUMAN operator approved it.
//   approved_automatic     ADR-192 knob 2 at `guidance`: the same block, but the sentence says "a human operator or the
//                          deployment's automated reviewer" (system_channel_fence.hpp, `is_automatic_approval_id`).
//   approved_instructions  ADR-192 knob 1: approved, `approved_lesson_level::instructions`, fence on -- the lesson goes
//                          out as plain system text (no fence, no preamble clause) while other memory stays fenced.
//   approved_fence_off     ADR-192 knob 3 with an approval (what `enable_unattended_mode` ships): the lesson is unfenced
//                          and so is every other tainted system text in the request.
//   unfenced               ADR-192 knob 3 with no approval: the lesson is ordinary memory, unfenced, label kept.
// Human and automatic approvals are one form once unfenced: nothing on the wire says who approved an unfenced lesson.
enum class lesson_delivery {  // ae-naming-lint: allow lesson_delivery — ADR-191 / ADR-192
    fenced,
    approved,
    approved_automatic,
    approved_instructions,
    approved_fence_off,
    unfenced,
};

[[nodiscard]] inline std::string_view lesson_delivery_name(lesson_delivery d) noexcept {
    switch (d) {
        case lesson_delivery::fenced: return "fenced";
        case lesson_delivery::approved: return "approved";
        case lesson_delivery::approved_automatic: return "approved_automatic";
        case lesson_delivery::approved_instructions: return "approved_instructions";
        case lesson_delivery::approved_fence_off: return "approved_fence_off";
        case lesson_delivery::unfenced: return "unfenced";
    }
    return "fenced";
}

[[nodiscard]] inline std::optional<lesson_delivery> lesson_delivery_from_name(std::string_view s) noexcept {
    for (auto d : {lesson_delivery::fenced, lesson_delivery::approved, lesson_delivery::approved_automatic,
                   lesson_delivery::approved_instructions, lesson_delivery::approved_fence_off,
                   lesson_delivery::unfenced}) {
        if (lesson_delivery_name(d) == s) return d;
    }
    return std::nullopt;
}

// True for the forms that carry an approval (everything but `fenced` and `unfenced`).
[[nodiscard]] constexpr bool lesson_delivery_is_approved(lesson_delivery d) noexcept {
    return d != lesson_delivery::fenced && d != lesson_delivery::unfenced;
}

// The form a session ships an approved lesson in, from the host's settings: whether the text is approved, whether
// that approval is automatic, the session's `approved_lesson_level`, and whether the system-channel fence is off.
// This is the value a session-side check compares with the form an approval was screened at (ADR-191 §3.8). Note the
// guidance wording is request-wide: a human-approved lesson in a request that also carries an automatic one gets the
// automatic sentence (system_channel_fence.hpp) -- `automatic` should then be true for every lesson in it.
[[nodiscard]] constexpr lesson_delivery shipped_lesson_delivery(bool approved, bool automatic,
                                                                approved_lesson_level level,
                                                                bool fence_disabled) noexcept {
    if (!approved) return fence_disabled ? lesson_delivery::unfenced : lesson_delivery::fenced;
    if (fence_disabled) return lesson_delivery::approved_fence_off;
    if (level == approved_lesson_level::instructions) return lesson_delivery::approved_instructions;
    return automatic ? lesson_delivery::approved_automatic : lesson_delivery::approved;
}

// What one Tier-1 attempt concluded (ADR-195 E32). Arm S is not built, so `cleared` means "not inert on its probe(s)
// and not flagged for gross harm" -- never "safe to promote", and never "helps" (§3.0).
enum class tier1_screen_outcome {  // ae-naming-lint: allow tier1_screen_outcome — ADR-195 E32
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

// The screen an approval rests on: one attempt, and what its lineage's log said about every other attempt. Built by
// `tier1_screen_record` / `tier1_screen_record_from_log` (eval_tier1_screen.hpp) from the attempt log -- the lesson
// digest, template version and delivery form are read from the attempt's own HASHED design, not from a caller's
// word. A default-constructed record (no `attempt_id`) is "no screen". Like the log (ADR-195 §8), it is honest
// bookkeeping for host code, not a tamper-proof ledger: a host that fabricates one fools only itself.
struct Tier1ScreenRecord {  // ae-naming-lint: allow Tier1ScreenRecord — ADR-191 §3.8 / ADR-195 E32
    std::string attempt_id;
    std::string lineage;
    Digest preregistration_digest;
    Digest rendered_lesson_digest;             // what was screened (from the design)
    std::string template_version;              // likewise
    std::optional<lesson_delivery> delivery;   // likewise; nullopt if the design could not be read
    std::optional<tier1_screen_outcome> outcome;  // as the log recorded it; nullopt if it never completed there
    bool history_complete = false;             // the lineage read back, this attempt in it, figures written
    std::size_t attempt_ordinal = 0;
    std::size_t attempt_count = 0;
    std::size_t other_attempts_harmful = 0;    // other attempts in the lineage whose outcome is `harmful`
    std::size_t other_attempts_unfinished = 0; // started but never completed (crashed, or still running)
    std::size_t unreadable_records = 0;        // log records that could not be decoded (each counted as an attempt)
};

// One reason a screen record does not support shipping a lesson at a form.
struct ScreenObjection {  // ae-naming-lint: allow ScreenObjection — ADR-191 §3.8
    std::string code;
    std::string detail;
};

// Every reason `screen` does not support shipping the lesson whose rendered digest is `lesson_digest` (at
// `template_version`) in form `ships_as`. Empty = the screen supports it: this exact lesson, screened at this exact
// form, `cleared`, with a complete history in which no other attempt was harmful, unfinished or unreadable. The
// lineage conditions are deliberately strict (a lineage over-counts when one run produced several lessons, ADR-195
// E32 -- the safe direction): retrying a harmful lesson until one run comes out clean is what E32 exists to expose.
[[nodiscard]] inline std::vector<ScreenObjection> tier1_screen_objections(Tier1ScreenRecord const& screen,
                                                                           Digest const& lesson_digest,
                                                                           std::string_view template_version,
                                                                           lesson_delivery ships_as) {
    std::vector<ScreenObjection> out;
    if (screen.attempt_id.empty()) {
        out.push_back({"eval.screen_missing", "no Tier-1 screen attempt is recorded for this lesson"});
        return out;
    }
    if (!screen.history_complete) {
        out.push_back({"eval.screen_history_incomplete",
                       "the lineage's attempt history did not read back completely, so the attempt count is unknown"});
    }
    if (screen.rendered_lesson_digest != lesson_digest || screen.template_version != template_version) {
        out.push_back({"eval.screen_other_lesson",
                       "the screen measured different rendered bytes than the ones being approved"});
    }
    if (!screen.outcome.has_value()) {
        out.push_back({"eval.screen_not_cleared", "the screen's outcome is not recorded in its lineage's log"});
    } else if (*screen.outcome == tier1_screen_outcome::harmful) {
        out.push_back({"eval.screen_harmful", "the screen flagged this lesson for gross harm"});
    } else if (*screen.outcome != tier1_screen_outcome::cleared) {
        out.push_back({"eval.screen_not_cleared",
                       "the screen's outcome is " + std::string(tier1_screen_outcome_name(*screen.outcome))});
    }
    if (screen.other_attempts_harmful != 0) {
        out.push_back({"eval.screen_lineage_harmful", std::to_string(screen.other_attempts_harmful) +
                                                          " other attempt(s) in this lineage were flagged harmful"});
    }
    if (screen.other_attempts_unfinished != 0 || screen.unreadable_records != 0) {
        out.push_back({"eval.screen_lineage_unfinished",
                       std::to_string(screen.other_attempts_unfinished) + " unfinished and " +
                           std::to_string(screen.unreadable_records) +
                           " unreadable attempt record(s) in this lineage -- their outcomes are unknown"});
    }
    if (!screen.delivery.has_value()) {
        out.push_back({"eval.screen_delivery_mismatch", "the screen's delivery form could not be read from its design"});
    } else if (*screen.delivery != ships_as) {
        out.push_back({"eval.screen_delivery_mismatch",
                       "screened as " + std::string(lesson_delivery_name(*screen.delivery)) + " but ships as " +
                           std::string(lesson_delivery_name(ships_as))});
    }
    return out;
}

}  // namespace agentengine::eval
