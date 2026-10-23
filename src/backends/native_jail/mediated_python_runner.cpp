// Implements mediated_python_runner.hpp -- see that header's file-top comment for exactly what
// changed versus the in-process shape this class had before the jailed-Python-worker redesign
// (rfcs/008-Sandbox-and-Isolation.md §1b/§3, rfcs/010-Python-Code-Interpreter.md §2/§6). This translation unit
// has NO CPython dependency at all -- every real interpreter concern lives in
// python_worker_mediation.cpp now, inside the separate worker process this class's `initialize()`
// spawns via `NativeJailBackend::create_python_worker()`.

#include "backends/native_jail/mediated_python_runner.hpp"

#include <filesystem>

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
        // A destructor cannot await the `SandboxBackend::destroy` seam (ADR-237 §5.1, #173); the concrete
        // backend's synchronous body is the same teardown.
        backend_.destroy_blocking(handle_);
        initialized_ = false;
    }
}

std::vector<cap::SandboxMount> MediatedPythonRunner::required_mounts(MediatedPythonConfig const& config) {
    std::vector<cap::SandboxMount> out;
    for (auto const& [mount_id, host_path] : config.mount_roots) {
        out.push_back(cap::SandboxMount{narrow(host_path), "/" + mount_id, /*read_write=*/true});
    }
    // The deployment paths `NativeJailBackend::create_python_worker()` mounts read-only at their own path.
    auto deployment = [&out](std::string path) {
        if (!path.empty()) out.push_back(cap::SandboxMount{path, path, /*read_write=*/false});
    };
    deployment(narrow(std::filesystem::path(std::string(AE_PYTHON_WORKER_EXE_PATH)).parent_path().wstring()));
    deployment(config.python_home);
    for (std::string const& p : config.extra_sys_path) deployment(p);
    return out;
}

result<void> MediatedPythonRunner::initialize(CapabilitySet const& host_grant, EffectContext& ctx) {
    if (initialized_) {
        return std::unexpected(error{failure_class::contract,
                                      "MediatedPythonRunner::initialize() called twice",
                                      "python.already_initialized"});
    }
    // ADR-244 §2.4: the runner belongs to the session that creates it -- bound here, at creation, so the
    // mounts granted for that session never serve whichever session happens to run first.
    if (!ctx.session_nonce.minted()) {
        return std::unexpected(error{failure_class::policy,
                                      "initialize() carries no session nonce; the Python runner is created for "
                                      "one session, by a context that session minted",
                                      "codeact.no_session"});
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
    session.expose_agent_output = config_.expose_agent_output;
    session.expose_agent_progress = config_.expose_agent_progress;
    session.output_cap_bytes = config_.output_cap_bytes;
    session.worker_exe_path = widen(std::string(AE_PYTHON_WORKER_EXE_PATH));
    // HandleRelay design draft §1 item 2: the SAME map that seeds `spec.mounts` below (the OS-level
    // ACL grant) also needs to reach `dispatch_worker_query`'s "open"/"listdir" handlers host-side, so
    // it can resolve a guest-supplied mount_id before ever calling `open_within_mount_root` -- a
    // second place this already-host-owned value is read from, not a new grant surface (I2).
    session.mount_roots = config_.mount_roots;

    SandboxSpec spec;
    // ADR-244 §2.4: the authority every mount and deployment path is checked against. Explicit host
    // input, never derived from `config_`.
    spec.capabilities = host_grant;
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

    auto created = backend_.create_python_worker(spec, std::move(session), ctx);
    // ADR-244 §2.6: the worker's creation is an effect like any other sandbox's, so it is audited (I4).
    run_event_payload::SandboxExec audit;
    audit.exec_id = "mediated-python-create";
    audit.backend = "mediated-python";
    audit.stage = "create";
    audit.limits = describe_limits(spec.limits);
    audit.ok = created.has_value();
    audit.error_code = created.has_value() ? std::string{} : created.error().code;
    ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(audit));
    if (!created.has_value()) return std::unexpected(created.error());

    handle_ = std::move(*created);
    {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        bound_session_ = ctx.session_nonce;
    }
    initialized_ = true;
    return {};
}

result<ExecOutcome> MediatedPythonRunner::run_blocking(ExecRequest request, ExecState& state, EffectContext& ctx) {
    if (!request.language.empty() && request.language != "python") {
        return std::unexpected(error{failure_class::contract,
                                      "MediatedPythonRunner cannot run language: " + request.language,
                                      "python.unsupported_language"});
    }
    if (!initialized_) {
        return std::unexpected(error{failure_class::fatal, "MediatedPythonRunner is not initialized",
                                      "python.not_initialized"});
    }
    // ADR-244 §2.4: one runner per session, enforced here rather than by host wiring.
    if (!ctx.session_nonce.minted()) {
        return std::unexpected(error{failure_class::policy,
                                      "this run carries no session nonce; the Python runner serves only a "
                                      "context its session minted",
                                      "codeact.no_session"});
    }
    {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        if (bound_session_->identity != ctx.session_nonce.identity) {
            return std::unexpected(error{failure_class::policy,
                                          "the Python runner is bound to a different session",
                                          "codeact.runner_bound_to_other_session"});
        } else if (bound_session_->epoch != ctx.session_nonce.epoch) {
            // The bound session was cleared, restored or forked since: its interpreter state is not the
            // new incarnation's to inherit. The host creates a new runner.
            return std::unexpected(error{failure_class::policy,
                                          "the Python runner's session has been cleared, restored or forked "
                                          "since it was bound; create a new runner",
                                          "codeact.runner_session_moved"});
        }
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
