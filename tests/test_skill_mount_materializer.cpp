// Implements src/backends/native_jail/skill_mount_materializer.hpp -- proves the bridge that was
// missing entirely before this session: `SkillsProvider::mounted()`'s Mounts, previously readable
// only from inside the provider's own private in-memory object/ref store (via `mount_read` called
// directly by test-harness C++), now land as REAL files on a REAL host directory via
// `materialize_mount` (worktree_mount_sync.hpp) -- the exact shape `MediatedPythonConfig::mount_roots`
// needs. Also proves the reserved-mount-id collision guard fails closed with nothing written to disk.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/skill_provider.hpp"
#include "backends/native_jail/skill_mount_materializer.hpp"

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

[[nodiscard]] SkillSourceResult make_skill(std::string name, std::string description,
                                            std::string body_text) {
    std::string const doc = "---\nname: " + name + "\ndescription: " + description + "\n---\n" + body_text;
    auto skill = parse_skill_md(doc, name);
    SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> manifest_bytes(reinterpret_cast<std::byte const*>(doc.data()),
                                           reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(SkillBundleFile{"SKILL.md", std::move(manifest_bytes)});
    std::string const notes = "supplementary notes";
    r.files.push_back(SkillBundleFile{
        "references/notes.txt", std::vector<std::byte>(reinterpret_cast<std::byte const*>(notes.data()),
                                                          reinterpret_cast<std::byte const*>(notes.data()) +
                                                              notes.size())});
    return r;
}

[[nodiscard]] std::string read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_skill_mount_materializer_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    // ---- R1: two real skills materialize to real files on disk -------------------------------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill("alpha-skill", "The alpha skill.", "Alpha body.\n")})));
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-b", {make_skill("beta-skill", "The beta skill.", "Beta body.\n")})));

        SkillsProvider<> provider(std::move(sources));
        std::filesystem::path const host_root = scratch / "r1";

        auto materialized =
            native_jail::materialize_skill_mounts(provider, host_root, {"work", "input", "out"});
        check(materialized.has_value(), "R1: materialize_skill_mounts succeeds for two non-colliding skills");
        if (materialized) {
            check(materialized->size() == 2, "R1: exactly two (mount_id, host_dir) pairs returned");

            std::filesystem::path const alpha_manifest = host_root / "alpha-skill" / "SKILL.md";
            std::filesystem::path const beta_manifest = host_root / "beta-skill" / "SKILL.md";
            check(std::filesystem::exists(alpha_manifest),
                  "R1: alpha-skill's SKILL.md is a real file on disk");
            check(std::filesystem::exists(beta_manifest),
                  "R1: beta-skill's SKILL.md is a real file on disk");
            check(read_file(alpha_manifest).find("alpha-skill") != std::string::npos,
                  "R1: alpha-skill's real on-disk bytes match its actual content -- a genuine "
                  "worktree-to-host round trip, not a stub");
            check(std::filesystem::exists(host_root / "alpha-skill" / "references" / "notes.txt"),
                  "R1: a nested bundle file (references/notes.txt) also materialized to a real path");
        }

        check(provider.mounted().size() == 2, "R1: ensure_loaded() was reached -- skills are resolved");
    }

    // ---- R2: a skill mount_id colliding with a reserved sandbox mount id fails closed --------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(
            InlineSkillSource("origin-a", {make_skill("work", "A skill literally named work.", "Body.\n")})));

        SkillsProvider<> provider(std::move(sources));
        std::filesystem::path const host_root = scratch / "r2";

        auto materialized = native_jail::materialize_skill_mounts(provider, host_root, {"work"});
        check(!materialized.has_value(),
              "R2: a skill named 'work' collides with the reserved sandbox mount id and is refused");
        if (!materialized) {
            check(materialized.error().code == "skill.mount_id_reserved",
                  "R2: the failure carries the specific, named reserved-collision error code");
        }
        check(!std::filesystem::exists(host_root),
              "R2: NOTHING was written to disk for this call -- fails closed before any directory "
              "creation, not partway through");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_skill_mount_materializer: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_mount_materializer: %d FAILURE(S)\n", g_failures);
    return 1;
}
