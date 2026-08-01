// decisions/ADR-002-pythonrunner-embedding-and-mediation.md prove phase, stretch item (§4 claim
// C1): does a native `PySys_AddAuditHook`-installed hook fire on an import attempt regardless of
// whether sys.meta_path has been tampered with by guest code? This test's trampoline
// (python_lockdown.cpp's `audit_hook_trampoline`) only COUNTS "import" events -- it does not
// itself enforce/deny anything -- so what this proves is narrower than a full C1 implementation:
// it proves the hook fires (observability), not that it can independently block (that would need
// the hook body to re-check the allowlist and return nonzero, which this pass did not implement;
// recorded honestly in the ADR §9 as a partial/observational result, not the full claim).

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
    cfg.allowed_top_level_modules = {"math",       "json",       "re",         "_json",
                                      "enum",       "types",      "functools",  "operator",
                                      "keyword",    "reprlib",    "_sre",       "itertools",
                                      "_collections_abc", "copyreg", "collections",
                                      "_functools", "_operator",  "_collections"};
    cfg.install_audit_hook = true;

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    std::uint64_t baseline = PythonLockdownInterpreter::audit_import_event_count();
    printf("audit import-event count immediately after initialize() (bootstrap's own imports): %llu\n",
           static_cast<unsigned long long>(baseline));

    // A fresh, allowed, not-yet-imported name: the hook should fire for a genuine first-load.
    PythonRunOutcome allowed = interp.run("import json\nprint('json ok', json.dumps({'a': 1}))\n");
    printf("[allowed import] ok=%d stdout=%sstderr=%s\n", allowed.ok, allowed.stdout_text.c_str(),
           allowed.stderr_text.c_str());
    assert(allowed.ok);
    std::uint64_t after_allowed = PythonLockdownInterpreter::audit_import_event_count();
    printf("count after `import json` (allowed, first load): %llu\n",
           static_cast<unsigned long long>(after_allowed));
    assert(after_allowed > baseline &&
           "the audit hook must observe a genuine, allowed first-load import event");

    // A denied name: does the hook ALSO fire for an attempt the finder is about to refuse?
    PythonRunOutcome denied = interp.run("import ctypes\n");
    assert(!denied.ok && !denied.escape_attempt);
    std::uint64_t after_denied = PythonLockdownInterpreter::audit_import_event_count();
    printf("count after denied `import ctypes`: %llu\n",
           static_cast<unsigned long long>(after_denied));
    printf("C1 (partial/observational): hook fires on denied attempts too -> %s\n",
           after_denied > after_allowed ? "YES" : "NO (see ADR §9 for what this means)");

    // Tamper: clear sys.meta_path (the A4 payload), then attempt an import in the SAME call.
    // Claim C1 asks whether the hook still fires "independent of the now-defeated Python-level
    // mechanisms." This test only checks that the audit hook counted the attempt -- it does NOT
    // deny the import itself (this trampoline is observation-only, see file header), so the
    // import's own success/failure here is a SEPARATE fact from whether the hook observed it.
    PythonRunOutcome tamper_and_import = interp.run(
        "import sys\n"
        "sys.meta_path.clear()\n"
        "try:\n"
        "    import ctypes\n"
        "    print('post-tamper import result: SUCCEEDED (meta_path empty -> ModuleNotFoundError expected instead)')\n"
        "except ImportError as e:\n"
        "    print('post-tamper import result: raised', type(e).__name__)\n");
    printf("[tamper+import] ok=%d stdout=%sstderr=%s\n", tamper_and_import.ok,
           tamper_and_import.stdout_text.c_str(), tamper_and_import.stderr_text.c_str());
    std::uint64_t after_tamper = PythonLockdownInterpreter::audit_import_event_count();
    printf("count after tamper+import attempt: %llu\n",
           static_cast<unsigned long long>(after_tamper));
    printf("C1 (tamper case): hook fired despite sys.meta_path being cleared -> %s\n",
           after_tamper > after_denied ? "YES" : "NO");
    assert(after_tamper > after_denied &&
           "the native audit hook must fire even when sys.meta_path has been fully cleared -- "
           "it has no Python-level object for guest code to have disabled");

    printf("test_python_audit_hook: PASS (see stdout above for the exact C1 verdict recorded in "
           "the ADR -- this test is observational, not enforcing)\n");
    return 0;
}
