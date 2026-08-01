// Correctness gate for src/backends/native_jail/{python_runner,shell_runner}.hpp. Proves that
// both types satisfy the `Runner` concept (010 §1a) against the real, merged headers.
//
// Neither ShellRunner nor PythonRunner is a stub anymore:
//  - ShellRunner (decisions/ADR-001-shellrunner-grammar-and-dispatch.md, prove phase) has a real
//    grammar/dispatch implementation, is constructor-injected with a FileSystemAdapter&/
//    CommandRegistry const&, and is exercised by tests/test_shell_runner_proof.cpp.
//  - PythonRunner (decisions/ADR-002-pythonrunner-embedding-and-mediation.md, prove phase) now
//    embeds a real CPython interpreter behind `native_jail::PythonLockdownInterpreter` and is
//    constructor-injected with a `PythonLockdownConfig`, so it is no longer default-constructible
//    either. It is exercised by its own dedicated tests (test_python_embed_smoke,
//    test_python_layer0_sweep, test_python_meta_path_finder, test_python_numpy_pandas_import —
//    only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON, since it requires linking a real
//    CPython), not by a fail-closed-stub helper here — mirroring exactly how ShellRunner's
//    fail-closed stub check was retired from this file once it stopped being a stub.
//
// This file is therefore reduced to what it can check unconditionally, without a Python
// installation: both concrete Runner types satisfy the `Runner` concept against the real, merged
// headers — a `static_assert`, requiring no interpreter and no test-time execution at all.

#include <cassert>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/shell_runner.hpp"

// PythonRunner's own Runner-concept static_assert now lives in
// tests/test_python_embed_smoke.cpp, built only when AGENTENGINE_BUILD_PYTHON_RUNNER is ON (it
// requires python_runner.hpp, which pulls in native_jail::PythonLockdownInterpreter and therefore
// needs the agentengine_python_runner target to be configured) — not duplicated here so this
// always-built test doesn't gain a conditional Python dependency of its own.

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

static_assert(agentengine::Runner<agentengine::ShellRunner>,
              "ShellRunner must satisfy the Runner concept (010 §1a)");

int main() {
    disable_crt_assert_dialog();
    return 0;
}
