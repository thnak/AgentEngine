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
//
// MEMORY LIMIT SIGNAL (closes the gap this file's earlier revision left as ADR-004 SS9.3/SS10.4
// "still unbuilt"): `create()` associates an I/O completion port with the Job Object
// (JobObjectAssociateCompletionPortInformation). The kernel posts JOB_OBJECT_MSG_JOB_MEMORY_LIMIT
// to that port the moment a commit attempt would exceed `JobMemoryLimit` -- a real, positive signal
// distinct from "any other unhandled-exception abort", not the peak-usage-vs-cap heuristic
// `native_jail_backend.cpp::exec()` used before this (that heuristic is now the fallback for when
// the completion port could not be created/associated, not the primary classifier).
// `wait_or_kill()` drains the port fully after every wait and sets `kill_reason::memory_limit` when
// seen -- draining is unconditional so a message can never leak from one `exec()` call's job-object
// reuse (the same `JobObjectLimits`, hence the same completion port, persists across every `exec()`
// call on one `SandboxHandle`) into a later, unrelated call's classification.

#include <chrono>
#include <cstdint>

#include <windows.h>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::native_jail {

// `memory_limit` is set from a real kernel signal (the completion-port notification, see this
// file's header comment) -- not inferred. `job_time_limit` and `process_limit` remain unset by this
// class today (job_time is confirmed unreliable per the MEASURED FINDING above and is not this
// class's trusted enforcement point; process_limit has no caller-visible distinction to make yet).
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

    // Jailed-Python-worker design (008 §1b/§3, superseding native_jail_backend.hpp's former
    // "Correction (2026-08-23)" comment) §6: a thin PUBLIC wrapper around the already-existing
    // PRIVATE `drain_memory_limit_notification()`, for the session-scoped watchdog thread
    // (native_jail_backend.cpp) to poll continuously across a worker's whole life -- not just once,
    // synchronously, right after ONE exec()'s own `wait_or_kill()` returns (`wait_or_kill()` itself
    // still drains via the private method directly; this public wrapper is for the watchdog's
    // repeated, standalone polls between and during exec calls, where nothing else is draining the
    // completion port). Safe to call repeatedly and interleaved with ordinary use; returns
    // `job_kill_reason::memory_limit` the moment a JOB_OBJECT_MSG_JOB_MEMORY_LIMIT notification is
    // seen, `job_kill_reason::none` otherwise. NEVER call this concurrently with `wait_or_kill()` on
    // the SAME instance -- this design never does, because `wait_or_kill()` is the per_exec path's
    // own watcher and this is the per_session watchdog's, and the two lifetimes never share one
    // `JobObjectLimits` (native_jail_backend.cpp's `Instance` holds exactly one `job` member; a
    // per_session `Instance` never also serves per_exec calls through the same handle).
    [[nodiscard]] result<job_kill_reason> poll_memory_limit_once();

    HANDLE native_handle() const { return job_; }

private:
    void close_now();
    // Drains every pending message off `completion_port_` (dwMilliseconds=0 each call, looped to
    // empty) and reports whether JOB_OBJECT_MSG_JOB_MEMORY_LIMIT was among them. No-op (returns
    // false) if the port was never created -- `create()`'s completion-port setup is best-effort, not
    // a hard requirement for this class to function at all.
    bool drain_memory_limit_notification();

    HANDLE job_ = nullptr;
    HANDLE completion_port_ = nullptr;
};

} // namespace agentengine::native_jail
