#pragma once
// Also: ADR-191 `approve_lesson` and ADR-192 `promote_lesson_automatically` (no acknowledgement, by design), both
// bound to the Tier-1 screen they rest on since ADR-191's round-4 red team (§3.8, lesson_screen_record.hpp).
// Implements ADR-195 E31 (round-4 fix for the FATAL "the approver acknowledges an excerpt, never
// the bytes the model reads" finding, §3.0 item 5 / §3.9): the approver's acknowledgement is bound
// to a digest of the VERBATIM rendered `MemoryItem`, and the promotion path re-renders and refuses
// the write if the recomputed digest differs from the acknowledged one. An ack that names no digest,
// or a promotion that writes without recomputing one, is exactly what E31 plants as a mutant.
//
// ADR-191 round 4 (Finding B): both approval paths used to bind only the rendered bytes, so a lesson whose lineage
// held harmful attempts -- or that was never screened -- approved exactly like a cleared one, although ADR-191 §5
// names "the Tier-1 screen's figures and attempt history shown to the approver" as the defence. Now:
//   - `PromotionAck::screen` records the screen the approver was shown with the bytes;
//   - `approve_lesson` refuses (`policy`) whenever `tier1_screen_objections` finds anything -- no screen, another
//     lesson's screen, an outcome other than `cleared`, an incomplete history, a harmful / unfinished / unreadable
//     attempt elsewhere in the lineage, or a screen at another delivery form than the one it will ship at -- unless a
//     `ScreenOverride` names the human who set the screen aside; the override is recorded on the approval itself
//     (so every delivery's audit event names it) and returned;
//   - `promote_lesson_automatically` takes no override at all (automatic = no human to own one) and requires a
//     screen with no objection at the form it will ship at.

#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/eval/lesson_screen_record.hpp"

