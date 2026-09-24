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
// `lesson_value_passes_validator` is ADR-179 §3.3's validator. Since the ADR-183 proportionality review (2026-09-24)
// it REFUSES only what is structural -- a length outside the bounds, a whole-value common token, a reserved bracket
// glyph, a control byte, the template's own join delimiters -- and `lesson_shape_warnings` reports the old shape
// heuristics (URL, path, shell, imperative) as advisory warnings for the human approving the exact bytes (E31). It
// never closed the injection channel ADR-180 §4b measured (fact-shaped lessons are followed even fenced), and a
// benign-looking value can still bias behaviour; approval by a human reading the text is the control.
//
// Round-5 red-team fix (two independent reviewers found the SAME bug the same day): the first version checked
// `candidate.value` only, while `render_lesson` concatenates `subject`/`key` into `content` and `tags` unmodified.
// `lesson_identifier_passes_validator` applies the same structural checks to `subject` and `key` (with a looser length
// bound for short identifiers), so no field can inject the digest's separators or make two different candidates
// render to the identical `content`.

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

// ADR-183 proportionality review (2026-09-24): the shape checks below used to REFUSE a lesson. Once a lesson is
// approved by a human who reads its exact bytes (E31), a fixed denylist over natural language mostly got in the way --
// its own disclosed false positives ("python is the primary...", "ssh access to the bastion...", "post mortems are
// stored in...", any ';') stopped a human from approving a sentence they had read, and a host could skip it anyway.
// So the gate keeps only what is STRUCTURAL -- what would make the rendered bytes differ from what was approved, or
// make two lessons render alike -- and the shape heuristics become warnings an approval UI shows beside the lesson.

// Refused: things that break the rendering itself.
[[nodiscard]] inline result<void> reject_structural_hazards(std::string_view text, char const* field_label) {
    // The provenance brackets (U+27E6/U+27E7) are reserved for the engine and stripped from any text
    // on the wire (ADR-183), so a lesson containing one would reach the model as bytes the approver never saw.
    for (std::string_view glyph : {"\xE2\x9F\xA6", "\xE2\x9F\xA7"}) {
        if (text.find(glyph) != std::string_view::npos) {
            return std::unexpected(error{failure_class::contract,
                                         std::string("lesson ") + field_label + " contains a reserved bracket glyph",
                                         "eval.marker_bracket"});
        }
    }
    // Round-5 fix: a control byte would let a value inject the digest's 0x1E/0x1F separators, and the template's own
    // join delimiters would let two different (subject, key) pairs render to the same `content`.
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

// Advisory: text shaped like a URL, a path, a shell fragment or an imperative. Returned as warning codes for the
// approver to see; never a refusal. (The needle lists and their history -- rounds 5-7 -- are kept as they were.)
[[nodiscard]] inline std::vector<std::string> shape_warnings(std::string_view text) {
    std::vector<std::string> out;
    std::string const lower = to_lower_ascii(text);
    static constexpr std::array<char const*, 9> kUrlOrPathNeedles = {
        "://", "www.", ".com", ".org", "\\\\",
        "javascript:", "data:", "mailto:", "vbscript:",
    };
    if (contains_any(lower, std::span{kUrlOrPathNeedles})) out.emplace_back("eval.value_url_shaped");
    if (lower.starts_with('/') || lower.starts_with('.')) out.emplace_back("eval.value_path_shaped");
    static constexpr std::array<char const*, 10> kShellNeedles = {
        ";", "|", "&&", "`", "$(", "$env:", "%comspec%", "<(", ">(", "${",
    };
    if (contains_any(lower, std::span{kShellNeedles})) out.emplace_back("eval.value_shell_shaped");
    std::string_view lower_trimmed = lower;
    while (!lower_trimmed.empty() && (lower_trimmed.front() == ' ' || lower_trimmed.front() == '\t')) {
        lower_trimmed.remove_prefix(1);
    }
    static constexpr std::array<char const*, 21> kImperativePrefixes = {
        "run ",     "delete ",  "exec ",   "curl ",       "rm ",        "sudo ",    "install ",
        "download ", "send ",   "email ",  "post ",       "call ",      "ssh ",     "scp ",
        "bash ",    "python ",  "powershell ", "cat ",    "chmod ",     "kill ",    "wget ",
    };
    if (starts_with_any(lower_trimmed, std::span{kImperativePrefixes})) out.emplace_back("eval.value_imperative_shaped");
    return out;
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
// applies every structural check `value` gets; only the length floor and the common-token check
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

    return detail::reject_structural_hazards(value, "value");
}

// Round-5 fix: applies the same structural checks `lesson_value_passes_validator` uses to `subject`/`key`, which the
// first draft of `render_lesson` left unvalidated beyond non-emptiness (a field that skipped the checks `value` got was
// the round-5 finding). The shape heuristics are warnings for all three fields alike (`lesson_shape_warnings`).
[[nodiscard]] inline result<void> lesson_identifier_passes_validator(std::string_view text) {
    if (text.size() < kLessonIdentifierMinLength || text.size() > kLessonIdentifierMaxLength) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson identifier length outside [min,max]", "eval.identifier_length"});
    }
    return detail::reject_structural_hazards(text, "identifier");
}

// ADR-183: the shape heuristics as advisory warnings for an approval UI -- "field:code" per hit, across subject, key
// and value. Empty means nothing looked unusual; non-empty never stops a lesson from rendering or being approved.
[[nodiscard]] inline std::vector<std::string> lesson_shape_warnings(LessonCandidate const& candidate) {
    std::vector<std::string> out;
    for (auto const& [field, text] : {std::pair<char const*, std::string const*>{"subject", &candidate.subject},
                                      std::pair<char const*, std::string const*>{"key", &candidate.key},
                                      std::pair<char const*, std::string const*>{"value", &candidate.value}}) {
        for (std::string& code : detail::shape_warnings(*text)) out.push_back(std::string(field) + ":" + code);
    }
    return out;
}

// The same, for one text (a value or an identifier on its own). Named apart from the candidate overload: a braced
// initializer would otherwise be ambiguous between the two.
[[nodiscard]] inline std::vector<std::string> lesson_text_shape_warnings(std::string_view text) {
    return detail::shape_warnings(text);
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
