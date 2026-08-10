#pragma once
// Implements 009-Plugin-and-Extension-System.md §8 -- "where does a skill come from," the loading/
// sourcing abstraction the user asked for MAF-parity on ("load from disk or inline skill, with skill
// provider support"). MAF's own `AgentSkillsProvider`/`SkillsProvider` (docs/research/2026-maf-
// provider-concepts.md §1) is the vocabulary precedent for the CONCEPT of "a pluggable skill origin";
// this project's own `ContextProviderDescriptor` (core/context_assembly.hpp) is the precedent for
// HOW to shape "N heterogeneous conformers, one runtime table" in THIS codebase -- reused here rather
// than inventing a second type-erasure pattern.
//
// Type-erased at runtime, not a compile-time `concept`/variadic-template-pack parameter on
// `SkillsProvider<Sources...>`: a session declares "load skills from these N sources" as RUNTIME
// configuration (disk paths, inline bundles), not a compile-time-fixed set the way `ChatClientT` is
// one template slot chosen once per binary. `Tool`/`ChatClient` dispatch is genuinely per-call hot;
// resolving skill sources happens once per run (skill_provider.hpp's own "resolve-once, freeze"
// rule) -- nothing here is on that hot path, so the type-erasure cost this pattern already accepts
// elsewhere in this codebase (006 §6's `ToolTable`) is exactly as acceptable here.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/skill.hpp"

namespace agentengine {

// One file inside a skill's bundle, `SKILL.md` itself included -- `relative_path` is POSIX-style
// (forward slashes), relative to the skill's own directory root (e.g. "SKILL.md",
// "references/api.md"), exactly the segment shape `worktree.hpp`'s `split_mount_path` expects.
struct SkillBundleFile {
    std::string relative_path;
    std::vector<std::byte> bytes;
};

// One resolved skill, ready to be mounted: the parsed manifest/body plus every file `SkillsProvider`
// (skill_provider.hpp) needs to assemble a worktree Tree for it. `files` is expected to include
// `SKILL.md` itself, so a mounted skill's own manifest is readable back out exactly like any other
// bundled file (026 §6's "ordinary file operations" -- no special-cased manifest access).
struct SkillSourceResult {
    Skill skill;
    std::vector<SkillBundleFile> files;
};

template <class T>
concept SkillSource = requires(T& s) {
    { s.origin_id() } -> std::convertible_to<std::string_view>;
    { s.load_skills() } -> std::same_as<result<std::vector<SkillSourceResult>>>;
};

// The type-erased, runtime-configurable table entry -- `SkillsProvider`'s own input list.
struct SkillSourceDescriptor {
    std::string origin_id;
    std::function<result<std::vector<SkillSourceResult>>()> load_skills;
};

template <class SourceT>
    requires SkillSource<SourceT>
[[nodiscard]] SkillSourceDescriptor make_skill_source_descriptor(SourceT source) {
    auto shared = std::make_shared<SourceT>(std::move(source));
    SkillSourceDescriptor d;
    d.origin_id = std::string(shared->origin_id());
    d.load_skills = [shared]() { return shared->load_skills(); };
    return d;
}

// Direct, in-memory "inline skill" -- the caller supplies already-resolved `SkillSourceResult`s
// (typically each built by hand or via `parse_skill_md` against a string literal, `builtin_skills.hpp`'s
// own pattern), no disk I/O at all. `load_skills()` returns a copy of what was constructed with,
// every call -- cheap and side-effect-free, matching `SkillsProvider`'s own "resolve-once" caller-
// side expectation without needing this class to track state itself.
class InlineSkillSource {
public:
    InlineSkillSource(std::string origin_id, std::vector<SkillSourceResult> skills)
        : origin_id_(std::move(origin_id)), skills_(std::move(skills)) {}

