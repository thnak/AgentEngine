#pragma once
// A small, dependency-free JSON value type + parser/serializer. CONVENTIONS.md's dependency-tier
// discipline keeps nlohmann::json test-only (tests/CMakeLists.txt: "never linked to
// agentengine_core or anything under include/agentengine/core") -- but 006 §3 step 2 ("validate
// arguments vs schema... reject, do not coerce") and step 9 ("normalize result -> parts") are real
// product-code JSON (de)serialization the tool pipeline needs at runtime, not just at test time.
// This is that seam: std-only, reflection-free, matching the project's existing "no third-party
// dependency in core" posture (CONVENTIONS.md).
//
// Not a general-purpose JSON library: numbers are `double` only (no separate int/float paths --
// json_schema.hpp's codec narrows on the way out), and object key order is preserved but lookup is
// linear (schemas here have single-digit field counts, so this is not a hot path).

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::json {

// ae-naming-lint: allow value_kind — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class value_kind { null, boolean, number, string, array, object };

// ae-naming-lint: allow Value — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class Value {
public:
    Value() : data_(std::monostate{}) {}

    static Value make_null() { return Value(); }
    static Value make_bool(bool b) {
        Value v;
        v.data_ = b;
        return v;
    }
    static Value make_number(double n) {
        Value v;
        v.data_ = n;
        return v;
    }
    static Value make_string(std::string s) {
        Value v;
        v.data_ = std::move(s);
        return v;
    }
    static Value make_array(std::vector<Value> a) {
        Value v;
        v.data_ = std::move(a);
        return v;
    }
    static Value make_object(std::vector<std::pair<std::string, Value>> o) {
        Value v;
        v.data_ = std::move(o);
        return v;
    }

    [[nodiscard]] value_kind kind() const noexcept { return static_cast<value_kind>(data_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return kind() == value_kind::null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind() == value_kind::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return kind() == value_kind::number; }
    [[nodiscard]] bool is_string() const noexcept { return kind() == value_kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return kind() == value_kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return kind() == value_kind::object; }

    [[nodiscard]] bool as_bool() const { return std::get<bool>(data_); }
    [[nodiscard]] double as_number() const { return std::get<double>(data_); }
    [[nodiscard]] std::string const& as_string() const { return std::get<std::string>(data_); }
    [[nodiscard]] std::vector<Value> const& as_array() const { return std::get<std::vector<Value>>(data_); }
    [[nodiscard]] std::vector<std::pair<std::string, Value>> const& as_object() const {
        return std::get<std::vector<std::pair<std::string, Value>>>(data_);
    }

    // nullptr if not an object or the key is absent -- never throws, so callers can chain lookups
    // through an optional shape without a try/catch (006 §3 step 2's "reject, do not coerce" is
    // enforced by the caller checking for nullptr, not by this throwing).
    [[nodiscard]] Value const* find(std::string_view key) const noexcept {
        if (!is_object()) return nullptr;
        for (auto const& [k, v] : as_object()) {
            if (k == key) return &v;
        }
        return nullptr;
    }

private:
    std::variant<std::monostate, bool, double, std::string, std::vector<Value>,
                 std::vector<std::pair<std::string, Value>>>
        data_;
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    result<Value> parse() {
        skip_ws();
        auto v = parse_value();
        if (!v) return v;
        skip_ws();
        if (pos_ != text_.size()) {
            return std::unexpected(error{failure_class::contract, "trailing content after JSON value",
                                          "json.trailing_content"});
        }
        return v;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return text_[pos_]; }

    void skip_ws() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }

    static result<Value> fail(std::string_view code, std::string_view message) {
        return std::unexpected(error{failure_class::contract, std::string(message), std::string(code)});
    }

    result<Value> parse_value() {
        if (at_end()) return fail("json.unexpected_eof", "unexpected end of input");
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string_value();
            case 't':
            case 'f': return parse_bool();
            case 'n': return parse_null();
            default: return parse_number();
        }
    }

    result<Value> parse_null() {
        if (!consume_literal("null")) return fail("json.malformed_literal", "expected 'null'");
        return Value::make_null();
    }

    result<Value> parse_bool() {
        if (peek() == 't') {
            if (!consume_literal("true")) return fail("json.malformed_literal", "expected 'true'");
            return Value::make_bool(true);
        }
        if (!consume_literal("false")) return fail("json.malformed_literal", "expected 'false'");
        return Value::make_bool(false);
    }

    bool consume_literal(std::string_view lit) {
        if (text_.substr(pos_).substr(0, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    result<Value> parse_number() {
        std::size_t start = pos_;
        if (!at_end() && peek() == '-') ++pos_;
        if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            return fail("json.malformed_number", "expected a digit");
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        if (!at_end() && peek() == '.') {
            ++pos_;
            if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return fail("json.malformed_number", "expected a digit after '.'");
            }
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
            if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return fail("json.malformed_number", "expected a digit in exponent");
            }
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        std::string_view digits = text_.substr(start, pos_ - start);
        double value = 0.0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
            return fail("json.malformed_number", "could not parse number");
        }
        return Value::make_number(value);
    }

    result<std::string> parse_string_raw() {
        if (at_end() || peek() != '"') {
            return std::unexpected(error{failure_class::contract, "expected '\"'", "json.expected_string"});
        }
        ++pos_;
        std::string out;
        out.reserve(text_.size() - pos_);  // safe upper bound: remaining input can't be shorter
        while (true) {
            if (at_end()) {
                return std::unexpected(
                    error{failure_class::contract, "unterminated string", "json.unterminated_string"});
            }
            char c = peek();
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                if (at_end()) {
                    return std::unexpected(
                        error{failure_class::contract, "unterminated escape", "json.unterminated_escape"});
                }
                char esc = peek();
                ++pos_;
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) {
                            return std::unexpected(error{failure_class::contract,
                                                          "truncated \\u escape", "json.bad_unicode_escape"});
                        }
                        std::uint32_t cp = 0;
                        auto [ptr, ec] =
                            std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, cp, 16);
                        if (ec != std::errc{} || ptr != text_.data() + pos_ + 4) {
                            return std::unexpected(error{failure_class::contract,
                                                          "invalid \\u escape", "json.bad_unicode_escape"});
                        }
                        pos_ += 4;
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return std::unexpected(
                            error{failure_class::contract, "unknown escape", "json.bad_escape"});
                }
                continue;
            }
            out += c;
            ++pos_;
        }
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    result<Value> parse_string_value() {
        auto s = parse_string_raw();
        if (!s) return std::unexpected(s.error());
        return Value::make_string(std::move(*s));
    }

    result<Value> parse_array() {
        ++pos_;  // consume '['
        std::vector<Value> items;
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return Value::make_array(std::move(items));
        }
        while (true) {
            skip_ws();
            auto v = parse_value();
            if (!v) return v;
            items.push_back(std::move(*v));
            skip_ws();
            if (at_end()) return fail("json.unexpected_eof", "unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return Value::make_array(std::move(items));
            }
            return fail("json.malformed_array", "expected ',' or ']'");
        }
    }

    result<Value> parse_object() {
        ++pos_;  // consume '{'
        std::vector<std::pair<std::string, Value>> members;
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return Value::make_object(std::move(members));
        }
        while (true) {
            skip_ws();
            auto key = parse_string_raw();
            if (!key) return std::unexpected(key.error());
            skip_ws();
            if (at_end() || peek() != ':') return fail("json.malformed_object", "expected ':'");
            ++pos_;
            skip_ws();
            auto v = parse_value();
            if (!v) return v;
            members.emplace_back(std::move(*key), std::move(*v));
            skip_ws();
            if (at_end()) return fail("json.unexpected_eof", "unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return Value::make_object(std::move(members));
            }
            return fail("json.malformed_object", "expected ',' or '}'");
        }
    }
};

