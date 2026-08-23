#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for the `native-jail` profile,
// Windows half (008-Sandbox-and-Isolation.md §1b, §2, §3) -- Milestone 2 Phase C task C2
// (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md). Written fresh against 008
// as currently Reviewed (per project-owner direction, 2026-08-05), carrying forward
// decisions/ADR-004-appcontainer-native-jail-windows-backend.md's *findings*, not its unbuilt spike
// code:
//   - AppContainer (`app_container_profile.hpp`) for process identity/authority: zero granted
//     Windows capabilities (denies NetOut/socket by construction, ADR-004 AC-S1) and
//     PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
//     (denies nested process creation, ADR-004 AC-S2, 008 §4's "Exec (nested): denied" row).
//   - Job Object (`job_object_limits.hpp`, already built) for memory_bytes/pids -- confirmed
//     reliable -- and cpu_ms -- confirmed BEST-EFFORT ONLY (ADR-004 §10.5); the wall-clock watch
//     that class implements is this backend's actual trusted timeout mechanism, not cpu_ms.
//   - AppContainer's ACL denial is the §1b layer-3 BACKSTOP for filesystem access, never the
//     primary boundary (ADR-004 §6 finding 1 -- a curated set of OS files carry `ALL (RESTRICTED)
//     APPLICATION PACKAGES` read ACEs by Windows' own default). Interpreter-level `open()`
//     mediation (010, layers 1-2) IS built (`MediatedFileSystemAdapter`/`MediatedPythonRunner`,
//     ADR-014, Judged).
//
//     SUPERSEDED (2026-08-23): this comment previously carried a "Correction" claiming real Python
//     execution never reaches `NativeJailBackend` at all, and that this was fine because "the
//     embedded CPython interpreter is the one mediated code-interpreter path, permanently, with no
//     second local isolation technology" closed the gap "by an entirely different, now-permanent
//     mechanism." That claim was WRONG and was made without any ADR amending 008/010's own "locked
//     to native-jail permanently" requirement (component-role-audit-tracker.md Findings Q and R,
//     2026-08-23: zero OS-level resource containment on the interpreter process, and the §1b layer-3
//     backstop 008 itself calls non-optional was simply absent for Python). The REAL fix, replacing
//     that claim: `MediatedPythonRunner` (mediated_python_runner.hpp) is now an IPC client --
//     `create_python_worker()`/`exec_session()`/`refresh_python_tools()` below spawn and drive a
//     SEPARATE, AppContainer+Job-Object-jailed OS process (`agentengine_python_worker.exe`,
//     `python_worker_main.cpp`/`python_worker_mediation.cpp`) that hosts the real CPython embed --
//     layer 1/2 mediation (`python_lockdown.hpp`'s own findings, carried into
//     `python_worker_mediation.cpp`) stays exactly as strong as before, ADDITIVE to this backend's
//     real OS-level jail, not replaced by it. `PythonLockdownInterpreter` itself
//     (`python_lockdown.{hpp,cpp}`) is untouched -- its own mediation logic was never this backend's
//     concern, before or after this fix. Whether the real *shell* (`MediatedShellRunner`,
//     `mediated_shell_dispatch.cpp`) is meant to route through this same jailed-worker shape for its
//     own process-spawning case is explicitly NOT decided here -- named as a residual this design's
//     `FramedChannel`/message-catalog split (`jailed_worker_rpc.hpp`,
//     `mediated_python_worker_protocol.hpp`) is shaped to compose with, not solved.
//
// Scope, matching decision 3 of the M2 breakdown doc: this backend's M2 exec() target is a
// compiled probe program, not the Python interpreter or a real shell -- 010's PythonRunner/
// ShellRunner were originally expected to become real Runners plugging into this same SandboxBackend
// in M3; per the correction above, that happened for neither in the way this sentence describes.
// `ExecRequest::source` is therefore, for M2 only, a full Win32 command line (an already-resolved
// executable path plus arguments) that the CALLER -- test code today, a closed Runner/Tool
// registry from M3 onward (008 §1b layer 2, 010 §1a) -- is trusted to have already resolved from a
// name. This backend does not itself interpret `language`; it exists on ExecRequest for forward
// compatibility with the Runner-routed shape 010 will need. A raw command line reaching this
// backend from anywhere an agent's own output could shape unmediated would violate I2/I3 -- that
// mediation is the caller's job, not this backend's, exactly as 008 §1b layer 2 already specifies
// for `subprocess`/`ToolCall` dispatch generally.
//
// NOT implemented here (see docs/issues/m2-phase-c-native-jail-sandbox.md): the abuse-case corpus
// and its G2 positive controls (C3), cross-platform parity (C4), the no-ambient-authority probe
// (C5), the teardown-cycle census (C6), and the Linux backend (C2's own follow-up, 021 §2
// sequencing -- Windows first). This file proves create/exec/destroy work end to end with
// ResourceLimits enforced and structured ExecOutcome values -- the minimum C2 itself commits to.
//
// `MountSpec::source` handling: only the `std::string` (host path) alternative is supported here.
// A `BlobRef` mount source fails closed with a policy error naming the gap -- materializing a
// blob-store mount into a live filesystem grant is unscoped, new work this task does not attempt
// (explicit gap, not silent, per CLAUDE.md).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/job_object_limits.hpp"
#include "backends/native_jail/tool_bridge.hpp"

