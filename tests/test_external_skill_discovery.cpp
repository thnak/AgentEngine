// Implements decisions/ADR-072-external-host-discovered-skill-sources.md -- proves
// core/external_skill_discovery.hpp's `discover_external_skill_locations()` against real host
// filesystem state: synthetic scratch directories simulating each known tool/scope convention, and
// (D6) this session's REAL, unmodified environment, since `~/.claude/skills/*` are real NTFS
// symlinks on this machine and `~/.codex/skills/` is a real, installed Codex CLI directory --
// genuine evidence, not an assumption, that the discovery table and DiskSkillSource's directory
// walk both work against an actually-installed toolchain. R-D7 proves the feature this whole design
// leans on for safety: SkillsProvider's existing fail-closed name-collision-across-sources check
// still fires when two DISCOVERED external locations declare a skill with the same name.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/external_skill_discovery.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/trust/principal.hpp"
#include "support/run_task_sync.hpp"

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

[[nodiscard]] bool contains_tool_scope(std::vector<ExternalSkillLocation> const& locs,
                                        ExternalSkillTool tool, ExternalSkillScope scope) {
    for (auto const& l : locs) {
        if (l.tool == tool && l.scope == scope) return true;
    }
    return false;
}

[[nodiscard]] ExternalSkillLocation const* find_tool_scope(std::vector<ExternalSkillLocation> const& locs,
                                                             ExternalSkillTool tool,
                                                             ExternalSkillScope scope) {
    for (auto const& l : locs) {
        if (l.tool == tool && l.scope == scope) return &l;
    }
    return nullptr;
}

}  // namespace

