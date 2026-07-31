#pragma once
// Implements 010-Python-Code-Interpreter.md §1a (the `Runner` concept) and §2 (the shell-dispatch
// design), per decisions/ADR-001-shellrunner-grammar-and-dispatch.md — Design A (§3): a
// recursive-descent parser produces a pmr-backed AST, which a tree-walking evaluator (§2.3's
// shared dispatch step over the fixed builtin table, §2.4) then executes against an injected
// `FileSystemAdapter` and `CommandRegistry`. Backend: native_jail (008 §1b, §3).
//
// `ShellRunner` is constructor-injected with the `FileSystemAdapter&`/`CommandRegistry const&` it
// dispatches against — the `Runner` concept (sandbox/runner.hpp) only constrains `run()`'s
// signature, not construction, so this doesn't affect `Runner<ShellRunner>` satisfaction. This
// mirrors real usage: a session's `ShellRunner` is bound to one worktree-backed adapter and one
// run's registered Runner/Tool tables for its whole lifetime, never re-bound per call.

#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/command_registry.hpp"
#include "backends/native_jail/shell_dispatch.hpp"
#include "backends/native_jail/shell_parser.hpp"

namespace agentengine {

// Satisfies the `Runner` concept (sandbox/runner.hpp). Backs the Shell mode (010 §1) under the
// native_jail SandboxBackend (008 §3).
class ShellRunner {
public:
    ShellRunner(FileSystemAdapter& fs, native_jail::CommandRegistry const& registry)
        : fs_(fs), registry_(registry) {}

    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        if (!request.language.empty() && request.language != "shell") {
            return std::unexpected(ae::error{failure_class::contract,
                                              "ShellRunner cannot run language: " + request.language,
                                              "shell.unsupported_language"});
        }
        // Two-phase, per §3: the ENTIRE script parses to a complete AST before any node
        // evaluates (A-C2). `shell::parse` is a pure `bytes -> result<ParsedScript>` function
        // with no dependency on fs_/registry_/state/ctx — none of the dispatch/authorization
        // surface below is reachable from inside it, by type.
        auto parsed = shell::parse(request.source);
        if (!parsed) return std::unexpected(parsed.error());
        return shell::evaluate(*parsed->script, registry_, fs_, state, ctx);
    }

private:
    FileSystemAdapter&                  fs_;
    native_jail::CommandRegistry const& registry_;
};

} // namespace agentengine
