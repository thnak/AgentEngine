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
// docs/planning/sandbox-spec-capability-enforcement-design-draft.md (2026-08-24, decisions/ADR-087-
// sandbox-spec-capability-enforcement.md): both `create()` and `create_python_worker()` below now
// call `agentengine::authorize_spec()` (sandbox/sandbox.hpp) first -- a real, opt-in check that
// `spec.mounts`/`spec.net` are covered by `spec.capabilities`' `cap::SandboxMount`/`cap::SandboxNetOut`
// grants, a no-op for every caller that doesn't hold one. `create_python_worker()` needed its OWN
// call, not just `create()`'s -- it is a structurally separate mount-granting entry point
// (`MediatedPythonRunner::initialize()` calls it directly, never `create()`), which this design's own
// red-team pass flagged (finding B1) as the one path that matters most in production. Same pass also
// closes a real per-backend divergence in `create()` (finding B3): it now fails closed on any
// `NetPolicy` beyond `deny_all=true`, matching `KataBackend`'s existing identical posture (ADR-086)
// -- this backend has no CNI/egress-proxy of any kind (AppContainer already denies `NetOut`/socket by
// construction regardless, ADR-004 AC-S1), so a caller's spec previously meant two different things
// depending on which backend was selected.
//
// Scope, matching decision 3 of the M2 breakdown doc: this backend's M2 exec() target is a
// compiled probe program, not the Python interpreter or a real shell -- 010's PythonRunner/
// ShellRunner were originally expected to become real Runners plugging into this same SandboxBackend
// in M3; per the correction above, that happened for neither in the way this sentence describes.
// `ExecRequest::source` is therefore, for M2 only, a full Win32 command line (an already-resolved
// executable path plus arguments) that the CALLER -- test code today, a closed Runner/Tool
// registry from M3 onward (008 §1b layer 2, 010 §1a) -- is trusted to have already resolved and
// mediated. This backend does not itself interpret `language`; it exists on ExecRequest for forward
// compatibility with the Runner-routed shape 010 will need. 008-Sandbox-and-Isolation.md §2's
// "`ExecRequest` mediation is the caller's responsibility, not the backend's" clause is the
// canonical statement of why (docs/planning/sandbox-exec-request-capability-mediation-design-
// draft.md investigated and confirmed this is correct-by-design, not unfinished, for every backend
// alike -- that same investigation also surfaced a real, SEPARATE, still-open gap, `cap::Exec`, the
// top-level "may this caller create a sandbox of this profile" capability, currently unenforced by
// any backend -- named there as a disclosed follow-on, not silently folded into this scope note).
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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// pal/net.hpp (winsock2.h/ws2tcpip.h, guarded by WIN32_LEAN_AND_MEAN/NOMINMAX) MUST be included
// before <windows.h> below -- windows.h's own unguarded internal `#include <winsock.h>` (the old,
// pre-winsock2 header) would otherwise already be in effect by the time winsock2.h's
// `_WINSOCKAPI_`-guarded include ran, which is the classic "WinSock.h has already been included"
// conflict. `PythonWorkerState::live_sockets` (below, HandleRelay design draft §2) needs `pal::fd_t`,
// so this header already has a real reason to see pal/net.hpp, not just a transitive-include workaround.
#include "agentengine/pal/net.hpp"

