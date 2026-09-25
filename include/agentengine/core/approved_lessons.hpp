#pragma once
// Implements decisions/ADR-191-approved-lesson-delivery.md: the host-owned record of which lesson texts a human
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
// Host code only (I3): nothing here accepts model output as a decision. An approval must name its approver -- that is
// what the audit event names (I4). The E31 acknowledgement it rests on is recorded when there is one
// (`eval::approve_lesson` checks it first), but not required: the engine cannot verify a host-supplied string, so
// demanding one only added friction (ADR-191 proportionality review). An evaluation's stand-in approval is a
// separate call and is marked `simulated` wherever it is reported. The
// engine does not persist the registry: a host loads it from its own storage, and a session without one delivers
// every lesson fenced as before (ADR-070 §4 property 2).
//
// ADR-192 (unattended mode) adds two host opt-ins: an approval with no human in the loop (`approve_automatic`, marked
// `automatic` wherever it is reported), and the level a session delivers approved lessons at
// (`approved_lesson_level`, set on `AgentSession::set_approved_lessons`).

#include <map>
#include <mutex>
#include <tuple>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

// Who approved a lesson text, and on the strength of what (I4).
struct LessonApproval {  // ae-naming-lint: allow LessonApproval — ADR-191
    std::string approver_id;
    std::string approved_at;      // host timestamp (ISO-8601)
    std::string acknowledgement;  // the E31 digest the approver acknowledged, when there is one (optional)
    bool simulated = false;       // an evaluation's stand-in for an approval (a Tier-1 screen's treatment arm)
    bool automatic = false;       // ADR-192: approved with no human -- by a host-named automated reviewer
};

// ADR-192: how a session delivers an approved lesson. `guidance` is ADR-191's route: fenced, tagged
// `approved-lesson:<code>`, and the preamble says it may be followed unless the user says otherwise. `instructions`
// sends it as plain system text, unfenced -- as the host's own instructions. Either way the item stays tainted and
// grants nothing; only how the model is told to read it changes.
enum class approved_lesson_level {  // ae-naming-lint: allow approved_lesson_level — ADR-192
    guidance,
    instructions,
};

[[nodiscard]] constexpr std::string_view approved_lesson_level_name(approved_lesson_level level) noexcept {
    return level == approved_lesson_level::instructions ? "instructions" : "guidance";
}

// Round-3 red team: who an approval may reach -- the tenant AND the principal. It was the principal id alone, so a
// lesson approved for "alice" in one tenant was delivered as approved to "alice" in another (018 §6 treats cross-tenant
// id collisions as real), and through ADR-193's `share_lessons` its text was injected there too. A bare principal id
// converts to a scope with an empty tenant -- a single-tenant deployment -- which a principal carrying a tenant never
// matches (fails safe: its lessons are simply fenced).
struct LessonScope {  // ae-naming-lint: allow LessonScope — ADR-191
    std::string tenant_id{};
    std::string principal_id{};

    LessonScope() = default;
    LessonScope(std::string_view principal) : principal_id(principal) {}     // implicit: the single-tenant form
    LessonScope(char const* principal) : principal_id(principal) {}          // implicit: the single-tenant form
    LessonScope(std::string const& principal) : principal_id(principal) {}   // implicit: the single-tenant form
    LessonScope(std::string_view tenant, std::string_view principal) : tenant_id(tenant), principal_id(principal) {}

    friend bool operator==(LessonScope const&, LessonScope const&) = default;
};

struct ApprovedLessonMatch {  // ae-naming-lint: allow ApprovedLessonMatch — ADR-191
    std::string approval_id;      // what `ContentItem::approval` records: the acknowledgement, else the approver
    LessonApproval approval;
};

