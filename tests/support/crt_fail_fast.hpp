#pragma once
// Make a failing test DIE LOUDLY on Windows instead of blocking on a dialog nobody can dismiss.
//
// Eleven files under tests/ carried a local `disable_crt_assert_dialog()` that called
// `_CrtSetReportMode`/`_CrtSetReportFile`, and two carried nothing. That was sufficient while those
// files' `assert()`s only ran in Debug builds. It stopped being sufficient the moment tests/
// started stripping NDEBUG (tests/CMakeLists.txt), because those two functions are **debug-CRT
// only**. From the Windows SDK's own header:
//
//     ucrt/crtdbg.h:599   #ifndef _DEBUG
//     ucrt/crtdbg.h:603       #define _CrtSetReportMode(t, f)   ((int)0)
//
// So in a Release build with assertions live -- exactly the configuration that change created --
// every one of those guards compiles to nothing, and a failing `assert()` reaches `_wassert` and
// then `abort()`, where Windows Error Reporting can put up a dialog. On a headless CI runner that
// is not a failure report, it is a hang until the job timeout: the same
// looks-like-slowness-but-is-actually-a-stuck-process signature as the ASan runtime-DLL bug in
// ADR-062's precondition and the MSVC ASan PATH bug before it. A test suite whose failure mode is
// "hang" teaches people to distrust the timeout, not the test.
//
// `_set_abort_behavior` and `SetErrorMode` are the halves that work in BOTH configurations, and are
// the point of this header. The two `_Crt*` calls are kept because they are still correct and still
// useful in Debug; they are simply not load-bearing in Release.
//
// Call `fail_fast_on_windows()` as the first statement of main(). No-op off Windows.

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentengine::test_support {

inline void fail_fast_on_windows() noexcept {
#if defined(_WIN32)
    // Debug-CRT only -- these expand to ((int)0) under Release, see the banner. Kept for Debug.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);

    // Effective in BOTH Debug and Release, and the reason this header exists.
    // _WRITE_ABORT_MSG keeps the message on stderr (we want the diagnostic);
    // clearing _CALL_REPORTFAULT stops abort() from handing off to Windows Error Reporting.
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // Belt and braces for the non-abort paths: a hard fault or a missing-DLL/critical-error box
    // would otherwise block the same way.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

}  // namespace agentengine::test_support
