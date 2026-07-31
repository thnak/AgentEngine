#pragma once
// Implements 010-Python-Code-Interpreter.md §1a (the `Runner` concept) and §2 (the shell-dispatch
// design: ShellRunner is engine-native code that parses a small non-POSIX-complete grammar and a
// fixed builtin set, and resolves anything else only to a registered Runner or Tool — never to an
// arbitrary named program on a search path). Backend: native_jail (008 §1b, §3).

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"

namespace agentengine {

// Satisfies the `Runner` concept (sandbox/runner.hpp). Backs the Shell mode (010 §1) under the
// native_jail SandboxBackend (008 §3).
class ShellRunner {
public:
    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        (void)request;
        (void)state;
        (void)ctx;
        // Real ShellRunner logic — the grammar parser (pipes, redirects, variable
        // assignment/expansion, &&/||, minimal control flow), the fixed builtin set (cd, pwd, ls,
        // cat, echo, export, mkdir, rm, mv, cp, ...) dispatched against the worktree (025) and the
        // capability layer (007), and the "anything else resolves only to a registered Runner or
        // Tool, never exec'd" rule (010 §2) — is deliberately NOT implemented here. It is
        // security-critical (I2: a real shell's resolve-and-exec shape is exactly what this design
        // must not reproduce) and needs its own design -> red-team -> prove -> judge cycle and an
        // ADR before it is real, per CLAUDE.md. This stub only proves the shape compiles and
        // satisfies the Runner concept; it is not a security boundary.
        return std::unexpected(
            ae::error{failure_class::fatal, "not implemented", "not_implemented"});
    }
};

} // namespace agentengine
