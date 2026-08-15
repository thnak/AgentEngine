#pragma once
// Implements decisions/ADR-023-response-format-codec-seam.md, Phase 1 AND Phase 2 (ADR §6 points 3
// and 4). Extracts clean `Reasoning`/`Text` content items when a raw, non-JSON-field model response
// format (OpenAI Harmony, DeepSeek's compound delimiter tokens, Hermes/Qwen's `<tool_call>` tags,
// DeepSeek-R1/Qwen3's `<think>` reasoning tags) leaks into an assistant `content` string instead of
// being normalized by the serving layer (docs/research/2026-08-10-provider-and-harmony-adapter-
// landscape.md, Parts 2-3 and 5).
//
// Still produces ONLY `ContentItem`s plus plain-data candidates -- NEVER a `ToolCallRequest` or
// anything that itself decides trust -- and still has no include of core/tool_pipeline.hpp. Every
// commentary-channel (tool-call-shaped) block yields BOTH: (a) the Phase-1 plainly-visible, inert,
// tainted `Text` diagnostic a human can read regardless of what happens next, and (b) if the block
// parsed cleanly (real recipient, JSON-valid arguments), a `DetectedToolCallCandidate` in
// `decode_result::candidates` -- structurally inert data (two strings), for whoever calls this
// function to decide what to do with. `OpenAIChatClient` (protocol/openai/chat_client.hpp) is that
// caller: it matches a candidate's recipient against the live `ChatRequest.tools` list and, only for
// a KNOWN tool name, promotes it to a real `ToolCall` content item tagged
// `provenance = call_provenance::text_derived` (core/content.hpp) -- never `vendor_structured`. The
// actual trust decision -- whether that tagged call runs without a human -- lives entirely in
// `core/tool_pipeline.hpp`'s `invoke_tool` step 5 (ADR-023 §6 point 4's capability-scoped
// declassifier), the ONE place `provenance` is ever read for an approval decision. This file makes
// that decision reachable; it never makes it.
//
// The table shape below is the spike's (…/scratchpad/adr023-spike/spike.cpp, independently
// recompiled and reverified 2026-08-10) REVISED shape: `stop_at` is a list of stop literals, not the
// single-literal v1 that was falsified by a real Harmony variant with no space before
// `<|constrain|>` (see the ADR §4a finding 1). `sizeof(Format)` stays a `static constexpr` POD array
// -- no lambdas, no `std::function`, no regex (ADR §4b finding 2 found an embedded-regex field would
// itself be a ReDoS/hot-path risk and a violation of 007 §5's "total and deterministic" requirement).

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"

namespace agentengine::response_format_codec {

// Where a field's raw text comes from: the header span (between `block_open` and `body_open`, e.g.
// Harmony's `assistant<|channel|>commentary to=functions.NAME <|constrain|>json`) or the body span
// (between `body_open` and whichever terminator matched).
// ae-naming-lint: allow field_source — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class field_source { header, body };

// `none`: field not present in this format. `whole`: the entire segment, untouched. `marker`: found
// after a literal marker (empty marker = start of segment), ending at the earliest of `stop_at`'s
// literals (or segment end if none match) -- ADR §4a finding 1's fix, a LIST of stop literals, not
// one. `json_field`: the value of a named top-level JSON key within the segment (Hermes/Qwen's body
// is itself one JSON object).
// ae-naming-lint: allow field_mode — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class field_mode { none, whole, marker, json_field };

// ae-naming-lint: allow field_spec — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct field_spec {
    field_source source = field_source::body;
    field_mode   mode   = field_mode::none;
    std::string_view marker{};
    std::array<std::string_view, 4> stop_at{};  // empty entries ignored
    std::string_view strip_prefix{};               // e.g. Harmony's "functions." recipient prefix
    std::string_view json_path{};                  // top-level key name, when mode == json_field
};

// ae-naming-lint: allow terminator_spec — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct terminator_spec {
    std::string_view token;
    // True for the terminator that means "this block is tool-call-shaped" (Harmony's `<|call|>` vs.
    // `<|end|>`/`<|return|>`; DeepSeek/Hermes have exactly one terminator each, always true). Distinct
    // from whether the block is actually treated as commentary -- `channel_kind_of` below is what
    // decides that, from `fixed_channel` or the extracted channel text.
    bool tool_call_shaped = false;
};

