// Fixture for cmake/check_sandbox_exec_limits.cmake: a "create" SandboxExec emitter that does not say what the
// environment was started under. The scan MUST reject this file.
inline void emit_create(EffectContext& ctx) {
    run_event_payload::SandboxExec audit;
    audit.exec_id = "x";
    audit.stage = "create";
    audit.ok = true;
    ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(audit));
}