inline void dump_escaped_string(std::string const& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

inline void dump_into(Value const& v, std::string& out) {
    switch (v.kind()) {
        case value_kind::null: out += "null"; break;
        case value_kind::boolean: out += v.as_bool() ? "true" : "false"; break;
        case value_kind::number: {
            double n = v.as_number();
            if (std::isfinite(n) && n == std::floor(n) && std::abs(n) < 1e15) {
                out += std::to_string(static_cast<std::int64_t>(n));
            } else {
                char buf[64];
                auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), n);
                out.append(buf, ptr);
            }
            break;
        }
        case value_kind::string: dump_escaped_string(v.as_string(), out); break;
        case value_kind::array: {
            out += '[';
            bool first = true;
            for (auto const& item : v.as_array()) {
                if (!first) out += ',';
                first = false;
                dump_into(item, out);
            }
            out += ']';
            break;
        }
        case value_kind::object: {
            out += '{';
            bool first = true;
            for (auto const& [k, val] : v.as_object()) {
                if (!first) out += ',';
                first = false;
                dump_escaped_string(k, out);
                out += ':';
                dump_into(val, out);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace detail

[[nodiscard]] inline result<Value> parse(std::string_view text) { return detail::Parser(text).parse(); }

[[nodiscard]] inline std::string dump(Value const& v) {
    std::string out;
    out.reserve(256);  // modest head-start; no cheap exact bound exists, just cuts reallocation churn
    detail::dump_into(v, out);
    return out;
}

}  // namespace agentengine::json
