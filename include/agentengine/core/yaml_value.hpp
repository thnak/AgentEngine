#pragma once
// Implements 015-Declarative-Agent-Format.md's YAML surface -- Milestone 7 Phase F1
// (docs/planning/milestone-7-protocol-conformance-breakdown.md). A hand-rolled, DELIBERATELY SCOPED
// YAML SUBSET parser, matching CONVENTIONS' core-tier discipline ("std + Quark only, no third-party
// dependency, ever") the same way `core/json_value.hpp` already does for JSON -- no YAML library
// exists anywhere in this codebase or is vendored here.
//
// Parses INTO `agentengine::json::Value` directly, not a separate YAML value type: YAML's data model
// (mappings, sequences, scalars) maps cleanly onto JSON's for the subset this parser targets, so every
// downstream consumer (a future 015 §2/§3 document compiler) works with the exact same `json::Value`
// API MCP/A2A/AG-UI already use, rather than a second tree type needing its own accessors.
//
// SCOPE (proven against 015's own §2 Agent / §3 Workflow example documents, which mix both styles):
//   - Block-style structure: indentation-based mappings (`key: value` lines) and sequences (`- item`
//     lines), including a sequence item that ITSELF opens a mapping on the same line (`- id: planner,
//     agent: researcher` written across a `- key: value` line plus further same-indent `key: value`
//     continuation lines -- 015 §3's own `executors`/`edges` entries are exactly this shape).
//   - Flow-style collections: `{ ... }` / `[ ... ]`, YAML-flavored (unquoted keys/values allowed,
//     unlike strict JSON) -- 015 §2's own `options: { temperature: 0.2 }` and
//     `fallback: [native-jail, remote]` are exactly this shape.
//   - Block literal scalars (`key: |` followed by more-indented lines) -- 015 §2's own multi-line
//     `instructions:` block. Default ("clip") chomping only: lines joined with `\n`, one trailing
//     newline. No explicit indentation indicator (`|2`) or chomping indicator (`|-`/`|+`) support.
//   - Plain (unquoted) and single/double-quoted scalars; `true`/`false`/`null`/`~`; integers and
//     floats (delegated to the same numeric parsing `json::Value` itself uses).
//   - `#` comments to end of line (outside quotes), blank lines ignored.
//   - Indentation is SPACES ONLY -- a literal tab in leading whitespace is a parse error (real YAML's
//     own rule: "tab characters... cannot be used for indentation").
//
// NOT built (named, not silently accepted as if supported): anchors/aliases (`&name`/`*name`), tags
// (`!!type`), multi-document streams (`---`/`...`), flow collections spanning awkward multi-line
// continuations beyond simple bracket matching, explicit block-scalar indentation/chomping indicators,
// merge keys (`<<:`). A document using any of these is rejected with a real parse error, never
// silently misparsed into something else.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::yaml {

