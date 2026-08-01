// decisions/ADR-002-pythonrunner-embedding-and-mediation.md prove phase, priority item 3: the
// sys.meta_path finder (Design A, §3.1) as a real C-implemented type (tp_dictoffset == 0, §5.5.2),
// installed as the sole sys.meta_path entry. Tests claim A1 (denial of a disallowed name) and A3
// (documented limitation: a name pre-seeded into sys.modules bypasses the finder entirely), plus
// the §3.4 item 3 / §5.5.2 per-call identity reassertion mechanism (not itself one of A1-A5/C1-C3,
// but load-bearing for A4, which this test also exercises).
//
// Deliberately uses a MINIMAL allowlist (just "math") — this is the clean "ctypes/winreg denied"
// baseline. See test_python_numpy_pandas_import.cpp for what happens once numpy/pandas ARE
// granted (spoiler, reported in the ADR: granting numpy/pandas on this concrete installed build
// transitively re-grants ctypes/winreg/_wmi/_winapi to guest code too — a real, separate finding).

#include <cassert>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/python_lockdown.hpp"

using agentengine::native_jail::PythonLockdownConfig;
using agentengine::native_jail::PythonLockdownInterpreter;
using agentengine::native_jail::PythonRunOutcome;

namespace {
void disable_crt_assert_dialog() {
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}
} // namespace

int main() {
    disable_crt_assert_dialog();

    PythonLockdownConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.allowed_top_level_modules = {"math"};

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    // Negative control: an allowed name still works -- proves the deny-path checks below aren't
    // vacuous (the finder isn't just denying everything).
    PythonRunOutcome ok1 = interp.run("import math\nprint('math ok', math.pi > 3)\n");
    printf("[negative control] ok=%d stdout=%sstderr=%s\n", ok1.ok, ok1.stdout_text.c_str(),
           ok1.stderr_text.c_str());
    assert(ok1.ok && "an explicitly allowed module must still import and run");
    assert(ok1.stdout_text.find("math ok True") != std::string::npos);

    // ---- A1: import ctypes (a name outside the granted set) raises ModuleNotFoundError, and is
    // never reached at all -- the finder returns None as the SOLE meta_path entry, so Python's own
    // import machinery raises the ordinary "missing package" shape (008 §1b), not a caught
    // security exception.
    PythonRunOutcome deny_ctypes = interp.run("import ctypes\n");
    printf("[A1: ctypes] ok=%d stderr=%s\n", deny_ctypes.ok, deny_ctypes.stderr_text.c_str());
    assert(!deny_ctypes.ok && !deny_ctypes.escape_attempt);
    assert(deny_ctypes.stderr_text.find("ModuleNotFoundError") != std::string::npos);
    assert(deny_ctypes.stderr_text.find("No module named 'ctypes'") != std::string::npos);

    // Same claim, a second concrete name (winreg): notable because winreg was EMPIRICALLY found
    // resident in sys.modules immediately post-bootstrap on this Windows target (ADR-002 §8's
    // dump_modules evidence) -- i.e. it survives naively if Layer 0's sweep is skipped or scoped
    // wrong. This asserts it is denied to fresh guest imports regardless of that residency, which
    // is exactly why Layer 0's sweep removes it before the finder is even installed.
    PythonRunOutcome deny_winreg = interp.run("import winreg\n");
    printf("[A1: winreg] ok=%d stderr=%s\n", deny_winreg.ok, deny_winreg.stderr_text.c_str());
    assert(!deny_winreg.ok && !deny_winreg.escape_attempt);
    assert(deny_winreg.stderr_text.find("ModuleNotFoundError") != std::string::npos);

    // ---- A3 (documented, PREDICTED limitation, ADR-002 §4): a name pre-seeded directly into
    // sys.modules by guest code is reachable WITHOUT the finder ever being consulted at all
    // (`_find_and_load`'s sys.modules cache-hit shortcut, §3.0). This is expected to SUCCEED, not
    // to be denied -- asserting that is exactly what proves the gap is real, not closed by the
    // finder alone (Layer 0 + §3.4's continuous checks are what have to close it, tested below).
    PythonRunOutcome preseed = interp.run(
        "import sys\n"
        "sys.modules['ctypes'] = 'PRETEND_CTYPES_MODULE'\n"
        "import ctypes\n"
        "print('A3 bypass reached:', ctypes)\n");
    printf("[A3] ok=%d stdout=%s\n", preseed.ok, preseed.stdout_text.c_str());
    assert(preseed.ok && "A3 is a documented, predicted gap -- the pre-seeded name IS reachable");
    assert(preseed.stdout_text.find("PRETEND_CTYPES_MODULE") != std::string::npos);

    // ---- Reassertion mechanism (§3.4 item 3 / §5.5.2): identity check has discriminating power
    // (negative control) while nothing has been tampered with yet.
    assert(interp.lockdown_identity_intact() &&
           "the reassertion check must report intact before any tamper attempt");

    // ---- A4: guest code clears sys.meta_path (the A4 payload from ADR-002 §4). The tamper CALL
    // ITSELF is allowed to complete (the reassertion runs at the next call's ENTRY, before its
    // source executes -- not mid-call; §3.4 item 3 states this is a known, intentional boundary).
    PythonRunOutcome tamper = interp.run(
        "import sys\n"
        "sys.meta_path.clear()\n"
        "print('tampered')\n");
    printf("[A4 tamper call] ok=%d stdout=%s\n", tamper.ok, tamper.stdout_text.c_str());
    assert(tamper.ok);
    assert(!interp.lockdown_identity_intact() &&
           "identity check must detect the tamper immediately after it happens");

    PythonRunOutcome next_call = interp.run("print('should never execute')\n");
    printf("[A4 next call] escape_attempt=%d ok=%d stdout='%s'\n", next_call.escape_attempt,
           next_call.ok, next_call.stdout_text.c_str());
    assert(next_call.escape_attempt &&
           "the call AFTER a detected tamper must fail closed as escape_attempt");
    assert(!next_call.ok);
    assert(next_call.stdout_text.empty() && "escape_attempt must mean source never executed");

    printf("test_python_meta_path_finder: PASS\n");
    return 0;
}
