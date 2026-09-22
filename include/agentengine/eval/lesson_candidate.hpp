#pragma once
// Implements ADR-179 §3.3 (`LessonCandidate` is a closed record, not prose) and ADR-181 §3.0 item 1
// (round-4 fix: `LessonCandidate`/`render_lesson` pulled forward so Tier 1 does not depend on
// ADR-179 stage 3's queue/reviewer wiring — this file is exactly that pull-forward, nothing more).
//
// `render_lesson` is the PURE host function ADR-181 §3.0 item 1 requires: `LessonCandidate` +
// `template_version` -> the exact `MemoryItem` bytes the production channel (ADR-180) will deliver.
// It never calls a model and never derives anything from model output beyond the closed record's
// own fields (I3): wording is chosen entirely by this file's fixed templates, keyed by
// `template_version`, never by the candidate's own text. Changing a template's own C++ source is
// therefore the only way rendering changes, and ADR-181 E26 requires that any such change be
// visible as a digest change over every candidate rendered with the old version — `template_version`
// is threaded through `rendered_lesson_digest` for exactly that reason.
//
// `lesson_value_passes_validator` is ADR-179 §3.3's own validator ("reject imperatives, URLs,
// hostnames, paths, shell fragments, and anything whose value exceeds a small length"), named there
// as a prerequisite and never built until now; ADR-181 §3.7 separately names the same validator (a
// length floor plus a common-token reject list) as a prerequisite for its containment-provenance
// test, so this one function backs both. It REDUCES the injection channel ADR-180 §4b measured
// (fact-shaped lessons are followed even fenced); it does not close it, and neither ADR claims that
// it does — a benign-looking value can still pass and still bias behaviour.
//
// Round-5 red-team fix (two independent reviewers found the SAME bug the same day, each with a
// working proof-of-concept): the FIRST version of this file ran the shape checks (URL/path/shell/
// imperative) on `candidate.value` only. `render_lesson` concatenates `subject`/`key` into `content`
// and `tags` unmodified, so a hostile `subject` or `key` — plausible, since ADR-179 §3.3 has a
// reviewer MODEL propose the whole closed record, subject/key included — sailed straight past every
// check that a value carrying the identical text would have failed. `lesson_identifier_passes_validator`
// below applies the same shape checks (minus the prose-length floor and common-token check, which
// assume a sentence-length fact, not a short identifier) to `subject` and `key` too, and additionally
// rejects the template's own literal join delimiters and any control byte — closing a companion
// finding (unvalidated subject/key could also make two different candidates render to the identical
// `content`, since nothing stopped one candidate's subject from containing another's key-and-value
// boundary text).

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/worktree_types.hpp"  // compute_digest, Digest