namespace detail {

struct Line {
    int         indent = 0;
    std::string content;  // comment-stripped, right-trimmed, indentation removed
    std::size_t line_no  = 0;
};

[[nodiscard]] inline result<void> fail(std::size_t line_no, std::string_view what) {
    return std::unexpected(error{failure_class::contract,
                                  "yaml parse error at line " + std::to_string(line_no) + ": " +
                                      std::string(what),
                                  "yaml.parse_error"});
}

// True if `c` is outside any quote at this point in a left-to-right scan -- used to find the FIRST
// unquoted occurrence of a delimiter (`:`, `#`) without being fooled by one inside a string.
class QuoteTracker {
public:
    void feed(char c) {
        if (in_single_) {
            if (c == '\'') in_single_ = false;
        } else if (in_double_) {
            if (escaped_) {
                escaped_ = false;
            } else if (c == '\\') {
                escaped_ = true;
            } else if (c == '"') {
                in_double_ = false;
            }
        } else {
            if (c == '\'') in_single_ = true;
            else if (c == '"') in_double_ = true;
        }
    }
    [[nodiscard]] bool in_quotes() const noexcept { return in_single_ || in_double_; }

private:
    bool in_single_ = false;
    bool in_double_ = false;
    bool escaped_   = false;
};

[[nodiscard]] inline std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}
[[nodiscard]] inline std::string_view ltrim(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

// Splits raw document text into indentation-tracked, comment-stripped `Line`s. Blank and
// comment-only lines are dropped entirely (they carry no structural meaning for this subset).
[[nodiscard]] inline result<std::vector<Line>> preprocess(std::string_view text) {
    std::vector<Line> lines;
    std::size_t pos = 0, line_no = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view raw = (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        ++line_no;

        // Strip a trailing '\r' (CRLF tolerance) before anything else.
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);

        std::size_t indent = 0;
        while (indent < raw.size() && (raw[indent] == ' ' || raw[indent] == '\t')) {
            if (raw[indent] == '\t') {
                return std::unexpected(
                    fail(line_no, "tab character in indentation -- YAML indentation is spaces only")
                        .error());
            }
            ++indent;
        }
        std::string_view rest = raw.substr(indent);

        // Strip a `#` comment -- only when unquoted and either at the start or preceded by whitespace
        // (real YAML's own rule: `#` inside a bareword like `a#b` is not a comment marker).
        QuoteTracker qt;
        std::size_t comment_at = std::string_view::npos;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            qt.feed(rest[i]);
            if (rest[i] == '#' && !qt.in_quotes() && (i == 0 || rest[i - 1] == ' ')) {
                comment_at = i;
                break;
            }
        }
        std::string_view content_view = (comment_at == std::string_view::npos) ? rest : rest.substr(0, comment_at);
        std::string content = rtrim(std::string(content_view));

        if (!content.empty()) {
            lines.push_back(Line{static_cast<int>(indent), std::move(content), line_no});
        }

        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// Finds the first unquoted top-level `:` followed by a space or end-of-string (the mapping
// key/value delimiter) -- never one inside quotes or inside a nested flow collection.
[[nodiscard]] inline std::size_t find_key_colon(std::string_view s) {
    QuoteTracker qt;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        qt.feed(c);
        if (qt.in_quotes()) continue;
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        else if (c == ':' && depth == 0 && (i + 1 == s.size() || s[i + 1] == ' ')) return i;
    }
    return std::string_view::npos;
}

[[nodiscard]] inline bool looks_like_mapping_key(std::string_view s) {
    return find_key_colon(s) != std::string_view::npos;
}

[[nodiscard]] inline result<json::Value> parse_flow(std::string_view text, std::size_t line_no);
[[nodiscard]] inline result<json::Value> parse_scalar(std::string_view text, std::size_t line_no);

// Parses one flow-style token starting at `pos` (a flow collection, a quoted string, or a bare
// scalar run up to the next unquoted `,`/`}`/`]`), advancing `pos` past it.
[[nodiscard]] inline result<json::Value> parse_flow_value(std::string_view s, std::size_t& pos,
                                                            std::size_t line_no) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    if (pos >= s.size()) return std::unexpected(fail(line_no, "unexpected end of flow value").error());

    if (s[pos] == '{' || s[pos] == '[') {
        bool const is_object = s[pos] == '{';
        char const close      = is_object ? '}' : ']';
        int depth = 1;
        std::size_t start = pos;
        QuoteTracker qt;
        ++pos;
        while (pos < s.size() && depth > 0) {
            qt.feed(s[pos]);
            if (!qt.in_quotes()) {
                if (s[pos] == (is_object ? '{' : '[')) ++depth;
                else if (s[pos] == close) --depth;
            }
            ++pos;
        }
        if (depth != 0) return std::unexpected(fail(line_no, "unterminated flow collection").error());
        return parse_flow(s.substr(start, pos - start), line_no);
    }

    if (s[pos] == '\'' || s[pos] == '"') {
        char const quote = s[pos];
        std::size_t start = pos;
        ++pos;
        while (pos < s.size()) {
            if (quote == '"' && s[pos] == '\\' && pos + 1 < s.size()) { pos += 2; continue; }
            if (s[pos] == quote) {
                if (quote == '\'' && pos + 1 < s.size() && s[pos + 1] == '\'') { pos += 2; continue; }
                ++pos;
                break;
            }
            ++pos;
        }
        return parse_scalar(s.substr(start, pos - start), line_no);
    }

    std::size_t start = pos;
    while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') ++pos;
    std::string_view raw = s.substr(start, pos - start);
    while (!raw.empty() && raw.back() == ' ') raw.remove_suffix(1);
    return parse_scalar(raw, line_no);
}

