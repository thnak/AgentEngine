#pragma once
// Implements 010-Python-Code-Interpreter.md §1a (the `Runner` concept) and §2 (runtime selection:
// embedded native CPython under native_jail, mediated per 008 §1b — never WASM, never a second
// runtime). Backend: native_jail (008 §1b, §3).
//
// Mediation is per decisions/ADR-002-pythonrunner-embedding-and-mediation.md's prove phase: Layer
// 0's `sys.modules` sweep + the `sys.meta_path` allowlist finder (§3.0/§3.1) are real and load-
// bearing (backed by `native_jail::PythonLockdownInterpreter`, python_lockdown.{hpp,cpp}). What is
// NOT yet implemented here, stated plainly rather than silently: the `builtins.__import__`/
// `importlib.import_module` defense-in-depth wrappers (§3.4 item 1's second half), the
// `open`/`socket`/`subprocess`-family mediation wrappers (§5), and per-call capability-freshness
// derivation from `EffectContext`/`CapabilitySet` (§3.4's closing paragraph — `CapabilitySet` has
// no enforcement machinery yet, trust/capability.hpp's own header comment). This `run()` therefore
// uses a FIXED allowlist baked in at construction time, not one derived per call from `ctx`'s
// capabilities — a known, stated scope limitation of this prove-phase implementation, not a design
// claim that capability-freshness is solved.

#include <unordered_set>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/python_lockdown.hpp"

namespace agentengine {

// Satisfies the `Runner` concept (sandbox/runner.hpp). Backs the Interpreter/CodeAct modes
// (010 §1) under the native_jail SandboxBackend (008 §3).
//
// Constructor-injected with the resolved lockdown config (python home + curated sys.path +
// allowlist) for the whole session, mirroring `ShellRunner`'s constructor-injection pattern
// (ADR-001) — a session's `PythonRunner` is bound to one interpreter for its whole lifetime, never
// re-bound per call, consistent with §5.5.6's one-process-per-session scope. Not default-
// constructible (see tests/test_native_jail_runner_stubs.cpp's comment on why ShellRunner and now
// PythonRunner are both exercised by their own dedicated tests instead).
class PythonRunner {
public:
    explicit PythonRunner(native_jail::PythonLockdownConfig config)
        : interpreter_(std::move(config)) {}

    bool initialize() { return interpreter_.initialize(); }
    std::string const& last_error() const { return interpreter_.last_error(); }

    // Test/introspection passthrough (native_jail::PythonLockdownInterpreter, see that header).
    native_jail::PythonLockdownInterpreter& interpreter() { return interpreter_; }
    native_jail::PythonLockdownInterpreter const& interpreter() const { return interpreter_; }

    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        (void)state; // ExecState.cwd/env mediation (os.chdir/os.getcwd/os.environ) is NOT yet
                     // wired to this interpreter -- NOT ATTEMPTED this pass, see the ADR's §9.
        (void)ctx;   // CapabilitySet-derived per-call allowlist freshness -- NOT ATTEMPTED this
                     // pass; this Runner uses the fixed allowlist given at construction time.
        if (!request.language.empty() && request.language != "python") {
            return std::unexpected(ae::error{failure_class::contract,
                                              "PythonRunner cannot run language: " + request.language,
                                              "python.unsupported_language"});
        }
        if (!interpreter_.ok()) {
            return std::unexpected(
                ae::error{failure_class::fatal,
                          "PythonRunner's interpreter is not initialized: " + interpreter_.last_error(),
                          "python.not_initialized"});
        }

        native_jail::PythonRunOutcome outcome = interpreter_.run(request.source);
        ExecOutcome result{};
        result.stdout_text = outcome.stdout_text;
        result.stderr_text = outcome.stderr_text;
        if (outcome.escape_attempt) {
            result.klass = exec_outcome_class::escape_attempt;
        } else if (outcome.ok) {
            result.klass = exec_outcome_class::ok;
        } else {
            result.klass = exec_outcome_class::policy_violation;
        }
        return result;
    }

private:
    native_jail::PythonLockdownInterpreter interpreter_;
};

} // namespace agentengine
