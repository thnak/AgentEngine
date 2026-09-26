// Implements 009-Plugin-and-Extension-System.md §8a -- proves core/skill.hpp's `parse_skill_md`
// against a valid SKILL.md document and against one real failure per named validation error code
// (§8a's "our importer therefore validates against the constraints explicitly and rejects rather
// than guessing," made concrete: a test per rejection, not just a test that SOME rejection happens).

#include <cstdio>
#include <string>

#include "agentengine/core/skill.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    // ---- R1: a valid, complete document parses cleanly ---------------------------------------------
    {
        std::string const doc =
            "---\n"
            "name: pdf-tools\n"
            "description: Extract text and tables from PDF files.\n"
            "license: Apache-2.0\n"
            "compatibility: \">=1.0\"\n"
            "metadata:\n"
            "  version: \"2\"\n"
            "  author: acme\n"
            "allowed-tools: execute_code execute_shell\n"
            "---\n"
            "# PDF tools\n"
            "\n"
            "Use `execute_code` to load a PDF and extract its text.\n";
        auto skill = parse_skill_md(doc, "pdf-tools");
        check(skill.has_value(), "R1: a valid, complete SKILL.md parses");
        if (skill) {
            check(skill->frontmatter.name == "pdf-tools", "R1: name round-trips");
            check(skill->frontmatter.description == "Extract text and tables from PDF files.",
                  "R1: description round-trips");
            check(skill->frontmatter.license == "Apache-2.0", "R1: optional license round-trips");
            check(skill->frontmatter.compatibility == ">=1.0", "R1: optional compatibility round-trips");
            check(skill->frontmatter.metadata.at("version") == "2" &&
                      skill->frontmatter.metadata.at("author") == "acme",
                  "R1: metadata map round-trips both entries");
            check(skill->frontmatter.allowed_tools.size() == 2 &&
                      skill->frontmatter.allowed_tools[0] == "execute_code" &&
                      skill->frontmatter.allowed_tools[1] == "execute_shell",
                  "R1: allowed-tools splits on whitespace into two entries, order preserved");
            check(skill->body.find("# PDF tools") != std::string::npos &&
                      skill->body.find("execute_code") != std::string::npos,
                  "R1: the Markdown body (everything after the closing '---') is preserved verbatim");
        }
    }

    // ---- R2: no directory-match constraint when none is supplied -----------------------------------
    {
        std::string const doc = "---\nname: standalone\ndescription: A skill with no directory context.\n---\nBody.\n";
        auto skill = parse_skill_md(doc);
        check(skill.has_value(),
              "R2: omitting expected_directory_name skips the directory-match check entirely");
    }

    // ---- R3: missing opening '---' -------------------------------------------------------------------
    {
        auto skill = parse_skill_md("name: x\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.frontmatter_missing",
              "R3: a document not opening with a bare '---' line fails with frontmatter_missing");
    }

    // ---- R4: unterminated frontmatter block ------------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: x\ndescription: y\nno closing delimiter here\n");
        check(!skill.has_value() && skill.error().code == "skill.frontmatter_missing",
              "R4: a frontmatter block never closed with a second '---' fails with frontmatter_missing");
    }

    // ---- R5: missing 'name' -------------------------------------------------------------------------
    {
        auto skill = parse_skill_md("---\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_missing",
              "R5: a frontmatter with no 'name' field fails with name_missing");
    }

    // ---- R6: name too long (>64 chars) ---------------------------------------------------------------
    {
        std::string const long_name(65, 'a');
        auto skill =
            parse_skill_md("---\nname: " + long_name + "\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_length",
              "R6: a 65-character name fails with name_length");
    }

    // ---- R7: name with invalid characters (uppercase) -------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: PDF-Tools\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_invalid_chars",
              "R7: a name containing uppercase letters fails with name_invalid_chars");
    }

    // ---- R8: name with leading hyphen ------------------------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: -pdf-tools\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_invalid_chars",
              "R8: a name starting with a hyphen fails with name_invalid_chars");
    }

    // ---- R9: name with trailing hyphen -----------------------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: pdf-tools-\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_invalid_chars",
              "R9: a name ending with a hyphen fails with name_invalid_chars");
    }

    // ---- R10: name with a double hyphen ----------------------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: pdf--tools\ndescription: y\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.name_invalid_chars",
              "R10: a name containing '--' fails with name_invalid_chars");
    }

    // ---- R11: name doesn't match the expected directory -------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: pdf-tools\ndescription: y\n---\nbody\n", "different-dir");
        check(!skill.has_value() && skill.error().code == "skill.name_directory_mismatch",
              "R11: a name that doesn't match expected_directory_name fails with name_directory_mismatch "
              "-- §8a's own 'must match the directory name' rule");
    }

    // ---- R12: missing 'description' ------------------------------------------------------------------
    {
        auto skill = parse_skill_md("---\nname: pdf-tools\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.description_missing",
              "R12: a frontmatter with no 'description' field fails with description_missing");
    }

    // ---- R13: description too long (>1024 chars) -------------------------------------------------------
    {
        std::string const long_desc(1025, 'x');
        auto skill =
            parse_skill_md("---\nname: pdf-tools\ndescription: " + long_desc + "\n---\nbody\n");
        check(!skill.has_value() && skill.error().code == "skill.description_length",
              "R13: a 1025-character description fails with description_length");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_skill_parser: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_parser: %d FAILURE(S)\n", g_failures);
    return 1;
}
