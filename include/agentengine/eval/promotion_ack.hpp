#pragma once
// Implements ADR-181 E31 (round-4 fix for the FATAL "the approver acknowledges an excerpt, never
// the bytes the model reads" finding, §3.0 item 5 / §3.9): the approver's acknowledgement is bound
// to a digest of the VERBATIM rendered `MemoryItem`, and the promotion path re-renders and refuses
// the write if the recomputed digest differs from the acknowledged one. An ack that names no digest,
// or a promotion that writes without recomputing one, is exactly what E31 plants as a mutant.

#include <string>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/eval/lesson_candidate.hpp"

namespace agentengine::eval {

// `digest`/`template_version` are what the approver actually saw and acknowledged (the verbatim
// rendered `content`/`tags`/`salience`, per ADR-181 §3.0 item 5); `approver_id`/`acknowledged_at`
// are the I4 attribution ADR-181 requires recorded alongside it.
struct PromotionAck {  // ae-naming-lint: allow PromotionAck — ADR-181 §3.0 item 5 / E31
    Digest      digest;
    std::string template_version;
    std::string approver_id;
    std::string acknowledged_at;  // ISO-8601, host-supplied — see MemoryItem::expires_at's own note
};

// Computes the ack a human approver would produce after being shown `rendered`'s verbatim bytes —
// the harness calls this once, right before presenting the fenced excerpt AND the rendered item to
// the approver, so the digest it records is provably the digest of what was actually shown.
[[nodiscard]] inline result<PromotionAck> acknowledge_rendered_lesson(MemoryItem const& rendered,
                                                                        std::string template_version,
                                                                        std::string approver_id,
                                                                        std::string acknowledged_at) {
    auto digest = rendered_lesson_digest(rendered, template_version);
    if (!digest) return std::unexpected(digest.error());

    PromotionAck ack{};
    ack.digest            = std::move(*digest);
    ack.template_version  = std::move(template_version);
    ack.approver_id       = std::move(approver_id);
    ack.acknowledged_at   = std::move(acknowledged_at);
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

// ADR-183: registers a lesson a human approved, for `scope` (the principal whose sessions may receive it), so a
// session given `registry` delivers it as an approved lesson (still tainted and fenced; the fence's preamble says
// it may be followed). It goes through E31 first: the candidate is re-rendered and its digest must equal what the
// approver acknowledged, or nothing is registered. Returns the rendered item -- the exact bytes that were approved,
// to be written to memory by the promotion.
[[nodiscard]] inline result<MemoryItem> approve_lesson(ApprovedLessonRegistry& registry, std::string_view scope,
                                                       LessonCandidate const& candidate, PromotionAck const& ack,
                                                       float salience) {
    auto rendered = verify_and_render_acknowledged_lesson(candidate, ack, salience);
    if (!rendered) return std::unexpected(rendered.error());
    auto approved =
        registry.approve(scope, rendered->content, LessonApproval{ack.approver_id, ack.acknowledged_at, ack.digest});
    if (!approved) return std::unexpected(approved.error());
    return rendered;
}

}  // namespace agentengine::eval