namespace agentengine::eval {

// `digest`/`template_version` are what the approver actually saw and acknowledged (the verbatim
// rendered `content`/`tags`/`salience`, per ADR-195 §3.0 item 5); `approver_id`/`acknowledged_at`
// are the I4 attribution ADR-195 requires recorded alongside it. `screen` is the Tier-1 attempt the approver was
// shown beside them (ADR-191 §3.8 round 4) -- empty when none was.
struct PromotionAck {  // ae-naming-lint: allow PromotionAck — ADR-195 §3.0 item 5 / E31
    Digest      digest;
    std::string template_version;
    std::string approver_id;
    std::string acknowledged_at;  // ISO-8601, host-supplied — see MemoryItem::expires_at's own note
    Tier1ScreenRecord screen;
};

// A named human setting aside the screen's objections for one approval (ADR-191 §3.8 round 4). Host-supplied (I3);
// `overridden_by` is required and may not be an engine-reserved `automatic:`/`simulated:` id.
struct ScreenOverride {  // ae-naming-lint: allow ScreenOverride — ADR-191 §3.8
    std::string overridden_by;
    std::string overridden_at;  // ISO-8601, host-supplied
    std::string reason;
};

// What an approval registered: the exact rendered item (to be written to memory by the promotion), the form it was
// approved to ship at, the screen it rests on, and -- when a human overrode the screen -- who, and what they set aside.
struct PromotedLesson {  // ae-naming-lint: allow PromotedLesson — ADR-191 §3.8
    MemoryItem item;
    lesson_delivery delivery = lesson_delivery::approved;
    Tier1ScreenRecord screen;
    std::optional<ScreenOverride> screen_override;
    std::vector<ScreenObjection> overridden;  // empty unless `screen_override` is set
};

// Computes the ack a human approver would produce after being shown `rendered`'s verbatim bytes —
// the harness calls this once, right before presenting the fenced excerpt AND the rendered item to
// the approver, so the digest it records is provably the digest of what was actually shown. `screen` is the Tier-1
// record shown with it (`tier1_screen_record_from_log`); omitted, the ack still verifies the bytes (E31) but
// `approve_lesson` will refuse it without an override.
[[nodiscard]] inline result<PromotionAck> acknowledge_rendered_lesson(MemoryItem const& rendered,
                                                                        std::string template_version,
                                                                        std::string approver_id,
                                                                        std::string acknowledged_at,
                                                                        Tier1ScreenRecord screen = {}) {
    auto digest = rendered_lesson_digest(rendered, template_version);
    if (!digest) return std::unexpected(digest.error());

    PromotionAck ack{};
    ack.digest            = std::move(*digest);
    ack.template_version  = std::move(template_version);
    ack.approver_id       = std::move(approver_id);
    ack.acknowledged_at   = std::move(acknowledged_at);
    ack.screen            = std::move(screen);
    return ack;
}

// The promotion-time check (E31): re-renders `candidate` at the acknowledged `template_version` and
// `salience`, recomputes the digest, and refuses (a `policy`-class error, matching this project's
// approval-denied convention, 001 §6) unless it EXACTLY matches `ack.digest`. A caller that renders
// once, acknowledges, and then writes the same in-memory `MemoryItem` without calling this — or that
// mutates `content`/`tags`/`salience` between the ack and the write — is exactly what E31's mutant
// (promotion writes without recomputing) is meant to catch.
[[nodiscard]] inline result<MemoryItem> verify_and_render_acknowledged_lesson(LessonCandidate const& candidate,
                                                                                PromotionAck const& ack,
                                                                                float salience) {
    auto rendered = render_lesson(candidate, ack.template_version, salience);
    if (!rendered) return std::unexpected(rendered.error());

    auto digest = rendered_lesson_digest(*rendered, ack.template_version);
    if (!digest) return std::unexpected(digest.error());

    if (*digest != ack.digest) {
        return std::unexpected(error{failure_class::policy,
                                      "promotion refused: rendered lesson no longer matches the "
                                      "acknowledged digest",
                                      "eval.ack_digest_mismatch"});
    }
    return *rendered;
}

namespace detail {

[[nodiscard]] inline bool promotion_reserved_id(std::string_view id) noexcept {
    while (!id.empty() && (id.front() == ' ' || id.front() == '\t')) id.remove_prefix(1);
    auto starts_ci = [id](std::string_view prefix) {
        if (id.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            char c = id[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i]) return false;
        }
        return true;
    };
    return starts_ci("automatic:") || starts_ci("simulated:");
}

[[nodiscard]] inline error screen_refusal(std::vector<ScreenObjection> const& objections, char const* who) {
    std::string msg = std::string(who) + " refused: the Tier-1 screen does not support shipping this lesson";
    for (ScreenObjection const& o : objections) msg += "; " + o.detail;
    return error{failure_class::policy, std::move(msg), objections.front().code};
}

[[nodiscard]] inline std::string objection_codes(std::vector<ScreenObjection> const& objections) {
    std::string out;
    for (ScreenObjection const& o : objections) {
        if (!out.empty()) out += ",";
        out += o.code;
    }
    return out;
}

}  // namespace detail