// Round 3: an id an audit line names -- an approver, a reviewer, ADR-192's operator -- must be visible and single-line:
// a blank id named nobody, and an embedded newline could forge a further audit line.
[[nodiscard]] inline bool id_has_control_char(std::string_view id) noexcept {
    for (char const ch : id) {
        auto const c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}
[[nodiscard]] inline bool is_attributable_id(std::string_view id) noexcept {
    if (id_has_control_char(id)) return false;
    for (char const ch : id) {
        if (ch != ' ') return true;
    }
    return false;
}

class ApprovedLessonRegistry {  // ae-naming-lint: allow ApprovedLessonRegistry — ADR-191
public:
    // Records that a human approved exactly `content` for `scope` (the tenant and principal it may reach -- an
    // approval for one tenant never reaches another). Replaces an earlier approval of the same text in the same scope.
    [[nodiscard]] result<void> approve(LessonScope const& scope, std::string_view content, LessonApproval approval) {
        if (!is_attributable_id(approval.approver_id)) {
            return std::unexpected(error{failure_class::contract,
                                         "an approval must name its approver: non-blank, no control characters (I4)",
                                         "memory.approval_unattributed"});
        }
        if (id_has_control_char(approval.acknowledgement)) {
            return std::unexpected(error{failure_class::contract, "an acknowledgement may not hold control characters",
                                         "memory.approval_unattributed"});
        }
        // ADR-192 red team: the id prefixes the engine writes for non-human approvals are reserved, so a recording's
        // approval id always tells a human approval from an automatic or simulated one. Round 2: checked on both
        // fields that can become the id (`find` uses the acknowledgement when there is one), ignoring case and
        // leading whitespace. Round 3: anywhere in the id, not only at its start -- a leading newline, NBSP or
        // zero-width space defeated the start-only check.
        if (uses_reserved_prefix(approval.approver_id) || uses_reserved_prefix(approval.acknowledgement)) {
            return std::unexpected(error{failure_class::contract,
                                         "a human approval may not use the reserved automatic:/simulated: prefix",
                                         "memory.approval_reserved_id"});
        }
        approval.simulated = false;
        approval.automatic = false;
        return put(scope, content, std::move(approval), /*may_replace_human=*/true);
    }

    // ADR-192: an approval with no human in the loop -- a host that runs a full-automation system lets its own
    // automated reviewer (the post-run review, a rule, a script) promote lessons. The host names the reviewer, which
    // is what the audit names; it is recorded as `automatic` and its id reads `automatic:<reviewer>`. Calling this IS
    // the opt-in: a host that never calls it has only human (or simulated) approvals. Round 3: it never replaces a
    // human's approval of the same text -- that silently erased the human's attribution from every later delivery.
    [[nodiscard]] result<void> approve_automatic(LessonScope const& scope, std::string_view content,
                                                 std::string const& reviewer_id, std::string approved_at = {}) {
        if (!is_attributable_id(reviewer_id)) {
            return std::unexpected(error{failure_class::contract,
                                         "an automatic approval must name its reviewer: non-blank, no control "
                                         "characters (I4)",
                                         "memory.approval_unattributed"});
        }
        return put(scope, content, LessonApproval{"automatic:" + reviewer_id, std::move(approved_at), "", false, true},
                   /*may_replace_human=*/false);
    }

    // An evaluation's stand-in approval: the screen measures a lesson delivered as if approved (ADR-191 §3.8). It is
    // recorded as simulated and names the trial, never a person.
    [[nodiscard]] result<void> approve_simulated(LessonScope const& scope, std::string_view content,
                                                 std::string const& trial_id) {
        if (!is_attributable_id(trial_id)) {
            return std::unexpected(error{failure_class::contract, "a simulated approval must name its trial (I4)",
                                         "memory.approval_unattributed"});
        }
        return put(scope, content, LessonApproval{"simulated:" + trial_id, "", "", true}, /*may_replace_human=*/false);
    }

    // Revokes an approval. It takes effect at the next request any session builds (ADR-191 §3.3).
    void revoke(LessonScope const& scope, std::string_view content) {
        std::unique_lock lock(mutex_);
        approved_.erase(key(scope, content));
    }

    // The approval for exactly these bytes in this scope, if any.
    [[nodiscard]] std::optional<ApprovedLessonMatch> find(LessonScope const& scope, std::string_view content) const {
        if (scope.principal_id.empty() || content.empty()) return std::nullopt;
        std::shared_lock lock(mutex_);
        auto it = approved_.find(key(scope, content));
        if (it == approved_.end()) return std::nullopt;
        LessonApproval const& a = it->second;
        bool const by_id = a.simulated || a.automatic || a.acknowledgement.empty();
        return ApprovedLessonMatch{by_id ? a.approver_id : a.acknowledgement, a};
    }

    // ADR-193: every lesson text approved for `scope`, in key order -- what a delegation chain shares with a child
    // (`SpawnTargetDescriptor::share_lessons`). The session still re-verifies each one when it builds a request.
    [[nodiscard]] std::vector<std::string> texts(LessonScope const& scope) const {
        std::vector<std::string> out;
        if (scope.principal_id.empty()) return out;
        std::shared_lock lock(mutex_);
        for (auto it = approved_.lower_bound(Key{scope.tenant_id, scope.principal_id, {}});
             it != approved_.end() && std::get<0>(it->first) == scope.tenant_id &&
             std::get<1>(it->first) == scope.principal_id;
             ++it) {
            out.push_back(std::get<2>(it->first));
        }
        return out;
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return approved_.size();
    }

private:
    [[nodiscard]] static bool uses_reserved_prefix(std::string_view id) noexcept {
        auto contains_ci = [id](std::string_view needle) {
            for (std::size_t at = 0; at + needle.size() <= id.size(); ++at) {
                bool hit = true;
                for (std::size_t i = 0; i < needle.size() && hit; ++i) {
                    char c = id[at + i];
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    hit = c == needle[i];
                }
                if (hit) return true;
            }
            return false;
        };
        return contains_ci("automatic:") || contains_ci("simulated:");
    }

    [[nodiscard]] result<void> put(LessonScope const& scope, std::string_view content, LessonApproval approval,
                                   bool may_replace_human) {
        if (scope.principal_id.empty()) {
            return std::unexpected(error{failure_class::contract, "an approval must name the principal it applies to",
                                         "memory.approval_unscoped"});
        }
        if (content.empty()) {
            return std::unexpected(error{failure_class::contract, "an approved lesson cannot be empty",
                                         "memory.approval_empty"});
        }
        std::unique_lock lock(mutex_);
        auto const k = key(scope, content);
        if (auto it = approved_.find(k); it != approved_.end() && !may_replace_human && !it->second.simulated &&
                                         !it->second.automatic) {
            return std::unexpected(error{failure_class::contract,
                                         "a human approved this text; an automatic or simulated approval may not "
                                         "replace it (revoke it first)",
                                         "memory.approval_would_replace_human"});
        }
        approved_[k] = std::move(approval);
        return {};
    }

    // Round 3: a structured key. It was scope + unit separator + text in one string, so a scope containing the
    // separator matched another scope's prefix scan and `find`: an approval for scope "victim<US>X" of text "C" read
    // as victim's approval of "X<US>C".
    using Key = std::tuple<std::string, std::string, std::string>;  // tenant, principal, text
    [[nodiscard]] static Key key(LessonScope const& scope, std::string_view content) {
        return Key{scope.tenant_id, scope.principal_id, std::string(content)};
    }

    mutable std::shared_mutex mutex_;
    std::map<Key, LessonApproval> approved_;
};

// The text an approval is checked against, for an item `MemoryProvider` rendered: its confidence label (029 §6,
// `⟦memory:...⟧ `) is dropped, since that label is the provider's rendering, not the approved lesson. Any other
// text is checked as is. A lesson value can never itself begin with the label: `lesson_value_passes_validator`
// refuses the bracket glyphs (ADR-191).
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
