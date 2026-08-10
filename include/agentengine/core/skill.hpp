#pragma once
// Implements 009-Plugin-and-Extension-System.md §8a-8b -- the SKILL.md interchange format (YAML
// frontmatter + Markdown body, agentskills.io/specification) as a value type plus a parser. Pure
// text-in/value-out: no worktree/VFS dependency, no mount, no source (disk/inline) concept -- those
// are `skill_source.hpp`/`skill_provider.hpp`, layered on top of this file, never the other way
// round.
//
// §8a's own words: "The spec is prose plus a constraints table -- no JSON Schema, no RFC-2119
// language, and no version identifier on the specification itself. Our importer therefore validates
// against the constraints explicitly and rejects rather than guessing." That is exactly what
// `parse_skill_md` does below: every named constraint gets its own error code, so a caller (and a
// test) can assert on WHICH rule rejected a document, not just that parsing failed.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/yaml_value.hpp"

namespace agentengine {

// §8a's frontmatter fields. `metadata.version` (upstream convention -- "there is no version field")
// lives in `metadata` like any other key; this type does not special-case it (009 §8a: "our loader
// records the package digest as the real identity and treats metadata.version as a label" -- the
// digest itself is the WORKTREE's concern, `skill_provider.hpp`, not this value type's).
struct SkillMetadata {
    std::string name;                                       // required, validated below
    std::string description;                                 // required, validated below
    std::optional<std::string> license;
    std::optional<std::string> compatibility;
    std::unordered_map<std::string, std::string> metadata;    // arbitrary string->string map
    std::vector<std::string> allowed_tools;                   // §8c: advisory, never a grant
};

// One parsed skill: the frontmatter manifest plus the Markdown instructions body (everything after
// the closing `---`, byte-for-byte, no Markdown parsing -- the body is model-facing prose, not
// something this engine interprets structurally).
struct Skill {
    SkillMetadata frontmatter;
    std::string body;
};

namespace skill_detail {

[[nodiscard]] inline bool is_valid_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// §8a: "1-64 chars, a-z0-9 and hyphens, no leading/trailing hyphen, no --".
[[nodiscard]] inline result<void> validate_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return std::unexpected(error{failure_class::contract,
                                      "skill name must be 1-64 characters", "skill.name_length"});
    }
    if (name.front() == '-' || name.back() == '-') {
        return std::unexpected(error{failure_class::contract,
                                      "skill name must not start or end with a hyphen",
                                      "skill.name_invalid_chars"});
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        char const c = name[i];
        if (c == '-') {
            if (i + 1 < name.size() && name[i + 1] == '-') {
                return std::unexpected(error{failure_class::contract,
                                              "skill name must not contain '--'",
                                              "skill.name_invalid_chars"});
            }
            continue;
        }
        if (!is_valid_name_char(c)) {
            return std::unexpected(error{failure_class::contract,
                                          "skill name must contain only a-z, 0-9, and hyphens",
                                          "skill.name_invalid_chars"});
        }
    }
    return {};
}

// §8a: "1-1024 chars, stating what it does and when to use it" -- length is the only mechanically
// checkable half of that sentence; the "and when to use it" half is a content-quality expectation
// no parser can enforce, and this function does not pretend to.
[[nodiscard]] inline result<void> validate_description(std::string_view description) {
    if (description.empty() || description.size() > 1024) {
        return std::unexpected(error{failure_class::contract,
                                      "skill description must be 1-1024 characters",
                                      "skill.description_length"});
    }
    return {};
}