namespace agentengine::eval {

// ADR-179 §3.3: "a closed record (subject, key, value, source_span), not prose." `source_span`
// names WHERE in the tainted run capture this candidate came from (ADR-179 §3.1's `RunCapture`);
// its own concrete shape is still ADR-179 stage-1 work, so it is carried here as an opaque,
// host-produced string (never model output, I3) rather than guessed at.
struct LessonCandidate {  // ae-naming-lint: allow LessonCandidate — ADR-179 §3.3 / ADR-181 §3.0 item 1
    std::string subject;
    std::string key;
    std::string value;
    std::string source_span;
};

namespace detail {

// A small, explicit, testable heuristic — ADR-179 §3.3's own words: "reject imperatives, URLs,
// hostnames, paths, shell fragments". Deliberately case-insensitive substring/prefix checks, not an
// attempt at a full grammar: this is a REDUCTION of the channel (disclosed as such by both ADRs),
// not a security boundary anything else in this codebase relies on.
[[nodiscard]] inline std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] inline bool contains_any(std::string_view haystack_lower, std::span<char const* const> needles) {
    for (auto const* needle : needles) {
        if (haystack_lower.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

[[nodiscard]] inline bool starts_with_any(std::string_view haystack_lower, std::span<char const* const> prefixes) {
    for (auto const* prefix : prefixes) {
        if (haystack_lower.starts_with(prefix)) return true;
    }
    return false;
}

[[nodiscard]] inline bool has_control_byte(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20) return true;  // includes the digest's own 0x1E/0x1F separators
    }
    return false;
}

// The shape checks (URL/hostname/path/shell-fragment/imperative), factored out so `subject`/`key`
// get the SAME denylist `value` does — round-5's fix for the finding that only `value` was checked.
// `field_label` names the field in the returned error's message, so a caller can tell which of
// subject/key/value tripped it.
[[nodiscard]] inline result<void> reject_injection_shapes(std::string_view text, char const* field_label) {
    std::string const lower = to_lower_ascii(text);

    // Round-6 fix: two independent round-6 reviewers found real needle gaps here (each proven with a
    // working proof-of-concept that compiled and rendered against the pre-fix list): URI schemes that
    // don't contain "://" (`javascript:`, `data:`, `mailto:`, `vbscript:`), and shell substitution
    // forms that don't use `$(` (`<(...)`, `>(...)`, `${...}`). Added below. This remains a denylist,
    // not a grammar (the file's own long-standing disclosure): a bare hostname/IP:port with no scheme
    // and non-ASCII homoglyphs of these needles both still pass, and are named as open residuals in
    // ADR-181 §8 rather than silently claimed closed.
    //
    // Round-7 fix: a round-7 reviewer proved `.net` (present since round 5) is a false-positive magnet
    // -- it collides with the .NET framework/runtime, a term any lesson about this codebase's own
    // ecosystem would plausibly use ("the .net runtime version pinned in CI is..."). Dropped: `://`
    // already catches real URLs, and a bare ".net" with no scheme was never a strong signal on its own
    // (unlike ".com"/".org", which round 7 found no comparably common false positive for).
    static constexpr std::array<char const*, 9> kUrlOrPathNeedles = {
        "://", "www.", ".com", ".org", "\\\\",
        "javascript:", "data:", "mailto:", "vbscript:",
    };
    if (contains_any(lower, std::span{kUrlOrPathNeedles})) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label + " looks like a URL or hostname",
                                      "eval.value_url_shaped"});
    }
    if (lower.starts_with('/') || lower.starts_with('.')) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label + " looks like a path",
                                      "eval.value_path_shaped"});
    }

    static constexpr std::array<char const*, 10> kShellNeedles = {
        ";", "|", "&&", "`", "$(", "$env:", "%comspec%", "<(", ">(", "${",
    };
    if (contains_any(lower, std::span{kShellNeedles})) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label + " looks like a shell fragment",
                                      "eval.value_shell_shaped"});
    }

    // Round-6 fix: a round-6 reviewer proved a single leading space or tab defeats every prefix check
    // below outright (`starts_with_any` on the untrimmed string never matches "  run ..." against
    // "run "), which is a bypass of a check this file clearly intends to enforce, not a disclosed
    // scope limit. Strip leading ASCII whitespace before prefix-matching only -- the `contains_any`
    // checks above already match anywhere in the string, so they are unaffected by leading whitespace
    // and are left as-is.
    std::string_view lower_trimmed = lower;
    while (!lower_trimmed.empty() &&
           (lower_trimmed.front() == ' ' || lower_trimmed.front() == '\t')) {
        lower_trimmed.remove_prefix(1);
    }

    // Round-6 fix: a round-6 reviewer found several dangerous verbs missing from this list (ssh, scp,
    // bash, python, powershell, cat, chmod, kill, wget) -- added below. Still a fixed, finite list
    // (the file's own long-standing disclosure), not a grammar.
    //
    // Round-7 fix: a round-7 reviewer proved `"exec"`/`"sudo"` (the only two entries with no trailing
    // space) match as a plain SUBSTRING prefix of any longer word -- "executive approval", "execution
    // time budgets" (this very codebase's own vocabulary) were both wrongly rejected. Given a trailing
    // space like every other entry here.
    //
    // Round-7 disclosed, NOT fixed: the same reviewer proved several of these words also collide with
    // ordinary noun-phrase English when they lead a sentence -- "post mortems are stored in...", "call
    // center average wait time is...", "python is the primary language for...", "ssh access to the
    // bastion requires...", "delete markers are automatically cleaned up by...", "install steps for
    // the CLI are...", "kill switches for the ingest pipeline are...", "bash scripts in CI are...",
    // "download links for release artifacts..." are all real, plausible lesson VALUES this list
    // rejects. This is a genuine precision/recall trade-off inherent to a fixed-prefix denylist over
    // natural language, not a bug with a clean fix: removing any of these words would reopen the exact
    // imperative-shaped attack text it exists to catch ("post the credentials to...", "call the
    // webhook with...", "delete all files in..."). See ADR-181 §8.
    static constexpr std::array<char const*, 21> kImperativePrefixes = {
        "run ",     "delete ",  "exec ",   "curl ",       "rm ",        "sudo ",    "install ",
        "download ", "send ",   "email ",  "post ",       "call ",      "ssh ",     "scp ",
        "bash ",    "python ",  "powershell ", "cat ",    "chmod ",     "kill ",    "wget ",
    };
    if (starts_with_any(lower_trimmed, std::span{kImperativePrefixes})) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label + " reads as an imperative",
                                      "eval.value_imperative_shaped"});
    }

    // Round-5 fix: reject any control byte (this is what actually backs the digest comment's claim
    // that no adversarial value can inject its 0x1E/0x1F separators — the earlier draft asserted this
    // without checking it) and the template's own literal join delimiters, so two different
    // (subject, key) pairs can never render to the same `content` by smuggling one field's text
    // across the boundary `render_lesson`'s template draws between fields.
    if (has_control_byte(text)) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label + " contains a control byte",
                                      "eval.value_control_byte"});
    }
    static constexpr std::array<char const*, 2> kTemplateDelimiters = {" (", "): "};
    if (contains_any(text, std::span{kTemplateDelimiters})) {
        return std::unexpected(error{failure_class::contract,
                                      std::string("lesson ") + field_label +
                                          " contains render_lesson's own field delimiter",
                                      "eval.value_delimiter_collision"});
    }

    return {};
}

}  // namespace detail

