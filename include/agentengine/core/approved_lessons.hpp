#pragma once
// Implements decisions/ADR-183-approved-lesson-delivery.md: the host-owned record of which lesson texts a human
// operator approved, verbatim, for which principal.
//
// A lesson is model-derived text and stays so: tainted, fenced, `content_origin::external`. What this registry adds
// is one host-held fact per text: "a human approved exactly these bytes for this principal". `AgentSession` -- the
// only reader that matters -- re-verifies every tainted system item against it when it builds a request, and only
// a match gets `ContentItem::approval` (the fence then names the block an approved lesson and its preamble says it
// may be followed). Membership is decided by the exact text itself, compared byte for byte at every lookup; nothing
// a provider, a plugin or a stored item says about itself counts. (No digest: that would make every `AgentSession`
// user link the worktree digest library -- found by the full build, where three examples stopped linking.)
//
// Host code only (I3): nothing here accepts model output as a decision. A real approval must name its approver and
// the E31 acknowledgement it rests on (`eval::approve_lesson` is the path that checks that acknowledgement first);
// an evaluation's stand-in approval is a separate call and is marked `simulated` wherever it is reported. The
// engine does not persist the registry: a host loads it from its own storage, and a session without one delivers
// every lesson fenced as before (ADR-070 §4 property 2).

#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine {

// Who approved a lesson text, and on the strength of what (I4).
struct LessonApproval {  // ae-naming-lint: allow LessonApproval — ADR-183
    std::string approver_id;
    std::string approved_at;      // host timestamp (ISO-8601)
    std::string acknowledgement;  // the E31 digest the approver acknowledged (the rendered item's)
    bool simulated = false;       // an evaluation's stand-in for an approval (a Tier-1 screen's treatment arm)
};

struct ApprovedLessonMatch {  // ae-naming-lint: allow ApprovedLessonMatch — ADR-183
    std::string approval_id;      // what `ContentItem::approval` records: the acknowledgement, or `simulated:<trial>`
    LessonApproval approval;
};

class ApprovedLessonRegistry {  // ae-naming-lint: allow ApprovedLessonRegistry — ADR-183
public:
    // Records that a human approved exactly `content` for `scope` (the principal it may reach -- an approval for
    // one tenant never reaches another). Replaces an earlier approval of the same text in the same scope.
    [[nodiscard]] result<void> approve(std::string_view scope, std::string_view content, LessonApproval approval) {
        if (approval.approver_id.empty() || approval.acknowledgement.empty()) {
            return std::unexpected(error{failure_class::contract,
                                         "an approval must name its approver and the acknowledgement it rests on (I4)",
                                         "memory.approval_unattributed"});
        }
        approval.simulated = false;
        return put(scope, content, std::move(approval));
    }

    // An evaluation's stand-in approval: the screen measures a lesson delivered as if approved (ADR-183 §3.8). It is
    // recorded as simulated and names the trial, never a person.
    [[nodiscard]] result<void> approve_simulated(std::string_view scope, std::string_view content,
                                                 std::string const& trial_id) {
        return put(scope, content, LessonApproval{"simulated:" + trial_id, "", "", true});
    }

    // Revokes an approval. It takes effect at the next request any session builds (ADR-183 §3.3).
    void revoke(std::string_view scope, std::string_view content) {
        std::unique_lock lock(mutex_);
        approved_.erase(key(scope, content));
    }

    // The approval for exactly these bytes in this scope, if any.
    [[nodiscard]] std::optional<ApprovedLessonMatch> find(std::string_view scope, std::string_view content) const {
        if (scope.empty() || content.empty()) return std::nullopt;
        std::shared_lock lock(mutex_);
        auto it = approved_.find(key(scope, content));
        if (it == approved_.end()) return std::nullopt;
        LessonApproval const& a = it->second;
        return ApprovedLessonMatch{a.simulated ? a.approver_id : a.acknowledgement, a};
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return approved_.size();
    }

private:
    [[nodiscard]] result<void> put(std::string_view scope, std::string_view content, LessonApproval approval) {
        if (scope.empty()) {
            return std::unexpected(error{failure_class::contract, "an approval must name the principal it applies to",
                                         "memory.approval_unscoped"});
        }
        if (content.empty()) {
            return std::unexpected(error{failure_class::contract, "an approved lesson cannot be empty",
                                         "memory.approval_empty"});
        }
        std::unique_lock lock(mutex_);
        approved_[key(scope, content)] = std::move(approval);
        return {};
    }

    // The scope, a unit separator, then the whole text. A principal id containing the separator could only ever
    // collide with another scope's key if its tail also equalled a lesson text -- scopes are host-assigned ids.
    [[nodiscard]] static std::string key(std::string_view scope, std::string_view content) {
        std::string k(scope);
        k += '\x1f';
        k += content;
        return k;
    }

    mutable std::shared_mutex mutex_;
    std::map<std::string, LessonApproval> approved_;
};

// The text an approval is checked against, for an item `MemoryProvider` rendered: its confidence label (029 §6,
// `⟦memory:...⟧ `) is dropped, since that label is the provider's rendering, not the approved lesson. Any other
// text is checked as is. A lesson value can never itself begin with the label: `lesson_value_passes_validator`
// refuses the bracket glyphs (ADR-183).
[[nodiscard]] inline std::string_view approved_lesson_candidate_text(std::string_view text) noexcept {
    constexpr std::string_view open = "\xE2\x9F\xA6memory:";  // memory_detail::memory_label_open()
    constexpr std::string_view close = "\xE2\x9F\xA7";        // memory_detail::memory_label_close()
    if (!text.starts_with(open)) return text;
    std::size_t const end = text.find(close, open.size());
    if (end == std::string_view::npos) return text;
    std::string_view rest = text.substr(end + close.size());
    if (rest.starts_with(' ')) rest.remove_prefix(1);
    return rest;
}

}  // namespace agentengine
