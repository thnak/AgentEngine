#pragma once
// Implements ADR-030 (session-scoped CodeAct wiring) -- enforces, in code, the constraint
// `decisions/ADR-002-pythonrunner-embedding-and-mediation.md` §5.5.6 Finding 7.8 already documents
// but never enforced: "this design requires one OS process per session." Confirmed directly (not
// assumed) by reading `src/backends/native_jail/mediated_python_runner.cpp`: `run()`/
// `refresh_agent_tools()` route through unsynchronized, process-wide-file-scope globals
// (`g_current_ctx`, `config_.tool_bridge` mutated in place, `g_effective_keep_set`) -- nothing about
// `MediatedPythonRunner` itself is per-caller-reentrant. Two `AgentSession` actor instances calling
// the SAME runner "concurrently" (interleaved across their own independent Quark worker threads --
// `quark::Sequential` only serializes handlers WITHIN one actor instance, never across two) is a
// real cross-session confused-deputy hazard: session B's `refresh_agent_tools()` can overwrite
// `config_.tool_bridge`/`g_current_ctx` while session A's `run()` is still executing A's own Python
// source, so A's next `agent.tools.foo(...)`/mediated `open()` call can be dispatched against B's
// bridged tools and B's `EffectContext` (B's capabilities) instead of A's own.
//
// `CodeActRunnerBinding<RunnerT>` makes it structurally IMPOSSIBLE for a second session to ever
// reach a runner already bound to a different one: `bind()` succeeds once, for whichever session_id
// calls it first, and fails closed (never silently races) for any other session_id thereafter, for
// the life of this binding object -- literally "one interpreter per process, enforced" rather than
// merely commented. This is deliberately NOT a mutex/lock (which would suggest interleaved,
// sequential-but-fair access is intended and safe) -- ADR-002 §5.5.6 does not claim that; multiple
// sessions were never meant to share one interpreter's lifetime at all, even one-at-a-time.
//
// Generic over `RunnerT` on purpose (not hardcoded to `native_jail::MediatedPythonRunner`) so this
// binding's own claim/fail-closed logic is independently, deterministically testable
// (`tests/test_codeact_runner_binding.cpp`) without needing a real embedded CPython interpreter --
// this codebase's own established "no `std::any`/type-erasure" convention (ADR-028 §2) rules out a
// type-erased alternative, and templating is the only fit that doesn't reintroduce that pattern.

#include <optional>
#include <string>

#include "agentengine/core/error.hpp"

namespace agentengine {

template <class RunnerT>
// ae-naming-lint: allow CodeActRunnerBinding — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class CodeActRunnerBinding {
public:
    // Non-owning: `runner` is the host's (e.g. `main()`'s own static-lifetime instance) — this
    // binding never constructs, initializes, or destroys it, mirroring every other "granted
    // externally, must outlive what holds the pointer" contract in this codebase
    // (`AgentSession::capabilities_`'s own comment is the closest precedent).
    explicit CodeActRunnerBinding(RunnerT& runner) noexcept : runner_(&runner) {}

    // Fails closed (mutates nothing) the moment a DIFFERENT session_id ever calls this — the whole
    // point of this type. Calling it again with the SAME session_id that already holds the binding
    // is a no-op success (idempotent — a session reconfiguring itself, e.g. after a host-side
    // no-op re-`configure()` call, must not be treated as a foreign claim).
    [[nodiscard]] result<void> bind(std::string session_id) {
        if (bound_session_id_.has_value() && *bound_session_id_ != session_id) {
            return std::unexpected(error{
                failure_class::policy,
                "the shared CodeAct runner is already bound to a different session for the life of "
                "this process (ADR-002 §5.5.6: one interpreter per process, enforced not just "
                "documented)",
                "codeact.runner_bound_to_other_session"});
        }
        bound_session_id_ = std::move(session_id);
        return {};
    }

    [[nodiscard]] bool is_bound_to(std::string const& session_id) const noexcept {
        return bound_session_id_.has_value() && *bound_session_id_ == session_id;
    }

    [[nodiscard]] bool is_bound() const noexcept { return bound_session_id_.has_value(); }

    // Only meaningful to call once `is_bound_to(caller's own session_id)` is true -- a caller
    // holding no binding has no business reaching the runner, but this type does not itself gate
    // that (the provider's own `configure()`/tool closures are what call `bind()` first).
    [[nodiscard]] RunnerT& runner() const noexcept { return *runner_; }

private:
    RunnerT*                   runner_;
    std::optional<std::string> bound_session_id_;
};

}  // namespace agentengine
