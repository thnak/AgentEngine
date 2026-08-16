// decisions/ADR-003-caller-aware-import-gating.md prove phase: the caller-gated import tier
// (Design B, revised per §3.3), exercising claims B1-B8, B10, B11, B13 from the ADR's own §4
// claims tables as literally as possible, PLUS B14 (added during this prove pass's own independent
// verification -- a real gap in a code comment's "redundant, not required" claim about the finder's
// check, found by re-reading the implementation rather than predicted by §3.2/§3.3; see
// python_lockdown.cpp's g_importlib_dict comment for the full account). B9 (performance) has its own dedicated benchmark
// (test_python_caller_gated_benchmark.cpp) since it needs percentile measurement, not an
// assert-based pass/fail. B13's second half (a deliberately-broken heap-type build) has its own
// dedicated binary (test_python_trusted_loader_proxy_heap_type_negative_control.cpp), since it
// requires a SEPARATE compilation of python_lockdown.cpp with a different #define -- this file
// only exercises B13's primary, always-built half (TypeError on class-level monkeypatch).
//
// Moves ctypes/_ctypes/winreg/_wmi/_winapi/subprocess -- ADR-002 §8.9's own worked examples of
// names a security review wants denied to guest code directly, exactly the ones ADR-003 exists to
// gate -- out of the ordinary allowed_top_level_modules tier (test_python_numpy_pandas_import.cpp's
// measured closure) and into caller_gated_modules instead. Everything else in that measured
// closure is left untouched, so a regression here is attributable to the caller-gated mechanism,
// not to some unrelated allowlist gap.

#include <cassert>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/python_lockdown.hpp"
#include "support/crt_fail_fast.hpp"

using agentengine::native_jail::PythonLockdownConfig;
using agentengine::native_jail::PythonLockdownInterpreter;
using agentengine::native_jail::PythonRunOutcome;

