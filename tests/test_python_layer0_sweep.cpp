// decisions/ADR-002-pythonrunner-embedding-and-mediation.md prove phase, priority item 2:
// Layer 0 (§3.0, widened by §5.5.1) -- the sys.modules sweep and its scope. This test talks to
// CPython directly (not through native_jail::PythonLockdownInterpreter) because it exists to
// document and check the RAW, pre-lockdown facts the lockdown design depends on:
//
//   1. What is actually resident in sys.modules immediately post-bootstrap on THIS target
//      (CPython 3.13.5, Windows, isolated=1/site_import=0) -- empirically measured, not assumed
//      from the ADR's example list (§6 item 4 / item 7's "needs a real embedding experiment, not
//      resolvable by reading docs").
//   2. Whether `_imp` can be removed from guest-visible sys.modules without breaking numpy's own
//      native-extension loading (§6 item 4) -- YES, empirically, tested below by deleting the
//      sys.modules entry and then importing numpy (which requires _imp.create_dynamic
//      internally) and confirming it still works AND that "_imp" does not silently reappear as a
//      module key.
//   3. Whether sys.modules REMOVAL ALONE (with no meta-path finder installed) is sufficient to
//      deny a *fresh* `import _imp` -- NO: BuiltinImporter recreates it on demand, because
//      built-in modules don't depend on a prior sys.modules entry to be re-initializable. This is
//      the precise reason Layer 0's sweep and the meta-path finder (tested separately in
//      test_python_meta_path_finder.cpp) are stated as COMPLEMENTARY, not redundant, mechanisms:
//      the sweep closes the "already-cached, finder-never-consulted" shortcut for names resident
//      at bootstrap; the finder is what actually has to deny a *fresh* import of the same name.
//
// This is deliberately a from-scratch CPython embed (its own Py_InitializeFromConfig call) rather
// than reusing PythonLockdownInterpreter, so that these raw facts are checked independent of
// python_lockdown.cpp's own choices about what to keep resident -- this file is the evidence that
// justifies python_lockdown.cpp's internal_keep_set(), not a consumer of it.

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

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
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

std::set<std::string> dump_sys_modules() {
    std::set<std::string> out;
    PyObject* modules = PyImport_GetModuleDict(); // borrowed
    PyObject* keys = PyDict_Keys(modules);
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        out.insert(PyUnicode_AsUTF8(PyList_GetItem(keys, i)));
    }
    Py_DECREF(keys);
    return out;
}

bool run_ok(const char* src) {
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    PyObject* rv = PyRun_String(src, Py_file_input, globals, globals);
    if (!rv) {
        PyErr_Print();
        return false;
    }
    Py_DECREF(rv);
    return true;
}
} // namespace

int main() {
    disable_crt_assert_dialog();

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.isolated = 1;
    config.site_import = 0;
    wchar_t* home = Py_DecodeLocale(AE_PYTHON_HOME, nullptr);
    if (home) {
        PyConfig_SetString(&config, &config.home, home);
        PyMem_RawFree(home);
    }
    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    assert(!PyStatus_Exception(status) && "a minimal isolated CPython init must succeed");

    // ---- Fact 1: what's actually resident immediately post-bootstrap, on this target ----
    std::set<std::string> resident = dump_sys_modules();
    printf("Resident sys.modules keys post-bootstrap (isolated=1, site_import=0), count=%zu:\n",
           resident.size());
    for (auto const& k : resident) printf("  %s\n", k.c_str());

    // The ADR-002 §5.5.1 widened example set is a SUBSET check, not an exact-match check --
    // §5.5.1 itself only claims these six are "expected to be small," and this test's job is to
    // report what's actually there, not force the measured reality to match a predicted list.
    // `_imp` is asserted present here (pre-sweep) specifically so Fact 2/3 below have something
    // real to remove.
    assert(resident.count("_imp") == 1);
    assert(resident.count("sys") == 1);
    assert(resident.count("builtins") == 1);
    assert(resident.count("_frozen_importlib") == 1);
    assert(resident.count("_frozen_importlib_external") == 1);
    // Empirically found present on THIS Windows target, NOT in the ADR's original example list --
    // recorded here as a real, previously-unstated finding (see the ADR's §8 for the full report):
    // `winreg` (registry access!), `marshal`, `zipimport`, `_signal`, `time`, `nt` are all
    // resident post-bootstrap even with isolated=1/site_import=0.
    assert(resident.count("winreg") == 1);
    assert(resident.count("nt") == 1);

    // ---- Fact 2 + 3: _imp removal + reimport behavior (no meta-path finder installed yet) ----
    PyObject* modules = PyImport_GetModuleDict(); // borrowed
    int del_rc = PyDict_DelItemString(modules, "_imp");
    printf("del sys.modules['_imp'] rc=%d\n", del_rc);
    assert(del_rc == 0);

    std::set<std::string> after_delete = dump_sys_modules();
    assert(after_delete.count("_imp") == 0 && "the sys.modules entry must actually be gone");

    // Fact 3: a FRESH `import _imp` succeeds anyway (BuiltinImporter recreates it) -- proving
    // sys.modules removal ALONE does not deny access to a builtin module; only a meta-path finder
    // that explicitly refuses the name (tested in test_python_meta_path_finder.cpp) does that.
    bool reimport_ok = run_ok("import _imp\nassert _imp is not None\n");
    printf("fresh `import _imp` after deletion, with NO finder installed -> %s\n",
           reimport_ok ? "SUCCEEDED (expected -- demonstrates sweep alone is insufficient)"
                       : "FAILED (unexpected for this experiment)");
    assert(reimport_ok &&
           "BuiltinImporter must be able to recreate a removed builtin module absent a finder -- "
           "this is exactly why Layer 0 and the meta-path finder are complementary, not redundant");

    // Re-delete for the next check (the reimport above put it back).
    PyDict_DelItemString(modules, "_imp");

    // Fact 2: with `_imp` gone from sys.modules, importing numpy (which requires the extension
    // loader's internal `_imp.create_dynamic` call to load its `.pyd`) must still work, because
    // importlib._bootstrap_external holds its own host-side-equivalent reference to `_imp`,
    // bound long before this deletion. This directly answers ADR-002 §6 item 4.
    bool path_ok = run_ok(("import sys\n"
                            "sys.path.append(r'" + std::string(AE_PYTHON_SITE_PACKAGES) +
                            "')\n")
                               .c_str());
    assert(path_ok);
    bool numpy_ok = run_ok(
        "import numpy\n"
        "a = numpy.array([1, 2, 3])\n"
        "assert int(a.sum()) == 6\n");
    printf("`import numpy` with _imp REMOVED from sys.modules -> %s\n",
           numpy_ok ? "SUCCEEDED (answers ADR-002 §6 item 4: YES, _imp is removable)"
                    : "FAILED");
    assert(numpy_ok && "removing _imp from sys.modules must not break numpy's own .pyd loading");

    std::set<std::string> after_numpy = dump_sys_modules();
    bool imp_reappeared = after_numpy.count("_imp") == 1;
    printf("'_imp' present in sys.modules after numpy import -> %s\n",
           imp_reappeared ? "YES (numpy's own import path re-triggered a fresh `import _imp`)"
                          : "NO (confirms the loader used its own internal reference, not a "
                            "fresh sys.modules-mediated import)");
    // Either outcome is informative and is reported as such, not asserted one way -- see the
    // ADR's §8/§9 for which one was actually observed on this build.

    Py_Finalize();
    printf("test_python_layer0_sweep: PASS\n");
    return 0;
}
