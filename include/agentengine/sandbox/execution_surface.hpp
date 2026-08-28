#pragma once
// Implements ADR-102 Phase 3 (identity-native sandbox/worktree design, ADR-099 §7 A3) --
// `ExecutionSurface`, the generic "give me an isolated place, put this tree's content in it, run one
// command, give me back whatever changed" concept `SandboxRuntime` (sandbox_runtime.hpp) drives.
//
// Ported from docs/planning/proofs/execution_surface/execution_surface.hpp (ADR-099's own standalone,
// live-Docker-tested prove-phase original -- kept as-is, this is a new file). Real change made
// during the port: `probe::ExecOutcome` (the run()-outcome shape) is renamed `SurfaceRunOutcome`, not
// reused as bare `ExecOutcome` -- the real, production `agentengine::ExecOutcome`
// (sandbox/sandbox.hpp, `SandboxBackend`'s own outcome vocabulary: klass/stdout_text/stderr_text/
// result_repr/artifacts/ask_prompt, no raw exit code) already exists and answers a genuinely
// different question. `ExecutionSurface::run()` needs a real process exit code
// (`probe_execution_surface.cpp`'s own real check `r3->exec.exit_code == 7`, ported into this
// phase's own test, proves callers genuinely rely on it) -- reusing the `SandboxBackend` name for a
// shape that cannot express what it expresses would be exactly the kind of collision ADR-102 Phase
// 1's own `IdentityHandle`-vs-`Principal` decision already established the discipline for: keep a
// distinct name where the concept genuinely differs, bridge explicitly if a bridge is ever needed
// (deferred to a later phase, per ADR-101's own, separate `TreeBackendExecutionSurface` bridging
// work -- not reused here, per this phase's own explicit scope decision to stay independent of
// ADR-101, itself still Proposed/unjudged).
//
// SCOPE, matching ADR-102 Phase 3's own boundary: this concept and its `SandboxRuntime` consumer are
// deliberately NOT built as a conforming `agentengine::SandboxBackend` (`sandbox.hpp`, 008 §2a) and
// are NOT wired to any of the real backends (`NativeJailBackend`/`WasmBackend`/`KataBackend`) --
// matching ADR-099 §7's own explicit project-owner direction, carried into this port unchanged.

#include <filesystem>
#include <string>

#include "agentengine/core/error.hpp"

namespace agentengine {

// The real outcome of ONE command run inside an ExecutionSurface -- a raw process exit code (a
// non-zero value is a normal, meaningful RESULT, never itself a `result<>`-level error) plus
// whatever the command wrote to stdout/stderr (merged, matching the real conformer's own `_popen`-
// based capture convention).
struct SurfaceRunOutcome {
    int exit_code = -1;
    std::string stdout_text;
};

// T::reset(host_dir)   -- wipe any prior execution state, then materialize `host_dir`'s CURRENT real
//                          content into the surface's own isolated view. Idempotent: calling it
//                          again re-synchronizes from `host_dir`'s latest content.
// T::run(command)      -- execute `command` INSIDE the surface's isolation boundary, never in this
//                          process. A non-zero `SurfaceRunOutcome::exit_code` is a normal, meaningful
//                          result (the contained command failed), not a `result<>` error -- only a
//                          failure to even ATTEMPT execution (the surface itself is broken) is.
// T::drain_to(host_dir) -- pull everything the surface's own view currently holds back onto real
//                          disk at `host_dir`, overwriting whatever was there. The caller is
//                          responsible for scanning `host_dir` afterward (`RealIoFileSystem::
//                          scan_and_drain_into_tree()`) to turn real bytes into a real, committed
//                          `Tree` -- this concept's own job ends at "the bytes are back on real
//                          disk."
template <class T>
concept ExecutionSurface = requires(T& t, std::filesystem::path const& host_dir,
                                       std::string const& command) {
    { t.reset(host_dir) } -> std::same_as<agentengine::result<void>>;
    { t.run(command) } -> std::same_as<agentengine::result<SurfaceRunOutcome>>;
    { t.drain_to(host_dir) } -> std::same_as<agentengine::result<void>>;
};

}  // namespace agentengine
