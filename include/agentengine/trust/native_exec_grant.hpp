#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §9 (I6) -- the DECLARATIVE form of a
// `cap::NativeExec` grant, so a host configuring native execution from YAML/JSON can express exactly what a
// CRTP `cap::decl::NativeExec<...>` declaration expresses, the held-shell opt-in (`live_session`) and its two
// caps included. Before ADR-209 no declarative path produced a capability at all (the declarative agent
// compiler carries none); this is the one place a configuration document becomes a `cap::NativeExec`.
//
//   native_exec:
//     program: "pwsh"            # required; a trailing '*' is a prefix grant (ADR-071)
//     worktree_mount: "workdir"  # required
//     cpu_ms_cap: 30000          # optional, each of these
//     wall_ms_cap: 30000
//     memory_bytes_cap: 536870912
//     live_session: true         # optional, default false (ADR-209 §9: the held-shell opt-in)
//     session_wall_ms_cap: 3600000
//     max_processes: 64
//
// Fails closed: an unknown key, a wrong type, a negative/fractional/out-of-range number or a missing required
// key is refused with `native_exec_grant.invalid` -- never ignored, so a misspelt `live_sesion: false` cannot
// silently mean something else. Host-authored configuration only (I3): nothing model-derived reaches this.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

[[nodiscard]] inline agentengine::result<cap::NativeExec> parse_native_exec_grant(json::Value const& v) {
    auto const bad = [](std::string why) -> agentengine::result<cap::NativeExec> {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                  "native_exec grant: " + std::move(why),
                                                  "native_exec_grant.invalid"});
    };
    if (!v.is_object()) return bad("must be a mapping");
    static constexpr std::string_view kKeys[] = {"program",          "worktree_mount", "cpu_ms_cap",
                                                 "wall_ms_cap",      "memory_bytes_cap", "live_session",
                                                 "session_wall_ms_cap", "max_processes"};
    for (auto const& [key, value] : v.as_object()) {
        (void)value;
        bool known = false;
        for (auto k : kKeys) known = known || key == k;
        if (!known) return bad("unknown key '" + key + "'");
    }
    cap::NativeExec g;
    auto const* program = v.find("program");
    auto const* mount = v.find("worktree_mount");
    if (program == nullptr || !program->is_string() || program->as_string().empty()) {
        return bad("'program' is required and must be a non-empty string");
    }
    if (mount == nullptr || !mount->is_string() || mount->as_string().empty()) {
        return bad("'worktree_mount' is required and must be a non-empty string");
    }
    g.program_pattern = program->as_string();
    g.worktree_mount_id = mount->as_string();
    auto const u64 = [&](std::string_view key, std::optional<std::uint64_t>& out) -> bool {
        auto const* f = v.find(key);
        if (f == nullptr) return true;
        auto n = json::as_bounded_integer(*f);
        if (!n.has_value()) return false;
        out = *n;
        return true;
    };
    if (!u64("cpu_ms_cap", g.cpu_ms_cap)) return bad("'cpu_ms_cap' must be a non-negative integer");
    if (!u64("wall_ms_cap", g.wall_ms_cap)) return bad("'wall_ms_cap' must be a non-negative integer");
    if (!u64("memory_bytes_cap", g.memory_bytes_cap)) return bad("'memory_bytes_cap' must be a non-negative integer");
    if (!u64("session_wall_ms_cap", g.session_wall_ms_cap)) {
        return bad("'session_wall_ms_cap' must be a non-negative integer");
    }
    if (auto const* f = v.find("max_processes"); f != nullptr) {
        auto n = json::as_bounded_integer(*f, UINT32_MAX);
        if (!n.has_value()) return bad("'max_processes' must be an integer in [0, 2^32)");
        g.max_processes = static_cast<std::uint32_t>(*n);
    }
    if (auto const* f = v.find("live_session"); f != nullptr) {
        if (!f->is_bool()) return bad("'live_session' must be true or false");
        g.live_session = f->as_bool();
    }
    return g;
}

// ADR-209 §9's use-site rule: a held native shell may be offered under `g` only if it opts in AND bounds both
// the session's life and its process count. A live grant missing either cap is not refused at construction
// (it is an aggregate); it is refused here, by every use.
[[nodiscard]] inline bool live_session_grant_usable(cap::NativeExec const& g) noexcept {
    return g.live_session && g.session_wall_ms_cap.has_value() && *g.session_wall_ms_cap > 0 &&
           g.max_processes.has_value() && *g.max_processes > 0;
}

}  // namespace agentengine::trust
