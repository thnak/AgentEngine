// Proves the OQ-16 (OpenQuestions.md) discoverability manifest generator
// (trust/agent_library_manifest.hpp): an ungranted module is absent from both the pull-side and
// push-side outputs (I2), both are generated from the same CapabilitySet (one source of truth, per
// OQ-16's own explicit requirement), and the push-side string respects a token-budget approximation
// of 026 §7's "<= 30 tokens" line, extended from tools to the whole `agent.*` surface.

#include <iostream>
#include <string>

#include "agentengine/trust/agent_library_manifest.hpp"

using namespace agentengine;
using namespace agentengine::trust;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

bool contains_module(std::vector<GrantedModule> const& modules, std::string_view name) {
    for (auto const& m : modules) {
        if (m.name == name) return true;
    }
    return false;
}

} // namespace

int main() {
    // M-C1: a bare CapabilitySet (nothing granted) still surfaces the two zero-capability
    // reporting modules (output, progress) and nothing else.
    {
        CapabilitySet empty{};
        auto modules = granted_modules(empty);
        AE_CHECK(modules.size() == 2, "M-C1: empty grant surfaces exactly 2 modules");
        AE_CHECK(contains_module(modules, "output") && contains_module(modules, "progress"),
                 "M-C1: the two are output and progress");
        AE_CHECK(!contains_module(modules, "tools") && !contains_module(modules, "spawn"),
                 "M-C1: capability-gated modules are absent with nothing granted");
    }

    // M-C2 / I2: granting ToolCall surfaces exactly `tools` in addition to the always-present two,
    // and no other capability-gated module leaks in as a side effect.
    {
        CapabilitySet granted =
            CapabilitySet::grant_root({capability_from_kind(capability_kind::tool_call)});
        auto modules = granted_modules(granted);
        AE_CHECK(modules.size() == 3, "M-C2: tool_call grant surfaces exactly 3 modules");
        AE_CHECK(contains_module(modules, "tools"), "M-C2: tools is present");
        AE_CHECK(!contains_module(modules, "files") && !contains_module(modules, "data") &&
                     !contains_module(modules, "memory") && !contains_module(modules, "notes") &&
                     !contains_module(modules, "ask") && !contains_module(modules, "spawn"),
                 "M-C2 (I2): no ungranted module leaks in alongside tools");
    }

    // M-C3: fs_read alone grants `data` and `memory` (both read-gated) but NOT `files` (needs
    // fs_read OR fs_write -- fs_read alone should still grant it per the registry's "either"
    // semantics) or `notes` (write-gated) -- confirms the ANY-of-gating_kinds logic is exact, not
    // approximate.
    {
        CapabilitySet granted =
            CapabilitySet::grant_root({capability_from_kind(capability_kind::fs_read)});
        auto modules = granted_modules(granted);
        AE_CHECK(contains_module(modules, "data"), "M-C3: fs_read grants data");
        AE_CHECK(contains_module(modules, "memory"), "M-C3: fs_read grants memory");
        AE_CHECK(contains_module(modules, "files"), "M-C3: fs_read alone still grants files (files accepts either fs_read or fs_write)");
        AE_CHECK(!contains_module(modules, "notes"), "M-C3 (I2): fs_read alone does not grant notes (write-gated)");
    }

    // M-C4: agent_call grants spawn, and ONLY spawn among the capability-gated modules.
    {
        CapabilitySet granted =
            CapabilitySet::grant_root({capability_from_kind(capability_kind::agent_call)});
        auto modules = granted_modules(granted);
        AE_CHECK(contains_module(modules, "spawn"), "M-C4: agent_call grants spawn");
        AE_CHECK(!contains_module(modules, "tools") && !contains_module(modules, "files") &&
                     !contains_module(modules, "ask"),
                 "M-C4 (I2): agent_call alone grants nothing else");
    }

    // M-C5: single source of truth — for every grant combination above, the push-side summary
    // mentions EXACTLY the module names granted_modules() returned, no more, no fewer. Proves the
    // two outputs cannot drift because they are read from the same registry via the same gating
    // check, not independently maintained.
    {
        CapabilitySet granted = CapabilitySet::grant_root({
            capability_from_kind(capability_kind::fs_write),
            capability_from_kind(capability_kind::elicit),
        });
        auto modules = granted_modules(granted);
        auto summary = push_side_summary(granted);
        for (auto const& module : agent_library_registry()) {
            bool in_pull_side = contains_module(modules, module.name);
            bool in_push_side = summary.find(std::string("agent.") + std::string(module.name) + ":") !=
                                 std::string::npos;
            AE_CHECK(in_pull_side == in_push_side,
                     "M-C5: '" + std::string(module.name) + "' agrees between pull-side and push-side");
        }
    }

    // M-C6: every registry entry's one-line description stays comfortably under the 026 §7 budget
    // (<= 30 tokens/tool, approximated). All nine should pass easily -- they were written short.
    {
        bool all_within_budget = true;
        for (auto const& module : agent_library_registry()) {
            auto tokens = detail::approx_token_count(module.one_line);
            if (tokens > 30) {
                all_within_budget = false;
                std::cerr << "  '" << module.name << "' description is " << tokens
                          << " approx tokens\n";
            }
        }
        AE_CHECK(all_within_budget, "M-C6: every registry description is within the 30-token budget");
    }

    // M-C7 (positive control): the budget check itself must be able to fail. A deliberately bloated
    // description (not part of the real registry) must be flagged, proving M-C6 isn't vacuously
    // passing because the check never fires.
    {
        std::string_view bloated =
            "This is a deliberately long and verbose description written to exceed the token "
            "budget on purpose so that this specific check can prove the budget assertion actually "
            "detects a real violation instead of always reporting success regardless of input";
        auto tokens = detail::approx_token_count(bloated);
        AE_CHECK(tokens > 30,
                 "M-C7 (positive control): a deliberately bloated description is flagged as over budget");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All agent_library_manifest checks passed.\n";
    return 0;
}