#include <windows.h>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/worktree_mount_fs.hpp"  // SafeFileHandle -- PythonWorkerState::open_files
#include "agentengine/sandbox/net_egress_proxy.hpp"  // VerifiedEndpoint -- set_test_connect_resolver_override
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
    // Host-only, same "never sent to the worker" posture as `tool_bridge` above (I2) -- guest-visible
    // mount_id -> real host directory, consulted by the "open"/"listdir" worker_query dispatch
    // handlers (HandleRelay design draft §1) to resolve a guest-supplied mount_id before ever calling
    // `open_within_mount_root`/`list_within_mount_root`. `MediatedPythonRunner::initialize()` already
    // has this same map (`MediatedPythonConfig::mount_roots`) and forwards it here in addition to the
    // existing `SandboxSpec::mounts` forwarding (the real ACL grant) -- this is wiring an
    // already-host-owned value into a second place it needs to be read from, not a new grant surface.
    std::unordered_map<std::string, std::wstring> mount_roots;
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

    // Grants this backend's shared, deployment-scoped AppContainer profile read+execute access to a
    // FIXED, host-controlled path (never a caller/model-supplied one -- I2), deduplicated for the life
    // of the process the same way `python_home`/`extra_sys_path`/the worker-binary directory already
    // are internally for `create_python_worker()`. Exists so a caller outside this translation unit
    // (e.g. a first-party `Tool<>` that needs one fixed scratch directory readable by an
    // AppContainer'd worker it spawns via plain `create()`/`exec()`, not `create_python_worker()`) can
    // reach that same dedup guarantee -- `grant_ro_deduped()`/`shared_profile()` are anonymous-
    // namespace internals of native_jail_backend.cpp and were never reachable from outside it before
    // this method (docs/planning/pdf-text-extraction-design-draft.md Revision 6, round-5/round-6
    // red-team finding). Calling this repeatedly with the SAME `path` is a safe no-op after the first
    // call -- it does not grow the shared profile's DACL. Does not itself add `path` to any
    // `SandboxSpec::mounts` list; the grant lives on the AppContainer SID's own ACL, independent of any
    // particular `SandboxHandle`'s `SandboxSpec` bookkeeping.
    [[nodiscard]] result<void> grant_ro_path_once(std::wstring const& path);

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

    // TEST-ONLY seam (HandleRelay design draft §2 item 1) -- mirrors `sandbox::HostEgressProxy::
    // resolver`'s own established "testability seam, not a security bypass" precedent exactly:
    // `dispatch_connect_authorize`'s own address-block check (`sandbox::resolve_and_validate`)
    // categorically blocks loopback/private/link-local/CGNAT addresses BY DESIGN, which means no
    // hermetic same-machine test target exists for proving the round trip's POST-resolution
    // composition (does connect_authorize correctly connect to what the resolver reports, does
    // connect_send/connect_recv correctly relay real bytes) -- exactly the same problem
    // `HostEgressProxy::resolver`'s own header comment names for the tool-bridged path. Overrides the
    // process-wide resolver every `dispatch_connect_authorize` call uses; `nullptr` (the default)
    // means "use the real `sandbox::resolve_and_validate`". Production code never calls this --
    // UNLIKE `HostEgressProxy::resolver` (a per-instance field, scoped to one object), this is a
    // process-WIDE static, so setting it affects every worker session sharing the process, and
    // "never used from production" is enforced by convention/review only, not the type system or a
    // build flag. Callers MUST reset to `nullptr` unconditionally (an RAII guard, not a bare
    // set-then-reset pair a thrown exception could skip) -- see
    // `tests/test_native_jail_python_worker_handle_relay.cpp`'s own `TestResolverOverrideGuard` for
    // the pattern every future caller should copy, not reinvent.
    static void set_test_connect_resolver_override(
        std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)> fn);

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

        // HandleRelay design draft §2: the host-owned live sockets a "connect_authorize" worker_query
        // has minted for this worker, keyed by a host-minted, strictly-increasing `socket_id` (never
        // worker-chosen, mirroring `next_exec_seq`'s own "host mints, worker never originates an id"
        // discipline). Every entry is force-closed by `terminate_worker`/`close_worker_handles`
        // (design draft §4 item 5) -- no socket ever outlives the worker process it belongs to.
        // `kMaxLiveSockets` bounds this map's growth regardless of whether the guest ever calls
        // `connect_close` (design draft §4 item 5's own DoS lens).
        std::unordered_map<std::uint64_t, agentengine::pal::fd_t> live_sockets;
        std::uint64_t next_socket_id = 1;
        static constexpr std::size_t kMaxLiveSockets = 16;

        // HandleRelay design draft §1 (revised): open files, keyed the SAME way as `live_sockets` --
        // real I/O against a DuplicateHandle'd file inside the AppContainer'd worker fails with
        // ERROR_INVALID_HANDLE (a reproduced, documented finding, not a design choice), so the host
        // keeps the real `SafeFileHandle` open here and relays read/write/close by id instead, sharing
        // `next_socket_id`'s id space (files and sockets are never confused: each `dispatch_file_*`/
        // `dispatch_connect_*` handler only ever looks its own id up in its own map).
        std::unordered_map<std::uint64_t, agentengine::SafeFileHandle> open_files;

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
    // Closes every raw HANDLE `PythonWorkerState` owns (process, both pipe ends, stop_event) and
    // resets each to nullptr -- idempotent, safe to call more than once on the same instance.
    // Correction (2026-08-23, independent red-team pass, REAL GAP finding): neither
    // `terminate_worker` (kills the process via the Job Object, never closes the HANDLE referring
    // to it) nor `stop_watchdog` (stops the watchdog thread only) ever closed these -- every one of
    // `create_python_worker()`'s failure-return paths after the worker process is created leaked
    // all four handles into the long-running host process. `destroy()` and every one of
    // `create_python_worker()`'s post-creation failure paths now call this instead of duplicating
    // (or omitting) the close logic inline.
    void close_worker_handles(Instance& inst);
    // Dispatches one worker_query frame -- call_tool (Slice 1) and open/listdir/connect_* (Slice 2,
    // HandleRelay design draft) are all live; native_jail_backend.cpp's own comment on this method has
    // the full per-kind breakdown -- and sends the worker_query_response back over
    // `inst.worker->downstream_write`.
    void dispatch_worker_query(Instance& inst, json::Value const& query_frame, EffectContext& ctx);

    // HandleRelay (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md) --
    // per-kind handlers dispatch_worker_query routes to. `static` (no `this` needed): PRIVATE, not
    // free functions, purely because `PythonWorkerState` itself is a private nested type these
    // signatures name -- see native_jail_backend.cpp for the full per-function rationale.
    using QueryFields = std::vector<std::pair<std::string, json::Value>>;
    static QueryFields dispatch_open(PythonWorkerState& ws, EffectContext& ctx, json::Value const& payload);
    static QueryFields dispatch_listdir(PythonWorkerState& ws, EffectContext& ctx, json::Value const& payload);
    static QueryFields dispatch_file_read(PythonWorkerState& ws, json::Value const& payload);
    static QueryFields dispatch_file_write(PythonWorkerState& ws, json::Value const& payload);
    static QueryFields dispatch_file_close(PythonWorkerState& ws, json::Value const& payload);
    static QueryFields dispatch_connect_authorize(PythonWorkerState& ws, EffectContext& ctx,
                                                    json::Value const& payload);
    static QueryFields dispatch_connect_send(PythonWorkerState& ws, json::Value const& payload);
    static QueryFields dispatch_connect_recv(PythonWorkerState& ws, json::Value const& payload);
    static QueryFields dispatch_connect_close(PythonWorkerState& ws, json::Value const& payload);
};

static_assert(SandboxBackend<NativeJailBackend>,
              "NativeJailBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::native_jail