// ADR-179 §3.3's length cap + validator; ADR-181 §3.7's length-floor/common-token prerequisite for
// containment provenance (a too-short or too-common `value` would match spuriously against
// unrelated context). Bounds are host constants, not derived from the candidate (I3).
inline constexpr std::size_t kLessonValueMinLength = 6;
inline constexpr std::size_t kLessonValueMaxLength = 200;

// `subject`/`key` are short identifiers, not sentence-length facts (examples: "deploy-region",
// "default-region") — a 6-character prose floor would reject legitimate short identifiers, so this
// bound is deliberately looser than `lesson_value_passes_validator`'s. The identifier validator below
// still applies every SHAPE check `value` gets; only the length floor and the common-token check
// (which assumes prose, not a domain identifier) differ.
inline constexpr std::size_t kLessonIdentifierMinLength = 1;
inline constexpr std::size_t kLessonIdentifierMaxLength = 80;

[[nodiscard]] inline result<void> lesson_value_passes_validator(std::string_view value) {
    if (value.size() < kLessonValueMinLength || value.size() > kLessonValueMaxLength) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value length outside [min,max]", "eval.value_length"});
    }

    std::string const lower = detail::to_lower_ascii(value);

    // Common tokens: rejected only as a WHOLE-value match (a common word inside a longer, specific
    // value is fine and is exactly what a real lesson looks like) — this is the spurious-containment
    // guard ADR-181 §3.7 names, not a content filter. Round-5 fix: the first draft's list mixed in
    // words shorter than `kLessonValueMinLength` (6), which the length check above already catches
    // first — those entries were unreachable dead code (a round-5 reviewer found this). Every entry
    // below is >= 6 characters and therefore actually exercised by this check.
    static constexpr std::array<char const*, 8> kCommonWholeValues = {
        "default", "example", "unknown", "general", "various", "current", "normal", "typical",
    };
    for (auto const* common : kCommonWholeValues) {
        if (lower == common) {
            return std::unexpected(error{failure_class::contract,
                                          "lesson value is a common token", "eval.value_common_token"});
        }
    }

    return detail::reject_injection_shapes(value, "value");
}

