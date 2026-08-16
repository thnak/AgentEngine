// Which clock does WaitForSingleObject's timeout expire against, and how far can it drift from the
// steady_clock (QPC) a caller measures the wait with?
//
// The question is not academic. src/backends/native_jail/job_object_limits.cpp's wait_or_kill()
// brackets a WaitForSingleObject with two steady_clock reads and reports the difference as
// JobWaitOutcome::wall_elapsed. tests/test_job_object_limits.cpp then asserted
// `wall_elapsed >= deadline` -- and that assertion failed intermittently in CI on an UNinstrumented
// MSVC Release build (runs 31925631415 and 31939239439), which ADR-062's sanitizer findings do not
// explain. Either wait_or_kill() is killing children early (a real containment defect), or the
// assertion is asserting a guarantee Win32 does not make. This program decides which, without any
// AgentEngine code in the picture.
//
// Build (MSVC developer shell):
//     cl /nologo /O2 /EHsc /std:c++20 tests/experiments/wait_timeout_vs_qpc.cpp /Fe:wait_qpc.exe
//     .\wait_qpc.exe 60
//
// Measured 2026-08-16, MSVC 14.44, Windows 11, idle machine, 60 waits on a 500 ms deadline:
//
//     system clock tick increment : 15.6250 ms
//     default timer res   n=60  min=+1.154  p50=+9.727  max=+14.155  under-shoots=0
//     1 ms timer res      n=60  min=-0.007  p50=+0.194  max=+0.849   under-shoots=3
//
// Reading: the timeout is released on the kernel's tick clock, not on QPC. At the default 15.625 ms
// granularity the rounding-up slack dwarfs the divergence between the two clocks, so the wait always
// overshoots and a `>= deadline` assertion looks sound. Raise the resolution to 1 ms and the slack
// disappears, leaving the raw divergence -- and ~5% of waits return with QPC-measured elapsed just
// *under* the requested deadline.
//
// The operative detail for CI: timer resolution is a MACHINE-GLOBAL setting that any process can
// raise via timeBeginPeriod. So whether the assertion held was decided by unrelated software running
// on the same runner. Same code, same compiler, passes almost always -- which is exactly the
// signature that got it written off as "environmental flakiness" for most of a session.
//
// Verdict: the platform, not wait_or_kill(). The test now allows one tick of slack; see the comment
// at tests/test_job_object_limits.cpp's wall-clock assertion.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>  // WIN32_LEAN_AND_MEAN drops mmsystem.h, which is where timeBeginPeriod lives

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

// One batch of waits on a never-signaled event, so WaitForSingleObject always runs the full timeout
// path -- exactly like wait_or_kill() watching a child that never exits on its own (the "spin" case).
void run(int iters, int deadline_ms, char const* label) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<double> deltas;
    deltas.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        WaitForSingleObject(ev, static_cast<DWORD>(deadline_ms));
        auto t1 = std::chrono::steady_clock::now();
        deltas.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count() - deadline_ms);
    }
    CloseHandle(ev);

    std::sort(deltas.begin(), deltas.end());
    int under = 0;
    for (double v : deltas) {
        if (v < 0) ++under;
    }
    printf("%-20s n=%d  min=%+.3f  p50=%+.3f  max=%+.3f  under-shoots=%d\n", label, iters,
           deltas.front(), deltas[deltas.size() / 2], deltas.back(), under);
}

}  // namespace

int main(int argc, char** argv) {
    int const iters = argc > 1 ? atoi(argv[1]) : 60;

    DWORD adj = 0, incr = 0;
    BOOL disabled = FALSE;
    GetSystemTimeAdjustment(&adj, &incr, &disabled);
    printf("system clock tick increment : %.4f ms\n", incr / 10000.0);

    run(iters, 500, "default timer res");

    // If the timeout were released against QPC itself, changing the tick quantum could not move this
    // distribution at all. It moves, and it moves the sign of the minimum -- that is the finding.
    timeBeginPeriod(1);
    run(iters, 500, "1 ms timer res");
    timeEndPeriod(1);

    return 0;
}