// ae-naming-lint: allow channel_kind — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class channel_kind { analysis, commentary, final_answer, unknown };

// One format's grammar, entirely as data. `fixed_channel` is set for formats with no explicit
// channel marker of their own (DeepSeek/Hermes tool-call rows are always `commentary`; the
// `<think>` row is always `analysis`) -- `channel_field` is only consulted when `fixed_channel ==
// unknown` (Harmony, whose single envelope carries all three channels distinguished by
// `<|channel|>NAME`).
// ae-naming-lint: allow format_spec — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct format_spec {
    std::string_view name;
    std::array<std::string_view, 2> sniff{};  // cheap pre-check; ANY non-empty entry must be present
    std::string_view block_open;
    std::string_view body_open;  // empty => no separate header segment
    std::array<terminator_spec, 3> terminators{};
    channel_kind fixed_channel = channel_kind::unknown;
    field_spec   channel_field{};
    field_spec   recipient_field{};
    field_spec   arguments_field{};
    field_spec   constraint_field{};
    std::string_view default_arg_type = "json";
};

namespace codec_detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// End offset (exclusive) of one JSON value starting at `i`, or npos on malformed/truncated input.
// Balanced-brace/bracket scan with string-aware quote tracking -- a parser primitive, not a regex,
// which is what makes it safe to run against adversarial/untrusted text (007 §5's determinism rule).
[[nodiscard]] inline std::size_t json_value_end(std::string_view s, std::size_t i) {
    if (i >= s.size()) return std::string_view::npos;
    if (s[i] == '"') {
        for (std::size_t j = i + 1; j < s.size(); ++j) {
            if (s[j] == '\\') { ++j; continue; }
            if (s[j] == '"') return j + 1;
        }
        return std::string_view::npos;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        bool in_string = false;
        for (std::size_t j = i; j < s.size(); ++j) {
            char c = s[j];
            if (in_string) {
                if (c == '\\') ++j;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return j + 1; }
        }
        return std::string_view::npos;
    }
    std::size_t j = i;
    while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != ' ' && s[j] != '\n') ++j;
    return j == i ? std::string_view::npos : j;
}

[[nodiscard]] inline bool is_valid_json(std::string_view s) {
    s = trim(s);
    return !s.empty() && json_value_end(s, 0) == s.size();
}

[[nodiscard]] inline bool json_field(std::string_view s, std::string_view key, std::string_view& out) {
    s = trim(s);
    std::string const pattern = "\"" + std::string(key) + "\"";
    std::size_t k = s.find(pattern);
    if (k == std::string_view::npos) return false;
    std::size_t i = k + pattern.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == ':')) ++i;
    std::size_t e = json_value_end(s, i);
    if (e == std::string_view::npos) return false;
    out = s.substr(i, e - i);
    if (out.size() >= 2 && out.front() == '"') out = out.substr(1, out.size() - 2);
    return true;
}

[[nodiscard]] inline bool extract_field(field_spec const& f, std::string_view header, std::string_view body,
                                          std::string_view& out) {
    if (f.mode == field_mode::none) return false;
    std::string_view seg = (f.source == field_source::header) ? header : body;
    if (f.mode == field_mode::whole) {
        out = seg;
    } else if (f.mode == field_mode::marker) {
        std::size_t start = 0;
        if (!f.marker.empty()) {
            std::size_t k = seg.find(f.marker);
            if (k == std::string_view::npos) return false;
            start = k + f.marker.size();
        }
        std::size_t end = seg.size();
        for (std::string_view stop : f.stop_at) {
            if (stop.empty()) continue;
            std::size_t k = seg.find(stop, start);
            if (k != std::string_view::npos && k < end) end = k;
        }
        out = seg.substr(start, end - start);
    } else {  // json_field
        if (!json_field(seg, f.json_path, out)) return false;
    }
    out = trim(out);
    if (!f.strip_prefix.empty() && out.starts_with(f.strip_prefix)) out.remove_prefix(f.strip_prefix.size());
    return true;
}

