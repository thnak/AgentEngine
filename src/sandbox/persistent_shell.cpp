// Implements decisions/ADR-209-persistent-shell-sessions.md §5 (snapshot diff, caps, replay scripts) -- the
// non-template bodies of include/agentengine/sandbox/persistent_shell.hpp. Std only.

#include "agentengine/sandbox/persistent_shell.hpp"

#include <algorithm>
#include <map>

#include "agentengine/core/base64.hpp"

namespace agentengine::persistent_shell {

namespace {

[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] bool is_identifier(std::string_view name) {
    if (name.empty()) return false;
    auto const alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (!alpha(name[0])) return false;
    return std::all_of(name.begin() + 1, name.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

}  // namespace

bool is_replayable_env_name(std::string_view name) {
    if (!is_identifier(name)) return false;
    static constexpr std::string_view kExact[] = {"IFS",     "ENV",      "BASH_ENV", "SHELLOPTS", "BASHOPTS",
                                                  "PROMPT_COMMAND", "PWD", "OLDPWD", "SHLVL",   "_",
                                                  "CDPATH",  "PSModulePath"};
    for (auto const e : kExact) {
        if (name == e) return false;
    }
    // `PS*` covers PS1/PS2/PS4 (sh) -- and, deliberately, PSModulePath above is listed too because pwsh
    // reads it to resolve modules; `LD_*` covers LD_PRELOAD/LD_LIBRARY_PATH; `BASH_FUNC_*` is how bash
    // exports functions through the environment.
    static constexpr std::string_view kPrefixes[] = {"LD_", "PS", "BASH_FUNC_", "DYLD_"};
    for (auto const p : kPrefixes) {
        if (starts_with(name, p)) return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> parse_environ_block(std::string_view block) {
    std::map<std::string, std::string> merged;
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t end = block.find('\0', pos);
        if (end == std::string_view::npos) end = block.size();
        std::string_view const entry = block.substr(pos, end - pos);
        pos = end + 1;
        std::size_t const eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        merged.insert_or_assign(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    return {merged.begin(), merged.end()};
}

ShellSnapshot make_snapshot(std::string cwd, std::vector<std::pair<std::string, std::string>> const& env_after,
                            std::vector<std::pair<std::string, std::string>> const& env_base) {
    ShellSnapshot snap;
    if (cwd.size() > kShellSnapshotMaxCwdBytes) {
        snap.truncated = true;
    } else {
        snap.cwd = std::move(cwd);
    }
    std::map<std::string, std::string> const base(env_base.begin(), env_base.end());
    std::map<std::string, std::string> const after(env_after.begin(), env_after.end());
    std::size_t vars = 0;
    std::size_t bytes = 0;
    auto const admit = [&](std::size_t entry_bytes) {
        if (vars + 1 > kShellSnapshotMaxVars || bytes + entry_bytes > kShellSnapshotMaxEnvBytes) {
            snap.truncated = true;
            return false;
        }
        ++vars;
        bytes += entry_bytes;
        return true;
    };
    for (auto const& [name, value] : after) {
        if (!is_replayable_env_name(name)) continue;
        auto const it = base.find(name);
        if (it != base.end() && it->second == value) continue;
        if (admit(name.size() + value.size())) snap.env_set.emplace_back(name, value);
    }
    for (auto const& [name, value] : base) {
        (void)value;
        if (!is_replayable_env_name(name) || after.contains(name)) continue;
        if (admit(name.size())) snap.env_unset.push_back(name);
    }
    return snap;
}

std::string sh_single_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char const c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string sh_replay_script(ShellSnapshot const& snapshot) {
    std::string script;
    if (!snapshot.cwd.empty()) script += "cd -- " + sh_single_quote(snapshot.cwd) + " 2>/dev/null\n";
    for (auto const& [name, value] : snapshot.env_set) {
        if (!is_replayable_env_name(name)) continue;
        script += "export " + name + "=" + sh_single_quote(value) + "\n";
    }
    for (auto const& name : snapshot.env_unset) {
        if (!is_replayable_env_name(name)) continue;
        script += "unset " + name + "\n";
    }
    script += ":\n";
    return script;
}

std::string pwsh_replay_script(ShellSnapshot const& snapshot) {
    auto const decode = [](std::string_view text) {
        return "[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" + agentengine::base64::encode(text) +
               "'))";
    };
    std::string script;
    if (!snapshot.cwd.empty()) {
        script += "try { Set-Location -LiteralPath (" + decode(snapshot.cwd) + ") -ErrorAction Stop } catch { }\n";
    }
    for (auto const& [name, value] : snapshot.env_set) {
        if (!is_replayable_env_name(name)) continue;
        script += "[Environment]::SetEnvironmentVariable((" + decode(name) + "), (" + decode(value) + "))\n";
    }
    for (auto const& name : snapshot.env_unset) {
        if (!is_replayable_env_name(name)) continue;
        script += "[Environment]::SetEnvironmentVariable((" + decode(name) + "), $null)\n";
    }
    return script;
}

}  // namespace agentengine::persistent_shell
