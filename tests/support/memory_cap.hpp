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
//   - POSIX: RLIMIT_AS -- the address-space bound, so a mmap/brk past it fails the same way.
//   - Under a sanitizer build the POSIX bound is NOT applied: ASan/TSan reserve terabytes of shadow
//     address space up front, and RLIMIT_AS would kill the process before main(). The Windows job limit
//     counts committed memory, not reservations, so it still applies there, at the larger
//     `sanitizer_bytes` to leave room for the ASan allocator's quarantine.
//
// Returns whether a cap is in force. A test should prove it with an over-cap allocation that must throw
// -- see test_json_dump_escape's M0 -- rather than trusting this return value alone; and should skip that
// probe when the cap is not in force (a sanitizer allocator aborts on failure instead of throwing).

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
        std::fprintf(stderr, "[memory_cap] sanitizer build: RLIMIT_AS not applied (shadow memory); UNCAPPED\n");
        return false;
    }
    rlimit const rl{static_cast<rlim_t>(limit), static_cast<rlim_t>(limit)};
    if (::setrlimit(RLIMIT_AS, &rl) != 0) {
        std::fprintf(stderr, "[memory_cap] setrlimit(RLIMIT_AS) failed; running UNCAPPED\n");
        return false;
    }
    std::fprintf(stderr, "[memory_cap] address space capped at %zu MiB\n", limit >> 20);
    return true;
#endif
}

}  // namespace agentengine::test_support
