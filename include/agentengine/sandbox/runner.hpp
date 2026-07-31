#pragma once
// Implements 010-Python-Code-Interpreter.md §1a, §3a — the Runner concept and ExecState. Named
// `Runner`, deliberately not `Executor` (027 §5 — that word already means a workflow graph node).

#include <string>
#include <unordered_map>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine {

// {cwd, env}, one instance per session, shared **by reference** across every Runner call (010
// §3a). This is the object that makes "shared context" exact rather than eventually consistent —
// PythonRunner and ShellRunner both take a reference to the same ExecState, never a copy.
struct ExecState {
    std::string                                  cwd;
    std::unordered_map<std::string, std::string> env;
};

// concept, not a base class (010 §1a). PythonRunner and ShellRunner are the concrete kinds;
// neither is declared here — each is a seam backend (CONVENTIONS tier 2) with its own
// implementation, not core vocabulary.
template <class T>
concept Runner = requires(T runner, ExecRequest request, ExecState& state, EffectContext& ctx) {
    { runner.run(request, state, ctx) };  // ae::task<result<ExecOutcome>>
};

} // namespace agentengine