// Parses a complete `{ ... }` / `[ ... ]` flow collection (braces/brackets included).
[[nodiscard]] inline result<json::Value> parse_flow(std::string_view text, std::size_t line_no) {
    std::string_view s = ltrim(text);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty() || (s.front() != '{' && s.front() != '[')) {
        return std::unexpected(fail(line_no, "expected a flow collection opener").error());
    }
    bool const is_object = s.front() == '{';
    if (s.back() != (is_object ? '}' : ']')) {
        return std::unexpected(fail(line_no, "flow collection is not properly closed").error());
    }
    std::string_view inner = s.substr(1, s.size() - 2);

    if (is_object) {
        std::vector<std::pair<std::string, json::Value>> members;
        std::size_t pos = 0;
        while (true) {
            while (pos < inner.size() && inner[pos] == ' ') ++pos;
            if (pos >= inner.size()) break;
            std::size_t colon = find_key_colon(inner.substr(pos));
            if (colon == std::string_view::npos) {
                return std::unexpected(fail(line_no, "flow mapping entry missing ':'").error());
            }
            std::string_view key_raw = inner.substr(pos, colon);
            while (!key_raw.empty() && key_raw.back() == ' ') key_raw.remove_suffix(1);
            auto key_value = parse_scalar(key_raw, line_no);
            if (!key_value) return std::unexpected(key_value.error());
            std::string key = key_value->is_string() ? key_value->as_string() : json::dump(*key_value);
            pos += colon + 1;
            auto val = parse_flow_value(inner, pos, line_no);
            if (!val) return std::unexpected(val.error());
            members.emplace_back(std::move(key), std::move(*val));
            while (pos < inner.size() && inner[pos] == ' ') ++pos;
            if (pos < inner.size() && inner[pos] == ',') { ++pos; continue; }
            break;
        }
        return json::Value::make_object(std::move(members));
    }

    std::vector<json::Value> items;
    std::size_t pos = 0;
    while (true) {
        while (pos < inner.size() && inner[pos] == ' ') ++pos;
        if (pos >= inner.size()) break;
        auto val = parse_flow_value(inner, pos, line_no);
        if (!val) return std::unexpected(val.error());
        items.push_back(std::move(*val));
        while (pos < inner.size() && inner[pos] == ' ') ++pos;
        if (pos < inner.size() && inner[pos] == ',') { ++pos; continue; }
        break;
    }
    return json::Value::make_array(std::move(items));
}

[[nodiscard]] inline bool is_number_literal(std::string_view s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    bool seen_digit = false, seen_dot = false;
    for (; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) { seen_digit = true; continue; }
        if (s[i] == '.' && !seen_dot) { seen_dot = true; continue; }
        return false;
    }
    return seen_digit;
}

