#pragma once
// Implements decisions/ADR-072-external-host-discovered-skill-sources.md -- locates real, on-disk
// skill directories belonging to OTHER AI coding tools already installed on the host machine
// (Claude Code, GitHub Copilot, OpenAI Codex), so a host can feed them into the EXISTING
// `DiskSkillSource`/`SkillsProvider` pipeline (core/skill_source.hpp, core/skill_provider.hpp)
// unchanged. All three tools converged on the identical `SKILL.md` wire format in 2026 (YAML
// frontmatter `name`/`description`/`allowed-tools` + Markdown body -- the exact shape
// `core/skill.hpp`'s `parse_skill_md` already parses) specifically so the same skill files work
// across agents -- so this header's ENTIRE job is finding the right real paths. No new parser, no
// new provider, no new capability kind: it produces `(origin_id, path)` pairs and nothing else.
//
// Host-invoked only -- nothing here is wired into any automatic startup path (ADR-070 property 1,
// explicit opt-in). `discover_external_skill_locations()` reads NO file content, only directory
// EXISTENCE -- a host decides, per returned location, whether to actually construct a
// `DiskSkillSource` from it and pass that into a `SkillsProvider`. Because host code (not model
// output) is the caller, I2 ("no ambient authority [to the model]") is not implicated the way
// ADR-071's PATH-scanning was -- this is the same trust category as a host already being free to
// point today's `DiskSkillSource` at any path it chooses. The real, honestly-scoped residual is a
// supply-chain-adjacent one: a host that opts in may pull in third-party skill content it did not
// personally author or review. `SkillsProvider`'s existing fail-closed name-collision-across-sources
// check still protects a project's own skills from being silently shadowed; it does not vet new,
// non-colliding content -- see ADR-072 §6 for the full red-team treatment.
//
// `kKnownRoots` below is sourced from `docs/research/2026-08-21-external-ai-tool-skill-conventions.md`
// (dated, cited -- CLAUDE.md's "research is dated and cited" rule), not asserted from memory.

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/pal/env.hpp"

namespace agentengine {

// ae-naming-lint: allow ExternalSkillTool — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class ExternalSkillTool { claude_code, codex, copilot, generic_agents };
// ae-naming-lint: allow ExternalSkillScope — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class ExternalSkillScope { user, project };

[[nodiscard]] inline std::string_view external_skill_tool_name(ExternalSkillTool tool) noexcept {
    switch (tool) {
        case ExternalSkillTool::claude_code: return "claude_code";
        case ExternalSkillTool::codex: return "codex";
        case ExternalSkillTool::copilot: return "copilot";
        case ExternalSkillTool::generic_agents: return "generic_agents";
    }
    return "unknown";  // unreachable for a valid enumerator; no undefined return value
}

[[nodiscard]] inline std::string_view external_skill_scope_name(ExternalSkillScope scope) noexcept {
    switch (scope) {
        case ExternalSkillScope::user: return "user";
        case ExternalSkillScope::project: return "project";
    }
    return "unknown";  // unreachable for a valid enumerator; no undefined return value
}

// A confirmed-existing (`std::filesystem::is_directory` already checked), real host directory that
// looks like one of the known external-tool skill roots. `origin_id` is ready to hand straight to
// `DiskSkillSource`/`make_skill_source_descriptor` -- also what `SkillsProvider`'s own
// name-collision-across-sources error names, so a host sees exactly which external root a colliding
// skill came from.
// ae-naming-lint: allow ExternalSkillLocation — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ExternalSkillLocation {
    ExternalSkillTool tool;
    ExternalSkillScope scope;
    std::string origin_id;
    std::filesystem::path path;
};

namespace external_skill_discovery_detail {

struct KnownRoot {
    ExternalSkillTool tool;
    char const* user_relative;     // relative to the host's home directory
    char const* project_relative;  // relative to a caller-supplied project root
};

// The researched, dated conventions (see ADR-072 §2 for citations): all four tools recognize a
// per-tool user-level directory plus one of three GitHub-docs-equivalent project-level directories.
// `generic_agents` is the tool-agnostic convention multiple tools also honor directly.
inline constexpr std::array<KnownRoot, 4> kKnownRoots{{
    {ExternalSkillTool::claude_code, ".claude/skills", ".claude/skills"},
    {ExternalSkillTool::codex, ".codex/skills", ".codex/skills"},
    {ExternalSkillTool::copilot, ".copilot/skills", ".github/skills"},
    {ExternalSkillTool::generic_agents, ".agents/skills", ".agents/skills"},
}};

// `USERPROFILE` on Windows, `HOME` everywhere else. Deliberately `_WIN32`, NOT `_MSC_VER`
// (`pal/env.hpp`'s own split is a DIFFERENT axis -- which non-deprecated CRT function to call on
// MSVC specifically -- while this one is which ENV VAR NAME the OS itself defines; a MinGW/clang
// toolchain on Windows has no `_MSC_VER` but still only ever sets `USERPROFILE`, never `HOME`).
[[nodiscard]] inline std::optional<std::filesystem::path> host_home_dir() {
#if defined(_WIN32)
    if (auto v = pal::env_var("USERPROFILE")) return std::filesystem::path(*v);
#else
    if (auto v = pal::env_var("HOME")) return std::filesystem::path(*v);
#endif
    return std::nullopt;
}

}  // namespace external_skill_discovery_detail

// Host-invoked, opt-in only. `project_root` is the HOST APPLICATION's own project directory (never
// AgentEngine's) -- a caller building on AgentEngine as an SDK passes its own repo root;
// `std::nullopt` skips every project-scope check. Returns only locations that genuinely exist as
// real directories right now -- a caller never receives a location doomed to fail at
// `DiskSkillSource::load_skills()`'s own "root is not a directory" check, and never has to
// special-case "this tool isn't installed on this machine."
[[nodiscard]] inline std::vector<ExternalSkillLocation> discover_external_skill_locations(
    std::optional<std::filesystem::path> project_root = std::nullopt) {
    std::vector<ExternalSkillLocation> out;

    std::optional<std::filesystem::path> const home = external_skill_discovery_detail::host_home_dir();
    std::error_code ec;

    for (auto const& root : external_skill_discovery_detail::kKnownRoots) {
        if (home) {
            std::filesystem::path const candidate = *home / root.user_relative;
            if (std::filesystem::is_directory(candidate, ec)) {
                out.push_back(ExternalSkillLocation{
                    root.tool, ExternalSkillScope::user,
                    "external:" + std::string(external_skill_tool_name(root.tool)) + ":user", candidate});
            }
        }
        if (project_root) {
            std::filesystem::path const candidate = *project_root / root.project_relative;
            if (std::filesystem::is_directory(candidate, ec)) {
                out.push_back(ExternalSkillLocation{
                    root.tool, ExternalSkillScope::project,
                    "external:" + std::string(external_skill_tool_name(root.tool)) + ":project", candidate});
            }
        }
    }

    return out;
}

}  // namespace agentengine
