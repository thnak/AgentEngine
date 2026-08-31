#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2/§3a/§9 G7 -- Milestone 3 Phase E2's `PythonRunner`
// (embedded CPython under native-jail, satisfying `sandbox/runner.hpp`'s `Runner` concept), REBUILT
// as an IPC CLIENT by the jailed-Python-worker design (008-Sandbox-and-Isolation.md §1b/§3,
// 010 §2/§6 -- the ADR superseding native_jail_backend.hpp's former "Correction (2026-08-23)"
// comment, which asserted the CPython-interpreter/host-filesystem-boundary gap was closed by a
// mechanism that turned out never to touch NativeJailBackend at all; it did not close the gap, and
// this design is the real fix).
//
// WHAT CHANGED from the previous (in-process) shape of this class, stated plainly: every method here
// now delegates to `NativeJailBackend::create_python_worker()`/`exec_session()`/`refresh_python_tools()`
// (native_jail_backend.hpp) instead of touching CPython's C API directly -- this translation unit no
// longer includes `<Python.h>` at all, does not link against the CPython import library, and holds no
// interpreter state of its own. The real CPython embedding, the meta-path finder, the Stage D
// open/socket/subprocess mediation wrappers, and the agent.tools/agent.files/agent.data/agent.ask
// bootstraps all moved to `python_worker_mediation.{hpp,cpp}`, which runs inside a SEPARATE,
// AppContainer+Job-Object-jailed OS process (`python_worker_main.cpp`, built as the
// `agentengine_python_worker` executable) -- not inside whatever process constructs a
// `MediatedPythonRunner`. `PythonLockdownInterpreter`/`python_lockdown.{hpp,cpp}` are UNCHANGED and
// uninvolved either way (that class's own mediation logic was never this file's concern, before or
// after this redesign).
//
// WHAT DID NOT CHANGE: the public API's SHAPE -- `initialize()`/`ok()`/`run()`/`refresh_agent_tools()`
// and `MediatedPythonConfig`'s field set (see that struct's own comments for the two additive fields
// this redesign needed: real `ResourceLimits` inputs the old in-process shape never needed, and
// `worker_exe_path` is NOT one of them -- it is derived automatically from the build-injected
// `AE_PYTHON_WORKER_EXE_PATH` macro, never a caller-supplied field, so every existing call site keeps
// compiling with ONLY its constructor call needing a second argument (see below)). `run()`'s Runner-
// concept signature is byte-identical.
//
// THE ONE CALL-SITE-VISIBLE BREAK: the constructor now takes a `NativeJailBackend&` (this class does
// not own one -- native_jail_backend.hpp's own header explains why `NativeJailBackend` is neither
// copyable nor movable, so every caller supplies its own long-lived instance, exactly the shape
// `tools/cli_chat.cpp`'s `shared_python_runner()` singleton already established for the runner itself
// one layer up). `MediatedPythonRunner(MediatedPythonConfig config, NativeJailBackend& backend)`.
//
// STAGE C (unchanged residual): `caller_gated_modules` is still accepted, still UNUSED -- see
// `python_worker_mediation.hpp`'s identical note; this redesign did not touch that decision.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/native_jail_backend.hpp"
#include "backends/native_jail/output_discipline.hpp"
#include "backends/native_jail/tool_bridge.hpp"

namespace agentengine::native_jail {

// Host-configured, per-session inputs -- constructed once, for this `MediatedPythonRunner` object's
// whole lifetime, matching ADR-002 §5.5.6's "one process per session" scope, now made literal by the
// jailed-worker redesign (the worker process this config spawns really does live for exactly this
// object's lifetime, not merely "for the host process's lifetime" the way the old in-process shape
// approximated it).
struct MediatedPythonConfig {
    std::string python_home;                              // e.g. "C:/Users/thanh/miniconda3"
    std::vector<std::string> extra_sys_path;               // curated site-packages, never site.py
    std::unordered_set<std::string> package_policy_allowlist;
    std::unordered_set<std::string> caller_gated_modules;  // accepted, UNUSED this pass -- Stage C.

    // guest-visible mount_id -> real host directory. UNCHANGED field, but its consequence changed:
    // the redesign forwards each entry into `SandboxSpec::mounts` (`initialize()`, .cpp) so the
    // worker's AppContainer identity gets a real ACL grant on the path -- real containment, not just
    // bookkeeping -- but the Slice-1 `open()`/`listdir()` worker_query dispatch (native_jail_backend.cpp)
    // still answers every call with the fixed `not_implemented_this_slice` deny regardless (§7,
    // deferred to Slice 2) -- so a mount configured here is GRANTED at the OS level already, but not
    // yet REACHABLE through the mediated open()/agent.files surface this pass.
    std::unordered_map<std::string, std::wstring> mount_roots;

