#pragma once
// Implements decisions/archive/ADR-209-persistent-shell-sessions.md §9 (I6) -- the DECLARATIVE form of a
// `cap::NativeExec` grant, so a host configuring native execution from YAML/JSON can express exactly what a
// CRTP `cap::decl::NativeExec<...>` declaration expresses, the held-shell opt-in (`live_session`) and its two
// caps included. Before ADR-209 no declarative path produced a capability at all (the declarative agent
// compiler carries none); this is the one place a configuration document becomes a `cap::NativeExec`.
//
//   native_exec:
//     program: "pwsh"            # required; a trailing '*' is a prefix grant (ADR-071)
//     worktree_mount: "workdir"  # required
//     cpu_ms_cap: 30000          # required: a positive integer or `unbounded` (ADR-244 slice 5)
//     wall_ms_cap: 30000
//     memory_bytes_cap: 536870912
//     live_session: true         # optional, default false (ADR-209 §9: the held-shell opt-in)
//     session_wall_ms_cap: 3600000   # required when live_session is true
//     max_processes: 64              # required when live_session is true
//
// Fails closed: an unknown key, a wrong type, a negative/fractional/out-of-range number or a missing required
// key is refused with `native_exec_grant.invalid` (an absent cap is `choice.missing.grant.<field>`, a `0` is
// `choice.zero_bound`; trust/grant_limit.hpp) -- never ignored, so a misspelt `live_sesion: false` cannot
// silently mean something else. Host-authored configuration only (I3): nothing model-derived reaches this.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/grant_limit.hpp"

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
    auto const* program = v.find("program");
    auto const* mount = v.find("worktree_mount");
    if (program == nullptr || !program->is_string() || program->as_string().empty()) {
        return bad("'program' is required and must be a non-empty string");
    }
    if (mount == nullptr || !mount->is_string() || mount->as_string().empty()) {
        return bad("'worktree_mount' is required and must be a non-empty string");
    }
    auto const live = [&]() -> bool {
        auto const* f = v.find("live_session");
        return f != nullptr && f->is_bool() && f->as_bool();
    }();
    auto const lim = [&](std::string_view key) { return parse_grant_limit(v, key); };
    auto cpu = lim("cpu_ms_cap");
    if (!cpu) return std::unexpected(cpu.error());
    auto wall = lim("wall_ms_cap");
    if (!wall) return std::unexpected(wall.error());
    auto mem = lim("memory_bytes_cap");
    if (!mem) return std::unexpected(mem.error());
    // The two held-shell caps are required only when the grant opts into a held shell (`live_session`); otherwise they
    // are inert, and stating one is still validated.
    auto session_wall = cap::Limit::unbounded();
    auto max_processes = cap::Limit::unbounded();
    if (live || v.find("session_wall_ms_cap") != nullptr) {
        auto r = lim("session_wall_ms_cap");
        if (!r) return std::unexpected(r.error());
        session_wall = *r;
    }
    if (live || v.find("max_processes") != nullptr) {
        auto r = lim("max_processes");
        if (!r) return std::unexpected(r.error());
        if (auto const n = r->limit(); n.has_value() && *n > UINT32_MAX) {
            return bad("'max_processes' must be at most 2^32-1 (the Job Object's active-process limit is a 32-bit count)");
        }
        max_processes = *r;
    }
    cap::NativeExec g{program->as_string(), mount->as_string(), *cpu, *wall, *mem, false, session_wall, max_processes};
    if (auto const* f = v.find("live_session"); f != nullptr) {
        if (!f->is_bool()) return bad("'live_session' must be true or false");
        g.live_session = f->as_bool();
    }
    return g;
}

// ADR-209 §9's use-site rule: a held native shell may be offered under `g` only if it opts in AND bounds both
// the session's life and its process count. A live grant missing either cap is not refused at construction
// (it is an aggregate); it is refused here, by every use.
// A cap above 2^62 ms is refused (§15.5): it would not survive the conversion to a signed duration, and a
// "bound" that large bounds nothing.
inline constexpr std::uint64_t kMaxLiveSessionWallMs = std::uint64_t{1} << 62;
[[nodiscard]] inline bool live_session_grant_usable(cap::NativeExec const& g) noexcept {
    auto const wall = g.session_wall_ms_cap.limit();
    auto const procs = g.max_processes.limit();
    return g.live_session && wall.has_value() && *wall > 0 && *wall <= kMaxLiveSessionWallMs && procs.has_value() &&
           *procs > 0;
}

}  // namespace agentengine::trust
