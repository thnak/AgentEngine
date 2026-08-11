// AgentEngine "get started" examples, 11 -- a skill that unlocks a tool on demand.
//
// Mirrors MAF's samples/02-agents/AgentSkills. In AgentEngine (009 §8c, ADR-024) a skill can name
// tools via its own `allowed-tools` frontmatter that are otherwise unreachable -- a tool call
// against a not-yet-mounted skill's tool is genuinely REJECTED by the real pipeline, not merely
// omitted from some advertised list, and the SAME call succeeds through the SAME pipeline once the
// skill is mounted. This example calls that real mechanism directly (`SkillsProvider`,
// `MountedSkillsState`, `scope_tools_to_mounted_skills`, `invoke_tool`) rather than through a full
// `AgentSession` turn loop -- `tools/cli_chat.cpp`'s own `mount_skill` tool is what wires this
// through a live model turn; this example is the offline, structural version of the same chain.
//
// Run: ./agentengine_example_11_skill_mount

#include <cstdio>
#include <string>
#include <vector>

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

[[nodiscard]] SkillSourceResult word_count_skill() {
    std::string const doc =
        "---\nname: word-counter\ndescription: Counts words in text.\n"
        "allowed-tools: word_count\n---\nMount this to unlock word_count.\n";
    auto skill = parse_skill_md(doc, "word-counter");
    SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> manifest(reinterpret_cast<std::byte const*>(doc.data()),
                                     reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(SkillBundleFile{"SKILL.md", std::move(manifest)});
    return r;
}

struct CountArgs { std::string text; };
AE_JSON_SCHEMA(CountArgs, text)
struct CountReply { int count = 0; };
AE_JSON_SCHEMA(CountReply, count)

// EffectClass<pure>, no Capabilities<> -- this example is about tool-table MEMBERSHIP and mount
// state, not capability-binding mechanics (see 06_capabilities_and_denial.cpp for that).
struct WordCountTool : Tool<WordCountTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "word_count";
    static constexpr std::string_view description = "Counts whitespace-separated words.";
    using Args = CountArgs;
    using Reply = CountReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        int count = 0;
        bool in_word = false;
        for (char c : a.text) {
            bool const is_space = (c == ' ' || c == '\t' || c == '\n');
            if (!is_space && !in_word) ++count;
            in_word = !is_space;
        }
        return Reply{count};
    }
};

[[nodiscard]] ToolCallRequest count_call() {
    return ToolCallRequest{"call-1", "word_count",
                            json::Value::make_object({{"text", json::Value::make_string("a real bridged call")}}),
                            /*arguments_tainted=*/false, 0};
}

}  // namespace

int main() {
    std::vector<SkillSourceDescriptor> sources;
    sources.push_back(
        make_skill_source_descriptor(InlineSkillSource("origin-a", {word_count_skill()})));
    SkillsProvider<> skills(std::move(sources));
    check(skills.ensure_loaded().has_value(), "the word-counter skill source resolves");

    MountedSkillsState mount_state;
    ToolTable const    universe = ToolTable::from_tools<WordCountTool>();
    CapabilitySet const held = CapabilitySet::grant_root({});
    EffectContext ctx;
    ctx.capabilities = &held;

    // ---- Before mounting: word_count is named by the skill, but the skill isn't mounted ----------
    {
        auto const allowed = skills.allowed_tool_names_for(mount_state.all());
        check(allowed.empty(), "before mount: nothing is unlocked yet");
        ToolTable const     scoped = scope_tools_to_mounted_skills(universe, allowed);
        ToolInvocationAudit audit;
        ToolResult const    result = invoke_tool(scoped, held, count_call(), ctx, {}, &audit);
        check(result.is_error, "before mount: word_count is genuinely REJECTED by the real pipeline");
        check(audit.error_code == "tool.unknown_name",
              "before mount: rejected as 'unknown tool', not a permission denial -- it isn't in the "
              "table at all while unmounted");
        std::printf("[before mount] word_count -> rejected (%s)\n", audit.error_code.c_str());
    }

    // ---- Mount the skill -- what a real mount_skill tool call does -------------------------------
    mount_state.mount("word-counter");

    // ---- After mounting: the SAME call now succeeds through the SAME pipeline ---------------------
    {
        auto const allowed = skills.allowed_tool_names_for(mount_state.all());
        check(allowed.size() == 1 && allowed[0] == "word_count",
              "after mount: word_count is now named as unlocked");
        ToolTable const     scoped = scope_tools_to_mounted_skills(universe, allowed);
        ToolInvocationAudit audit;
        ToolResult const    result = invoke_tool(scoped, held, count_call(), ctx, {}, &audit);
        check(!result.is_error, "after mount: the SAME call to word_count now succeeds");
        if (!result.is_error && !result.content.empty()) {
            auto const* data = std::get_if<Data>(&result.content.front().value);
            check(data != nullptr, "after mount: the tool result carries a Data content item");
            if (data != nullptr) {
                auto parsed = json::parse(data->json);
                check(parsed.has_value(), "after mount: the reply JSON parses");
                if (parsed.has_value()) {
                    auto reply = schema::from_json<CountReply>(*parsed);
                    check(reply.has_value() && reply->count == 4,
                          "after mount: word_count actually ran (4 words)");
                    std::printf("[after mount]  word_count -> %d\n", reply.has_value() ? reply->count : -1);
                }
            }
        }
    }

    std::fprintf(stderr,
                 g_failures == 0 ? "example_11_skill_mount: OK\n" : "example_11_skill_mount: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
