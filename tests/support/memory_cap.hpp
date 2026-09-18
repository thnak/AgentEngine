#pragma once
// Cap a test process's own memory, so a test that goes wrong fails fast instead of taking the machine
// with it (CLAUDE.md "Machine safety").
//
// Written after a planted-bug negative control for test_json_dump_escape made the escaper re-append the
// whole string on every escaped byte: quadratic growth on a 4 MiB input, many GiB before the allocator
// gave up, and a near-hang of the developer's machine. The real test peaks at ~23 MiB; nothing bounded
// what a broken version of it could do.
//
// `cap_process_memory(bytes)` as the first statement of main() (after `fail_fast_on_windows()`):
//   - Windows: puts the process in a Job Object with JOB_OBJECT_LIMIT_PROCESS_MEMORY -- a commit beyond
//     the cap fails, and `operator new` throws std::bad_alloc. (Nested jobs are supported since Windows 8,
//     so this also works under a CI runner that already runs tests inside a job.)
//   - POSIX: RLIMIT_DATA -- the data-segment bound (brk plus private anonymous mappings, which is what
//     a runaway `new` actually consumes), so an allocation past it fails the same way.
//   - Under a sanitizer build the POSIX bound is NOT applied: ASan/TSan reserve terabytes of shadow
//     address space up front, and a POSIX memory rlimit would kill the process before main(). The Windows
//     job limit counts committed memory, not reservations, so it still applies there, at the larger
//     `sanitizer_bytes` to leave room for the ASan allocator's quarantine.
//
// NOT RLIMIT_AS, and this is load-bearing rather than a preference. RLIMIT_AS bounds the whole address
// space including PROT_NONE RESERVATIONS, and it is INHERITED ACROSS fork/exec -- so it applies to a test's
// child processes too. Every container CLI this repo shells out to (`docker`, `ctr`) is a Go binary, and
// the Go runtime reserves a large virtual arena during `mallocinit()` before main(); under an RLIMIT_AS of
// any size a test would tolerate, that reservation fails and the child dies with
// "fatal error: failed to reserve page summary memory". This is not hypothetical: it is exactly how
// commit a3864e1 turned test_execution_surface_image_identity red on the Linux CI leg, where the capped
// test spawned `docker run` and the CLI aborted in its own runtime startup. RLIMIT_DATA does not count
// reservations, so the Go children start normally -- measured on this project's WSL2 Ubuntu (kernel 6.6,
// Docker 29.7.2, containerd 2.2.2): `docker run --rm alpine echo` and `ctr` both run under a 256 MiB
// RLIMIT_DATA and both die under a 256 MiB RLIMIT_AS.
//
// What this costs: RLIMIT_DATA does not bound file-backed or shared mappings, so it is a narrower cap than
// RLIMIT_AS was. That is the right trade here -- the failure mode being defended against is a broken test
// allocating without bound, which is private anonymous memory -- but it is a real narrowing, not a
// no-cost substitution. RLIMIT_DATA has only covered mmap since Linux 4.7; on an older kernel it bounds
// brk alone and a large `new` would escape it. Nothing here detects that, which is why a caller must prove
// the cap with an over-cap allocation (test_json_dump_escape's M0) rather than trust the return value:
// on a platform where this mechanism does not hold, that probe fails loudly instead of silently passing.
//
// Returns whether a cap is in force. A test should prove it with an over-cap allocation that must throw
// -- see test_json_dump_escape's M0 -- rather than trusting this return value alone; and should skip that
// probe when the cap is not in force AND under any sanitizer even when it is: a sanitizer allocator aborts
// on an over-cap allocation instead of throwing (Windows ASan CI legs, where the job limit does apply).

#include <cstddef>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace agentengine::test_support {

[[nodiscard]] inline bool running_under_sanitizer() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

inline bool cap_process_memory(std::size_t bytes, std::size_t sanitizer_bytes) noexcept {
    bool const sanitized = running_under_sanitizer();
    std::size_t const limit = sanitized ? sanitizer_bytes : bytes;
#if defined(_WIN32)
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        std::fprintf(stderr, "[memory_cap] CreateJobObject failed (%lu); running UNCAPPED\n", ::GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    info.ProcessMemoryLimit = limit;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
        !::AssignProcessToJobObject(job, ::GetCurrentProcess())) {
        std::fprintf(stderr, "[memory_cap] job setup failed (%lu); running UNCAPPED\n", ::GetLastError());
        ::CloseHandle(job);
        return false;
    }
    // The handle is deliberately never closed: the limit must last as long as the process does.
    std::fprintf(stderr, "[memory_cap] process memory capped at %zu MiB%s\n", limit >> 20,
                 sanitized ? " (sanitizer build)" : "");
    return true;
#else
    if (sanitized) {
        std::fprintf(stderr, "[memory_cap] sanitizer build: RLIMIT_DATA not applied (shadow memory); UNCAPPED\n");
        return false;
    }
    // RLIMIT_DATA, never RLIMIT_AS: this limit is inherited by child processes, and a Go child (`docker`,
    // `ctr`) cannot start under an address-space bound. See the header comment.
    rlimit const rl{static_cast<rlim_t>(limit), static_cast<rlim_t>(limit)};
    if (::setrlimit(RLIMIT_DATA, &rl) != 0) {
        std::fprintf(stderr, "[memory_cap] setrlimit(RLIMIT_DATA) failed; running UNCAPPED\n");
        return false;
    }
    std::fprintf(stderr, "[memory_cap] data segment capped at %zu MiB\n", limit >> 20);
    return true;
#endif
}

}  // namespace agentengine::test_support