    [[nodiscard]] std::string_view origin_id() const noexcept { return origin_id_; }
    [[nodiscard]] result<std::vector<SkillSourceResult>> load_skills() const { return skills_; }

private:
    std::string origin_id_;
    std::vector<SkillSourceResult> skills_;
};
static_assert(SkillSource<InlineSkillSource>);

namespace skill_source_detail {

// Recursively collects every regular file under `dir` into `out`, `relative_to`-prefixed and
// POSIX-joined -- `scripts/`, `references/`, `assets/` (§8a's own directory layout) are walked
// uniformly, whichever of the three (or none) actually exist under a given skill directory.
[[nodiscard]] inline result<void> collect_files(std::filesystem::path const& dir,
                                                  std::filesystem::path const& skill_root,
                                                  std::vector<SkillBundleFile>& out) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return {};
    for (auto const& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            return std::unexpected(error{failure_class::contract,
                                          "failed to walk skill bundle directory: " + ec.message(),
                                          "skill.disk_read_failed"});
        }
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
            return std::unexpected(error{failure_class::contract,
                                          "failed to open skill bundle file: " + entry.path().string(),
                                          "skill.disk_read_failed"});
        }
        std::string const content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(content.data()),
                                      reinterpret_cast<std::byte const*>(content.data()) + content.size());
        std::filesystem::path const rel = entry.path().lexically_relative(skill_root);
        std::string posix_rel = rel.generic_string();
        out.push_back(SkillBundleFile{std::move(posix_rel), std::move(bytes)});
    }
    return {};
}

}  // namespace skill_source_detail

// Loads every `skill-name/SKILL.md`-shaped subdirectory of a real host directory. A subdirectory
// with no `SKILL.md` is silently skipped (a real corpus may mix skill directories with stray, non-
// skill content); a subdirectory WITH an invalid `SKILL.md` fails the whole `load_skills()` call --
// all-or-nothing, matching §8a's "rejects rather than guessing" (a caller must never silently run
// with half a source it believed loaded cleanly).
class DiskSkillSource {
public:
    DiskSkillSource(std::string origin_id, std::filesystem::path root)
        : origin_id_(std::move(origin_id)), root_(std::move(root)) {}

    [[nodiscard]] std::string_view origin_id() const noexcept { return origin_id_; }

    [[nodiscard]] result<std::vector<SkillSourceResult>> load_skills() const {
        std::error_code ec;
        if (!std::filesystem::is_directory(root_, ec) || ec) {
            return std::unexpected(error{failure_class::contract,
                                          "skill source root is not a directory: " + root_.string(),
                                          "skill.disk_root_not_a_directory"});
        }

        std::vector<SkillSourceResult> results;
        for (auto const& entry : std::filesystem::directory_iterator(root_, ec)) {
            if (ec) {
                return std::unexpected(error{failure_class::contract,
                                              "failed to list skill source root: " + ec.message(),
                                              "skill.disk_read_failed"});
            }
            if (!entry.is_directory()) continue;

            std::filesystem::path const manifest_path = entry.path() / "SKILL.md";
            std::error_code manifest_ec;
            if (!std::filesystem::is_regular_file(manifest_path, manifest_ec) || manifest_ec) {
                continue;  // no SKILL.md here -- not a skill directory, not an error
            }

            std::ifstream in(manifest_path, std::ios::binary);
            if (!in) {
                return std::unexpected(error{failure_class::contract,
                                              "failed to open " + manifest_path.string(),
                                              "skill.disk_read_failed"});
            }
            std::string const manifest_text((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            std::string const directory_name = entry.path().filename().string();
            auto skill = parse_skill_md(manifest_text, directory_name);
            if (!skill) return std::unexpected(skill.error());

            std::vector<SkillBundleFile> files;
            files.push_back(SkillBundleFile{
                "SKILL.md", std::vector<std::byte>(reinterpret_cast<std::byte const*>(manifest_text.data()),
                                                    reinterpret_cast<std::byte const*>(manifest_text.data()) +
                                                        manifest_text.size())});
            for (char const* sub : {"scripts", "references", "assets"}) {
                auto collected =
                    skill_source_detail::collect_files(entry.path() / sub, entry.path(), files);
                if (!collected) return std::unexpected(collected.error());
            }

            results.push_back(SkillSourceResult{std::move(*skill), std::move(files)});
        }
        return results;
    }

private:
    std::string origin_id_;
    std::filesystem::path root_;
};
static_assert(SkillSource<DiskSkillSource>);

}  // namespace agentengine