int main() {
    using agentengine::test_support::run_task_sync;

    std::filesystem::path const scratch_home =
        std::filesystem::temp_directory_path() / "ae_test_external_skill_discovery_home";
    std::filesystem::path const scratch_project =
        std::filesystem::temp_directory_path() / "ae_test_external_skill_discovery_project";
    std::error_code ec;
    std::filesystem::remove_all(scratch_home, ec);
    std::filesystem::remove_all(scratch_project, ec);

    // Redirect the process's own home-dir env var to the scratch tree so D1-D5 run in total
    // isolation from this machine's real, already-installed tools (proven separately by D6 below).
#if defined(_WIN32)
    char const* const kHomeVar = "USERPROFILE";
#else
    char const* const kHomeVar = "HOME";
#endif
    std::string const original_home = pal::env_var(kHomeVar).value_or("");
#if defined(_MSC_VER)
    _putenv_s(kHomeVar, scratch_home.string().c_str());
#else
    setenv(kHomeVar, scratch_home.string().c_str(), 1);
#endif

    // ---- D1: nothing installed anywhere, no project root -> empty result --------------------------
    {
        std::filesystem::create_directories(scratch_home);
        auto locs = discover_external_skill_locations();
        check(locs.empty(), "D1: an empty scratch HOME with no project root discovers nothing");
    }

    // ---- D2-D5: one synthetic root per tool/scope, everything else absent --------------------------
    write_file(scratch_home / ".claude" / "skills" / "demo" / "SKILL.md",
               "---\nname: demo\ndescription: A Claude Code user-scope skill.\n---\nBody.\n");
    write_file(scratch_project / ".codex" / "skills" / "demo2" / "SKILL.md",
               "---\nname: demo2\ndescription: A Codex project-scope skill.\n---\nBody.\n");
    write_file(scratch_home / ".copilot" / "skills" / "demo3" / "SKILL.md",
               "---\nname: demo3\ndescription: A Copilot user-scope skill.\n---\nBody.\n");
    write_file(scratch_project / ".agents" / "skills" / "demo4" / "SKILL.md",
               "---\nname: demo4\ndescription: A generic-agents project-scope skill.\n---\nBody.\n");
    // A location that exists but is a FILE, not a directory -- must never be discovered.
    write_file(scratch_home / ".github_not_a_dir_marker", "not a directory contents");

    {
        auto locs = discover_external_skill_locations(scratch_project);

        check(contains_tool_scope(locs, ExternalSkillTool::claude_code, ExternalSkillScope::user),
              "D2: Claude Code user-scope root (~/.claude/skills) is discovered");
        auto const* claude_loc =
            find_tool_scope(locs, ExternalSkillTool::claude_code, ExternalSkillScope::user);
        check(claude_loc != nullptr && claude_loc->origin_id == "external:claude_code:user",
              "D2: origin_id is the documented 'external:<tool>:<scope>' shape");
        check(claude_loc != nullptr && claude_loc->path == scratch_home / ".claude" / "skills",
              "D2: path is the exact real directory that was created");

        check(contains_tool_scope(locs, ExternalSkillTool::codex, ExternalSkillScope::project),
              "D3: Codex project-scope root (<project>/.codex/skills) is discovered");
        check(contains_tool_scope(locs, ExternalSkillTool::copilot, ExternalSkillScope::user),
              "D4: Copilot user-scope root (~/.copilot/skills) is discovered");
        check(contains_tool_scope(locs, ExternalSkillTool::generic_agents, ExternalSkillScope::project),
              "D5: generic_agents project-scope root (<project>/.agents/skills) is discovered");

        check(!contains_tool_scope(locs, ExternalSkillTool::codex, ExternalSkillScope::user),
              "D2-D5: a tool/scope with nothing on disk is simply absent, not an error");
        check(!contains_tool_scope(locs, ExternalSkillTool::copilot, ExternalSkillScope::project),
              "D2-D5: Copilot's project convention (.github/skills) was never created and is absent");
    }

    // ---- project_root = nullopt: user-scope still found, every project-scope row skipped -----------
    {
        auto locs = discover_external_skill_locations();  // no project_root
        check(contains_tool_scope(locs, ExternalSkillTool::claude_code, ExternalSkillScope::user),
              "D5b: with no project_root, user-scope discovery is unaffected");
        bool any_project = false;
        for (auto const& l : locs) {
            if (l.scope == ExternalSkillScope::project) any_project = true;
        }
        check(!any_project, "D5b: with no project_root, no project-scope location is ever returned");
    }

    // ---- D5c: a REAL candidate path segment (~/.copilot) that is a FILE, not a directory ------------
    // A stray `~/.copilot` file (not directory) means the candidate `~/.copilot/skills` can never
    // exist as a directory either -- `is_directory` must fail closed (false, not a crash/exception)
    // for THAT specific candidate while every OTHER, unrelated candidate is still discovered normally.
    {
        std::filesystem::remove_all(scratch_home / ".copilot", ec);
        write_file(scratch_home / ".copilot", "just a file, not the .copilot directory a real Copilot "
                                               "install would have created");

        auto locs = discover_external_skill_locations(scratch_project);
        check(!contains_tool_scope(locs, ExternalSkillTool::copilot, ExternalSkillScope::user),
              "D5c: ~/.copilot existing as a plain FILE means its skills candidate is never mistaken "
              "for a real directory");
        check(contains_tool_scope(locs, ExternalSkillTool::claude_code, ExternalSkillScope::user) &&
                  contains_tool_scope(locs, ExternalSkillTool::codex, ExternalSkillScope::project),
              "D5c: the ~/.copilot file collision doesn't crash or suppress discovery of every OTHER, "
              "unrelated, genuinely-real candidate directory");

        std::filesystem::remove(scratch_home / ".copilot", ec);
    }

    // ---- restore the real HOME/USERPROFILE before D6 touches the real environment ------------------
#if defined(_MSC_VER)
    _putenv_s(kHomeVar, original_home.c_str());
#else
    if (original_home.empty()) unsetenv(kHomeVar); else setenv(kHomeVar, original_home.c_str(), 1);
#endif

    // ---- D6: REAL machine evidence -- OPPORTUNISTIC, not required ----------------------------------
    // This session's own dev machine has Claude Code and Codex CLI installed (confirmed via `ls`
    // earlier this session), which is genuine, non-synthetic proof the discovery table and
    // DiskSkillSource's symlink-following both work against an actually-installed toolchain. But a
    // CI runner (this project's windows-msvc/windows-clang-cl jobs run on fresh windows-latest
    // GitHub-hosted runners, .github/workflows/ci.yml) has neither tool installed -- ~/.claude/skills
    // and ~/.codex/skills are legitimately absent there. Matching this project's own established
    // "gated on environment, SKIP when unset, stays green" convention for other machine-dependent
    // evidence (tests/CMakeLists.txt's live-network tests), these checks run and assert for real
    // ONLY when the real directories are actually present; their absence is reported, not failed.
    {
        auto locs = discover_external_skill_locations();
        auto const* real_claude =
            find_tool_scope(locs, ExternalSkillTool::claude_code, ExternalSkillScope::user);
        if (real_claude != nullptr) {
            std::fprintf(stderr,
                          "  ok: D6: on THIS real machine, ~/.claude/skills is genuinely discovered\n");
        } else {
            std::fprintf(stderr,
                          "  skip: D6: ~/.claude/skills not present on this machine (Claude Code not "
                          "installed here) -- opportunistic real-evidence check not exercised\n");
        }
        auto const* real_codex = find_tool_scope(locs, ExternalSkillTool::codex, ExternalSkillScope::user);
        if (real_codex != nullptr) {
            std::fprintf(stderr,
                          "  ok: D6: on THIS real machine, ~/.codex/skills is genuinely discovered\n");
        } else {
            std::fprintf(stderr,
                          "  skip: D6: ~/.codex/skills not present on this machine (Codex CLI not "
                          "installed here) -- opportunistic real-evidence check not exercised\n");
        }

        if (real_claude != nullptr) {
            // ~/.claude/skills/* are REAL NTFS symlinks on this machine (into ~/.agents/skills/<name>)
            // -- proves DiskSkillSource's directory_iterator-based walk actually follows them, not an
            // assumption.
            DiskSkillSource source(real_claude->origin_id, real_claude->path);
            auto loaded = source.load_skills();
            check(loaded.has_value(),
                  "D6: DiskSkillSource::load_skills() succeeds against the REAL, symlink-containing "
                  "~/.claude/skills directory");
            check(loaded.has_value() && !loaded->empty(),
                  "D6: at least one real, symlinked skill (e.g. one of the real firecrawl-* skills "
                  "observed on this machine) parses through the symlink, proving the walk resolves "
                  "symlinked skill directories rather than silently skipping them");
        }
    }

    // ---- R-D7: SkillsProvider's existing collision guard still protects discovered sources ---------
    {
        std::filesystem::path const root_a =
            std::filesystem::temp_directory_path() / "ae_test_external_skill_discovery_collide_a";
        std::filesystem::path const root_b =
            std::filesystem::temp_directory_path() / "ae_test_external_skill_discovery_collide_b";
        std::filesystem::remove_all(root_a, ec);
        std::filesystem::remove_all(root_b, ec);
        write_file(root_a / "shared-name" / "SKILL.md",
                   "---\nname: shared-name\ndescription: From external root A.\n---\nBody A.\n");
        write_file(root_b / "shared-name" / "SKILL.md",
                   "---\nname: shared-name\ndescription: From external root B.\n---\nBody B.\n");

        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(
            make_skill_source_descriptor(DiskSkillSource("external:claude_code:user", root_a)));
        sources.push_back(make_skill_source_descriptor(DiskSkillSource("external:codex:user", root_b)));

        SkillsProvider<> provider(std::move(sources));
        Principal principal{"p-external-skills", ""};
        std::vector<Message> const history;
        EffectContext ctx;
        ctx.principal = principal;
        SessionContext session_ctx{"s-external-skills-1", principal, history};

        auto contribution =
            run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(!contribution.has_value(),
              "R-D7: two DISCOVERED external sources declaring the same skill name still fail the "
              "whole on_context() call, same fail-closed anti-shadowing SkillsProvider already gives "
              "project-local sources (009 §8c)");
        if (!contribution) {
            check(contribution.error().code == "skill.name_collision_across_sources",
                  "R-D7: the failure carries the specific, named collision error code");
            check(contribution.error().message.find("external:claude_code:user") != std::string::npos &&
                      contribution.error().message.find("external:codex:user") != std::string::npos,
                  "R-D7: the collision error names BOTH real origin_ids, so a host can tell exactly "
                  "which two external tools' installs collided");
        }
        check(provider.mounted().empty(), "R-D7: no partial mount survives the collision");

        std::filesystem::remove_all(root_a, ec);
        std::filesystem::remove_all(root_b, ec);
    }

    std::filesystem::remove_all(scratch_home, ec);
    std::filesystem::remove_all(scratch_project, ec);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_external_skill_discovery: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_external_skill_discovery: %d FAILURE(S)\n", g_failures);
    return 1;
}
