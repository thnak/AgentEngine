#pragma once
// PROVE-PHASE PROBE (A3): a fresh `ExecutionSurface` concept for THIS design specifically -- NOT
// `agentengine::sandbox::SandboxBackend` (008 Sandbox-and-Isolation.md §2a, the real, shipped
// concept `NativeJailBackend`/`WasmBackend`/`KataBackend` conform to). Per explicit project-owner
// direction, this design's own A3/A9 work is built fresh on this design's own primitives, not
// designed around reusing the real `SandboxBackend`/`ExecRequest`/`SandboxHandle` shape -- reuse
// (or replacing this with a real `SandboxBackend` conformer) is an IMPLEMENTATION-time decision for
// whoever eventually wires this in, not a design-time constraint.
//
// Deliberately narrower than `SandboxBackend`: this design's own `SandboxSession` only ever needs
// "give me an isolated place, put this tree's content in it, run one command, give me back
// whatever changed" -- not the full generic `ExecRequest`/lifecycle-handle shape. Three verbs,
// matching exactly what `docker_sandbox/docker_backend.hpp`'s own already-proven, already-fixed
// (§35 finding 9) `create()`/`copy_to_container()`/`exec()`/`copy_from_container()`/`destroy()`
// sequence already demonstrates end to end (§31) -- this concept is the missing GENERALIZATION of
// that probe's own ad hoc sequence into something `SandboxSession` can drive against more than one
// concrete technology.

#include <concepts>
#include <filesystem>
#include <string>

#include "../common/exec_outcome.hpp"
#include "../common/result.hpp"

namespace probe {

// T::reset(host_dir)   -- wipe any prior execution state, then materialize `host_dir`'s CURRENT
//                          real content into the surface's own isolated view. Idempotent: calling
//                          it again re-synchronizes from `host_dir`'s latest content.
// T::run(command)      -- execute `command` INSIDE the surface's isolation boundary, never in this
//                          process. A non-zero `ExecOutcome::exit_code` is a normal, meaningful
//                          result (the contained command failed), not a `result<>` error -- only a
//                          failure to even ATTEMPT execution (the surface itself is broken) is.
// T::drain_to(host_dir) -- pull everything the surface's own view currently holds back onto real
//                          disk at `host_dir`, overwriting whatever was there. The caller is
//                          responsible for scanning `host_dir` afterward (this design's own
//                          `RealIoFileSystem::scan_and_drain_into_tree()`, §27/§29) to turn real
//                          bytes into a real, committed `Tree` -- this concept's own job ends at
//                          "the bytes are back on real disk," matching `MediatedFileSystem`'s own
//                          layering (§22/§26) of "stage the bytes" vs. "commit the bytes" as two
//                          separate, composable steps.
template <class T>
concept ExecutionSurface = requires(T& t, std::filesystem::path const& host_dir,
                                       std::string const& command) {
    { t.reset(host_dir) } -> std::same_as<result<void>>;
    { t.run(command) } -> std::same_as<result<ExecOutcome>>;
    { t.drain_to(host_dir) } -> std::same_as<result<void>>;
};

}  // namespace probe
