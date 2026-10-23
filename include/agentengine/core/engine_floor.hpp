#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 ("Engine floors are named", slice 5, G-C12).
//
// A host-safety bound the engine keeps UNDER an `unbounded()` choice is a named, disclosed value here, not a literal
// inside one backend: the developer who states `unbounded()` for an output or wall-clock cap gets exactly this floor
// and can read it (`describe()`: "unbounded (engine floor 16 MiB)"). Each backend that applies a floor uses the
// constant below, so the value recorded and the value enforced are one object (G-C12); the table is what ADR-242 §3a
// and a recorded choice read. A floor is not a default for an unset choice (an unset choice does not start); it is the
// residual an explicit `unbounded()` still carries, listed so it is a disclosed residual and not a hidden one.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "agentengine/core/bound.hpp"

namespace agentengine {

struct EngineFloor {  // ae-naming-lint: allow EngineFloor — ADR-244 §2.1 new vocabulary (a named host-safety floor); 027 not yet updated
    std::string_view id;       // stable, dotted: "<subsystem>.<quantity>"
    std::uint64_t    value;
    std::string_view unit;     // "bytes", "ms", "tokens"
    std::string_view applies;  // where it is applied, in one clause
};

namespace engine_floor {

// stdout/stderr drains of every spawned process (native process, native jail, Linux jail, Kata exec): an output cap
// stated `unbounded()` still stops reading here, because an unread pipe is a denial of service on the host.
inline constexpr EngineFloor kOutputDrain{"process.output_drain", 16ull * 1024 * 1024, "bytes",
                                          "stdout and stderr drains of a spawned process"};
// A native process whose grant states `wall_ms_cap: unbounded` is still killed here.
inline constexpr EngineFloor kProcessWall{"native_process.wall_clock", 5ull * 60 * 1000, "ms",
                                          "the wait-or-kill of a one-shot native process"};
// A held native shell whose grant states `memory_bytes_cap: unbounded` still gets a Job Object memory limit here.
inline constexpr EngineFloor kShellMemory{"native_shell.memory", 1024ull * 1024 * 1024, "bytes",
                                          "the Job Object memory limit of a held native shell"};
// The container CLIs' (docker, containerd, Kata) per-invocation timeout when the exec states none.
inline constexpr EngineFloor kCliInvocation{"container_cli.invocation_timeout", 30, "seconds",
                                            "one container CLI invocation (create/exec/rm)"};
// Anthropic's Messages API requires `max_tokens`; a client that states no output cap sends this one.
inline constexpr EngineFloor kProviderMaxTokens{"anthropic.max_tokens", 4096, "tokens",
                                                "the request's max_tokens when ChatClientCapabilities names none"};

inline constexpr std::array<EngineFloor, 5> kAll{kOutputDrain, kProcessWall, kShellMemory, kCliInvocation,
                                                 kProviderMaxTokens};

[[nodiscard]] constexpr std::span<EngineFloor const> all() noexcept { return kAll; }

// "unbounded (engine floor 16 MiB)" / "at most 4096": the sentence a recorded choice carries.
[[nodiscard]] inline std::string describe(Bound<std::uint64_t> bound, EngineFloor const& floor) {
    if (!bound.is_unbounded()) return "at most " + bound.describe() + " " + std::string(floor.unit);
    return "unbounded (engine floor " + std::to_string(floor.value) + " " + std::string(floor.unit) + ")";
}

}  // namespace engine_floor
}  // namespace agentengine
