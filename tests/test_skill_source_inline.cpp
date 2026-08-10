// Implements 009-Plugin-and-Extension-System.md §8 -- proves core/skill_source.hpp's
// `InlineSkillSource` (the direct "inline skill" case the user asked for MAF-parity on) and
// core/builtin_skills.hpp's `make_builtin_skills_source()` (§8f's one shipped example: real content,
// parsed at construction time, no disk I/O).

#include <cstdio>
#include <string>

#include "agentengine/core/builtin_skills.hpp"
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

}  // namespace

int main() {
    // ---- R1: InlineSkillSource returns exactly what it was constructed with, no I/O -----------------
    {
        auto skill = parse_skill_md("---\nname: inline-skill\ndescription: A programmatically defined skill.\n---\nBody.\n");
        check(skill.has_value(), "R1: fixture skill parses");

        std::vector<SkillBundleFile> files;
        files.push_back(SkillBundleFile{"SKILL.md", {}});
        std::vector<SkillSourceResult> supplied;
        supplied.push_back(SkillSourceResult{*skill, files});

        InlineSkillSource source("test-origin", supplied);
        check(source.origin_id() == "test-origin", "R1: origin_id() round-trips");

        auto loaded = source.load_skills();
        check(loaded.has_value() && loaded->size() == 1,
              "R1: load_skills() returns exactly the supplied skill");
        if (loaded && loaded->size() == 1) {
            check((*loaded)[0].skill.frontmatter.name == "inline-skill",
                  "R1: the supplied skill's own name round-trips through load_skills()");
        }

        auto loaded_again = source.load_skills();
        check(loaded_again.has_value() && loaded_again->size() == 1,
              "R1: calling load_skills() a second time is side-effect-free and returns the same data "
              "(no disk, no mutation)");
    }

    // ---- R2: make_skill_source_descriptor type-erases correctly ------------------------------------
    {
        std::vector<SkillSourceResult> empty;
        auto descriptor = make_skill_source_descriptor(InlineSkillSource("erased-origin", empty));
        check(descriptor.origin_id == "erased-origin",
              "R2: make_skill_source_descriptor's origin_id matches the wrapped source's own");
        auto loaded = descriptor.load_skills();
        check(loaded.has_value() && loaded->empty(),
              "R2: the type-erased load_skills() closure forwards correctly through the shared_ptr");
    }

    // ---- R3: builtin_skills.hpp's five shipped §8f skills all parse cleanly, real content ------------
    {
        auto descriptor = make_builtin_skills_source();
        check(descriptor.origin_id == "builtin", "R3: make_builtin_skills_source()'s origin_id is 'builtin'");

        auto loaded = descriptor.load_skills();
        check(loaded.has_value(), "R3: the builtin skill set resolves without error");
        if (loaded) {
            check(loaded->size() == 5,
                  "R3: all five §8f generic skills ship (using-the-code-interpreter, using-codeact, "
                  "reading-large-content, producing-structured-output, shell-pipelines)");

            char const* expected_names[] = {"using-the-code-interpreter", "using-codeact",
                                             "reading-large-content", "producing-structured-output",
                                             "shell-pipelines"};
            for (std::size_t i = 0; i < loaded->size() && i < 5; ++i) {
                auto const& skill = (*loaded)[i].skill;
                check(skill.frontmatter.name == expected_names[i],
                      "R3: skill order/name matches the declared §8f entry list");
                check(!skill.frontmatter.description.empty(),
                      "R3: every shipped skill carries a real, non-empty description");
                check(!skill.body.empty(), "R3: every shipped skill has a real, non-empty body");
                check((*loaded)[i].files.size() == 1 && (*loaded)[i].files[0].relative_path == "SKILL.md",
                      "R3: every bundle includes SKILL.md itself as a readable-back file, matching every "
                      "other source's own convention");
            }

            if (loaded->size() >= 1) {
                check((*loaded)[0].skill.body.find("execute_code") != std::string::npos,
                      "R3: using-the-code-interpreter's body actually mentions the tool it teaches");
            }
            if (loaded->size() >= 2) {
                check((*loaded)[1].skill.body.find("agent.tools") != std::string::npos,
                      "R3: using-codeact's body actually mentions the agent.* module surface it teaches");
            }
            if (loaded->size() >= 5) {
                check((*loaded)[4].skill.body.find("ShellRunner") != std::string::npos,
                      "R3: shell-pipelines' body actually mentions ShellRunner");
            }
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_skill_source_inline: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_source_inline: %d FAILURE(S)\n", g_failures);
    return 1;
}