// Parses one scalar: quoted string, `true`/`false`/`null`/`~`, a number, or a plain (bareword)
// string -- YAML's own scalar-resolution rules, the subset this file supports.
[[nodiscard]] inline result<json::Value> parse_scalar(std::string_view text, std::size_t line_no) {
    std::string_view s = ltrim(text);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return json::Value::make_null();

    if (s.front() == '"') {
        if (s.size() < 2 || s.back() != '"') {
            return std::unexpected(fail(line_no, "unterminated double-quoted string").error());
        }
        // Reuse json::Value's own string parsing for real backslash-escape handling -- a
        // double-quoted YAML scalar's escape rules are the same superset JSON already implements.
        auto parsed = json::parse(s);
        if (!parsed || !parsed->is_string()) {
            return std::unexpected(fail(line_no, "malformed double-quoted string").error());
        }
        return parsed;
    }
    if (s.front() == '\'') {
        if (s.size() < 2 || s.back() != '\'') {
            return std::unexpected(fail(line_no, "unterminated single-quoted string").error());
        }
        std::string out;
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\'' && i + 1 < s.size() - 1 && s[i + 1] == '\'') { out += '\''; ++i; continue; }
            out += s[i];
        }
        return json::Value::make_string(std::move(out));
    }
    if (s.front() == '{' || s.front() == '[') return parse_flow(s, line_no);

    if (s == "true") return json::Value::make_bool(true);
    if (s == "false") return json::Value::make_bool(false);
    if (s == "null" || s == "~") return json::Value::make_null();
    if (is_number_literal(s)) {
        auto parsed = json::parse(s);
        if (parsed && parsed->is_number()) return parsed;
    }
    return json::Value::make_string(std::string(s));
}

[[nodiscard]] result<json::Value> parse_block(std::vector<Line> const& lines, std::size_t& idx, int indent);

// A sequence item's own content after "- " opens a mapping continued by further same-indent
// `key: value` lines (015 §3's own `executors`/`edges` shape) -- builds that mapping starting from
// the inline first key, then consumes subsequent lines at `item_indent` as more keys of it.
[[nodiscard]] inline result<json::Value> parse_inline_mapping_item(std::string_view first_key_line,
                                                                     int item_indent,
                                                                     std::vector<Line> const& lines,
                                                                     std::size_t& idx, std::size_t line_no) {
    std::vector<Line> synthetic;
    synthetic.push_back(Line{item_indent, std::string(first_key_line), line_no});
    while (idx < lines.size() && lines[idx].indent == item_indent && looks_like_mapping_key(lines[idx].content)) {
        synthetic.push_back(lines[idx]);
        ++idx;
    }
    std::size_t sub_idx = 0;
    return parse_block(synthetic, sub_idx, item_indent);
}

[[nodiscard]] inline result<json::Value> parse_sequence(std::vector<Line> const& lines, std::size_t& idx,
                                                          int indent) {
    std::vector<json::Value> items;
    while (idx < lines.size() && lines[idx].indent == indent && lines[idx].content[0] == '-' &&
           (lines[idx].content.size() == 1 || lines[idx].content[1] == ' ')) {
        Line const& line = lines[idx];
        std::string_view after_dash = std::string_view(line.content).substr(line.content.size() == 1 ? 1 : 2);
        int const item_indent = indent + static_cast<int>(line.content.size() == 1 ? 1 : 2);
        ++idx;
        if (after_dash.empty()) {
            // The value is a nested block on subsequent, deeper-indented lines.
            if (idx < lines.size() && lines[idx].indent > indent) {
                auto v = parse_block(lines, idx, lines[idx].indent);
                if (!v) return v;
                items.push_back(std::move(*v));
            } else {
                items.push_back(json::Value::make_null());
            }
        } else if (looks_like_mapping_key(after_dash)) {
            auto v = parse_inline_mapping_item(after_dash, item_indent, lines, idx, line.line_no);
            if (!v) return v;
            items.push_back(std::move(*v));
        } else if (after_dash == std::string_view("|")) {
            return std::unexpected(
                fail(line.line_no,
                     "block literal scalar directly under a sequence item is not supported")
                    .error());
        } else {
            auto v = parse_scalar(after_dash, line.line_no);
            if (!v) return v;
            items.push_back(std::move(*v));
        }
    }
    return json::Value::make_array(std::move(items));
}