[[nodiscard]] inline channel_kind channel_kind_of_text(std::string_view text) {
    if (text == "analysis") return channel_kind::analysis;
    if (text == "commentary") return channel_kind::commentary;
    if (text == "final") return channel_kind::final_answer;
    return channel_kind::unknown;
}

// The built-in table. New rows are data, not code -- 006's spike (ADR-023 §4a) proved this shape
// covers delimiter-based, JSON-payload tool-call formats and reasoning-tag formats; it does NOT
// cover non-JSON-payload formats (e.g. Llama 3.1's `<|python_tag|>brave_search.call(query="x")`
// Python-expression syntax) or formats needing real escaping/nesting -- named residuals in the ADR,
// not silently glossed over here.
inline constexpr format_spec kBuiltinTable[] = {
    // OpenAI Harmony (gpt-oss). One envelope, three channels distinguished by `<|channel|>NAME`.
    {.name = "harmony",
     .sniff = {"<|channel|>", "<|message|>"},
     .block_open = "<|start|>",
     .body_open = "<|message|>",
     .terminators = {{{"<|call|>", true}, {"<|end|>", false}, {"<|return|>", false}}},
     .fixed_channel = channel_kind::unknown,
     .channel_field = {.source = field_source::header,
                        .mode = field_mode::marker,
                        .marker = "<|channel|>",
                        .stop_at = {" ", "<|message|>"}},
     .recipient_field = {.source = field_source::header,
                          .mode = field_mode::marker,
                          .marker = "to=",
                          .stop_at = {" ", "<|constrain|>", "<|channel|>", "<|message|>"},
                          .strip_prefix = "functions."},
     .arguments_field = {.source = field_source::body, .mode = field_mode::whole},
     .constraint_field = {.source = field_source::header,
                           .mode = field_mode::marker,
                           .marker = "<|constrain|>",
                           .stop_at = {" ", "<|message|>"}}},

    // DeepSeek V3/R1 tool calls. U+FF5C (｜) / U+2581 (▁) compound delimiter tokens, always
    // commentary-shaped -- DeepSeek's own spec forbids any other text alongside a call. Named
    // residual: this row matches the INNER per-call wrapper (`<｜tool▁call▁begin｜>...<｜tool▁call▁
    // end｜>`) only, not the OUTER `<｜tool▁calls▁begin｜>`/`<｜tool▁calls▁end｜>` (plural) wrapper a
    // real multi-call response also carries -- those plural tokens fall through as ordinary leftover
    // text (visible, inert, never lost; just not stripped). Fine for the single-call case; a real
    // parallel-tool-call response would show the outer tokens literally alongside the clean inner
    // extraction. Not corrected here -- correctness/safety are unaffected, only cosmetics.
    {.name = "deepseek_tool_call",
     .sniff = {"<\xef\xbd\x9c" "tool\xe2\x96\x81" "call\xe2\x96\x81" "begin\xef\xbd\x9c>", ""},
     .block_open = "<\xef\xbd\x9c" "tool\xe2\x96\x81" "call\xe2\x96\x81" "begin\xef\xbd\x9c>",
     .body_open = "\n```json\n",
     .terminators = {{{"\n```<\xef\xbd\x9c" "tool\xe2\x96\x81" "call\xe2\x96\x81" "end\xef\xbd\x9c>", true}}},
     .fixed_channel = channel_kind::commentary,
     .recipient_field = {.source = field_source::header,
                          .mode = field_mode::marker,
                          .marker = "<\xef\xbd\x9c" "tool\xe2\x96\x81" "sep\xef\xbd\x9c>"},
     .arguments_field = {.source = field_source::body, .mode = field_mode::whole}},

    // Hermes/NousResearch style, used by Qwen 2/2.5/3's default chat template. A bare JSON object
    // wrapped in literal tags -- the body IS the JSON, so recipient/arguments are json_field reads
    // rather than marker scans.
    {.name = "hermes_qwen_tool_call",
     .sniff = {"<tool_call>", ""},
     .block_open = "<tool_call>",
     .body_open = "",
     .terminators = {{{"</tool_call>", true}}},
     .fixed_channel = channel_kind::commentary,
     .recipient_field = {.source = field_source::body, .mode = field_mode::json_field, .json_path = "name"},
     .arguments_field = {.source = field_source::body, .mode = field_mode::json_field, .json_path = "arguments"}},

    // DeepSeek-R1 / Qwen3 reasoning tags (ADR-023 §5) -- no recipient/constraint, always analysis.
    // Text AFTER `</think>` is ordinary leftover (final-channel) content, handled by the interpreter's
    // own between/after-block leftover-text pass, not by this row.
    {.name = "think_tag",
     .sniff = {"<think>", ""},
     .block_open = "<think>",
     .body_open = "",
     .terminators = {{{"</think>", false}}},
     .fixed_channel = channel_kind::analysis},
};

}  // namespace codec_detail

