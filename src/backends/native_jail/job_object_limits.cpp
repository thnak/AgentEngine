// Implements job_object_limits.hpp. See that header for the spec/ADR citations this satisfies.

#include "backends/native_jail/job_object_limits.hpp"

namespace agentengine::native_jail {

namespace {

std::unexpected<ae::error> win32_error(char const* what, failure_class klass, char const* code) {
    DWORD last = GetLastError();
    return std::unexpected(ae::error{
        klass,
        std::string(what) + " failed: Win32 error " + std::to_string(last),
        code,
    });
}

} // namespace

JobObjectLimits::~JobObjectLimits() { close_now(); }

JobObjectLimits::JobObjectLimits(JobObjectLimits&& other) noexcept
    : job_(other.job_), completion_port_(other.completion_port_) {
    other.job_ = nullptr;
    other.completion_port_ = nullptr;
}

JobObjectLimits& JobObjectLimits::operator=(JobObjectLimits&& other) noexcept {
    if (this != &other) {
        close_now();
        job_ = other.job_;
        completion_port_ = other.completion_port_;
        other.job_ = nullptr;
        other.completion_port_ = nullptr;
    }
    return *this;
}

void JobObjectLimits::close_now() {
    if (job_ != nullptr) {
        CloseHandle(job_); // KILL_ON_JOB_CLOSE (always set in create()) terminates any assigned
                            // process still alive at this point -- 008 §2 clause 4.
        job_ = nullptr;
    }
    if (completion_port_ != nullptr) {
        CloseHandle(completion_port_);
        completion_port_ = nullptr;
    }
}

bool JobObjectLimits::drain_memory_limit_notification() {
    if (completion_port_ == nullptr) return false;  // never associated -- see create()'s best-effort note
    bool found = false;
    for (;;) {
        DWORD         num_bytes = 0;
        ULONG_PTR     completion_key = 0;
        LPOVERLAPPED  overlapped = nullptr;
        // dwMilliseconds=0: a poll, not a wait -- by the time this is called, the process this
        // Job Object was watching has already exited or been terminated, so any message the kernel
        // was going to post for that exec() is already queued; there is nothing left to wait for.
        if (!GetQueuedCompletionStatus(completion_port_, &num_bytes, &completion_key, &overlapped, 0)) {
            break;  // port empty -- fully drained
        }
        // Per MSDN: for job-object completion messages, lpNumberOfBytesTransferred carries the
        // message identifier itself (JOB_OBJECT_MSG_*), and lpOverlapped carries the process id of
        // the process that caused it -- NOT a real OVERLAPPED* to dereference.
        if (num_bytes == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) found = true;
    }
    return found;
}

result<void> JobObjectLimits::create(ResourceLimits const& limits) {
    if (job_ != nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "JobObjectLimits::create called twice on the same instance",
                                          "job_object.already_created"});
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return win32_error("CreateJobObjectW", failure_class::fatal, "job_object.create_failed");
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    // Always set, unconditionally: teardown must not depend on any ResourceLimits field being
    // nonzero (008 §2 clause 4 is not optional).
    info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (limits.memory_bytes > 0) {
        info.JobMemoryLimit = static_cast<SIZE_T>(limits.memory_bytes);
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    }
    if (limits.pids > 0) {
        info.BasicLimitInformation.ActiveProcessLimit = limits.pids;
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    }
    if (limits.cpu_ms > 0) {
        // PerJobUserTimeLimit is in 100-nanosecond units, cumulative across every process the job
        // has ever held -- this is ResourceLimits::cpu_ms's actual semantics (a CPU-time budget),
        // not JobObjectCpuRateControlInformation's percentage-of-core throttle, which answers a
        // different question ("how fast may it run") than this one ("how much may it consume").
        ULONGLONG hundred_ns = static_cast<ULONGLONG>(limits.cpu_ms) * 10'000ULL;
        info.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = static_cast<LONGLONG>(hundred_ns);
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
    }

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        DWORD last = GetLastError();
        CloseHandle(job);
        SetLastError(last);
        return win32_error("SetInformationJobObject", failure_class::fatal,
                            "job_object.set_limits_failed");
    }

    job_ = job;

    // Best-effort: gives wait_or_kill() a REAL JOB_OBJECT_MSG_JOB_MEMORY_LIMIT signal instead of
    // the peak-usage-vs-cap heuristic native_jail_backend.cpp falls back to when this setup fails.
    // Not fatal to create() overall -- every ResourceLimits axis this class enforces is already
    // applied above regardless of whether this optional signal is wired.
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (port != nullptr) {
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc{};
        assoc.CompletionKey  = job_;
        assoc.CompletionPort = port;
        if (SetInformationJobObject(job_, JobObjectAssociateCompletionPortInformation, &assoc,
                                     sizeof(assoc))) {
            completion_port_ = port;
        } else {
            CloseHandle(port);
        }
    }

    return {};
}

