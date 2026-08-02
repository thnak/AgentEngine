#pragma once
// Implements 008-Sandbox-and-Isolation.md §2 (ResourceLimits enforcement) and the wall-clock-kill
// clause of §2 item 2, as the Windows Job Object half of
// decisions/ADR-004-appcontainer-native-jail-windows-backend.md §3 steps 2-3 (design-only there;
// this header is the build-and-measure follow-up named in that ADR's §9 next steps, and first
// evidence toward 021-Platform-Support-and-Portability.md Q2). Backend: native_jail (008 §1b, §3),
// Windows only.
//
// Scope: an RAII wrapper around ONE Windows Job Object enforcing ResourceLimits::{memory_bytes,
// pids, cpu_ms} plus a host-side wall-clock watch for `wall_ms` (Job Objects have no native
// wall-clock-kill primitive -- JOB_OBJECT_LIMIT_JOB_TIME is cumulative CPU time consumed, not
// wall-clock elapsed; ADR-004 §3 step 3 already names this distinction). This header does NOT
// implement AppContainer (ADR-004's own scope, already built) -- the two compose (a process can be
// launched with both a SECURITY_CAPABILITIES attribute list entry and assigned to this Job Object)
// but are tested independently here, matching this project's practice of isolating one load-bearing
// invariant per test.
//
// disk_bytes, net_bytes, output_bytes, and fds are explicitly NOT enforced here -- 008 §1b already
// assigns those to interpreter-level mediation at the point of use (open/socket byte counters), and
// a Job Object has no native primitive for any of the three.
//
// MEASURED FINDING (tests/test_job_object_limits.cpp test_job_time_limit, 11-run sample, this
// session): JOB_OBJECT_LIMIT_JOB_TIME (the mechanism behind ResourceLimits::cpu_ms) is NOT a
// reliable enforcement point. Across 11 runs of a 100%-CPU-bound child under a 500ms job-time
// budget: it auto-terminated the process (STATUS_QUOTA_EXCEEDED) in only 3/11 runs, with the
// actual CPU time consumed before termination (not wall-clock elapsed -- these can differ) ranging
// 687.5ms-4109.4ms (1.38x-8.22x the configured budget, no discernible relationship to it) -- and in
// the remaining 8/11 runs it did not fire at all within a 5-second observation window, letting
// ~4.8-4.9s of CPU time accumulate (~9.5x-9.8x the budget) before wait_or_kill's own wall-clock
// watch (`wall_ms`, enforced entirely in this class, not by the kernel) terminated the job.
// cpu_ms/JOB_OBJECT_LIMIT_JOB_TIME must therefore be treated as best-effort only -- a caller that
// needs a dependable CPU/time bound MUST also set `wall_ms` to a value it actually wants enforced;
// `wall_ms` is confirmed reliable (fired within a few ms of the requested deadline in every
// measured run) and is the mechanism this class's contract actually rests on, not a redundant
// backstop on top of a trustworthy native limit. This finding feeds
// decisions/ADR-004-appcontainer-native-jail-windows-backend.md §10 and
// 021-Platform-Support-and-Portability.md Q2.

#include <chrono>
#include <cstdint>

#include <windows.h>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::native_jail {

enum class job_kill_reason { none, memory_limit, job_time_limit, wall_clock_timeout, process_limit };

struct JobWaitOutcome {
    bool                        exited_normally = false;
    DWORD                       exit_code = 0;
    job_kill_reason             kill_reason = job_kill_reason::none;
    std::chrono::milliseconds   wall_elapsed{0};
};

struct JobUsage {
    std::uint64_t peak_job_memory_bytes = 0;
    std::uint64_t total_user_time_100ns = 0;
};

// RAII: the Job Object handle is closed (and, per JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE -- always set,
// unconditionally, not gated by any ResourceLimits field -- every process still assigned to it is
// terminated) when this object is destroyed or moved-from. This is 008 §2 clause 4's "Full
// teardown" for whatever this Job Object is holding, independent of whether the caller explicitly
// tears down first.
class JobObjectLimits {
public:
    JobObjectLimits() = default;
    ~JobObjectLimits();

    JobObjectLimits(JobObjectLimits const&) = delete;
    JobObjectLimits& operator=(JobObjectLimits const&) = delete;
    JobObjectLimits(JobObjectLimits&& other) noexcept;
    JobObjectLimits& operator=(JobObjectLimits&& other) noexcept;

    // Creates the Job Object and applies every nonzero field of `limits`. A zero field means "do
    // not set this limit axis at all" -- this class does not decide whether an all-zero
    // ResourceLimits is a policy error; that is the SandboxSpec-validation layer's job, above this
    // one (008 §2's "no backend may weaken the contract by configuration" is about NOT silently
    // ignoring a limit it was asked to enforce, not about this constructor inventing a default).
    result<void> create(ResourceLimits const& limits);

    // Assigns an already-created process (CREATE_SUSPENDED recommended, so the cap applies before
    // the entry point runs) to this job.
    result<void> assign_process(HANDLE process_handle);

    // Waits for the assigned process to exit or for `wall_ms` to elapse, whichever comes first. On
    // timeout, calls TerminateJobObject (which — per JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE-independent
    // behavior — terminates every process in the job, not just `process_handle`) and reports
    // job_kill_reason::wall_clock_timeout. This IS ADR-004 §3 step 3's watcher: a bounded
    // WaitForSingleObject on the caller's own thread rather than a separate thread, since this call
    // already blocks the caller for the duration.
    result<JobWaitOutcome> wait_or_kill(HANDLE process_handle, std::chrono::milliseconds wall_ms);

    // 008 §8 observability.
    result<JobUsage> query_usage() const;

    HANDLE native_handle() const { return job_; }

private:
    void close_now();

    HANDLE job_ = nullptr;
};

} // namespace agentengine::native_jail
