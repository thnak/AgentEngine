// Implements 009-Plugin-and-Extension-System.md §8c's Skills Phase 3 addendum (decisions/ADR-024) --
// agent-triggered, on-demand skill mounting. Proves two things without a live model or native_jail
// dependency: (R1) `MountedSkillsState`'s own trivial contract, and (R2) the load-bearing end-to-end
// chain -- BEFORE any mount, a tool named by a skill's `allowed-tools` is genuinely unreachable via
// `invoke_tool` (not just undeclared); AFTER recording a mount for that skill, the SAME tool call
// succeeds through the SAME real pipeline (`SkillsProvider::allowed_tool_names_for` ->
// `scope_tools_to_mounted_skills` -> `invoke_tool`). This is the offline structural proof
// `tools/cli_chat.cpp`'s own live-model run (this session) demonstrated interactively.

#include <cstdio>
#include <string>

#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"

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

[[nodiscard]] SkillSourceResult make_skill_with_tool(std::string name, std::string tool_name) {
    std::string const doc = "---\nname: " + name +
                             "\ndescription: A skill that names one tool.\nallowed-tools: " + tool_name +
                             "\n---\nBody.\n";
    auto skill = parse_skill_md(doc, name);
    SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> manifest_bytes(reinterpret_cast<std::byte const*>(doc.data()),
                                           reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(SkillBundleFile{"SKILL.md", std::move(manifest_bytes)});
    return r;
}

struct FakeToolArgs {
    std::string note;
};
AE_JSON_SCHEMA(FakeToolArgs, note)
struct FakeToolReply {
    bool ok;
};
AE_JSON_SCHEMA(FakeToolReply, ok)

// EffectClass<pure>, no Capabilities<> -- matches test_tool_table_scoping.cpp's own fixture shape, so
// this test is entirely about table membership/mount state, not capability-binding mechanics.
struct FakeTool : Tool<FakeTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "fake_tool";
    static constexpr std::string_view description = "A tool a test skill names via allowed-tools.";
    using Args = FakeToolArgs;
    using Reply = FakeToolReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

}  // namespace

int main() {
    // ---- R1: MountedSkillsState's own trivial contract ----------------------------------------------
    {
        MountedSkillsState state;
        check(!state.is_mounted("test-skill"), "R1: nothing is mounted before any mount() call");
        check(state.all().empty(), "R1: all() is empty before any mount() call");

        state.mount("test-skill");
        check(state.is_mounted("test-skill"), "R1: is_mounted() reflects a real mount() call");
        check(state.all().size() == 1 && state.all()[0] == "test-skill",
              "R1: all() lists exactly the mounted name");

        state.mount("test-skill");
        check(state.all().size() == 1, "R1: mounting an already-mounted skill is idempotent, not a "
                                        "duplicate entry");
    }

    // ---- R2: the load-bearing chain -- reject before mount, succeed after ---------------------------
    {
        std::vector<SkillSourceDescriptor> sources;
        sources.push_back(make_skill_source_descriptor(InlineSkillSource(
            "origin-a", {make_skill_with_tool("test-skill", "fake_tool")})));
        SkillsProvider<> skills(std::move(sources));
        auto loaded = skills.ensure_loaded();
        check(loaded.has_value(), "R2 setup: the one-skill source resolves cleanly");

        MountedSkillsState mount_state;
        ToolTable const universe = ToolTable::from_tools<FakeTool>();
        CapabilitySet const held = CapabilitySet::grant_root({});
        EffectContext ctx;
        ctx.capabilities = &held;

        // Before any mount: fake_tool is named by test-skill's allowed-tools, but test-skill is not
        // yet mounted -- allowed_tool_names_for(empty) must be empty, and the resulting scoped table
        // must genuinely reject a call to fake_tool, not merely omit it from some advertised list.
        {
            auto const allowed = skills.allowed_tool_names_for(mount_state.all());
            check(allowed.empty(), "R2-before: allowed_tool_names_for(nothing mounted) is empty");
            ToolTable const scoped = scope_tools_to_mounted_skills(universe, allowed);

            ToolCallRequest const req{"call-1", "fake_tool",
                                       json::Value::make_object({{"note", json::Value::make_string("x")}}),
                                       /*arguments_tainted=*/false, 0};
            ToolInvocationAudit audit;
            ToolResult const result = invoke_tool(scoped, held, req, ctx, {}, &audit);
            check(result.is_error,
                  "R2-before: invoke_tool REJECTS fake_tool while test-skill is unmounted -- a real "
                  "restriction, not a cosmetic one");
            check(audit.error_code == "tool.unknown_name",
                  "R2-before: the rejection is specifically 'unknown tool', invoke_tool's own step-1 "
                  "table.find() -- the real enforcement boundary");
        }

        // Mount test-skill (what MountSkillTool::invoke does in cli_chat.cpp, minus the CLI's own
        // pending-skill-name validation, out of this core-tier test's scope) -- then the SAME call
        // must now succeed through the SAME pipeline.
        mount_state.mount("test-skill");
        {
            auto const allowed = skills.allowed_tool_names_for(mount_state.all());
            check(allowed.size() == 1 && allowed[0] == "fake_tool",
                  "R2-after: allowed_tool_names_for({\"test-skill\"}) now names fake_tool");
            ToolTable const scoped = scope_tools_to_mounted_skills(universe, allowed);

            ToolCallRequest const req{"call-2", "fake_tool",
                                       json::Value::make_object({{"note", json::Value::make_string("x")}}),
                                       /*arguments_tainted=*/false, 0};
            ToolInvocationAudit audit;
            ToolResult const result = invoke_tool(scoped, held, req, ctx, {}, &audit);
            check(!result.is_error,
                  "R2-after: the SAME call to fake_tool succeeds once test-skill is mounted -- the "
                  "unlock is real, through the real pipeline, not a separate code path");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_on_demand_skill_mount: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_on_demand_skill_mount: %d FAILURE(S)\n", g_failures);
    return 1;
}
