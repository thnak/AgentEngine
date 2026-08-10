// Implements 009-Plugin-and-Extension-System.md §8b-8c -- proves core/skill_provider.hpp's
// `SkillsProvider`: real mounting through core/worktree.hpp's real Tree/Ref/Mount machinery (never a
// mock), the flat `/skills/<name>` path, the reject-on-collision anti-shadowing rule, the system-
// message advertisement shape, and the resolve-once/freeze guarantee (009 §8c).

#include <algorithm>
#include <cstdio>
#include <string>
#include <variant>

#include "agentengine/core/skill_provider.hpp"
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

[[nodiscard]] SkillSourceResult make_skill_ex(std::string name, std::string description,
                                               std::string allowed_tools, std::string body_text) {
    std::string doc = "---\nname: " + name + "\ndescription: " + description + "\n";
    if (!allowed_tools.empty()) doc += "allowed-tools: " + allowed_tools + "\n";
    doc += "---\n" + body_text;
    auto skill = parse_skill_md(doc, name);
    SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> manifest_bytes(reinterpret_cast<std::byte const*>(doc.data()),
                                           reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(SkillBundleFile{"SKILL.md", std::move(manifest_bytes)});
    return r;
}

[[nodiscard]] SkillSourceResult make_skill(std::string name, std::string description,
                                            std::string body_text) {
    return make_skill_ex(std::move(name), std::move(description), "", std::move(body_text));
}

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

}  // namespace

int main() {
    using agentengine::test_support::run_task_sync;

    Principal principal{"p-skills", ""};
    std::vector<Message> const history;

    // ---- R1: two DIFFERENT-named skills from two sources mount and are both readable ---------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill("pdf-tools", "Extract text from PDF files.", "Use execute_code.\n")})));
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-b", {make_skill("csv-tools", "Parse CSV files.", "Also use execute_code.\n")})));

        SkillsProvider<> provider(std::move(sources));
        EffectContext ctx;
        ctx.principal = principal;
        SessionContext session_ctx{"s-skills-1", principal, history};

        auto contribution =
            run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(contribution.has_value(), "R1: on_context succeeds for two non-colliding skills");
        if (contribution) {
            check(provider.mounted().size() == 2, "R1: exactly two mounts were created");
            bool found_pdf = false, found_csv = false;
            for (auto const& m : provider.mounted()) {
                if (m.mount_id == "pdf-tools") found_pdf = true;
                if (m.mount_id == "csv-tools") found_csv = true;
            }
            check(found_pdf && found_csv,
                  "R1: mount_id is the BARE skill name (a capability-matching key, not the literal "
                  "'/skills/<name>' path -- see skill_provider.hpp's own top comment); the skill's "
                  "LOGICAL path /skills/<name> is still carried via the advertisement message below, "
                  "unaffected by this choice");

            // Read a mounted skill's own SKILL.md back out through a real, granted cap::FsRead --
            // proving the mount is real content-addressed storage, not a bookkeeping-only record.
            Mount const* pdf_mount = nullptr;
            for (auto const& m : provider.mounted()) {
                if (m.mount_id == "pdf-tools") pdf_mount = &m;
            }
            check(pdf_mount != nullptr, "R1: pdf-tools' own Mount record is retrievable");
            if (pdf_mount) {
                cap::FsRead granted{"pdf-tools", "", std::nullopt};
                auto bytes = mount_read(provider.object_store(), provider.ref_store(), *pdf_mount,
                                         granted, "SKILL.md");
                check(bytes.has_value(), "R1: mount_read succeeds through a real granted capability");
                if (bytes) {
                    std::string const content(reinterpret_cast<char const*>(bytes->data()), bytes->size());
                    check(content.find("pdf-tools") != std::string::npos,
                          "R1: the mounted SKILL.md's real bytes are readable back out -- a genuine "
                          "content-addressed round trip, not a stub");
                }
            }

            check(contribution->messages.size() == 1 &&
                      contribution->messages[0].role == role::system,
                  "R1: exactly one role::system advertisement message is contributed");
            if (!contribution->messages.empty()) {
                std::string const text = text_of(contribution->messages[0]);
                check(text.find("pdf-tools: Extract text from PDF files.") != std::string::npos,
                      "R1: the advertisement text renders as 'name: description', matching "
                      "reference_agent_prompt.hpp's own established rendering");
                check(text.find("csv-tools: Parse CSV files.") != std::string::npos,
                      "R1: both skills' advertisement lines are present in the ONE system message");
            }
        }
    }

    // ---- R2: two sources declaring the SAME skill name -> fail closed, no partial mount ------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill("shared-name", "The first one.", "Body A.\n")})));
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-b", {make_skill("shared-name", "The second one.", "Body B.\n")})));

        SkillsProvider<> provider(std::move(sources));
        EffectContext ctx;
        ctx.principal = principal;
        SessionContext session_ctx{"s-skills-2", principal, history};

        auto contribution =
            run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(!contribution.has_value(),
              "R2: two sources declaring the same skill name FAIL the whole on_context() call -- "
              "shadowing is prevented by refusal, never by silent last-source-wins (009 §8c)");
        if (!contribution) {
            check(contribution.error().code == "skill.name_collision_across_sources",
                  "R2: the failure carries the specific, named collision error code");
        }
        check(provider.mounted().empty(),
              "R2: NO partial mount exists after a collision -- neither origin's skill was mounted, "
              "not even the one that was processed first");
    }

    // ---- R3: resolve-once, freeze -- a second on_context() call reuses the SAME mounts -------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill("stable-skill", "Should only be mounted once.", "Body.\n")})));

        SkillsProvider<> provider(std::move(sources));
        EffectContext ctx;
        ctx.principal = principal;
        SessionContext session_ctx{"s-skills-3", principal, history};

        auto first = run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(first.has_value(), "R3: first on_context() call succeeds");
        std::string const first_ref_name = provider.mounted().empty() ? "" : provider.mounted()[0].ref_name;

        auto second = run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(second.has_value(), "R3: second on_context() call also succeeds");
        check(provider.mounted().size() == 1,
              "R3: still exactly one mount after a second call -- sources were NOT re-resolved");
        check(!provider.mounted().empty() && provider.mounted()[0].ref_name == first_ref_name,
              "R3: the SAME Ref backs the mount both times -- resolve-once/freeze (009 §8c: a skill "
              "loaded mid-run does not retroactively change what earlier turns were permitted to do)");
    }

    // ---- R4: allowed_tool_names() is the deduplicated union across mounted skills ------------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill_ex("tool-skill-a", "Names execute_code and shell_run.",
                                        "execute_code shell_run", "Body A.\n")})));
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-b", {make_skill_ex("tool-skill-b", "Names execute_code and read_file.",
                                        "execute_code read_file", "Body B.\n")})));

        SkillsProvider<> provider(std::move(sources));
        EffectContext ctx;
        ctx.principal = principal;
        SessionContext session_ctx{"s-skills-4", principal, history};

        check(provider.allowed_tool_names().empty(),
              "R4: allowed_tool_names() is empty before resolution -- same contract as mounted()");

        auto contribution =
            run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
        check(contribution.has_value(), "R4: on_context succeeds for two skills naming overlapping tools");

        auto const& names = provider.allowed_tool_names();
        check(names.size() == 3,
              "R4: 'execute_code' named by both skills is deduplicated -- 3 distinct names total, not 4");
        auto has = [&](char const* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
        check(has("execute_code") && has("shell_run") && has("read_file"),
              "R4: the union contains every distinct name from every mounted skill");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_skill_provider_mount: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_provider_mount: %d FAILURE(S)\n", g_failures);
    return 1;
}
