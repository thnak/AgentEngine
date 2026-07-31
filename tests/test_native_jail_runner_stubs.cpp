// Correctness gate for src/backends/native_jail/{python_runner,shell_runner}.hpp. Proves that
// both types satisfy the `Runner` concept (010 §1a) against the real, merged headers, and that
// PythonRunner's stub fails closed — a `run()` call returns a structured `error` (never a fake
// success), matching the "structured outcomes, never host faults" rule (008 §2) even for an
// intentionally-unimplemented backend.
//
// ShellRunner is NO LONGER a stub (decisions/ADR-001-shellrunner-grammar-and-dispatch.md, prove
// phase) — it has a real grammar/dispatch implementation and is constructor-injected with a
// FileSystemAdapter&/CommandRegistry const&, so it is no longer default-constructible and is
// exercised by its own dedicated test, tests/test_shell_runner_proof.cpp, not by the
// fail-closed-stub helper below.
//
// What this test does NOT cover, because it does not exist yet: real Python execution, the import
// allowlist and interpreter-level mediation (008 §1b), or CodeAct (010 §6, 026 §5) — CodeAct is a
// configuration of PythonRunner via the `agent` library, and no `agent.*` module vocabulary exists
// yet to configure. That logic is security-critical and is deliberately deferred to its own
// design -> red-team -> prove -> judge cycle per CLAUDE.md; this test exists so PythonRunner's
// *stub* behaviour (shape + fail-closed) is a checked invariant in the interim, not something that
// quietly regresses.

#include <cassert>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/python_runner.hpp"
#include "backends/native_jail/shell_runner.hpp"

namespace {
// See tests/test_real_filesystem_adapter.cpp's identical helper for why this matters: a failed
// assert() under the MSVC CRT otherwise pops a blocking interactive dialog in a non-interactive
// CTest run (CLAUDE.md Machine Safety).
void disable_crt_assert_dialog() {
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}
} // namespace

static_assert(agentengine::Runner<agentengine::PythonRunner>,
              "PythonRunner must satisfy the Runner concept (010 §1a)");
static_assert(agentengine::Runner<agentengine::ShellRunner>,
              "ShellRunner must satisfy the Runner concept (010 §1a)");

namespace {

// PythonRunner must fail closed, not silently succeed, until real logic exists (CLAUDE.md).
void assert_python_runner_fails_closed_with_not_implemented() {
    agentengine::PythonRunner  runner;
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
    disable_crt_assert_dialog();
    assert_python_runner_fails_closed_with_not_implemented();
    return 0;
}
