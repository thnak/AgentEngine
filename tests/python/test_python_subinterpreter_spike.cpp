// decisions/ADR-002-pythonrunner-embedding-and-mediation.md Section 6 item 5, backlog item #39:
// "Whether Py_NewInterpreterFromConfig with PyInterpreterConfig_OWN_GIL (PEP 684, per-interpreter
// GIL) is a viable per-session isolation unit that keeps sys.modules genuinely separate per
// session, and whether numpy/pandas -- as installed in the target environment -- actually load and
// run correctly inside such a subinterpreter... answerable only by trying it." This is that
// experiment. Talks to CPython directly (same as test_python_layer0_sweep.cpp, for the identical
// reason: this is raw-fact-finding independent of MediatedPythonRunner's own design choices, not a
// consumer of them).
//
// This spike answers ONLY the Part A question §6 item 5 poses (is the primitive itself viable on
// this concrete build/environment). It does NOT answer, and does not attempt to answer, the
// SEPARATE Part B question §5.5.6 (finding 7.8) already closed as its own decision: even a "yes"
// here does not make subinterpreter pooling adoptable without also turning the single host-side
// allowlist slot into a per-interpreter-keyed store and the audit hook into a
// PyInterpreterState_Get()-resolving lookup -- a materially bigger change this spike does not
// attempt or need to, per §5.5.6's own text ("this item stays open only for a genuine follow-up").
//
// TWO SEPARATE PROCESS INVOCATIONS, not two phases of one process -- a real, MEASURED finding this
// investigation made (not assumed going in) forces this shape: a FAILED `import numpy` inside a
// STRICT (own-GIL, check_multi_interp_extensions=1) subinterpreter leaves the process's heap
// corrupted -- `Py_EndInterpreter()` on that subinterpreter afterward crashes with
// STATUS_HEAP_CORRUPTION (0xC0000374), reproduced and pinpointed exactly to that call via
// incremental fflush()-before-every-step instrumentation before this file reached its current
// shape. CLAUDE.md's "a test proving a fork bomb is contained must not be able to take the machine
// with it" applies directly: an experiment that can crash the host process must run in its own
// process, not share one with other work. `argv[1]` selects which config this run exercises
// ("strict" or "legacy", tests/CMakeLists.txt registers both as separate ctest entries); whichever
// mode runs, if its numpy import fails, this file deliberately SKIPS `Py_EndInterpreter`/
// `Py_Finalize` and calls `std::_Exit(0)` instead -- a one-shot diagnostic process needs no clean
// CPython shutdown (the OS reclaims everything on exit), and skipping the now-known-dangerous
// teardown call is what keeps this test safely re-runnable instead of crashing every time.
//
// Three independent facts per run, each reported plainly rather than forced to match a predicted
// answer (matching test_python_layer0_sweep.cpp's own discipline):
//   1. Does Py_NewInterpreterFromConfig succeed at creating a subinterpreter under this run's config?
//   2. Is sys.modules genuinely separate between the main interpreter and the subinterpreter (a
//      module imported in one is not silently visible in the other)?
//   3. Does `import numpy` succeed inside such a subinterpreter?

#define PY_SSIZE_T_CLEAN
// See src/backends/native_jail/python_lockdown.cpp's identical comment: this project's default
// (debug CRT) build has no debug CPython to link against, so _DEBUG is undefined around the
// include per the standard embedding workaround.
#ifdef _DEBUG
#define AE_PYTHON_TEST_UNDEF_DEBUG
#undef _DEBUG
#endif
#include <Python.h>
#ifdef AE_PYTHON_TEST_UNDEF_DEBUG
#define _DEBUG
#undef AE_PYTHON_TEST_UNDEF_DEBUG
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

namespace {

void disable_crt_assert_dialog() {
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

int g_failures = 0;
// stdout is fully buffered once redirected to a file/pipe -- flushed after every line so a crash
// (this file exists to find those) never loses a diagnostic printed before it.
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
    } else {
        std::fprintf(stdout, "  ok: %s\n", what);
        std::fflush(stdout);
    }
}

void report(char const* what) {
    std::fprintf(stdout, "  %s\n", what);
    std::fflush(stdout);
}

// Runs `src` in whichever interpreter's thread state is currently active. Returns false and
// prints the exception (via PyErr_Print, to stderr) on failure -- same shape as
// test_python_layer0_sweep.cpp's own `run_ok`, duplicated rather than shared per this project's
// established per-embedding-experiment-file pattern.
bool run_ok(char const* src) {
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    PyObject* rv = PyRun_String(src, Py_file_input, globals, globals);
    if (!rv) {
        PyErr_Print();
        std::fflush(stderr);
        return false;
    }
    Py_DECREF(rv);
    return true;
}

