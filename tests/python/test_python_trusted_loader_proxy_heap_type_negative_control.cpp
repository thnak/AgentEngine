// decisions/ADR-003-caller-aware-import-gating.md prove phase, claim B13's second half:
// lockdown_identity_intact()'s §3.3.4 check must actually have discriminating power against a
// TrustedLoaderProxy built as a MUTABLE heap type (no Py_TPFLAGS_IMMUTABLETYPE) -- not merely
// describe an accident of how the type happens to be built today. This requires a SEPARATE
// compilation of python_lockdown.cpp with AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY defined (see
// tests/CMakeLists.txt's dedicated agentengine_python_runner_heap_proxy_probe library target),
// building TrustedLoaderProxy via PyType_FromSpec without the immutability flag instead of the
// real, classic non-heap construction python_lockdown.cpp otherwise uses.
//
// Positive-control structure: everything else about the interpreter is set up completely
// normally (real finder, real wrappers, correct meta_path identity) -- ONLY this one type's
// construction differs, via the compile-time flag. If lockdown_identity_intact() still reported
// true against this build, section 3.3.4's check would be provably not doing its stated job --
// this file is the disproving experiment for that claim, not a description of it.

#include <cassert>
#include <cstdio>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/python_lockdown.hpp"
#include "support/crt_fail_fast.hpp"

using agentengine::native_jail::PythonLockdownConfig;
using agentengine::native_jail::PythonLockdownInterpreter;

namespace {
} // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    PythonLockdownConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.allowed_top_level_modules = {"math"};
    cfg.caller_gated_modules = {"ctypes"};

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    // ADR-003 claim B13, second half: this binary links a TrustedLoaderProxy built (via this TU's
    // own AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY-guarded branch) as a mutable heap type, with no
    // Py_TPFLAGS_IMMUTABLETYPE. lockdown_identity_intact() must detect this and report false, per
    // section 3.3.4's requirement -- proving the reassertion check is itself load-bearing, not
    // merely descriptive of an accident of how the type happens to be built in the real backend.
    bool intact = interp.lockdown_identity_intact();
    printf("lockdown_identity_intact() against the deliberately-broken heap-type build -> %s\n",
           intact ? "true (WRONG -- the check has no discriminating power)" : "false (correct)");
    assert(!intact &&
           "lockdown_identity_intact() must detect a mutable-heap-type TrustedLoaderProxy build");

    printf("test_python_trusted_loader_proxy_heap_type_negative_control: PASS\n");
    return 0;
}