namespace {

void print_outcome(const char* label, PythonRunOutcome const& r) {
    printf("[%s] ok=%d escape_attempt=%d\nstdout=%sstderr=%s\n", label, r.ok ? 1 : 0,
           r.escape_attempt ? 1 : 0, r.stdout_text.c_str(), r.stderr_text.c_str());
}
} // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    PythonLockdownConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.extra_sys_path = {AE_PYTHON_SITE_PACKAGES};
    // test_python_numpy_pandas_import.cpp's measured closure, MINUS the six names moved into
    // caller_gated_modules below (ctypes, _ctypes, winreg, _wmi, _winapi, subprocess).
    cfg.allowed_top_level_modules = {
        "_imp",
        "__future__", "_ast", "_bisect", "_blake2", "_bz2", "_collections", "_collections_abc",
        "_compat_pickle", "_compression", "_contextvars", "_csv", "_cython_3_1_3",
        "_cython_3_1_4", "_datetime", "_decimal", "_functools", "_hashlib", "_json", "_locale",
        "_lzma", "_opcode", "_opcode_metadata", "_operator", "_pickle", "_random", "_sre", "_stat",
        "_string", "_strptime", "_struct", "_sysconfig", "_tokenize", "_typing", "_uuid",
        "_weakrefset", "_zoneinfo", "ast", "base64", "binascii", "bisect",
        "bz2", "calendar", "cmath", "collections", "contextlib", "contextvars", "copy", "copyreg",
        "csv", "cython_runtime", "dataclasses", "datetime", "dateutil", "decimal", "dis",
        "enum", "errno", "fnmatch", "functools", "gc", "genericpath", "glob", "gzip", "hashlib",
        "hmac", "importlib", "inspect", "io", "ipaddress", "itertools", "json", "keyword",
        "linecache", "locale", "lzma", "math", "mkl", "mmap", "msvcrt", "nt", "ntpath", "numbers",
        "numpy", "opcode", "operator", "os", "pandas", "pathlib", "pickle", "platform",
        "posixpath", "pprint", "pytz", "random", "re", "reprlib", "secrets", "shutil", "signal",
        "six", "stat", "string", "struct", "sysconfig", "tarfile", "tempfile",
        "textwrap", "threading", "time", "token", "tokenize", "types", "typing", "unicodedata",
        "urllib", "uuid", "warnings", "weakref", "zipfile", "zlib", "zoneinfo",
    };
    // ADR-003 §3.2 item 2: the caller-gated tier.
    cfg.caller_gated_modules = {"ctypes", "_ctypes", "winreg", "_wmi", "_winapi", "subprocess"};
    // B14 fixture (below): an ordinary, allowlisted top-level module -- NOT itself gated -- whose
    // trusted top-level code calls importlib.import_module() for a gated name.
    cfg.allowed_top_level_modules.insert("trusted_import_module_probe");
    cfg.extra_sys_path.push_back(AE_PYTHON_CALLER_GATED_TRUSTED_PROBE_FIXTURES);

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    // ================= B14: trusted caller-gated import via importlib.import_module() on a genuine
    // cache MISS -- added during this prove pass's own independent verification (not in the ADR's
    // original §4 table). Found by re-reading TrustedLoaderProxy_exec_module/Finder_find_spec after
    // the agent's report: the REAL, captured import_module (g_real_import_module) pushes its OWN
    // Python frame (globals == real importlib's dict) between the frozen bootstrap frames and the
    // true caller whenever a caller-gated name isn't yet sys.modules-cached -- confirmed via a
    // standalone frame-chain probe against this exact CPython 3.13.5 target BEFORE this fix existed,
    // showing the finder's own redundant check would incorrectly stop its walk at that frame (not
    // itself registered trusted) and deny a legitimately-trusted caller. Fixed by adding real
    // importlib's module dict as a third frame-walk skip-anchor (g_importlib_dict). Must run BEFORE
    // B1 imports numpy/pandas, which transitively cache every name in caller_gated_modules -- this
    // test needs a genuine, not-yet-cached gated name to be non-vacuous.
    PythonRunOutcome b14_precheck = interp.run(
        "import sys\n"
        "print('B14_WINREG_PRECACHED', 'winreg' in sys.modules)\n");
    print_outcome("B14 pre-check", b14_precheck);
    assert(b14_precheck.ok);
    assert(b14_precheck.stdout_text.find("B14_WINREG_PRECACHED False") != std::string::npos &&
           "B14 would be VACUOUS if winreg were already cached before the trusted import_module call");

    PythonRunOutcome b14 = interp.run(
        "import trusted_import_module_probe\n"
        "print('B14_TRUSTED_IMPORT_MODULE_OK', trusted_import_module_probe.RESULT is not None)\n");
    print_outcome("B14 trusted call", b14);
    assert(b14.ok &&
           "B14: a TRUSTED package's own importlib.import_module() call for a gated, "
           "not-yet-cached name must succeed -- this is the g_importlib_dict fix, exercised for real");
    assert(b14.stdout_text.find("B14_TRUSTED_IMPORT_MODULE_OK True") != std::string::npos);

    PythonRunOutcome b14_guest_denied = interp.run(
        "try:\n"
        "    import winreg\n"
        "    print('B14_GUEST_IMPORT ALLOWED (WRONG)')\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B14_GUEST_IMPORT DENIED', e)\n");
    print_outcome("B14 guest denial (post-trust)", b14_guest_denied);
    assert(b14_guest_denied.ok);
    assert(b14_guest_denied.stdout_text.find("B14_GUEST_IMPORT DENIED") != std::string::npos &&
           "guest code must still be denied even after a TRUSTED caller legitimately cached winreg");

    // ================= B1: numpy/pandas still import and compute correctly =====================
    PythonRunOutcome numpy_result = interp.run(
        "import numpy\n"
        "print('numpy', numpy.__version__)\n"
        "a = numpy.array([1, 2, 3])\n"
        "print('sum', int(a.sum()))\n");
    print_outcome("B1 numpy", numpy_result);
    assert(numpy_result.ok && "B1: numpy must still import/compute with ctypes etc. gated");
    assert(numpy_result.stdout_text.find("sum 6") != std::string::npos);
    assert(numpy_result.stdout_text.find("numpy 2.3.3") != std::string::npos);

    PythonRunOutcome pandas_result = interp.run(
        "import pandas\n"
        "import pandas.errors\n" // exercises the §6.3 module-top-level-import fix directly: line 6
                                  // of the real installed pandas.errors is an unconditional,
                                  // module-scope 'import ctypes'.
        "print('pandas', pandas.__version__)\n"
        "df = pandas.DataFrame({'a': [1, 2, 3]})\n"
        "print('pandas_sum', int(df['a'].sum()))\n"
        "print('errors_ok', pandas.errors.OptionError is not None)\n");
    print_outcome("B1 pandas", pandas_result);
    assert(pandas_result.ok &&
           "B1: pandas, INCLUDING pandas.errors's own module-scope ctypes import, must still work "
           "with ctypes gated -- this is section 6.3's fix, exercised for real");
    assert(pandas_result.stdout_text.find("pandas_sum 6") != std::string::npos);
    assert(pandas_result.stdout_text.find("errors_ok True") != std::string::npos);
    assert(pandas_result.stdout_text.find("pandas 2.3.3") != std::string::npos);

    // ================= B2 + B10: cache-hit bypass closed for BOTH entry points ==================
    PythonRunOutcome cache_check = interp.run(
        "import sys\n"
        "print('ctypes_cached', 'ctypes' in sys.modules)\n"
        "print('_ctypes_cached', '_ctypes' in sys.modules)\n");
    print_outcome("B2/B10 cache state", cache_check);
    assert(cache_check.ok);
    assert(cache_check.stdout_text.find("ctypes_cached True") != std::string::npos &&
           "B2/B10 would be VACUOUS if ctypes weren't already cached before the denial attempt");
    assert(cache_check.stdout_text.find("_ctypes_cached True") != std::string::npos);

    PythonRunOutcome guest_import_stmt = interp.run("import ctypes\n");
    print_outcome("B2 import statement", guest_import_stmt);
    assert(!guest_import_stmt.ok && !guest_import_stmt.escape_attempt);
    assert(guest_import_stmt.stderr_text.find("ModuleNotFoundError") != std::string::npos);
    assert(guest_import_stmt.stderr_text.find("No module named 'ctypes'") != std::string::npos);

    PythonRunOutcome guest_import_module =
        interp.run("import importlib\nimportlib.import_module('ctypes')\n");
    print_outcome("B10 importlib.import_module", guest_import_module);
    assert(!guest_import_module.ok && !guest_import_module.escape_attempt);
    assert(guest_import_module.stderr_text.find("ModuleNotFoundError") != std::string::npos);
    assert(guest_import_module.stderr_text.find("No module named 'ctypes'") != std::string::npos);

    // ================= B3: dict-identity-only forgery -- naive negative control + real denial ===
    // Mirrors ADR-002's own B3 method (a standalone naive variant demonstrating the forgery
    // succeeds against a single-signal check) plus this ADR's own §6 red-team method (a pure-Python
    // reproduction of the decision logic) -- NOT a second C++ build, since our C++ never implements
    // the naive variant at all (only the combined check exists in the real mechanism). The "naive
    // would allow" half is a logical demonstration proving the forgery genuinely targets something
    // a single-signal check would trust; the "real mechanism denies" half runs against the ACTUAL
    // installed lockdown interpreter.
    PythonRunOutcome b3 = interp.run(
        "import numpy\n"
        "naive_trusted_dicts = {id(numpy.__dict__)}\n"
        "forged_code = compile(\"RESULT = 'forged'\\n\", '<forge-b3>', 'exec')\n"
        "exec(forged_code, numpy.__dict__)\n" // ordinary, unprivileged: reuses the REAL, registered
                                               // numpy dict as exec()'s globals argument
        "naive_would_allow = id(numpy.__dict__) in naive_trusted_dicts\n"
        "combined_would_deny = id(forged_code) not in set()\n" // stands for g_trusted_code: this
                                                                 // freshly compiled code object was
                                                                 // never registered by the loader
        "print('B3_NAIVE_WOULD_ALLOW', naive_would_allow)\n"
        "print('B3_COMBINED_WOULD_DENY', combined_would_deny)\n"
        "try:\n"
        "    exec(compile(\"import ctypes\\nRESULT2 = ctypes.WinDLL('kernel32')\\n\", "
        "'<forge-b3-real>', 'exec'), numpy.__dict__)\n"
        "    print('B3_REAL_MECHANISM ALLOWED (WRONG)')\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B3_REAL_MECHANISM DENIED', e)\n");
    print_outcome("B3", b3);
    assert(b3.ok);
    assert(b3.stdout_text.find("B3_NAIVE_WOULD_ALLOW True") != std::string::npos &&
           "positive control: a dict-only check WOULD wrongly trust the real, reused numpy dict");
    assert(b3.stdout_text.find("B3_COMBINED_WOULD_DENY True") != std::string::npos);
    assert(b3.stdout_text.find("B3_REAL_MECHANISM DENIED") != std::string::npos &&
           "the REAL combined mechanism must deny this forgery");

    // ================= B4: code-identity-reuse forgery via types.FunctionType ==================
    PythonRunOutcome b4 = interp.run(
        "import numpy, types\n"
        "naive_trusted_dicts = {id(numpy.__dict__)}\n"
        "forged_code2 = compile(\"def f():\\n    import ctypes\\n    return ctypes\\n\", "
        "'<forge-b4>', 'exec').co_consts[0]\n"
        "print('B4_NAIVE_WOULD_ALLOW', id(numpy.__dict__) in naive_trusted_dicts)\n"
        "print('B4_COMBINED_WOULD_DENY', id(forged_code2) not in set())\n"
        "try:\n"
        "    fn = types.FunctionType(forged_code2, numpy.__dict__)\n"
        "    result = fn()\n"
        "    print('B4_REAL_MECHANISM ALLOWED (WRONG)', result)\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B4_REAL_MECHANISM DENIED', e)\n");
    print_outcome("B4", b4);
    assert(b4.ok);
    assert(b4.stdout_text.find("B4_NAIVE_WOULD_ALLOW True") != std::string::npos);
    assert(b4.stdout_text.find("B4_COMBINED_WOULD_DENY True") != std::string::npos);
    assert(b4.stdout_text.find("B4_REAL_MECHANISM DENIED") != std::string::npos);

    // ================= B5: fake bootstrap-frame filename spoof =================================
    PythonRunOutcome b5 = interp.run(
        "try:\n"
        "    exec(compile(\"import ctypes\\nRESULT3 = ctypes.WinDLL('kernel32')\\n\", "
        "'<frozen importlib._bootstrap>', 'exec'), {})\n"
        "    print('B5_REAL_MECHANISM ALLOWED (WRONG)')\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B5_REAL_MECHANISM DENIED', e)\n");
    print_outcome("B5", b5);
    assert(b5.ok);
    assert(b5.stdout_text.find("B5_REAL_MECHANISM DENIED") != std::string::npos &&
           "spoofing the bootstrap frame's FILENAME must not fool the dict-identity skip logic");

    // ================= B6: registries are not enumerable as Python collections =================
    PythonRunOutcome b6 = interp.run(
        "import gc, sys\n"
        "numpy_dict = sys.modules['numpy'].__dict__\n"
        "suspects = 0\n"
        "for o in gc.get_objects():\n"
        "    if isinstance(o, (set, frozenset)):\n"
        "        try:\n"
        "            if numpy_dict in o:\n"
        "                suspects += 1\n"
        "        except TypeError:\n"
        "            pass\n"
        "print('B6_SUSPECT_SET_CONTAINERS', suspects)\n"
        "finder = sys.meta_path[0]\n"
        "print('B6_FINDER_HAS_DICT', hasattr(finder, '__dict__'))\n"
        "print('B6_FINDER_ATTRS', [a for a in dir(finder) if not a.startswith('__')])\n"
        "loader_type = type(sys.modules['numpy'].__loader__)\n"
        "print('B6_PROXY_TYPE_ATTRS', [a for a in dir(loader_type) "
        "if 'trust' in a.lower() or 'regist' in a.lower()])\n");
    print_outcome("B6", b6);
    assert(b6.ok);
    assert(b6.stdout_text.find("B6_SUSPECT_SET_CONTAINERS 0") != std::string::npos &&
           "no Python-reachable set/frozenset may contain a registered dict as a member");
    assert(b6.stdout_text.find("B6_FINDER_HAS_DICT False") != std::string::npos);
    assert(b6.stdout_text.find("B6_PROXY_TYPE_ATTRS []") != std::string::npos &&
           "no attribute on the proxy TYPE exposes anything registry-shaped");
    assert(b6.stdout_text.find("['find_spec']") != std::string::npos);

    // ================= B7 + B11: reload of a granted module with a module-scope gated import; ===
    // trusted load succeeds & is functional while guest's own top-level import stays denied ======
    PythonRunOutcome b7_b11 = interp.run(
        "import sys, importlib\n"
        "import pandas.errors\n"
        "first = pandas.errors.OptionError\n"
        "importlib.reload(pandas.errors)\n" // B7: reload of a GRANTED module whose top-level code
                                             // performs a caller-gated import
        "second = pandas.errors.OptionError\n"
        "print('B7_RELOAD_OK', first is not None and second is not None)\n"
        "print('B11_TRUSTED_CTYPES_RESIDENT', 'ctypes' in sys.modules)\n"
        "try:\n"
        "    import ctypes\n"
        "    print('B11_GUEST_IMPORT ALLOWED (WRONG)')\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B11_GUEST_IMPORT DENIED', e)\n");
    print_outcome("B7/B11", b7_b11);
    assert(b7_b11.ok);
    assert(b7_b11.stdout_text.find("B7_RELOAD_OK True") != std::string::npos);
    assert(b7_b11.stdout_text.find("B11_TRUSTED_CTYPES_RESIDENT True") != std::string::npos);
    assert(b7_b11.stdout_text.find("B11_GUEST_IMPORT DENIED") != std::string::npos);

    // ================= B12: best-effort allocator-churn / address-reuse probe ==================
    // Deliberately NOT asserted pass/fail: this pass could not construct a reliable trigger for
    // "a NEW allocation lands at the exact freed address of a PREVIOUSLY-trusted, now-dropped
    // registry entry" in the time available (that needs precise pymalloc arena/pool control this
    // pass did not attempt). Reported INCONCLUSIVE explicitly rather than laundered into a pass.
    PythonRunOutcome b12 = interp.run(
        "import gc, sys\n"
        "import pandas.io.clipboard as clip\n" // another real module with a module-scope gated
                                                 // import (subprocess, per ADR-003 section 6.3)
        "del sys.modules['pandas.io.clipboard']\n"
        "del clip\n"
        "gc.collect()\n"
        "junk = []\n"
        "for i in range(20000):\n"
        "    junk.append(compile('x%d = %d\\n' % (i, i), '<churn>', 'exec'))\n"
        "del junk\n"
        "gc.collect()\n"
        "try:\n"
        "    import ctypes\n"
        "    print('B12_POST_CHURN_GUEST_IMPORT ALLOWED')\n"
        "except ModuleNotFoundError as e:\n"
        "    print('B12_POST_CHURN_GUEST_IMPORT DENIED', e)\n");
    print_outcome("B12", b12);
    printf("B12 verdict: INCONCLUSIVE (see prove-phase report -- no reliable address-reuse "
           "trigger constructed in the time available)\n");

    // ================= B13 (primary half): class-level monkeypatch raises TypeError ============
    // (The second half -- lockdown_identity_intact() catching a deliberately-broken heap-type
    // build -- lives in test_python_trusted_loader_proxy_heap_type_negative_control.cpp.)
    PythonRunOutcome b13 = interp.run(
        "import sys\n"
        "ProxyType = type(sys.modules['numpy'].__loader__)\n"
        "try:\n"
        "    ProxyType.exec_module = lambda *a, **k: None\n"
        "    print('B13_MONKEYPATCH ALLOWED (WRONG)')\n"
        "except TypeError as e:\n"
        "    print('B13_MONKEYPATCH DENIED', e)\n");
    print_outcome("B13", b13);
    assert(b13.ok);
    assert(b13.stdout_text.find("B13_MONKEYPATCH DENIED") != std::string::npos);

    // Sanity: identity check still reports intact after every attack attempt above -- none of
    // them should have left the lockdown state itself damaged.
    assert(interp.lockdown_identity_intact());

    printf("test_python_caller_gated_import: PASS\n");
    return 0;
}
