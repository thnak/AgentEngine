// decisions/ADR-002-pythonrunner-embedding-and-mediation.md prove phase, priority item 4: claim
// A2 -- `import numpy` / `import pandas` actually working under the locked-down interpreter, with
// numpy/pandas explicitly allowlisted. This is also the practical, empirical check on whether
// Layer 0's sweep (§3.0/§5.5.1) broke anything real (§4 claim A5).
//
// The allowlist below is NOT hand-guessed -- it is the measured, top-level transitive-import
// closure of `import numpy, pandas; pandas.DataFrame(...).sum()` against the actual installed
// build at AGENTENGINE_PYTHON_ROOT (captured with an unmediated interpreter during this ADR's
// prove-phase experimentation; see the ADR §8 for the exact dump and the real, load-bearing
// finding it produced: granting "numpy + pandas" on THIS concrete Anaconda/MKL-linked build also
// transitively requires granting `ctypes`, `winreg`, `_wmi`, `_winapi`, `platform`, `subprocess`,
// `shutil`, `threading`, and more -- none of which a naive reading of "grant numpy and pandas"
// would expect, and several of which (`ctypes`, `winreg`, `subprocess`) are exactly the kind of
// name a security-conscious allowlist would otherwise want to deny outright. The finder cannot
// distinguish "numpy's own import of ctypes" from "guest code's own import of ctypes" once the
// name is allowed -- this is reported plainly in the ADR as a real tension, not resolved here.

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
    cfg.extra_sys_path = {AE_PYTHON_SITE_PACKAGES};
    // Measured top-level closure (ADR-002 §8) of `import numpy, pandas` + one DataFrame op, on
    // CPython 3.13.5 / numpy 2.3.3 / pandas 2.3.3 at the configured miniconda install.
    cfg.allowed_top_level_modules = {
        // "_imp" is REQUIRED here, and this is itself a real, load-bearing finding from this
        // pass, not part of the originally-measured closure: `importlib/__init__.py` does a
        // plain, top-level `import _imp` ("Just the builtin component, NOT the full Python
        // module" per its own source comment), and pandas reaches `inspect` ->
        // `importlib.machinery` -> that exact `import _imp` line during its own import. This
        // means `_imp` cannot be denied to guest code once ANY allowed code path imports
        // `importlib.machinery` (an extremely ordinary, hard-to-avoid stdlib dependency --
        // `inspect`, `pkgutil`, `pydoc` all pull it in) -- a materially sharper, more concrete
        // version of ADR-002 §6 item 4's open question than "might break numpy's own .pyd
        // loading": it broke pandas via `inspect`, not numpy's extension loader at all, and it is
        // NOT a native-extension-loading concern -- it is ordinary top-level Python stdlib code.
        "_imp",
        "__future__", "_ast", "_bisect", "_blake2", "_bz2", "_collections", "_collections_abc",
        "_compat_pickle", "_compression", "_contextvars", "_csv", "_ctypes", "_cython_3_1_3",
        "_cython_3_1_4", "_datetime", "_decimal", "_functools", "_hashlib", "_json", "_locale",
        "_lzma", "_opcode", "_opcode_metadata", "_operator", "_pickle", "_random", "_sre", "_stat",
        "_string", "_strptime", "_struct", "_sysconfig", "_tokenize", "_typing", "_uuid",
        "_weakrefset", "_winapi", "_wmi", "_zoneinfo", "ast", "base64", "binascii", "bisect",
        "bz2", "calendar", "cmath", "collections", "contextlib", "contextvars", "copy", "copyreg",
        "csv", "ctypes", "cython_runtime", "dataclasses", "datetime", "dateutil", "decimal", "dis",
        "enum", "errno", "fnmatch", "functools", "gc", "genericpath", "glob", "gzip", "hashlib",
        "hmac", "importlib", "inspect", "io", "ipaddress", "itertools", "json", "keyword",
        "linecache", "locale", "lzma", "math", "mkl", "mmap", "msvcrt", "nt", "ntpath", "numbers",
        "numpy", "opcode", "operator", "os", "pandas", "pathlib", "pickle", "platform",
        "posixpath", "pprint", "pytz", "random", "re", "reprlib", "secrets", "shutil", "signal",
        "six", "stat", "string", "struct", "subprocess", "sysconfig", "tarfile", "tempfile",
        "textwrap", "threading", "time", "token", "tokenize", "types", "typing", "unicodedata",
        "urllib", "uuid", "warnings", "weakref", "winreg", "zipfile", "zlib", "zoneinfo",
    };

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    PythonRunOutcome numpy_result = interp.run(
        "import numpy\n"
        "print('numpy', numpy.__version__)\n"
        "a = numpy.array([1, 2, 3])\n"
        "print('sum', int(a.sum()))\n");
    printf("[numpy] ok=%d\nstdout=%sstderr=%s\n", numpy_result.ok, numpy_result.stdout_text.c_str(),
           numpy_result.stderr_text.c_str());
    assert(numpy_result.ok && "numpy must import and compute under the lockdown interpreter (A2)");
    assert(numpy_result.stdout_text.find("sum 6") != std::string::npos);

    PythonRunOutcome pandas_result = interp.run(
        "import pandas\n"
        "print('pandas', pandas.__version__)\n"
        "df = pandas.DataFrame({'a': [1, 2, 3]})\n"
        "print('pandas_sum', int(df['a'].sum()))\n");
    printf("[pandas] ok=%d\nstdout=%sstderr=%s\n", pandas_result.ok,
           pandas_result.stdout_text.c_str(), pandas_result.stderr_text.c_str());
    assert(pandas_result.ok && "pandas must import and compute under the lockdown interpreter (A2)");
    assert(pandas_result.stdout_text.find("pandas_sum 6") != std::string::npos);

    // A2's "transparent, bit-identical delegation" half: version strings and a representative
    // computed value match what an unmediated interpreter would report for this exact installed
    // build (numpy 2.3.3, pandas 2.3.3, verified independently during this ADR's prove-phase
    // experimentation -- see the ADR's §8 for the unmediated dump this is compared against).
    assert(numpy_result.stdout_text.find("numpy 2.3.3") != std::string::npos);
    assert(pandas_result.stdout_text.find("pandas 2.3.3") != std::string::npos);

    printf("test_python_numpy_pandas_import: PASS\n");
    return 0;
}
