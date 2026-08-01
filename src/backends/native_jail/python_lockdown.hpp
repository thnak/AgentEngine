#pragma once
// Implements decisions/ADR-002-pythonrunner-embedding-and-mediation.md (prove phase) --
// specifically §3.0 (Layer 0's `sys.modules` sweep), §3.1 (Design A meta-path finder as the
// primary import-allowlist mechanism), §3.4 items 1 and 3 (setup-time install + per-call identity
// reassertion), and §5.5.2's fix (every enforcement object is a pure C type with no Python-visible
// mutable state). Backend: native_jail (008 §1b, §3).
//
// This header intentionally does NOT include <Python.h> or expose any `PyObject*` -- consumers
// (including python_runner.hpp) see only std types, so a TU that never builds with
// AGENTENGINE_BUILD_PYTHON_RUNNER off doesn't need CPython headers on its include path at all.
// The CPython-facing implementation lives entirely in python_lockdown.cpp.
//
// SCOPE, stated plainly per ADR-002 §5.5.6: this class assumes at most one instance is alive at
// any moment in the process (Py_Initialize/Py_Finalize are process-wide CPython state, and this
// class does not implement `Py_NewInterpreterFromConfig` subinterpreter pooling -- §6 item 5 is
// explicitly NOT adopted by this design). One `PythonLockdownInterpreter` == "one OS process
// hosting one Python-capable session," matching the ADR's own committed scope.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentengine::native_jail {

// Host-configured, per-session inputs to lockdown. `allowed_top_level_modules` is this ADR's
// closed import allowlist (010 §5 package policy's resolved output) -- checked against a name's
// TOP-LEVEL package component only (`numpy.linalg` is permitted iff `numpy` is granted); see
// python_lockdown.cpp's finder implementation for why per-submodule granularity was not attempted
// this pass. `extra_sys_path` lets the host point at a curated venv's site-packages without
// running `site.py` (this implementation always disables `site_import`, per the embedding
// experiment's finding that `site.py` pulls in far more than a lockdown interpreter should trust
// unreviewed -- see the ADR's §8 evidence).
struct PythonLockdownConfig {
    std::string python_home; // e.g. "C:/Users/thanh/miniconda3"
    std::vector<std::string> extra_sys_path;
    std::unordered_set<std::string> allowed_top_level_modules;
    bool install_audit_hook = false; // ADR-002 §3.4 item 2 / claim C1 -- opt-in, NOT ATTEMPTED by
                                      // default in every test (only the dedicated audit-hook test
                                      // exercises it), because PySys_AddAuditHook is irreversible
                                      // and process-wide for the interpreter's lifetime.
};

struct PythonRunOutcome {
    bool ok = false;
    bool escape_attempt = false; // ExecOutcome::exec_outcome_class::escape_attempt, ADR-002 §3.4 item 3
    std::string stdout_text;
    std::string stderr_text;
    std::string error_message; // set when !ok and !escape_attempt (e.g. a genuine Python exception)
};

// Owns exactly one embedded CPython interpreter for the lifetime of the process it's constructed
// in (see file header SCOPE note). Not copyable, not movable (there is exactly one CPython
// runtime per process; a moved-from instance would leave two C++ objects both believing they own
// it).
class PythonLockdownInterpreter {
public:
    explicit PythonLockdownInterpreter(PythonLockdownConfig config);
    ~PythonLockdownInterpreter();

    PythonLockdownInterpreter(PythonLockdownInterpreter const&) = delete;
    PythonLockdownInterpreter& operator=(PythonLockdownInterpreter const&) = delete;
    PythonLockdownInterpreter(PythonLockdownInterpreter&&) = delete;
    PythonLockdownInterpreter& operator=(PythonLockdownInterpreter&&) = delete;

    // Py_InitializeFromConfig, Layer 0 sweep, meta-path finder install (ADR-002 §3.0/§3.1/§3.4
    // item 1), and (if configured) the native audit hook. Must be called exactly once, before any
    // guest code runs. Returns false and sets last_error() on failure.
    bool initialize();

    bool ok() const { return initialized_; }
    std::string const& last_error() const { return last_error_; }

    // Executes `source` as guest code. Performs the per-call identity reassertion (ADR-002 §3.4
    // item 3, strengthened per §5.5.2) BEFORE executing; on a mismatch, `source` is never run and
    // the result carries escape_attempt=true, matching 008/010's existing ExecOutcome vocabulary
    // (mapped onto exec_outcome_class::escape_attempt by PythonRunner, sandbox.hpp).
    PythonRunOutcome run(std::string const& source);

    // --- Test/introspection surface (host-side C API only; used by the prove-phase tests) ---

    // Top-level names currently in sys.modules (deduplicated; a name like "numpy.linalg"
    // contributes "numpy"). Used to assert Layer 0's sweep left exactly the expected minimal set,
    // and to inspect what a real numpy/pandas import pulls in transitively (ADR-002 §4 claim A5).
    std::vector<std::string> snapshot_sys_modules() const;

    // True iff sys.meta_path is still exactly [the one finder instance installed at setup] by
    // pointer identity, AND that instance's type still has tp_dictoffset == 0 (no writable
    // instance __dict__ -- §5.5.2's strengthened check). Exposed standalone (not just folded into
    // run()) so tests can assert the identity check itself has discriminating power (a negative
    // control), per this project's testing standard (ADR-001 §8.8).
    bool lockdown_identity_intact() const;

    // Number of times the native audit hook's "import" branch has fired since initialize()
    // (0 if install_audit_hook was false). ADR-002 claim C1.
    static std::uint64_t audit_import_event_count();

private:
    PythonLockdownConfig config_;
    bool initialized_ = false;
    std::string last_error_;
};

} // namespace agentengine::native_jail