[[nodiscard]] inline std::vector<std::string> split_on_whitespace(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        std::size_t const start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

// Slices the `---`-delimited frontmatter block out of `file_text`: the first line must be exactly
// `---`, and the next line that is exactly `---` closes it. `yaml::parse` (yaml_value.hpp) has no
// multi-document/delimiter support of its own (by design -- its own file-top comment names this),
// so slicing is this file's job, not a gap in that parser. Returns {frontmatter_text, body_text}.
[[nodiscard]] inline result<std::pair<std::string, std::string>> slice_frontmatter(
    std::string_view file_text) {
    std::size_t pos = 0;
    auto next_line = [&](std::size_t from) -> std::size_t {
        auto const nl = file_text.find('\n', from);
        return nl == std::string_view::npos ? file_text.size() : nl;
    };

    std::size_t const first_end = next_line(pos);
    std::string_view first_line = file_text.substr(pos, first_end - pos);
    if (!first_line.empty() && first_line.back() == '\r') first_line.remove_suffix(1);
    if (first_line != "---") {
        return std::unexpected(error{failure_class::contract,
                                      "SKILL.md must open with a '---' frontmatter delimiter as its "
                                      "first line",
                                      "skill.frontmatter_missing"});
    }
    pos = (first_end < file_text.size()) ? first_end + 1 : first_end;

    std::size_t const frontmatter_start = pos;
    while (pos < file_text.size()) {
        std::size_t const line_end = next_line(pos);
        std::string_view line = file_text.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "---") {
            std::string frontmatter(file_text.substr(frontmatter_start, pos - frontmatter_start));
            std::size_t const body_start = (line_end < file_text.size()) ? line_end + 1 : line_end;
            std::string body(file_text.substr(body_start));
            return std::make_pair(std::move(frontmatter), std::move(body));
        }
        pos = (line_end < file_text.size()) ? line_end + 1 : line_end;
    }
    return std::unexpected(error{failure_class::contract,
                                  "SKILL.md's frontmatter block is never closed with a second '---' "
                                  "line",
                                  "skill.frontmatter_missing"});
}

}  // namespace skill_detail

// Parses one SKILL.md file's full text into a `Skill`. `expected_directory_name`, when non-empty,
// enforces §8a's "must match the directory name" rule -- callers that don't yet know the directory
// (e.g. an inline skill with no filesystem origin at all) simply omit it.
[[nodiscard]] inline result<Skill> parse_skill_md(std::string_view file_text,
                                                    std::string_view expected_directory_name = {}) {
    auto sliced = skill_detail::slice_frontmatter(file_text);
    if (!sliced) return std::unexpected(sliced.error());
    auto& [frontmatter_text, body] = *sliced;

    auto parsed = yaml::parse(frontmatter_text);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->is_object()) {
        return std::unexpected(error{failure_class::contract,
                                      "SKILL.md frontmatter must be a YAML mapping",
                                      "skill.frontmatter_not_a_mapping"});
    }

    Skill skill;
    skill.body = std::move(body);

    auto const* name_field = parsed->find("name");
    if (!name_field || !name_field->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "SKILL.md frontmatter is missing required field 'name'",
                  "skill.name_missing"});
    }
    if (auto v = skill_detail::validate_name(name_field->as_string()); !v) {
        return std::unexpected(v.error());
    }
    skill.frontmatter.name = name_field->as_string();

    if (!expected_directory_name.empty() && skill.frontmatter.name != expected_directory_name) {
        return std::unexpected(error{failure_class::contract,
                                      "skill name '" + skill.frontmatter.name +
                                          "' does not match its directory name '" +
                                          std::string(expected_directory_name) + "'",
                                      "skill.name_directory_mismatch"});
    }

    auto const* description_field = parsed->find("description");
    if (!description_field || !description_field->is_string()) {
        return std::unexpected(error{failure_class::contract,
                                      "SKILL.md frontmatter is missing required field 'description'",
                                      "skill.description_missing"});
    }
    if (auto v = skill_detail::validate_description(description_field->as_string()); !v) {
        return std::unexpected(v.error());
    }
    skill.frontmatter.description = description_field->as_string();

    if (auto const* f = parsed->find("license"); f && f->is_string()) {
        skill.frontmatter.license = f->as_string();
    }
    if (auto const* f = parsed->find("compatibility"); f && f->is_string()) {
        skill.frontmatter.compatibility = f->as_string();
    }
    if (auto const* f = parsed->find("metadata"); f && f->is_object()) {
        for (auto const& [key, value] : f->as_object()) {
            if (value.is_string()) skill.frontmatter.metadata.emplace(key, value.as_string());
        }
    }
    if (auto const* f = parsed->find("allowed-tools"); f && f->is_string()) {
        skill.frontmatter.allowed_tools = skill_detail::split_on_whitespace(f->as_string());
    }

    return skill;
}

}  // namespace agentengine
