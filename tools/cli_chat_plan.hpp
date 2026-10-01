#pragma once
// GitHub issue #47; decisions/ADR-224-host-premounted-builtin-skills.md. The part of
// tools/cli_chat.cpp's wiring that is a DECISION rather than plumbing -- which shell tools the session
// offers, which capabilities that requires minting, which tools a human must approve, and which
// built-in skills the host pre-mounts -- factored out of cli_chat.cpp so it can be tested without a
// model, a network or an embedded interpreter (tests/tools/test_cli_chat_plan.cpp).
//
// Two shell tools, two trust tiers. They coexist; neither replaces the other:
//
//   mediated   `run_shell` -- src/backends/native_jail/session_shell_wiring.hpp. Engine-native code
//              over the session's "work" directory: a fixed builtin set, no process creation at all,
//              every file access checked against the session's FsRead/FsWrite grants on "work".
//              Session-persistent: `cd` carries over between calls. Needs no capability cli_chat does
//              not already mint for `execute_code` (FsRead/FsWrite on "work"), so offering it adds no
//              authority (I2). Not approval-gated, for the same reason `execute_code` is not.
//   container  `run_command` -- include/agentengine/sandbox/mandatory_sandbox_provider.hpp. A real
//              POSIX `sh -c` in a fresh Docker container per call: arbitrary binaries, real process
//              isolation, files carried between calls by Ledger checkpoints, no shell state carried.
//              Needs `cap::RunCommand`, which is minted ONLY when this tier is selected, and every call
//              asks a human first.
//
// The DEFAULT is `mediated`: the tier that mints the least authority, needs no Docker daemon, and
// creates no process. `container` and `both` are explicit opt-ins. `none` offers neither.
//
// Skill pre-mounting (ADR-224) is on by default here -- this CLI is the host, and opting in is the
// host's call -- and `--no-premount-skills` restores pure on-demand mounting (ADR-024).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/skill_premount.hpp"

namespace agentengine::cli_chat {

// ae-naming-lint: allow shell_tier — cli_chat's own vocabulary (issue #47)
enum class shell_tier : std::uint8_t { mediated, container, both, none };

// ae-naming-lint: allow Options — cli_chat's own vocabulary (issue #47)
struct Options {
    shell_tier shell = shell_tier::mediated;
    bool premount_skills = true;
    bool help = false;
};

[[nodiscard]] inline std::string_view to_string(shell_tier t) noexcept {
    switch (t) {
        case shell_tier::mediated: return "mediated";
        case shell_tier::container: return "container";
        case shell_tier::both: return "both";
        case shell_tier::none: return "none";
    }
    return "mediated";
}

[[nodiscard]] inline std::string_view help_text() noexcept {
    return "usage: agentengine_cli_chat [--shell=mediated|container|both|none] [--no-premount-skills]\n"
           "\n"
           "Interactive chat with a real model. Provider and credentials come from the environment\n"
           "(AGENTENGINE_PROVIDER, AGENTENGINE_<PROVIDER>_API_KEY; see the file header).\n"
           "\n"
           "  --shell=TIER   which shell tool the agent gets (default: mediated)\n"
           "      mediated   run_shell: engine-native shell over the session's work directory. A fixed\n"
           "                 builtin set (cd pwd ls cat echo mkdir rm mv cp), no programs, no\n"
           "                 processes; cd persists between calls. Uses only the work-directory grants\n"
           "                 execute_code already has (so `export`, which needs an environment grant,\n"
           "                 is refused). Not approval-gated.\n"
           "      container  run_command: a real sh in a fresh Docker container per call (needs a\n"
           "                 Docker daemon). Any program; files persist between calls, shell state\n"
           "                 does not. Every call asks you first.\n"
           "      both       offer both tools.\n"
           "      none       no shell tool (execute_code only).\n"
           "  --no-premount-skills\n"
           "                 do not pre-mount the built-in skill for each offered tool (shell-pipelines,\n"
           "                 using-the-code-interpreter); the agent must call mount_skill itself.\n"
           "  -h, --help     print this help and exit.\n";
}

// Parses the arguments AFTER argv[0]. Unknown arguments and unknown tiers are errors, never ignored: a
// mistyped `--shell=containr` must not silently fall back to some tier the user did not ask for. A
// repeated flag: the last one wins.
[[nodiscard]] inline result<Options> parse_args(std::vector<std::string_view> const& args) {
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view const a = args[i];
        if (a == "-h" || a == "--help") {
            o.help = true;
            continue;
        }
        if (a == "--no-premount-skills") {
            o.premount_skills = false;
            continue;
        }
        std::string_view value;
        if (a.starts_with("--shell=")) {
            value = a.substr(std::string_view("--shell=").size());
        } else if (a == "--shell") {
            if (i + 1 >= args.size()) {
                return std::unexpected(error{failure_class::contract, "--shell needs a value",
                                             "cli_chat.bad_argument"});
            }
            value = args[++i];
        } else {
            return std::unexpected(error{failure_class::contract, "unknown argument: " + std::string(a),
                                         "cli_chat.bad_argument"});
        }
        if (value == "mediated") o.shell = shell_tier::mediated;
        else if (value == "container") o.shell = shell_tier::container;
        else if (value == "both") o.shell = shell_tier::both;
        else if (value == "none") o.shell = shell_tier::none;
        else {
            return std::unexpected(error{failure_class::contract,
                                         "unknown --shell tier: '" + std::string(value) +
                                             "' (expected mediated, container, both or none)",
                                         "cli_chat.bad_argument"});
        }
    }
    return o;
}

// ae-naming-lint: allow ToolPlan — cli_chat's own vocabulary (issue #47)
struct ToolPlan {
    bool offer_run_shell = false;
    bool offer_run_command = false;
    // `cap::RunCommand` is minted into the session's root grant only when `run_command` is offered.
    bool mint_run_command_capability = false;
    // Every top-level tool the host declares into the session, whether or not a skill gates it.
    std::vector<std::string> host_declared_tools;
    // Raised to approval_mode::always_require for this deployment.
    std::vector<std::string> approval_required;
    // Built-in skills the host mounts before the first model call (ADR-224); empty when disabled.
    std::vector<std::string> premount_skills;
};

[[nodiscard]] inline ToolPlan plan_tools(Options const& o) {
    ToolPlan p;
    p.offer_run_shell = o.shell == shell_tier::mediated || o.shell == shell_tier::both;
    p.offer_run_command = o.shell == shell_tier::container || o.shell == shell_tier::both;
    p.mint_run_command_capability = p.offer_run_command;
    // execute_code and mount_skill are always part of this CLI's universe (execute_code is unlocked by
    // its skill; mount_skill is always declared).
    p.host_declared_tools = {"execute_code", "mount_skill"};
    if (p.offer_run_shell) p.host_declared_tools.emplace_back("run_shell");
    if (p.offer_run_command) {
        p.host_declared_tools.emplace_back("run_command");
        p.approval_required.emplace_back("run_command");
    }
    if (o.premount_skills) p.premount_skills = builtin_skills_for_tools(p.host_declared_tools);
    return p;
}

}  // namespace agentengine::cli_chat