// Collects a `|` block literal scalar's own body -- every subsequent line strictly deeper-indented
// than `key_indent`, joined with '\n', default ("clip") chomping.
[[nodiscard]] inline std::string collect_block_literal(std::vector<Line> const& lines, std::size_t& idx,
                                                        int key_indent) {
    if (idx >= lines.size() || lines[idx].indent <= key_indent) return "";
    int const base_indent = lines[idx].indent;
    std::string out;
    while (idx < lines.size() && lines[idx].indent >= base_indent) {
        out += std::string(lines[idx].indent - base_indent, ' ');
        out += lines[idx].content;
        out += '\n';
        ++idx;
    }
    return out;
}

[[nodiscard]] inline result<json::Value> parse_mapping(std::vector<Line> const& lines, std::size_t& idx,
                                                         int indent) {
    std::vector<std::pair<std::string, json::Value>> members;
    while (idx < lines.size() && lines[idx].indent == indent && looks_like_mapping_key(lines[idx].content)) {
        Line const& line = lines[idx];
        std::size_t colon = find_key_colon(line.content);
        std::string_view key_raw = std::string_view(line.content).substr(0, colon);
        auto key_value = parse_scalar(key_raw, line.line_no);
        if (!key_value) return std::unexpected(key_value.error());
        std::string key = key_value->is_string() ? key_value->as_string() : json::dump(*key_value);

        std::string_view value_raw = ltrim(std::string_view(line.content).substr(colon + 1));
        ++idx;

        if (value_raw.empty()) {
            if (idx < lines.size() && lines[idx].indent > indent) {
                auto v = parse_block(lines, idx, lines[idx].indent);
                if (!v) return v;
                members.emplace_back(std::move(key), std::move(*v));
            } else {
                members.emplace_back(std::move(key), json::Value::make_null());
            }
        } else if (value_raw == std::string_view("|")) {
            members.emplace_back(std::move(key),
                                  json::Value::make_string(collect_block_literal(lines, idx, indent)));
        } else {
            auto v = parse_scalar(value_raw, line.line_no);
            if (!v) return v;
            members.emplace_back(std::move(key), std::move(*v));
        }
    }
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<json::Value> parse_block(std::vector<Line> const& lines, std::size_t& idx,
                                                       int indent) {
    if (idx >= lines.size()) return json::Value::make_null();
    if (lines[idx].indent != indent) {
        return std::unexpected(fail(lines[idx].line_no, "unexpected indentation").error());
    }
    Line const& first = lines[idx];
    bool const is_seq = first.content[0] == '-' && (first.content.size() == 1 || first.content[1] == ' ');
    if (is_seq) return parse_sequence(lines, idx, indent);
    if (looks_like_mapping_key(first.content)) return parse_mapping(lines, idx, indent);
    // A bare scalar document (no key, no dash) -- only sensible as a whole-document value.
    auto v = parse_scalar(first.content, first.line_no);
    ++idx;
    return v;
}

}  // namespace detail

// Parses a document in this file's own supported YAML subset (see file-top comment) into a
// `json::Value` tree -- a mapping/sequence/scalar structurally identical to what parsing the
// equivalent JSON document would produce.
[[nodiscard]] inline result<json::Value> parse(std::string_view text) {
    auto lines = detail::preprocess(text);
    if (!lines) return std::unexpected(lines.error());
    if (lines->empty()) return json::Value::make_null();
    std::size_t idx = 0;
    int const root_indent = (*lines)[0].indent;
    auto result = detail::parse_block(*lines, idx, root_indent);
    if (!result) return result;
    if (idx != lines->size()) {
        return std::unexpected(
            detail::fail((*lines)[idx].line_no,
                         "unexpected content -- indentation does not match any enclosing block "
                         "(possibly a second root-level document, not supported by this parser)")
                .error());
    }
    return result;
}

}  // namespace agentengine::yaml