namespace agentengine::native_jail {

// Jailed-Python-worker design (008-Sandbox-and-Isolation.md §1b/§3, 010-Python-Code-Interpreter.md
// §2/§6 -- decisions/ADR-081-jailed-python-worker-process-slice-1.md is the ADR superseding this
// header's former "Correction (2026-08-23)" comment, which claimed the CPython-interpreter/host-
// filesystem-boundary gap was closed "by an entirely different, now-permanent mechanism" without any
// ADR amending 008/010's "locked to native-jail permanently" requirement; it was not closed, and this
// design is the real fix). Host-configured, per-session
// inputs for a `sandbox_lifetime::per_session` Python worker -- the create_python_worker()/
// exec_session() surface below, additive to the existing per_exec create()/exec()/destroy() (which
// this design leaves byte-for-byte untouched; see native_jail_backend.cpp for the boundary).
struct PythonWorkerSessionConfig {
    std::wstring python_home;
    std::vector<std::wstring> extra_sys_path;
    std::vector<std::string>  package_policy_allowlist;
    std::vector<std::string>  caller_gated_modules;  // accepted, UNUSED this pass -- mirrors
                                                        // MediatedPythonConfig's own Stage C residual.
    // Host-only; consulted by the call_tool worker_query dispatch handler AND used to RENDER
    // `agent.tools`' Python source text (agent_tools_codegen.hpp's
    // generate_agent_tools_module_source, called from create_python_worker()/refresh_python_tools())
    // -- never sent to the worker itself. A `ToolDescriptor::InvokeFn` is a live closure over host
    // state (core/tool_pipeline.hpp) that must never reach a jailed child (I2); only the rendered
    // TEXT crosses into `init_request`/`refresh_tools_request`.
    std::optional<ToolBridgeConfig> tool_bridge;
    bool   expose_agent_files_data = false;
    bool   expose_agent_ask = false;
    std::size_t output_cap_bytes = 0;  // 0 -> the worker's own default (output_discipline.hpp)
    std::wstring worker_exe_path;      // fixed, host-controlled path to agentengine_python_worker.exe
                                        // -- NEVER derived from anything guest/model-influenced (I2).
                                        // Set by the caller (MediatedPythonRunner, from the
                                        // build-injected AE_PYTHON_WORKER_EXE_PATH macro); this class
                                        // stays generic (it does not itself know it is "for Python").

    // Watchdog knobs (RT2 Findings 1 and 4's fix -- session-scoped watchdog, native_jail_backend.cpp).
    std::chrono::milliseconds init_timeout_ms{10000};
    std::chrono::milliseconds idle_cpu_window_ms{1000};
    std::chrono::milliseconds idle_cpu_budget_ms{100};
};

class NativeJailBackend {
public:
    static constexpr ProfileTraits traits{
        /*strength=*/50,
        /*platform_mask=*/static_cast<std::uint8_t>(platform_id::windows_x86_64),  // Linux: C2 follow-up
        cold_start_class::milliseconds,
    };

    NativeJailBackend() = default;
    ~NativeJailBackend() = default;
    NativeJailBackend(NativeJailBackend const&) = delete;
    NativeJailBackend& operator=(NativeJailBackend const&) = delete;
    NativeJailBackend(NativeJailBackend&&) = delete;   // instances_ holds handles other objects
    NativeJailBackend& operator=(NativeJailBackend&&) = delete;  // reference by opaque_id; keep simple

    result<SandboxHandle> create(SandboxSpec const& spec, EffectContext& ctx);
    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx);
    void destroy(SandboxHandle& handle);

    // ---- Jailed-Python-worker surface (additive; NOT part of the SandboxBackend concept -- see
    // sandbox/sandbox.hpp's own `exec_session` is intentionally absent from that `requires` clause,
    // matching the final spec §4's "additive non-concept method" statement). ----

    // Spawns a fresh, AppContainer+Job-Object-jailed worker process (agentengine_python_worker.exe),
    // starts its session-scoped watchdog, and blocks for the worker's `init_response` (bounded by
    // `session.init_timeout_ms` via that same watchdog). Fails closed on any step.
    [[nodiscard]] result<SandboxHandle> create_python_worker(SandboxSpec const& spec,
                                                               PythonWorkerSessionConfig session,
                                                               EffectContext& ctx);

    // One REPL-style call against an already-created worker: single-flight (a second concurrent call
    // on the same handle fails fast, RT1 Finding 2), exec_seq-checked against replay/misattribution
    // (RT1 Finding 1), globals persist across calls within the worker's one process. `state` mirrors
    // `ExecRequest`'s own in/out `cwd`/`env` contract one layer up (sandbox/runner.hpp's `ExecState`).
    [[nodiscard]] result<ExecOutcome> exec_session(SandboxHandle const& handle, ExecRequest request,
                                                     ExecState& state, EffectContext& ctx);

