#pragma once
// Implements 009-Plugin-and-Extension-System.md §8c's per-skill tool-scoping mechanism: a skill's
// `allowed-tools` frontmatter field (parsed into `SkillsProvider::allowed_tool_names()`,
// skill_provider.hpp) restricts which tools are offered/invocable this run to the union declared by
// whichever skills are currently mounted, PLUS a caller-chosen `always_on` base set. This is a
// deliberate, user-directed departure from the precedent this project surveyed before building it
// (MAF's fixed three-meta-tool indirection; Anthropic's/OpenAI's "container" model, where the code-
// execution tool declaration is always static and only the underlying file/skill content varies) --
// see decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md for the recorded reasoning.
//
// THE ACTUAL ENFORCEMENT BOUNDARY, READ THIS BEFORE USING THIS HEADER:
// `core/tool_pipeline.hpp`'s own file-top comment states a run's `ToolTable` is "resolved once into
// an immutable table at run start -- a mid-run change to what's registered cannot alter what a run is
// allowed to call." And `invoke_tool()`'s actual step-1 authorization check
// (`ToolDescriptor const* tool = table.find(request.tool_name)`) is against whatever `ToolTable` the
// CALLER passes AT INVOCATION TIME -- not against whatever was declared to the model via
// `ContextContribution.tools`. Those are two independently-constructed things unless a caller makes
// them the same object. Filtering ONLY `ContextContribution.tools` and leaving a broader table at the
// `invoke_tool(...)` call site is COSMETIC, not a real restriction: a tool "hidden" from the model's
// declared list is still callable if the invocation-time table still contains it (I3: model output
// -- including a tool name the model happens to emit -- is data, never itself an authorization
// decision, but that only holds if the INVOKE-TIME check is real).
//
// So: whatever `allowed` set a caller passes -- a fixed, run-frozen union (Skills Phase 2: every
// RESOLVED skill, unconditionally) or one that changes mid-run as the agent activates skills on demand
// (Skills Phase 3, decisions/ADR-024's addendum: only currently MOUNTED skills) -- the declaration side
// (`ContextContribution.tools`, computed inside a turn's `ContextProvider::on_context`) and the
// invocation side (whatever table a caller passes to `invoke_tool(...)`/`invoke_agent_tool(...)`) MUST
// be recomputed from the SAME live state, at the SAME cadence, so they never diverge. A fixed set can
// safely be computed once, at run start; a state-dependent set (Phase 3's `MountedSkillsState`) must be
// recomputed fresh on the SAME schedule both sides already run on (each turn's `on_context`, and each
// round's `invoke_tool` call) -- what must never happen is computing it once for declaration and once,
// separately, for invocation, at different points relative to when the state could have changed.

#include <algorithm>
#include <string>
#include <vector>

#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// Filters `universe`'s descriptors down to those whose `name` is in `allowed` (typically
// `SkillsProvider::allowed_tool_names()`) unioned with `always_on` (tools a caller wants
// unconditionally available regardless of mount state -- a caller-level policy choice; empty by
// default, meaning "nothing is offered unless some currently-mounted skill actually names it").
[[nodiscard]] inline ToolTable scope_tools_to_mounted_skills(
    ToolTable const& universe, std::vector<std::string> const& allowed,
    std::vector<std::string> const& always_on = {}) {
    std::vector<ToolDescriptor> scoped;
    for (ToolDescriptor const& d : universe.descriptors()) {
        bool const in_allowed = std::ranges::find(allowed, d.name) != allowed.end();
        bool const in_always_on = std::ranges::find(always_on, d.name) != always_on.end();
        if (in_allowed || in_always_on) scoped.push_back(d);
    }
    return ToolTable::from_descriptors(std::move(scoped));
}

}  // namespace agentengine
