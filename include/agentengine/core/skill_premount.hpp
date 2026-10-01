#pragma once
// Implements decisions/ADR-224-host-premounted-builtin-skills.md (GitHub issue #47): a HOST may mount
// the built-in skill that teaches a tool, before the first model call, when the host itself declares
// that tool into a session. 009 §8c's on-demand mount (ADR-024) leaves discovery to the model, and a
// live session showed a model probing a shell grammar blind for fifteen turns rather than calling
// `mount_skill("shell-pipelines")` once.
//
// What this header is, and is not:
//   - A pure tool->skill pairing table over the BUILT-IN skills (core/builtin_skills.hpp), and a
//     function that applies it to a `MountedSkillsState` through `mount_by_host()`, the host-only
//     entry point that records `skill_mount_origin::host` (core/mounted_skills_state.hpp).
//   - Opt-in. Nothing in the engine calls `premount_skills_by_host()`; a host does, from its own
//     configuration (tools/cli_chat.cpp does by default and `--no-premount-skills` turns it off).
//   - Never driven by model output (I3). Its inputs are the tool names the HOST chose to declare and
//     the skill names the host's own skill sources resolved -- not a ContextContribution, not a tool
//     call, not anything the model wrote. A model cannot reach `mount_by_host()`: the `mount_skill`
//     tool calls `mount()`.
//   - No authority (I2). Mounting a skill grants nothing (ADR-024 §I2/I3 argument, unchanged): the
//     skill's files are already readable and its `allowed-tools` only narrows within tools the
//     session's grants already cover. A host pre-mount therefore changes what the model is TOLD up
//     front, never what it may DO.
//   - Attributed. `render_mounted_skill_bodies()` labels a host-mounted body as host-mounted in the
//     context it injects, so the model is never shown a skill it did not mount presented as one it did.

#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/mounted_skills_state.hpp"

namespace agentengine {

// ae-naming-lint: allow ToolSkillPairing — ADR-224's own vocabulary
struct ToolSkillPairing {
    std::string_view tool_name;
    std::string_view skill_name;
};

// Which built-in skill teaches which tool. `run_shell` is the mediated shell whose grammar
// `shell-pipelines` documents; `run_command` is a real POSIX `sh` in a container, which the same skill
// covers in its own closing section (the two differ, and the skill says how). `execute_code` is the
// embedded interpreter `using-the-code-interpreter` teaches.
inline constexpr ToolSkillPairing kBuiltinToolSkillPairings[] = {
    {"run_shell", "shell-pipelines"},
    {"run_command", "shell-pipelines"},
    {"execute_code", "using-the-code-interpreter"},
};

// Pure: the built-in skills paired with `host_declared_tools`, deduplicated, in pairing-table order (so
// the result does not depend on the order the host listed its tools in). A tool with no pairing
// contributes nothing.
[[nodiscard]] inline std::vector<std::string> builtin_skills_for_tools(
    std::vector<std::string> const& host_declared_tools) {
    std::vector<std::string> out;
    for (ToolSkillPairing const& p : kBuiltinToolSkillPairings) {
        bool declared = false;
        for (std::string const& t : host_declared_tools) {
            if (t == p.tool_name) {
                declared = true;
                break;
            }
        }
        if (!declared) continue;
        bool seen = false;
        for (std::string const& s : out) {
            if (s == p.skill_name) {
                seen = true;
                break;
            }
        }
        if (!seen) out.emplace_back(p.skill_name);
    }
    return out;
}

// ae-naming-lint: allow SkillPremountReport — ADR-224's own vocabulary
struct SkillPremountReport {
    std::vector<std::string> mounted;          // newly mounted by the host, origin host
    std::vector<std::string> already_mounted;  // was mounted before; its origin was NOT rewritten
    std::vector<std::string> not_resolvable;   // not among the session's resolved skills; NOT mounted
};

// Host-only. Mounts each of `skills` that is among `resolvable_skill_names` (the names the session's
// own skill sources resolved -- the same check a model's `mount_skill` call is held to), recording
// `skill_mount_origin::host`. Never mounts a name the session did not resolve, so a host typo cannot
// inject a dangling mount. Call it before the session's first model call.
[[nodiscard]] inline SkillPremountReport premount_skills_by_host(
    MountedSkillsState& state, std::vector<std::string> const& skills,
    std::vector<std::string> const& resolvable_skill_names) {
    SkillPremountReport report;
    for (std::string const& skill : skills) {
        bool resolvable = false;
        for (std::string const& r : resolvable_skill_names) {
            if (r == skill) {
                resolvable = true;
                break;
            }
        }
        if (!resolvable) {
            report.not_resolvable.push_back(skill);
            continue;
        }
        if (state.is_mounted(skill)) {
            report.already_mounted.push_back(skill);
            continue;
        }
        state.mount_by_host(skill);
        report.mounted.push_back(skill);
    }
    return report;
}

// The line that introduces one mounted skill's body in the context a host injects. Host-mounted skills
// say so: the model must not read a skill the host chose as one it chose itself.
[[nodiscard]] inline std::string mounted_skill_heading(std::string const& name, skill_mount_origin origin) {
    if (origin == skill_mount_origin::host) {
        return "Skill '" + name +
               "' (pre-mounted by the host for a tool this session offers -- not mounted by a "
               "mount_skill call):\n";
    }
    return "Mounted skill '" + name + "':\n";
}

// Every mounted skill's body, in mount order, each under its provenance-labelled heading and preceded
// by a newline -- the text a host appends to its one skills system message. `SkillsT` is anything with
// `std::optional<std::string> body_of(std::string const&) const` (core/skill_provider.hpp's
// `SkillsProvider`). A mounted name with no body is skipped.
template <class SkillsT>
[[nodiscard]] std::string render_mounted_skill_bodies(SkillsT const& skills, MountedSkillsState const& state) {
    std::string out;
    for (std::string const& name : state.all()) {
        auto body = skills.body_of(name);
        if (!body) continue;
        auto const origin = state.origin_of(name).value_or(skill_mount_origin::model);
        out += "\n" + mounted_skill_heading(name, origin) + *body;
    }
    return out;
}

}  // namespace agentengine
