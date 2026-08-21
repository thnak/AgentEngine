#pragma once
// Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md's process-spawn
// primitive for NativeShellProvider/NativeBashProvider/NativePythonProvider/NativeNodeProvider.
//
// Deliberately NOT a `SandboxBackend` (008): there is no AppContainer/seccomp isolation here at
// all -- this is the genuinely new, weaker-isolation capability class ADR-070's "host-configurable
// responsibility boundary" makes room for, not a variant of the existing sandbox subsystem.
// `cap::NativeExec`'s cpu_ms_cap/wall_ms_cap/memory_bytes_cap ARE still enforced, best-effort, via
// the SAME Windows Job Object primitive src/backends/native_jail/job_object_limits.hpp already
// provides -- reused, not duplicated -- but used here WITHOUT the AppContainer half. Resource
// accounting is an orthogonal safety net (CLAUDE.md "Machine safety": a runaway process must not be
// able to take the machine with it), never a substitute for the process/filesystem/network
// isolation this ADR deliberately omits.
//
// Worktree confinement (materializing a real cwd, validating argv path-shaped arguments) is
// EXPLICITLY NOT this file's job -- that is native_worktree_bridge.hpp / the provider layer one
// level up, which reuses the EXISTING src/backends/native_jail/worktree_mount_sync.hpp
// materialize_mount()/harvest_mount() machinery rather than duplicating it here. This file's own
// contract is narrower: given an already-resolved absolute program path, an already-validated
// argv, and an already-materialized real cwd, spawn it, cap resource usage best-effort, and return
// raw (host-safety-capped, NOT yet model-output-capped -- that is output_discipline.hpp's job, one
// layer up again) stdout/stderr. This function never resolves a bare program name against PATH --
// PATH resolution/discovery is native_path_scan.hpp's job, and the actual authority decision is
// cap::NativeExec's (trust/capability.hpp) -- by the time a caller reaches this function, "is this
// invocation authorized" has already been decided elsewhere.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::native_process {

struct NativeExecRequest {
    // Resolved, absolute path to the executable. Never a bare name -- this function does not
    // search PATH (I2: PATH-based name resolution is ambient authority, per
    // AgentEngineSpecification.md §5's own reasoning; the caller must have already resolved this
    // via native_path_scan.hpp and checked it against a held cap::NativeExec).
    std::string program_path;
    // argv[0] is conventionally the program name/path by OS convention; the caller decides what to
    // put there. Every entry has already been validated by the caller (native_worktree_bridge.hpp)
    // if it is path-shaped -- this function trusts its input, it does not re-validate.
    std::vector<std::string> argv;
    // Real, already-materialized host directory the child's cwd is set to (worktree confinement).
    // Required -- empty is a contract violation, not "use the current process's cwd".
    std::string cwd;
    std::optional<std::uint64_t> cpu_ms_cap;
    std::optional<std::uint64_t> wall_ms_cap;      // unset -> a fixed internal safety ceiling is
                                                    // still applied (never literally unbounded).
    std::optional<std::uint64_t> memory_bytes_cap;
    std::uint64_t output_cap_bytes = 1024ull * 1024;  // raw host-safety cap (0 -> internal default)
};

enum class native_exec_outcome_class { ok, nonzero_exit, timeout, oom, spawn_failed };

struct NativeExecOutcome {
    native_exec_outcome_class klass = native_exec_outcome_class::spawn_failed;
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
};

[[nodiscard]] result<NativeExecOutcome> spawn_native_process(NativeExecRequest const& request);

// Exposed for direct unit testing (tests/test_native_process_spawn.cpp): the Microsoft C runtime
// argv-quoting algorithm this module's command-line construction depends on is security-relevant
// (an incorrect quote/backslash escape can make one intended argument split into two, or inject
// what reads as a second flag) -- it gets its own dedicated test vectors, the same way this
// project's ShellRunner grammar parsing does, rather than being verified only indirectly through a
// real spawned process's observed behavior.
namespace detail {
[[nodiscard]] std::wstring quote_one_argument(std::wstring const& arg);
[[nodiscard]] std::wstring build_command_line(std::vector<std::string> const& argv);
}  // namespace detail

}  // namespace agentengine::native_process
