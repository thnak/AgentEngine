// Implements mediated_python_runner.hpp -- see that header's file-top comment for exactly what
// changed versus the in-process shape this class had before the jailed-Python-worker redesign
// (008-Sandbox-and-Isolation.md §1b/§3, 010-Python-Code-Interpreter.md §2/§6). This translation unit
// has NO CPython dependency at all -- every real interpreter concern lives in
// python_worker_mediation.cpp now, inside the separate worker process this class's `initialize()`
// spawns via `NativeJailBackend::create_python_worker()`.

#include "backends/native_jail/mediated_python_runner.hpp"

#include "backends/native_jail/native_jail_win32_helpers.hpp"  // widen/narrow

#ifndef AE_PYTHON_WORKER_EXE_PATH
#error "AE_PYTHON_WORKER_EXE_PATH must be defined (CMakeLists.txt: agentengine_mediated_python_runner \
propagates it PUBLIC from $<TARGET_FILE:agentengine_python_worker>) -- a fixed, host-controlled, \
build-time path, never derived at runtime from anything guest/model-influenced (I2)."
#endif

namespace agentengine::native_jail {

MediatedPythonRunner::MediatedPythonRunner(MediatedPythonConfig config, NativeJailBackend& backend)
    : config_(std::move(config)), backend_(backend) {}

MediatedPythonRunner::~MediatedPythonRunner() {
    if (initialized_) {
        backend_.destroy(handle_);
        initialized_ = false;
    }
}

result<void> MediatedPythonRunner::initialize() {
    if (initialized_) {
        return std::unexpected(error{failure_class::contract,
                                      "MediatedPythonRunner::initialize() called twice",
                                      "python.already_initialized"});
    }

    PythonWorkerSessionConfig session;
    session.python_home = widen(config_.python_home);
    session.extra_sys_path.reserve(config_.extra_sys_path.size());
    for (auto const& p : config_.extra_sys_path) session.extra_sys_path.push_back(widen(p));
    session.package_policy_allowlist.assign(config_.package_policy_allowlist.begin(),
                                             config_.package_policy_allowlist.end());
    session.caller_gated_modules.assign(config_.caller_gated_modules.begin(),
                                         config_.caller_gated_modules.end());
    session.tool_bridge = config_.tool_bridge;
    session.expose_agent_files_data = config_.expose_agent_files_data;
    session.expose_agent_ask = config_.expose_agent_ask;
    session.output_cap_bytes = config_.output_cap_bytes;
    session.worker_exe_path = widen(std::string(AE_PYTHON_WORKER_EXE_PATH));
    // HandleRelay design draft §1 item 2: the SAME map that seeds `spec.mounts` below (the OS-level
    // ACL grant) also needs to reach `dispatch_worker_query`'s "open"/"listdir" handlers host-side, so
    // it can resolve a guest-supplied mount_id before ever calling `open_within_mount_root` -- a
    // second place this already-host-owned value is read from, not a new grant surface (I2).
    session.mount_roots = config_.mount_roots;

    SandboxSpec spec;
    spec.mounts.reserve(config_.mount_roots.size());
    for (auto const& [mount_id, host_path] : config_.mount_roots) {
        spec.mounts.push_back(MountSpec{
            .source = narrow(host_path),
            .guest_path = "/" + mount_id,
            .read_write = true,
        });
    }
    spec.limits.wall_ms = config_.exec_wall_ms;
    spec.limits.memory_bytes = config_.memory_bytes;
    spec.limits.pids = config_.pids;

    EffectContext init_ctx{};  // create_python_worker() does not examine ctx today, matching create()'s
                                // own identical unused-ctx-parameter precedent one layer down; accepted
                                // for interface symmetry with a future audit use, not a live dependency.
    auto created = backend_.create_python_worker(spec, std::move(session), init_ctx);
    if (!created.has_value()) return std::unexpected(created.error());

    handle_ = std::move(*created);
    initialized_ = true;
    return {};
}

result<ExecOutcome> MediatedPythonRunner::run(ExecRequest request, ExecState& state, EffectContext& ctx) {
    if (!request.language.empty() && request.language != "python") {
        return std::unexpected(error{failure_class::contract,
                                      "MediatedPythonRunner cannot run language: " + request.language,
                                      "python.unsupported_language"});
    }
    if (!initialized_) {
        return std::unexpected(error{failure_class::fatal, "MediatedPythonRunner is not initialized",
                                      "python.not_initialized"});
    }
    return backend_.exec_session(handle_, std::move(request), state, ctx);
}

result<void> MediatedPythonRunner::refresh_agent_tools(ToolBridgeConfig config) {
    if (!initialized_) {
        return std::unexpected(error{failure_class::fatal, "MediatedPythonRunner is not initialized",
                                      "python.agent_tools_not_initialized"});
    }
    config_.tool_bridge = config;
    return backend_.refresh_python_tools(handle_, std::move(config));
}

} // namespace agentengine::native_jail
