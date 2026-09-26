// Implements 009-Plugin-and-Extension-System.md §8 -- proves core/skill_source.hpp's
// `DiskSkillSource` against a REAL temp directory on the real host filesystem: the "load from disk"
// half of the user's MAF-parity ask. A subdirectory with no SKILL.md is skipped, not an error; a
// subdirectory WITH an invalid SKILL.md fails the whole load_skills() call (all-or-nothing).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/skill_source.hpp"

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

void write_file(std::filesystem::path const& path, std::string const& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

int main() {
    std::filesystem::path const root =
        std::filesystem::temp_directory_path() / "ae_test_skill_source_disk";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // clean slate -- a prior interrupted run may have left this

    // A real, valid skill directory with a bundled reference file.
    write_file(root / "pdf-tools" / "SKILL.md",
               "---\nname: pdf-tools\ndescription: Extract text from PDF files.\n---\nUse execute_code.\n");
    write_file(root / "pdf-tools" / "references" / "api.md", "# API\n\nfoo(bar) -> baz\n");

    // A second real, valid skill directory (no bundle files at all).
    write_file(root / "csv-tools" / "SKILL.md",
               "---\nname: csv-tools\ndescription: Parse and transform CSV files.\n---\nUse execute_code.\n");

    // A stray subdirectory with no SKILL.md at all -- must be silently skipped, not an error.
    write_file(root / "not-a-skill" / "readme.txt", "just some other content, not a skill");

    {
        DiskSkillSource source("local", root);
        check(source.origin_id() == "local", "R1: origin_id() round-trips");

        auto loaded = source.load_skills();
        check(loaded.has_value(), "R1: a real host directory with two valid skill dirs loads cleanly");
        if (loaded) {
            check(loaded->size() == 2,
                  "R1: exactly two skills loaded -- the stray non-skill subdirectory was skipped, not "
                  "counted and not an error");

            auto find_by_name = [&](std::string const& name) -> SkillSourceResult const* {
                for (auto const& r : *loaded) {
                    if (r.skill.frontmatter.name == name) return &r;
                }
                return nullptr;
            };

            auto const* pdf = find_by_name("pdf-tools");
            check(pdf != nullptr, "R1: pdf-tools was loaded");
            if (pdf) {
                check(pdf->skill.frontmatter.description == "Extract text from PDF files.",
                      "R1: pdf-tools' description round-trips from the real file");
                bool found_manifest = false, found_reference = false;
                std::string reference_bytes;
                for (auto const& f : pdf->files) {
                    if (f.relative_path == "SKILL.md") found_manifest = true;
                    if (f.relative_path == "references/api.md") {
                        found_reference = true;
                        reference_bytes.assign(reinterpret_cast<char const*>(f.bytes.data()), f.bytes.size());
                    }
                }
                check(found_manifest,
                      "R1: pdf-tools' bundle includes SKILL.md itself, readable back like any other file");
                check(found_reference && reference_bytes == "# API\n\nfoo(bar) -> baz\n",
                      "R1: pdf-tools' references/api.md is present and its bytes round-trip EXACTLY, "
                      "byte-for-byte, from the real host file");
            }

            auto const* csv = find_by_name("csv-tools");
            check(csv != nullptr && csv->files.size() == 1,
                  "R1: csv-tools loaded with exactly one file (SKILL.md itself, no scripts/references/"
                  "assets subdirectories present on disk for it)");
        }
    }

    // ---- R2: a subdirectory with an INVALID SKILL.md fails the WHOLE call, all-or-nothing ----------
    {
        std::filesystem::path const bad_root =
            std::filesystem::temp_directory_path() / "ae_test_skill_source_disk_invalid";
        std::filesystem::remove_all(bad_root, ec);
        write_file(bad_root / "good-skill" / "SKILL.md",
                   "---\nname: good-skill\ndescription: This one is fine.\n---\nBody.\n");
        write_file(bad_root / "bad-skill" / "SKILL.md",
                   "---\nname: WRONG-CASE\ndescription: This one has an invalid name.\n---\nBody.\n");

        DiskSkillSource source("local", bad_root);
        auto loaded = source.load_skills();
        check(!loaded.has_value(),
              "R2: one invalid SKILL.md anywhere under the root fails load_skills() ENTIRELY -- a "
              "caller never silently runs with only the good half of a source it believed loaded "
              "cleanly (§8a: 'rejects rather than guessing')");
        if (!loaded) {
            check(loaded.error().code == "skill.name_invalid_chars",
                  "R2: the failure is the real, specific validation error from the bad SKILL.md, not a "
                  "generic wrapper");
        }
        std::filesystem::remove_all(bad_root, ec);
    }

    // ---- R3: a root that isn't a directory at all fails closed --------------------------------------
    {
        DiskSkillSource source("local", root / "pdf-tools" / "SKILL.md");  // a FILE, not a directory
        auto loaded = source.load_skills();
        check(!loaded.has_value() && loaded.error().code == "skill.disk_root_not_a_directory",
              "R3: pointing a DiskSkillSource at a file (not a directory) fails closed with a named "
              "error code, not a crash or a silently empty result");
    }

    std::filesystem::remove_all(root, ec);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_skill_source_disk: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_source_disk: %d FAILURE(S)\n", g_failures);
    return 1;
}
