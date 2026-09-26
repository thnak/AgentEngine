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

#include <cstddef>
#include <iterator>
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
    // ADR-191 round 4: the delivery form the Tier-1 screen this approval rests on measured (an `eval::lesson_delivery`
    // name, e.g. "approved_instructions"). A session delivers the lesson as approved only in that form; in any other
    // it goes out as ordinary memory and the event says why. Empty: no screened form recorded (a host's own
    // registration) -- delivered in whatever form the session is set to.
    std::string screened_delivery{};
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

// ADR-191 round 4: the form (its `eval::lesson_delivery` name) a session ships an approved lesson in, from the host's
// settings -- the value `LessonApproval::screened_delivery` is compared with. `automatic` is request-wide: the guidance
// wording names the automated reviewer when ANY approval delivered in the request is automatic. Kept equal to
// `eval::shipped_lesson_delivery` (test_approval_resume L6 checks every combination).
[[nodiscard]] constexpr std::string_view approved_lesson_delivery_name(bool automatic, approved_lesson_level level,
                                                                       bool fence_disabled) noexcept {
    if (fence_disabled) return "approved_fence_off";
    if (level == approved_lesson_level::instructions) return "approved_instructions";
    return automatic ? "approved_automatic" : "approved";
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

// Issue #112 B4 (ADR-191 §7 round 5): the id checks below read UTF-8 code points, not bytes. The round-3 checks were
// ASCII-only, so an id of only U+200B or U+00A0 was "non-blank", U+2028 / U+0085 slipped a line break past the
// control-character test, and `аutomatic:` (Cyrillic а), `automatic：` (full-width colon) or `auto<U+200B>matic:`
// passed the reserved-prefix test. The tables are deliberately small and closed; ADR-191 §7 lists what they cover and
// the residual (a full UTS #39 skeleton is not attempted).
namespace id_check_detail {

struct CodePointRange {  // ae-naming-lint: allow CodePointRange — ADR-191 §7 round 5: private helper of the id checks
    char32_t lo;
    char32_t hi;
};

// Decodes one strict UTF-8 code point at `at` (RFC 3629: no overlongs, no surrogates, nothing above U+10FFFF,
// no truncation). Returns U+FFFFFFFF on any error; `len` is how many bytes to skip.
[[nodiscard]] constexpr char32_t decode_utf8(std::string_view s, std::size_t at, std::size_t& len) noexcept {
    constexpr char32_t bad = 0xFFFFFFFF;
    auto const b0 = static_cast<unsigned char>(s[at]);
    len = 1;
    if (b0 < 0x80) return b0;
    std::size_t need = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) { need = 1; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { need = 2; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { need = 3; cp = b0 & 0x07; min = 0x10000; }
    else return bad;
    if (at + need >= s.size()) return bad;  // truncated sequence
    for (std::size_t i = 1; i <= need; ++i) {
        auto const b = static_cast<unsigned char>(s[at + i]);
        if ((b & 0xC0) != 0x80) return bad;
        cp = (cp << 6) | (b & 0x3F);
    }
    len = need + 1;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return bad;
    return cp;
}

// Code points that break an audit line or reorder how it displays: C0/DEL/C1 controls (U+0085 NEL among them), the
// line and paragraph separators, and the bidirectional controls (embeddings, overrides, isolates, marks).
inline constexpr CodePointRange kLineOrOrderBreaking[] = {
    {0x0000, 0x001F}, {0x007F, 0x009F}, {0x061C, 0x061C}, {0x200E, 0x200F},
    {0x2028, 0x2029}, {0x202A, 0x202E}, {0x2066, 0x2069},
};

// Code points that show nothing (or only space): Unicode White_Space, the default-ignorable format characters and
// fillers most often used to fake a visible id (ZWSP/ZWNJ/ZWJ, word joiner and invisible operators, BOM, soft
// hyphen, CGJ, Hangul and braille blanks, variation selectors, tags). An id made only of these names nobody.
inline constexpr CodePointRange kInvisible[] = {
    {0x0009, 0x000D}, {0x0020, 0x0020}, {0x0085, 0x0085}, {0x00A0, 0x00A0}, {0x00AD, 0x00AD},
    {0x034F, 0x034F}, {0x061C, 0x061C}, {0x115F, 0x1160}, {0x1680, 0x1680}, {0x17B4, 0x17B5},
    {0x180B, 0x180F}, {0x2000, 0x200F}, {0x2028, 0x202F}, {0x205F, 0x206F}, {0x2800, 0x2800},
    {0x3000, 0x3000}, {0x3164, 0x3164}, {0xFE00, 0xFE0F}, {0xFEFF, 0xFEFF}, {0xFFA0, 0xFFA0},
    {0xFFF9, 0xFFFB}, {0x1D173, 0x1D17A}, {0xE0000, 0xE0FFF},
};

[[nodiscard]] constexpr bool in_ranges(char32_t cp, CodePointRange const* first, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (cp >= first[i].lo && cp <= first[i].hi) return true;
    }
    return false;
}
[[nodiscard]] constexpr bool breaks_line_or_order(char32_t cp) noexcept {
    return in_ranges(cp, kLineOrOrderBreaking, std::size(kLineOrOrderBreaking));
}
[[nodiscard]] constexpr bool is_invisible(char32_t cp) noexcept {
    return in_ranges(cp, kInvisible, std::size(kInvisible));
}

// Look-alikes of the letters of `automatic:` / `simulated:` (a c d e i l m o s t u and the colon), mapped to ASCII for
// the reserved-prefix check. Cyrillic and Greek homoglyphs, dotless i, script l, and colon look-alikes. 'l', 'I' and
// '1' are folded together as 'i' by the check itself (they read alike in many fonts), so they map to 'i' here too.
struct Confusable {  // ae-naming-lint: allow Confusable — ADR-191 §7 round 5: private helper of the id checks
    char32_t cp;
    char ascii;
};
inline constexpr Confusable kConfusables[] = {
    // Cyrillic
    {0x0430, 'a'}, {0x0410, 'a'}, {0x0441, 'c'}, {0x0421, 'c'}, {0x0501, 'd'}, {0x0435, 'e'}, {0x0415, 'e'},
    {0x0456, 'i'}, {0x0406, 'i'}, {0x04CF, 'i'}, {0x04C0, 'i'}, {0x043C, 'm'}, {0x041C, 'm'}, {0x043E, 'o'},
    {0x041E, 'o'}, {0x0455, 's'}, {0x0405, 's'}, {0x0442, 't'}, {0x0422, 't'},
    // Greek
    {0x03B1, 'a'}, {0x0391, 'a'}, {0x03F2, 'c'}, {0x03F9, 'c'}, {0x03B5, 'e'}, {0x0395, 'e'}, {0x03B9, 'i'},
    {0x0399, 'i'}, {0x039C, 'm'}, {0x03BF, 'o'}, {0x039F, 'o'}, {0x03C4, 't'}, {0x03A4, 't'}, {0x03C5, 'u'},
    // Armenian, Latin, letterlike
    {0x057D, 'u'}, {0x0585, 'o'}, {0x0131, 'i'}, {0x2113, 'i'}, {0x217C, 'i'}, {0x2170, 'i'},
    // colon look-alikes
    {0x02D0, ':'}, {0x02F8, ':'}, {0x0589, ':'}, {0x05C3, ':'}, {0x2236, ':'}, {0xA789, ':'}, {0xFE13, ':'},
    {0xFE55, ':'},
};

// The skeleton-lite fold used only by the reserved-prefix check: invisible code points dropped, full-width ASCII
// (U+FF01-U+FF5E) narrowed, the look-alikes above mapped, ASCII case folded, l/1/| folded into i and 0 into o. Any other
// non-ASCII code point (or an undecodable byte) becomes a byte that matches no needle, so it cannot join a match.
[[nodiscard]] inline std::string reserved_prefix_skeleton(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (std::size_t at = 0; at < id.size();) {
        std::size_t len = 1;
        char32_t cp = decode_utf8(id, at, len);
        at += len;
        if (cp != 0xFFFFFFFF && is_invisible(cp)) continue;
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        char c = '\x01';
        if (cp < 0x80) {
            c = static_cast<char>(cp);
        } else {
            for (Confusable const& k : kConfusables) {
                if (k.cp == cp) {
                    c = k.ascii;
                    break;
                }
            }
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == 'l' || c == '1' || c == '|') c = 'i';
        if (c == '0') c = 'o';
        out += c;
    }
    return out;
}

}  // namespace id_check_detail

// Round 3: an id an audit line names -- an approver, a reviewer, ADR-192's operator -- must be visible and single-line:
// a blank id named nobody, and an embedded newline could forge a further audit line. Issue #112 B4: "control
// character" now means any code point that breaks a line or reorders display (C0, DEL, C1 incl. U+0085, U+2028/9,
// the bidi controls) -- and bytes that are not valid UTF-8 count too, since no reader can say what they display.
[[nodiscard]] inline bool id_has_control_char(std::string_view id) noexcept {
    for (std::size_t at = 0; at < id.size();) {
        std::size_t len = 1;
        char32_t const cp = id_check_detail::decode_utf8(id, at, len);
        if (cp == 0xFFFFFFFF || id_check_detail::breaks_line_or_order(cp)) return true;
        at += len;
    }
    return false;
}
// Non-blank means at least one code point that is not whitespace or an invisible format character (issue #112 B4:
// an id of only ZWSP or NBSP was accepted).
[[nodiscard]] inline bool is_attributable_id(std::string_view id) noexcept {
    if (id_has_control_char(id)) return false;
    for (std::size_t at = 0; at < id.size();) {
        std::size_t len = 1;
        char32_t const cp = id_check_detail::decode_utf8(id, at, len);
        if (!id_check_detail::is_invisible(cp)) return true;
        at += len;
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
                                                 std::string const& reviewer_id, std::string approved_at = {},
                                                 std::string screened_delivery = {}) {
        if (!is_attributable_id(reviewer_id)) {
            return std::unexpected(error{failure_class::contract,
                                         "an automatic approval must name its reviewer: non-blank, no control "
                                         "characters (I4)",
                                         "memory.approval_unattributed"});
        }
        return put(scope, content,
                   LessonApproval{"automatic:" + reviewer_id, std::move(approved_at), "", false, true,
                                  std::move(screened_delivery)},
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
    // Issue #112 B4: matched on `reserved_prefix_skeleton` (invisible characters dropped, full-width and Cyrillic /
    // Greek look-alikes mapped, case and l/1/i folded), so `аutomatic:`, `automatic：` and `auto<ZWSP>matic:` are
    // refused like `automatic:`. The needles are written in the skeleton's alphabet ("simulated" folds to "simuiated").
    [[nodiscard]] static bool uses_reserved_prefix(std::string_view id) {
        std::string const skeleton = id_check_detail::reserved_prefix_skeleton(id);
        return skeleton.find("automatic:") != std::string::npos || skeleton.find("simuiated:") != std::string::npos;
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
