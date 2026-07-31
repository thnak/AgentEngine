#pragma once
// Implements 010-Python-Code-Interpreter.md §1a (the `Runner` concept) and §2 (runtime selection:
// embedded native CPython under native_jail, mediated per 008 §1b — never WASM, never a second
// runtime). Backend: native_jail (008 §1b, §3).

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"

namespace agentengine {

// Satisfies the `Runner` concept (sandbox/runner.hpp). Backs the Interpreter/CodeAct modes
// (010 §1) under the native_jail SandboxBackend (008 §3).
class PythonRunner {
public:
    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        (void)request;
        (void)state;
        (void)ctx;
        // Real PythonRunner logic — embedding CPython, the closed-by-construction import
        // allowlist over the granted package policy (010 §5, 008 §1b layer 1), and mediating the
        // dangerous entry points that remain reachable through allowed modules (`open` against the
        // worktree mount, `socket` through host-mediated egress, `subprocess`/`os.system` as a
        // declared RunnerCall/ToolCall rather than a raw fork — 008 §1b layer 2) — is deliberately
        // NOT implemented here. It is security-critical and needs its own design -> red-team ->
        // prove -> judge cycle and an ADR before it is real, per CLAUDE.md. This stub only proves
        // the shape compiles and satisfies the Runner concept; it is not a security boundary.
        return std::unexpected(
            ae::error{failure_class::fatal, "not implemented", "not_implemented"});
    }
};

} // namespace agentengine