// ADR-023 §6 point 4 (Phase 2). Plain, inert data -- two strings -- extracted from a commentary-
// channel block whose recipient and arguments both parsed cleanly. Carries no trust of its own;
// see this file's own top comment for the full chain from here to an actual approval decision.
// ae-naming-lint: allow DetectedToolCallCandidate — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct DetectedToolCallCandidate {
    std::string recipient;
    std::string arguments_json;
    // Index into the SAME `decode_result::items` this candidate came from -- the inert diagnostic
    // `Text` item this candidate is the structured twin of. Lets a caller that decides to promote a
    // candidate replace EXACTLY the right item in place (position, not string-content matching,
    // which would be fragile against two candidates sharing a recipient name).
    std::size_t diagnostic_item_index = 0;
};

// ae-naming-lint: allow decode_result — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct decode_result {
    std::vector<ContentItem> items;
    // Phase 2: one entry per commentary-channel block that had BOTH a non-empty recipient and
    // JSON-valid arguments -- a strict subset of the tool-call-shaped blocks that produced a
    // diagnostic `Text` item in `items` above (every candidate has a matching diagnostic; not every
    // diagnostic has a matching candidate, e.g. recipient extraction failed but the block still
    // parsed as tool-call-shaped).
    std::vector<DetectedToolCallCandidate> candidates{};
    // True when suspicious tokens were seen (sniff hit) but the content could not be safely,
    // completely segmented (a dangling/truncated block, or a channel this table can't classify) --
    // ADR §4a finding 2's fail-closed behavior. `items` then holds exactly one untouched `Text`
    // item carrying the ORIGINAL content, byte-for-byte -- never a partially-mangled hybrid.
    bool partial = false;
};

