// Correctness gate for src/backends/native_jail/{python_runner,shell_runner}.hpp. Proves the two
// things that are actually implemented right now: both types satisfy the `Runner` concept
// (010 §1a) against the real, merged headers, and both stubs fail closed — a `run()` call returns
// a structured `error` (never a fake success), matching the "structured outcomes, never host
// faults" rule (008 §2) even for an intentionally-unimplemented backend.
//
// What this test does NOT cover, because it does not exist yet: real Python execution, real shell
// grammar parsing/dispatch, the import allowlist and interpreter-level mediation (008 §1b), or
// CodeAct (010 §6, 026 §5) — CodeAct is a configuration of PythonRunner via the `agent` library,
// and no `agent.*` module vocabulary exists yet to configure. That logic is security-critical and
// is deliberately deferred to its own design -> red-team -> prove -> judge cycle per CLAUDE.md; this
// test exists so the *stub* behaviour (shape + fail-closed) is a checked invariant in the interim,
// not something that quietly regresses.

#include <cassert>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/python_runner.hpp"
#include "backends/native_jail/shell_runner.hpp"

static_assert(agentengine::Runner<agentengine::PythonRunner>,
              "PythonRunner must satisfy the Runner concept (010 §1a)");
static_assert(agentengine::Runner<agentengine::ShellRunner>,
              "ShellRunner must satisfy the Runner concept (010 §1a)");

namespace {

// Both runners must fail closed, not silently succeed, until real logic exists (CLAUDE.md).
template <class RunnerT>
void assert_fails_closed_with_not_implemented() {
    RunnerT runner;
    agentengine::ExecRequest  request{};
    agentengine::ExecState    state{};
    agentengine::EffectContext ctx{};

    agentengine::result<agentengine::ExecOutcome> outcome = runner.run(request, state, ctx);

    assert(!outcome.has_value() && "a not-implemented Runner must never report success");
    assert(outcome.error().klass == agentengine::failure_class::fatal);
    assert(outcome.error().code == "not_implemented");
}

} // namespace

int main() {
    assert_fails_closed_with_not_implemented<agentengine::PythonRunner>();
    assert_fails_closed_with_not_implemented<agentengine::ShellRunner>();
    return 0;
}