bool try_import_numpy() {
    std::string const src = std::string("import sys\n"
                                         "sys.path.append(r'") +
                             AE_PYTHON_SITE_PACKAGES +
                             "')\n"
                             "import numpy\n"
                             "a = numpy.array([1, 2, 3])\n"
                             "assert int(a.sum()) == 6\n"
                             "print('NUMPY_OK', numpy.__version__)\n";
    return run_ok(src.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    disable_crt_assert_dialog();

    bool const legacy_mode = argc > 1 && std::strcmp(argv[1], "legacy") == 0;
    report(legacy_mode ? "MODE: legacy (shared GIL, check_multi_interp_extensions=0)"
                        : "MODE: strict (own GIL, check_multi_interp_extensions=1 -- CPython's "
                          "own documented default subinterpreter shape, _PyInterpreterConfig_INIT)");

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.isolated = 1;
    config.site_import = 0;
    wchar_t* home = Py_DecodeLocale(AE_PYTHON_HOME, nullptr);
    if (home) {
        PyConfig_SetString(&config, &config.home, home);
        PyMem_RawFree(home);
    }
    PyStatus init_status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    check(!PyStatus_Exception(init_status), "setup: main interpreter initializes");

    PyThreadState* main_tstate = PyThreadState_Get();

    // ---- setup: leave a real, non-trivial marker resident in the MAIN interpreter's sys.modules
    check(run_ok("import json\n"), "setup: `import json` in the MAIN interpreter succeeds");

    PyInterpreterConfig sub_cfg{};
    if (legacy_mode) {
        sub_cfg.use_main_obmalloc = 1;
        sub_cfg.allow_fork = 1;
        sub_cfg.allow_exec = 1;
        sub_cfg.allow_threads = 1;
        sub_cfg.allow_daemon_threads = 1;
        sub_cfg.check_multi_interp_extensions = 0;
        sub_cfg.gil = PyInterpreterConfig_SHARED_GIL;
    } else {
        sub_cfg.use_main_obmalloc = 0;
        sub_cfg.allow_fork = 0;
        sub_cfg.allow_exec = 0;
        sub_cfg.allow_threads = 1;
        sub_cfg.allow_daemon_threads = 0;
        sub_cfg.check_multi_interp_extensions = 1;
        sub_cfg.gil = PyInterpreterConfig_OWN_GIL;
    }

    PyThreadState* sub = nullptr;
    PyStatus sub_status = Py_NewInterpreterFromConfig(&sub, &sub_cfg);
    bool const sub_created = !PyStatus_Exception(sub_status) && sub != nullptr;
    check(sub_created, "Fact 1: Py_NewInterpreterFromConfig succeeds under this run's config");

    if (!sub_created) {
        std::printf("test_python_subinterpreter_spike (%s): could not create a subinterpreter at "
                    "all -- no further facts measurable this run.\n",
                    legacy_mode ? "legacy" : "strict");
        Py_Finalize();
        return g_failures != 0 ? 1 : 0;
    }

    // Py_NewInterpreterFromConfig makes `sub` current on this thread already.

    // ---- Fact 2: sys.modules isolation -- the MAIN interpreter's `json` import must NOT be
    // silently visible here.
    check(run_ok("import sys\n"
                  "assert 'json' not in sys.modules, "
                  "'the MAIN interpreter\\'s json import leaked into the subinterpreter'\n"
                  "print('ISOLATION_OK')\n"),
          "Fact 2: sys.modules is genuinely separate -- the main interpreter's `json` import is "
          "invisible inside the subinterpreter");

    // ---- Fact 3: does numpy actually load under this run's config? ----
    bool const numpy_ok = try_import_numpy();
    std::fprintf(stdout, "  measured: `import numpy` under %s config -> %s\n",
                legacy_mode ? "LEGACY" : "STRICT",
                numpy_ok ? "SUCCEEDED" : "FAILED (see stderr traceback above)");
    std::fflush(stdout);

    if (!numpy_ok) {
        // Empirically confirmed by this investigation (reproduced and pinpointed via incremental
        // fflush()-before-every-step instrumentation before this file reached its current shape):
        // calling Py_EndInterpreter on a subinterpreter after a FAILED numpy import crashes the
        // process with STATUS_HEAP_CORRUPTION (0xC0000374) -- deliberately not re-triggered here
        // every run. A one-shot diagnostic process needs no clean CPython shutdown; the OS reclaims
        // everything on exit.
        report("numpy import failed -- SKIPPING Py_EndInterpreter/Py_Finalize (known to crash the "
              "process after this specific failure mode, see this file's own header comment) and "
              "exiting via std::_Exit instead");
        std::fflush(stdout);
        std::_Exit(g_failures != 0 ? 1 : 0);
    }

    // Only reached when numpy actually imported successfully -- teardown is not known-dangerous on
    // this path (nothing partially failed to load), so it is exercised for real.
    Py_EndInterpreter(sub);
    PyThreadState_Swap(main_tstate);

    // ---- positive control for Fact 2: prove the MAIN interpreter's own `json` import is still
    // genuinely there after ending the subinterpreter -- the isolation claim above would be
    // meaningless if ending the subinterpreter had somehow torn down or corrupted main's own state.
    check(run_ok("import sys\nassert 'json' in sys.modules\nprint('MAIN_STILL_INTACT')\n"),
          "positive control: the MAIN interpreter's own state (its `json` import) is intact after "
          "ending the subinterpreter");

    Py_Finalize();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_python_subinterpreter_spike (%s): PASS (see stdout above for the measured, "
                "not-predicted, per-fact results)\n",
                legacy_mode ? "legacy" : "strict");
    return 0;
}