result<void> JobObjectLimits::assign_process(HANDLE process_handle) {
    if (job_ == nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "JobObjectLimits::assign_process called before create()",
                                          "job_object.not_created"});
    }
    if (!AssignProcessToJobObject(job_, process_handle)) {
        return win32_error("AssignProcessToJobObject", failure_class::fatal,
                            "job_object.assign_failed");
    }
    return {};
}

result<JobWaitOutcome> JobObjectLimits::wait_or_kill(HANDLE process_handle,
                                                      std::chrono::milliseconds wall_ms) {
    if (job_ == nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "JobObjectLimits::wait_or_kill called before create()",
                                          "job_object.not_created"});
    }

    auto t0 = std::chrono::steady_clock::now();
    DWORD wait_ms = wall_ms.count() > 0 ? static_cast<DWORD>(wall_ms.count()) : INFINITE;
    DWORD wait_rc = WaitForSingleObject(process_handle, wait_ms);
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    JobWaitOutcome outcome{};
    outcome.wall_elapsed = elapsed;

    if (wait_rc == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_handle, &exit_code)) {
            return win32_error("GetExitCodeProcess", failure_class::fatal,
                                "job_object.exit_code_query_failed");
        }
        outcome.exited_normally = true;
        outcome.exit_code = exit_code;
        // Distinguish "exited on its own" from "the job's own PerJobUserTimeLimit killed it before
        // our wall-clock deadline arrived" -- both surface here as WAIT_OBJECT_0 (the process handle
        // becomes signaled either way). The memory-limit case has a REAL signal now (see this file's
        // header comment): drain the completion port and check for
        // JOB_OBJECT_MSG_JOB_MEMORY_LIMIT before falling back to "unknown". The job-time-limit case
        // still has no positive signal of its own (JOB_OBJECT_LIMIT_JOB_TIME posts no distinguishing
        // completion message this class watches for) -- callers still have to infer it from an
        // unexpected exit_code alongside a wall_elapsed far shorter than wall_ms, as before.
        // A WAIT_OBJECT_0 with exit_code == STATUS_QUOTA_EXCEEDED (0xC0000044) is the kernel's own
        // JOB_OBJECT_LIMIT_JOB_TIME firing (measured, tests/test_job_object_limits.cpp's
        // test_job_time_limit -- see job_object_limits.hpp's header comment for the finding this
        // confirms and the reliability caveat that goes with it). This method does not special-case
        // that exit code into its own kill_reason value; callers that care distinguish it themselves.
        outcome.kill_reason =
            drain_memory_limit_notification() ? job_kill_reason::memory_limit : job_kill_reason::none;
        return outcome;
    }

    if (wait_rc == WAIT_TIMEOUT) {
        // ADR-004 §3 step 3's watcher: wall-clock deadline hit before the process exited on its own
        // (whether by normal completion or by a Job Object limit firing first) -- terminate the
        // whole job now.
        TerminateJobObject(job_, static_cast<UINT>(0xE0000001)); // recognizable sentinel exit code
        WaitForSingleObject(process_handle, INFINITE); // ensure the handle is signaled before return
        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);
        outcome.exited_normally = false;
        outcome.exit_code = exit_code;
        // `wall_ms` is what actually ended this exec() -- that stays the reported reason regardless
        // of whether a memory-limit message also happened to be queued. Still drain unconditionally:
        // the SAME JobObjectLimits (and completion port) is reused across every exec() call on one
        // SandboxHandle (NativeJailBackend::Instance), so an undrained message here would otherwise
        // leak into and misclassify a LATER, unrelated call.
        drain_memory_limit_notification();
        outcome.kill_reason = job_kill_reason::wall_clock_timeout;
        return outcome;
    }

    return win32_error("WaitForSingleObject", failure_class::fatal, "job_object.wait_failed");
}

result<JobUsage> JobObjectLimits::query_usage() const {
    if (job_ == nullptr) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "JobObjectLimits::query_usage called before create()",
                                          "job_object.not_created"});
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    DWORD returned = 0;
    if (!QueryInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info),
                                    &returned)) {
        return win32_error("QueryInformationJobObject", failure_class::fatal,
                            "job_object.query_usage_failed");
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    // BasicLimitInformation on JOBOBJECT_EXTENDED_LIMIT_INFORMATION does not carry accounting
    // fields (total user time) -- that requires the separate accounting query.
    if (QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting,
                                   sizeof(accounting), &returned)) {
        JobUsage usage{};
        usage.peak_job_memory_bytes = static_cast<std::uint64_t>(info.PeakJobMemoryUsed);
        usage.total_user_time_100ns = static_cast<std::uint64_t>(accounting.TotalUserTime.QuadPart);
        return usage;
    }
    return win32_error("QueryInformationJobObject(BasicAccounting)", failure_class::fatal,
                        "job_object.query_accounting_failed");
}

} // namespace agentengine::native_jail