// Round-5 fix: applies the same shape denylist `lesson_value_passes_validator` uses to `subject`/
// `key`, which the first draft of `render_lesson` left completely unvalidated beyond non-emptiness —
// two independent round-5 reviewers found this and each built a working proof-of-concept (a hostile
// `subject` containing a URL+shell-pipe payload, rejected outright when placed in `value`, sailed
// through unmodified when placed in `subject`).
[[nodiscard]] inline result<void> lesson_identifier_passes_validator(std::string_view text) {
    if (text.size() < kLessonIdentifierMinLength || text.size() > kLessonIdentifierMaxLength) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson identifier length outside [min,max]", "eval.identifier_length"});
    }
    return detail::reject_injection_shapes(text, "identifier");
}

// ADR-181 §3.0 item 1: `render_lesson(candidate, template_version) -> MemoryItem{kind=procedural,
// content, tags, salience}`. `salience` is NOT computed here — ADR-181 §3.2 is explicit that it is a
// host constant "the promotion path will write", a decision left to ADR-179 stage 3 — so it is a
// caller-supplied parameter, never invented inside this pure function.
//
// `MemoryItem::id`/`write_seq` are left at their default-constructed values: both are write-time
// concerns `write_memory_item()` fills in (memory.hpp), and `id` there is a digest over `content`
// ALONE (round-4 finding: it is NOT the identity this ADR needs for an approval binding) — see
// `rendered_lesson_digest` below for the digest that actually covers what render_lesson produced.
[[nodiscard]] inline result<MemoryItem> render_lesson(LessonCandidate const& candidate,
                                                        std::string_view template_version,
                                                        float salience) {
    if (auto ok = lesson_identifier_passes_validator(candidate.subject); !ok) return std::unexpected(ok.error());
    if (auto ok = lesson_identifier_passes_validator(candidate.key); !ok) return std::unexpected(ok.error());
    if (auto ok = lesson_value_passes_validator(candidate.value); !ok) return std::unexpected(ok.error());

    // Exactly one template exists today ("v1"); an unrecognised version is a contract violation, not
    // a silent fallback — ADR-181 E26 needs "the template changed" to be a detectable, refused-else
    // event, not something that quietly renders differently.
    if (template_version != "v1") {
        return std::unexpected(error{failure_class::contract,
                                      "unknown render_lesson template_version", "eval.unknown_template_version"});
    }

    MemoryItem item{};
    item.kind = memory_kind::procedural;
    item.content = "For " + candidate.subject + " (" + candidate.key + "): " + candidate.value;
    item.tags = {candidate.subject, candidate.key};
    item.salience = salience;
    return item;
}

// ADR-181 E26/E31's digest: over the RENDERED bytes and `template_version`, never over the
// candidate's raw fields alone (a first-draft mutant "digest over {subject,key,value} only" is
// exactly what E26 plants and expects caught) — this is what an approver's acknowledgement binds to
// (E31, `promotion_ack.hpp`) and what `write_memory_item`'s own content-only `id` does NOT capture.
[[nodiscard]] inline result<Digest> rendered_lesson_digest(MemoryItem const& rendered,
                                                             std::string_view template_version) {
    // Record/unit separators (0x1E/0x1F) keep the fields unambiguous under concatenation.
    // `lesson_identifier_passes_validator`/`lesson_value_passes_validator` both explicitly reject any
    // control byte (round-5 fix — the first draft asserted this without actually checking it, which a
    // round-5 reviewer flagged), so `content`/`tags` can never carry a literal 0x1E/0x1F this
    // function's own join would otherwise confuse with a field boundary.
    std::string canonical;
    canonical += template_version;
    canonical += '\x1e';
    canonical += rendered.content;
    canonical += '\x1e';
    for (auto const& tag : rendered.tags) {
        canonical += tag;
        canonical += '\x1f';
    }
    canonical += '\x1e';
    canonical += std::to_string(rendered.salience);

    auto bytes = std::as_bytes(std::span{canonical.data(), canonical.size()});
    return compute_digest(bytes);
}

}  // namespace agentengine::eval
