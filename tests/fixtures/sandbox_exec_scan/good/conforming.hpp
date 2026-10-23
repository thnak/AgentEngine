// Fixture for cmake/check_sandbox_exec_limits.cmake: the same emitter, stating its limits. The scan MUST accept it.
inline void emit_create(EffectContext& ctx) {
    run_event_payload::SandboxExec audit;
    audit.exec_id = "x";
    audit.stage = "create";
    audit.limits = "wall_ms=1000";
    audit.ok = true;
    ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(audit));
}