    // Milestone 3 Phase F2 (010 §6): the pre-registered `call_tool` bridge for this session -- which
    // tools are reachable from inside the interpreter, at what capability set (the SANDBOX's own,
    // never any agent-level ceiling), and whether the host bundled-approved the whole set at
    // `execute_code` time. `nullopt` means no tools are bridged this session -- `call_tool(...)`
    // fails closed with a PermissionError, matching every other host-configured surface here.
    std::optional<ToolBridgeConfig> tool_bridge;

    // Milestone 3 Phase G2 (026 §5) / ADR-057 §9 (026 §5's `agent.ask`) -- unchanged from before this
    // redesign; see MediatedPythonConfig's own historical comment (now carried in
    // python_worker_mediation.hpp's WorkerInitConfig) for why these are separate opt-ins from
    // `mount_roots`/`tool_bridge` rather than derived from them.
    bool expose_agent_files_data = false;
    bool expose_agent_ask = false;
    // decisions/ADR-154-agent-output-codeact-module.md, 026 §5's `agent.output`.
    bool expose_agent_output = false;
    // decisions/ADR-155-agent-progress-codeact-module.md, 026 §5's `agent.progress`.
    bool expose_agent_progress = false;

    // Milestone 3 Phase F3 (010 §3 items 4/5): the per-session output-discipline cap.
    std::uint64_t output_cap_bytes = kDefaultOutputCapBytes;

    // ---- NEW this redesign: real ResourceLimits inputs for the jailed worker's Job Object. The old
    // in-process shape needed none of these (there was no Job Object, no OS-level cap at all -- that
    // gap is exactly Finding Q/R this whole redesign closes). `exec_wall_ms` is BOTH the per-`run()`
    // call wall-clock deadline (native_jail_backend.cpp's exec_session()) AND, unlike the old shape,
    // now genuinely, externally, preemptibly enforced (the session watchdog thread, not a pre-flight
    // check). Defaults are deliberately generous (this is a correctness floor, not a tuned production
    // budget -- 023's real per-tool budget plumbing is still TBD-baselined, matching every other
    // provisional constant in this subsystem, e.g. output_discipline.hpp's own kDefaultOutputCapBytes)
    // -- a caller that needs a tighter bound sets these explicitly; CLAUDE.md's Machine Safety rule
    // ("sandbox and hostile tests are resource-capped") is why these are never zero (an unset/zero
    // ResourceLimits field means "do not enforce this axis at all," job_object_limits.hpp's own
    // documented contract) even by default.
    std::uint64_t exec_wall_ms = 30'000;
    std::uint64_t memory_bytes = 1024ull * 1024 * 1024;  // 1 GiB
    std::uint32_t pids = 8;
};

// Owns the SandboxHandle for exactly one jailed Python worker process, for this object's lifetime
// (mirrors the old in-process class's "one embedded interpreter" scope, now realized as "one worker
// process" instead). Not copyable, not movable -- `NativeJailBackend::instances_` is keyed by opaque
// id and this object is the sole owner of tearing that entry down (destructor -> backend_.destroy()).
class MediatedPythonRunner {
public:
    MediatedPythonRunner(MediatedPythonConfig config, NativeJailBackend& backend);
    ~MediatedPythonRunner();

    MediatedPythonRunner(MediatedPythonRunner const&) = delete;
    MediatedPythonRunner& operator=(MediatedPythonRunner const&) = delete;
    MediatedPythonRunner(MediatedPythonRunner&&) = delete;
    MediatedPythonRunner& operator=(MediatedPythonRunner&&) = delete;

    // Spawns the jailed worker process (NativeJailBackend::create_python_worker()) and blocks for its
    // `init_response`. Must be called exactly once, before `run()`.
    [[nodiscard]] result<void> initialize();

    [[nodiscard]] bool ok() const { return initialized_; }

    // Satisfies `Runner` (sandbox/runner.hpp) -- byte-identical signature to before this redesign.
    // Delegates to `NativeJailBackend::exec_session()`, which owns exec_seq/single-flight/watchdog
    // concerns entirely; this method is now just the Runner-shaped forwarding call.
    [[nodiscard]] result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx);

    // Reconfigures `agent.tools` on the ALREADY-running worker (NativeJailBackend::refresh_python_tools()).
    [[nodiscard]] result<void> refresh_agent_tools(ToolBridgeConfig config);

private:
    MediatedPythonConfig config_;
    NativeJailBackend& backend_;
    bool initialized_ = false;
    SandboxHandle handle_;
};

} // namespace agentengine::native_jail
