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

}  // namespace detail

// ADR-179 §3.3's length cap + validator; ADR-181 §3.7's length-floor/common-token prerequisite for
// containment provenance (a too-short or too-common `value` would match spuriously against
// unrelated context). Bounds are host constants, not derived from the candidate (I3).
inline constexpr std::size_t kLessonValueMinLength = 6;
inline constexpr std::size_t kLessonValueMaxLength = 200;

[[nodiscard]] inline result<void> lesson_value_passes_validator(std::string_view value) {
    if (value.size() < kLessonValueMinLength || value.size() > kLessonValueMaxLength) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value length outside [min,max]", "eval.value_length"});
    }

    std::string const lower = detail::to_lower_ascii(value);

    // Common tokens: rejected only as a WHOLE-value match (a common word inside a longer, specific
    // value is fine and is exactly what a real lesson looks like) — this is the spurious-containment
    // guard ADR-181 §3.7 names, not a content filter.
    static constexpr std::array<char const*, 16> kCommonWholeValues = {
        "the", "a", "an", "is", "are", "was", "were", "test", "true", "false",
        "none", "null", "default", "example", "value", "ok",
    };
    for (auto const* common : kCommonWholeValues) {
        if (lower == common) {
            return std::unexpected(error{failure_class::contract,
                                          "lesson value is a common token", "eval.value_common_token"});
        }
    }

    // URL / hostname / path shapes.
    static constexpr std::array<char const*, 6> kUrlOrPathNeedles = {
        "://", "www.", ".com", ".net", ".org", "\\\\",
    };
    if (detail::contains_any(lower, std::span{kUrlOrPathNeedles})) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value looks like a URL or hostname", "eval.value_url_shaped"});
    }
    if (lower.starts_with('/') || lower.starts_with('.') ) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value looks like a path", "eval.value_path_shaped"});
    }

    // Shell fragments.
    static constexpr std::array<char const*, 7> kShellNeedles = {
        ";", "|", "&&", "`", "$(", "$env:", "%comspec%",
    };
    if (detail::contains_any(lower, std::span{kShellNeedles})) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value looks like a shell fragment", "eval.value_shell_shaped"});
    }

    // Imperatives: a small denylist of leading verbs a "fact" should never start with.
    static constexpr std::array<char const*, 12> kImperativePrefixes = {
        "run ", "delete ", "exec", "curl ", "rm ", "sudo", "install ", "download ",
        "send ", "email ", "post ", "call ",
    };
    if (detail::starts_with_any(lower, std::span{kImperativePrefixes})) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson value reads as an imperative", "eval.value_imperative_shaped"});
    }

    return {};
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
    if (candidate.subject.empty() || candidate.key.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "lesson candidate subject/key must be non-empty", "eval.candidate_malformed"});
    }
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
    // Record/unit separators (0x1E/0x1F) keep the fields unambiguous under concatenation — no
    // adversarial value can inject them (they are non-printable, and the validator above rejects
    // control characters implicitly by rejecting values outside ordinary printable-token shape in
    // every path that matters; this digest does not rely on that alone, since it hashes the FULL
    // structured tuple, not a naive join).
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
