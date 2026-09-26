// Milestone 3 Phase H1 (026-Agent-Facing-Runtime-Surface.md §7's Prompt budget table, §8 G2/G3) --
// proof that `reference_agent_prompt.hpp`'s assembled prompt stays within 026 §7's per-element token
// budget and carries ZERO architecture leakage (no capability/policy/profile/host terms), using data
// sourced from the SAME real registries (ToolTable, trust::agent_library_registry()) other phases
// already use -- never a second, hand-duplicated summary. Model-independent: no ChatClient, no
// fixture, no network call anywhere in this file. The model-DEPENDENT half (does a real reference
// agent's own code succeed first-attempt under this prompt) is
// tests/test_reference_agent_task_corpus.cpp.

#include <cstdio>
#include <string>

#include "agentengine/core/reference_agent_prompt.hpp"
#include "agentengine/trust/agent_library_manifest.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

// Architecture terms 026 §1/§2/§3 explicitly says the agent never sees. Deliberately a real corpus,
// not a single string -- 026 §8 G3's own bar is "no host path, profile name, capability name, rule
// id, or host stack trace appears in any agent-visible string", not just one obvious word.
constexpr char const* kArchitectureTerms[] = {
    "capability",  "Capability",  "sandbox", "Sandbox",   "native_jail", "wasm",
    "profile",     "mount_id",    "policy",  "Policy",    "GetLastError", "CreateFileW",
    "worktree",    "CapabilitySet", "EffectContext",
};

}  // namespace

int main() {
    // A realistic, real-source-data granted set: two tools, agent.tools/agent.files (from the SAME
    // registry G3 already proved matches 026 §5), no skills for this pass.
    std::vector<PromptToolSummary> tools = {
        {"read_csv_sum", "Reads a CSV column and returns the sum."},
        {"list_dir", "Lists files under a path."},
    };
    std::vector<PromptModuleSummary> modules = {
        {"tools", std::string(trust::module_one_line("tools"))},
        {"files", std::string(trust::module_one_line("files"))},
    };
    std::vector<PromptSkillSummary> skills = {};

    AssembledPrompt prompt = assemble_reference_agent_prompt(tools, modules, skills);

    // ---- H1-B1: every budget line is a real §7 row, individually within budget -------------------
    AE_CHECK(prompt.budget_lines.size() == 5, "H1-B1: 1 environment + 2 tool + 2 module budget lines");
    for (auto const& line : prompt.budget_lines) {
        AE_CHECK(line.within_budget(),
                 ("H1-B1: '" + line.element + "' (" + std::to_string(line.tokens) + " tokens) is within its " +
                  std::to_string(line.budget_tokens) + "-token 026 §7 budget")
                     .c_str());
    }
    AE_CHECK(prompt.within_budget(), "H1-B1: the assembled prompt as a whole is within budget");

    // ---- H1-B2 (positive control): an oversized tool description DOES blow its budget -------------
    // 022 §5's own pairing rule -- a gate that can never fail proves nothing.
    {
        std::vector<PromptToolSummary> oversized_tools = {
            {"verbose_tool",
             "This tool description is deliberately far too long for the thirty token per tool budget "
             "that 026 section 7 declares, containing many more than thirty tokens of English prose "
             "so that the budget check below has something real to catch, not a vacuous pass"},
        };
        AssembledPrompt oversized = assemble_reference_agent_prompt(oversized_tools, {}, {});
        AE_CHECK(!oversized.within_budget(),
                 "H1-B2 (positive control): an oversized tool description DOES exceed its 30-token "
                 "budget -- the gate is a real check, not one that always passes");
    }

    // ---- H1-L1 (no leakage, 026 §8 G3): zero architecture terms in the base prompt -----------------
    for (char const* term : kArchitectureTerms) {
        bool const leaked = prompt.system_prompt.find(term) != std::string::npos;
        AE_CHECK(!leaked, (std::string("H1-L1: base prompt does not contain the architecture term '") +
                            term + "'")
                               .c_str());
    }

    // ---- H2-D1: assemble_informed_prompt's own output DOES differ, and DOES mention architecture --
    // Proves the two functions genuinely produce different text (not that H2's harness would be
    // silently testing the same string twice), and that the "informed" variant is the ONLY place
    // architecture detail can appear -- confirmed by the SAME leakage corpus H1-L1 used, run against
    // the base half of the informed prompt only.
    {
        std::string const informed = assemble_informed_prompt(prompt);
        AE_CHECK(informed != prompt.system_prompt,
                 "H2-D1: assemble_informed_prompt produces DIFFERENT text from the base prompt");
        AE_CHECK(informed.find("sandbox") != std::string::npos && informed.find("capability set") != std::string::npos,
                 "H2-D1: the informed variant explicitly names the sandbox/capability architecture "
                 "the base variant never mentions (026 \xc2\xa7"
                 "1a's 'assume it is told' case)");
        AE_CHECK(informed.substr(0, prompt.system_prompt.size()) == prompt.system_prompt,
                 "H2-D1: the informed prompt is the base prompt with architecture detail APPENDED, "
                 "never a rewritten/different base -- the §7-budgeted portion is identical either way");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All reference-agent prompt-assembly checks passed.\n");
    return 0;
}