// The one entry point. Runs `sniff()` for each table row (cheap, ADR §4b finding 3's answer to the
// false-positive-tax risk: ordinary content with no format markers touches nothing past this loop)
// and, on the first hit, attempts a full left-to-right segmentation. Never throws; never produces a
// `ToolCall`/`ToolCallRequest` -- see this file's own top comment for why that boundary is load-
// bearing, not incidental.
[[nodiscard]] inline decode_result decode_response_format(std::string_view content) {
    using namespace codec_detail;

    format_spec const* matched = nullptr;
    for (format_spec const& f : kBuiltinTable) {
        bool sniff_hit = false;
        for (std::string_view marker : f.sniff) {
            if (marker.empty()) continue;
            if (content.find(marker) != std::string_view::npos) { sniff_hit = true; break; }
        }
        if (sniff_hit) { matched = &f; break; }
    }

    auto const fallback = [&content]() -> decode_result {
        ContentItem item;
        item.value = Text{std::string(content)};
        item.origin = content_origin::assistant;
        item.tainted = false;
        return decode_result{.items = {std::move(item)}, .partial = true};
    };

    if (!matched) {
        // Nothing suspicious at all -- the overwhelmingly common case. Preserve verbatim.
        ContentItem item;
        item.value = Text{std::string(content)};
        item.origin = content_origin::assistant;
        item.tainted = false;
        return decode_result{.items = {std::move(item)}, .partial = false};
    }

    format_spec const& f = *matched;
    std::vector<ContentItem> items;
    std::vector<DetectedToolCallCandidate> candidates;
    std::size_t pos = 0;

    auto push_text = [&items](std::string_view text, bool tainted) {
        if (text.empty()) return;
        ContentItem item;
        item.value = Text{std::string(text)};
        item.origin = content_origin::assistant;
        item.tainted = tainted;
        items.push_back(std::move(item));
    };
    auto push_reasoning = [&items](std::string_view text) {
        ContentItem item;
        item.value = Reasoning{std::string(text), /*encrypted=*/false};
        item.origin = content_origin::assistant;
        item.tainted = false;
        items.push_back(std::move(item));
    };

    while (true) {
        std::size_t const block_start = content.find(f.block_open, pos);
        if (block_start == std::string_view::npos) {
            push_text(content.substr(pos), /*tainted=*/false);
            return decode_result{.items = std::move(items), .candidates = std::move(candidates), .partial = false};
        }
        push_text(content.substr(pos, block_start - pos), /*tainted=*/false);

        std::size_t const after_open = block_start + f.block_open.size();
        std::string_view header;
        std::size_t body_start = after_open;
        if (!f.body_open.empty()) {
            std::size_t const header_end = content.find(f.body_open, after_open);
            if (header_end == std::string_view::npos) return fallback();  // dangling, truncated block
            header = content.substr(after_open, header_end - after_open);
            body_start = header_end + f.body_open.size();
        }

        std::size_t best_terminator_pos = std::string_view::npos;
        terminator_spec const* best_terminator = nullptr;
        for (terminator_spec const& t : f.terminators) {
            if (t.token.empty()) continue;
            std::size_t const k = content.find(t.token, body_start);
            if (k != std::string_view::npos && (best_terminator_pos == std::string_view::npos || k < best_terminator_pos)) {
                best_terminator_pos = k;
                best_terminator = &t;
            }
        }
        if (!best_terminator) return fallback();  // dangling, truncated block

        std::string_view const body = content.substr(body_start, best_terminator_pos - body_start);

        channel_kind channel = f.fixed_channel;
        if (channel == channel_kind::unknown) {
            std::string_view channel_text;
            if (!extract_field(f.channel_field, header, body, channel_text)) return fallback();
            channel = channel_kind_of_text(channel_text);
            if (channel == channel_kind::unknown) return fallback();
        }

        switch (channel) {
            case channel_kind::analysis:
                push_reasoning(body);
                break;
            case channel_kind::final_answer:
                push_text(body, /*tainted=*/false);
                break;
            case channel_kind::commentary: {
                // Never promoted to anything invokable (this file's own top comment). Recipient is
                // used only to make the diagnostic readable; extraction failure still yields an
                // inert, visible item, never a dropped one.
                std::string_view recipient;
                bool const has_recipient = extract_field(f.recipient_field, header, body, recipient);
                std::string_view arguments;
                bool const has_arguments = extract_field(f.arguments_field, header, body, arguments);
                std::string_view constraint = f.default_arg_type;
                (void)extract_field(f.constraint_field, header, body, constraint);  // absent -> keep default
                bool const args_look_valid = (constraint != "json") || (has_arguments && is_valid_json(arguments));

                std::string diagnostic = "[unrecognized tool-call attempt, not executed";
                if (has_recipient && !recipient.empty()) diagnostic += ": " + std::string(recipient);
                if (has_arguments && args_look_valid) diagnostic += "(" + std::string(arguments) + ")";
                diagnostic += "]";
                ContentItem item;
                item.value = Text{std::move(diagnostic)};
                item.origin = content_origin::assistant;
                item.tainted = true;  // 003 §2 / 007 §4: model-originated, policy-relevant-looking text
                items.push_back(std::move(item));

                // Phase 2 (ADR-023 §6 point 4): a candidate is emitted ALONGSIDE the diagnostic above,
                // never instead of it -- the diagnostic Text is what a human/UI sees regardless of
                // whether anything downstream ever attempts declassification. Only when recipient AND
                // arguments both parsed cleanly (a non-empty name, JSON-valid args) -- an incomplete
                // extraction stays diagnostic-only, nothing for a caller to act on.
                if (has_recipient && !recipient.empty() && has_arguments && args_look_valid) {
                    candidates.push_back(DetectedToolCallCandidate{
                        std::string(recipient), std::string(arguments), items.size() - 1});
                }
                break;
            }
            case channel_kind::unknown:
                return fallback();  // unreachable (handled above), kept for -Wswitch completeness
        }

        pos = best_terminator_pos + best_terminator->token.size();
    }
}

}  // namespace agentengine::response_format_codec