    // Re-renders and re-runs the agent.tools bootstrap against an ALREADY-running worker (mirrors
    // MediatedPythonRunner::refresh_agent_tools()'s own "no interpreter teardown" contract).
    [[nodiscard]] result<void> refresh_python_tools(SandboxHandle const& handle, ToolBridgeConfig config);

private:
    // ae-naming-lint: allow watchdog_phase — this design's own new vocabulary, not yet in 027's registry
    enum class watchdog_phase { awaiting_init, idle, call_active, stopping };

    // Populated only for a `create_python_worker()`-created Instance -- see `Instance::worker` below.
    // RT1/RT2's fixes (final spec §4) live entirely on these fields; §6's watchdog thread reads and
    // writes them from a SEPARATE thread than exec_session()'s caller, hence the atomics/mutex.
    struct PythonWorkerState {
        PROCESS_INFORMATION process{};
        HANDLE downstream_write = nullptr;  // host's end: host WRITES here, worker reads
        HANDLE upstream_read = nullptr;     // host's end: host READS here, worker writes

        // Host-side counter for the "pycall-N" ToolCallRequest ids dispatch_worker_query() mints for
        // each call_tool relay (native_jail_backend.cpp) -- a SEPARATE sequence from the worker's own
        // `call_id` field on the worker_query envelope (that one is the worker's own request/response
        // pairing device, minted worker-side; this one is invoke_tool()'s own request-id convention).
        std::uint64_t next_call_id = 0;
        std::uint64_t next_exec_seq = 1;

        std::atomic<std::uint64_t> active_exec_seq{0};  // 0 == no exec_request outstanding -- RT1 F1
        std::mutex call_mutex;                          // try_lock at exec_session() entry -- RT1 F2
        std::atomic<bool> alive{true};                  // sticky; false once a kill/death is observed

        std::thread watchdog_thread;
        std::atomic<watchdog_phase> phase{watchdog_phase::awaiting_init};
        HANDLE stop_event = nullptr;
        std::atomic<job_kill_reason> kill_reason{job_kill_reason::none};
        std::chrono::steady_clock::time_point phase_deadline{};
        std::uint64_t idle_window_start_cpu_100ns = 0;
        std::chrono::steady_clock::time_point idle_window_start_time{};

        PythonWorkerSessionConfig session_config;  // retained for idle_cpu_*/init_timeout_ms, read by
                                                    // the watchdog thread (immutable after
                                                    // create_python_worker() returns -- safe to read
                                                    // unsynchronized from the watchdog thread).
    };

    // Defined fully here, not merely forward-declared: unlike std::vector, std::unordered_map does
    // NOT support an incomplete value_type by standard guarantee, and MSVC STL enforces this at
    // instantiation time (a Pimpl-style forward declaration compiles cleanly on some standard
    // library implementations and hard-errors on this one) -- `Instance` stays private, just not
    // opaque across the header boundary.
    struct Instance {
        JobObjectLimits job;
        ResourceLimits  limits;
        std::wstring    cwd;  // first read-write mount's host path, if any; empty = inherit
        std::optional<PythonWorkerState> worker;  // populated only for the per_session/Python path
                                                    // (create_python_worker()); nullopt for every
                                                    // ordinary per_exec Instance create() makes.
    };
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;

    void session_watchdog_loop(Instance& inst);
    // Hard-kill only (TerminateJobObject) -- safe to call from ANY thread, including from WITHIN the
    // watchdog thread's own loop (the wall-clock/memory/idle-CPU kill cases) -- never joins
    // `watchdog_thread` itself (a thread cannot join itself). Use `stop_watchdog` below to actually
    // tear the watchdog thread down; that one must never be called from inside the watchdog thread.
    void terminate_worker(Instance& inst);
    // Orderly watchdog shutdown: phase -> stopping, signal stop_event, join. Callable ONLY from a
    // thread other than `watchdog_thread` itself (destroy(), or create_python_worker()'s own
    // failure-recovery paths) -- exec_session()'s protocol-violation path deliberately does NOT call
    // this (it calls `terminate_worker` only, see that method's own comment), leaving the watchdog
    // thread running harmlessly against an already-dead process until destroy() tears it down for
    // real.
    void stop_watchdog(Instance& inst);
    // Dispatches one worker_query frame (call_tool live this pass; open/listdir/connect_* return the
    // fixed Slice-2 deny -- native_jail_backend.cpp's own header comment on this method has the full
    // per-kind breakdown) and sends the worker_query_response back over `inst.worker->downstream_write`.
    void dispatch_worker_query(Instance& inst, json::Value const& query_frame, EffectContext& ctx);
};

static_assert(SandboxBackend<NativeJailBackend>,
              "NativeJailBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::native_jail