// ADR-191: registers a lesson a human approved, for `scope` (the principal whose sessions may receive it), so a
// session given `registry` delivers it as an approved lesson in form `ships_as` (`approved`, `approved_instructions`
// or `approved_fence_off` -- the host states how its sessions are configured; `shipped_lesson_delivery` computes it).
// It goes through E31 first: the candidate is re-rendered and its digest must equal what the approver acknowledged.
// Then the screen (round 4): any `tier1_screen_objections` refuses the approval (the first objection's code) unless
// `screen_override` names the human who set them aside; the override then rides on the registered approval's
// `approver_id` ("<approver> [screen override by <who>: <codes>]"), so the session's audit event for every delivery
// names it, and is returned. Nothing is registered on any refusal.
[[nodiscard]] inline result<PromotedLesson> approve_lesson(ApprovedLessonRegistry& registry, std::string_view scope,
                                                           LessonCandidate const& candidate, PromotionAck const& ack,
                                                           float salience, lesson_delivery ships_as,
                                                           std::optional<ScreenOverride> screen_override = std::nullopt) {
    if (ships_as != lesson_delivery::approved && ships_as != lesson_delivery::approved_instructions &&
        ships_as != lesson_delivery::approved_fence_off) {
        return std::unexpected(error{failure_class::contract,
                                     "a human approval ships as approved, approved_instructions or approved_fence_off, "
                                     "not " + std::string(lesson_delivery_name(ships_as)),
                                     "eval.delivery_not_human_approval"});
    }
    auto rendered = verify_and_render_acknowledged_lesson(candidate, ack, salience);
    if (!rendered) return std::unexpected(rendered.error());

    PromotedLesson out;
    out.delivery = ships_as;
    out.screen = ack.screen;
    std::string approver = ack.approver_id;
    auto objections = tier1_screen_objections(ack.screen, ack.digest, ack.template_version, ships_as);
    if (!objections.empty()) {
        if (!screen_override.has_value()) return std::unexpected(detail::screen_refusal(objections, "approval"));
        if (screen_override->overridden_by.empty() || detail::promotion_reserved_id(screen_override->overridden_by)) {
            return std::unexpected(error{failure_class::contract,
                                         "a screen override must name the human who made it (I4), not an "
                                         "automatic:/simulated: id",
                                         "eval.screen_override_unattributed"});
        }
        approver += " [screen override by " + screen_override->overridden_by + ": " +
                    detail::objection_codes(objections) + "]";
        out.screen_override = std::move(screen_override);
        out.overridden = std::move(objections);
    }
    auto approved = registry.approve(scope, rendered->content, LessonApproval{approver, ack.acknowledged_at, ack.digest});
    if (!approved) return std::unexpected(approved.error());
    out.item = std::move(*rendered);
    return out;
}

// ADR-192 (unattended mode): the same promotion with no human -- for a full-automation host whose own automated
// reviewer decides. The lesson is still rendered by the fixed templates (and so still passes the structural checks
// `render_lesson` applies), and registered as `automatic:<reviewer_id>`. There is no acknowledgement to verify; the
// host calling this is the decision, and the audit names the reviewer. Round 4: it also requires `screen` -- a Tier-1
// attempt of exactly these rendered bytes, at exactly `ships_as` (`approved_automatic`, `approved_instructions` or
// `approved_fence_off`), `cleared`, with a complete, clean lineage -- and it takes NO override: an override is a
// human's call, and this path has no human. Returns the rendered item to write to memory.
[[nodiscard]] inline result<PromotedLesson> promote_lesson_automatically(ApprovedLessonRegistry& registry,
                                                                         std::string_view scope,
                                                                         LessonCandidate const& candidate,
                                                                         std::string_view template_version,
                                                                         float salience, std::string const& reviewer_id,
                                                                         Tier1ScreenRecord const& screen,
                                                                         lesson_delivery ships_as,
                                                                         std::string approved_at = {}) {
    if (ships_as != lesson_delivery::approved_automatic && ships_as != lesson_delivery::approved_instructions &&
        ships_as != lesson_delivery::approved_fence_off) {
        return std::unexpected(error{failure_class::contract,
                                     "an automatic approval ships as approved_automatic, approved_instructions or "
                                     "approved_fence_off, not " + std::string(lesson_delivery_name(ships_as)),
                                     "eval.delivery_not_automatic_approval"});
    }
    auto rendered = render_lesson(candidate, template_version, salience);
    if (!rendered) return std::unexpected(rendered.error());
    auto digest = rendered_lesson_digest(*rendered, template_version);
    if (!digest) return std::unexpected(digest.error());
    if (auto objections = tier1_screen_objections(screen, *digest, template_version, ships_as); !objections.empty()) {
        return std::unexpected(detail::screen_refusal(objections, "automatic promotion"));
    }
    auto approved = registry.approve_automatic(scope, rendered->content, reviewer_id, std::move(approved_at));
    if (!approved) return std::unexpected(approved.error());
    PromotedLesson out;
    out.item = std::move(*rendered);
    out.delivery = ships_as;
    out.screen = screen;
    return out;
}

}  // namespace agentengine::eval
