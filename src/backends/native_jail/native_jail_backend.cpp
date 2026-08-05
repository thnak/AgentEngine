// Implements native_jail_backend.hpp. See that header for the spec/ADR citations this satisfies.

#include "backends/native_jail/native_jail_backend.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

#include "backends/native_jail/app_container_profile.hpp"
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

// UTF-8 (this codebase's std::string convention) -> UTF-16, for the Win32 *W APIs the AppContainer
// and process-creation calls below need.
std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { close_now(); }
    void close_now() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    }
};

// 008 SS9 G3 ("no ambient authority"): a guest must see exactly what it was granted, nothing more.
// SandboxSpec has no explicit env field yet, so lpEnvironment=nullptr (Win32's "inherit the calling
// process's environment") would hand the guest the FULL host environment -- including anything the
// launching process set for its own use (API keys, tokens, ...), a real ambient-authority leak
// caught by M2 Phase C task C5's probe. LinuxNativeJailBackend already avoids the POSIX equivalent
// with a fixed `envp[] = {"PATH=/usr/bin:/bin", nullptr}`; this is that same fixed-minimal-block
// idea on Windows -- just enough for the OS loader/CRT and, empirically, AppContainer process
// creation itself to function, never whatever the launching process happened to have set.
//
// MEASURED FINDING (M2 Phase C task C5): a block containing only SystemRoot/Path is not enough --
// CreateProcessW(..., CREATE_UNICODE_ENVIRONMENT, ...) together with
// PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES (i.e. specifically an AppContainer launch) fails
// with ERROR_ENVVAR_NOT_FOUND (203) unless LOCALAPPDATA is present -- AppContainer's own process
// setup resolves the per-package data folder under %LOCALAPPDATA%\Packages\... and apparently reads
// it from the caller-supplied block rather than deriving it from the token. USERPROFILE is included
// alongside it for the same reason (LOCALAPPDATA is ordinarily derived from it). Neither is a
// secret -- both are ordinary system path facts about the host machine, not anything the launching
// process set for its own use -- so including them does not reopen the leak this function exists
// to close.
std::wstring build_minimal_environment_block() {
    wchar_t system_root_buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"SystemRoot", system_root_buf, MAX_PATH);
    std::wstring root =
        (len > 0 && len < MAX_PATH) ? std::wstring(system_root_buf, len) : std::wstring(L"C:\\Windows");

    wchar_t local_app_data_buf[MAX_PATH]{};
    DWORD lad_len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data_buf, MAX_PATH);
    wchar_t user_profile_buf[MAX_PATH]{};
    DWORD up_len = GetEnvironmentVariableW(L"USERPROFILE", user_profile_buf, MAX_PATH);

    std::wstring block;
    block += L"SystemRoot=" + root;
    block.push_back(L'\0');
    block += L"Path=" + root + L"\\System32;" + root;
    block.push_back(L'\0');
    if (lad_len > 0 && lad_len < MAX_PATH) {
        block += L"LOCALAPPDATA=" + std::wstring(local_app_data_buf, lad_len);
        block.push_back(L'\0');
    }
    if (up_len > 0 && up_len < MAX_PATH) {
        block += L"USERPROFILE=" + std::wstring(user_profile_buf, up_len);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');  // double-null terminator required by CREATE_UNICODE_ENVIRONMENT
    return block;
}

// One AppContainer profile for the whole process (ADR-004 §3: reused across sessions, the SID is
// stable identity). Magic-static init is thread-safe and runs exactly once; the fallible part
// (CreateAppContainerProfile/DeriveAppContainerSidFromAppContainerName) is captured into
// `init_error` rather than thrown, matching this project's `result<T>`-everywhere convention.
struct SharedProfileState {
    std::optional<AppContainerProfile> profile;
    std::optional<ae::error> init_error;
};

SharedProfileState& shared_profile_state() {
    static SharedProfileState state = [] {
        SharedProfileState s;
        auto created = AppContainerProfile::ensure(
            L"AgentEngine.NativeJail", L"AgentEngine Native Jail",
            L"AgentEngine native-jail sandbox AppContainer profile (008 SS1b, ADR-004)");
        if (created.has_value()) {
            s.profile.emplace(std::move(*created));
        } else {
            s.init_error = created.error();
        }
        return s;
    }();
    return state;
}

// Bounded pipe drain: reads until EOF (the write end closes once the child -- the only other
// handle holder, our own duplicate having already been closed at launch time -- exits) or until
// `cap_bytes` is reached, whichever first. Stops reading past the cap rather than buffering an
// unbounded amount into host memory (008 §2 item 2's "enforcement of output size... an unbounded
// stdout is a denial-of-service on the host" -- this is that enforcement point). `cap_bytes == 0`
// (SandboxSpec did not set output_bytes) still uses a fixed internal safety ceiling, never
// literally unbounded.
std::string drain_pipe_bounded(HANDLE read_handle, std::uint64_t cap_bytes) {
    constexpr std::uint64_t kDefaultSafetyCapBytes = 16ull * 1024 * 1024;  // 16 MiB, host-safety floor
    std::uint64_t const cap = cap_bytes > 0 ? cap_bytes : kDefaultSafetyCapBytes;

    std::string out;
    char buf[4096];
    for (;;) {
        DWORD read = 0;
        BOOL ok = ReadFile(read_handle, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) break;  // EOF (broken pipe) or read error -- either way, done
        std::uint64_t remaining = cap > out.size() ? cap - out.size() : 0;
        if (remaining == 0) break;  // at cap -- discard the rest by simply not reading further
        std::uint64_t take = static_cast<std::uint64_t>(read) < remaining
                                  ? static_cast<std::uint64_t>(read)
                                  : remaining;
        out.append(buf, static_cast<std::size_t>(take));
        if (out.size() >= cap) break;
    }
    return out;
}

}  // namespace

result<SandboxHandle> NativeJailBackend::create(SandboxSpec const& spec, EffectContext&) {
    SharedProfileState& shared = shared_profile_state();
    if (shared.init_error.has_value()) return std::unexpected(*shared.init_error);

    auto instance = std::make_unique<Instance>();
    instance->limits = spec.limits;

    for (MountSpec const& mount : spec.mounts) {
        if (!std::holds_alternative<std::string>(mount.source)) {
            return std::unexpected(ae::error{
                failure_class::policy,
                "MountSpec::source as a BlobRef is not supported by NativeJailBackend yet "
                "(M2 Phase C scope gap -- host paths only)",
                "native_jail.blob_mount_unsupported",
            });
        }
        std::wstring host_path = widen(std::get<std::string>(mount.source));
        auto granted = shared.profile->grant_path(host_path, mount.read_write);
        if (!granted.has_value()) return std::unexpected(granted.error());
        if (mount.read_write && instance->cwd.empty()) instance->cwd = host_path;
    }

    auto job_created = instance->job.create(spec.limits);
    if (!job_created.has_value()) return std::unexpected(job_created.error());

    static std::atomic<std::uint64_t> counter{0};
    std::string id = "native_jail-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    instances_.emplace(id, std::move(instance));
    return SandboxHandle{id};
}

result<ExecOutcome> NativeJailBackend::exec(SandboxHandle& handle, ExecRequest const& request,
                                             EffectContext&) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end()) {
        return std::unexpected(ae::error{
            failure_class::contract,
            "exec() called on an unknown or already-destroyed SandboxHandle",
            "native_jail.unknown_handle",
        });
    }
    Instance& inst = *it->second;

    SharedProfileState& shared = shared_profile_state();
    if (shared.init_error.has_value()) return std::unexpected(*shared.init_error);

    // 008 §2's "static constexpr ProfileTraits traits" line documents ExecRequest::source as this
    // backend's M2-only convention: a fully-resolved Win32 command line, not a name a Runner/Tool
    // registry has yet to mediate (see this file's header comment for the full scope statement).
    std::wstring cmdline = widen(request.source);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    SECURITY_CAPABILITIES sec_cap{};
    sec_cap.AppContainerSid = shared.profile->sid();
    sec_cap.Capabilities = nullptr;
    sec_cap.CapabilityCount = 0;

    SIZE_T attr_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_list_size);
    std::vector<std::byte> attr_buf(attr_list_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 2, 0, &attr_list_size)) {
        return win32_error("InitializeProcThreadAttributeList", failure_class::fatal,
                            "native_jail.attr_list_init_failed");
    }
    struct AttrListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrListGuard() { DeleteProcThreadAttributeList(list); }
    } attr_guard{attr_list};

    DWORD child_process_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;  // 008 §4 "Exec (nested): denied"

    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                    &sec_cap, sizeof(sec_cap), nullptr, nullptr)) {
        return win32_error("UpdateProcThreadAttribute(SECURITY_CAPABILITIES)", failure_class::fatal,
                            "native_jail.attr_security_capabilities_failed");
    }
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                    &child_process_policy, sizeof(child_process_policy), nullptr,
                                    nullptr)) {
        return win32_error("UpdateProcThreadAttribute(CHILD_PROCESS_POLICY)", failure_class::fatal,
                            "native_jail.attr_child_process_policy_failed");
    }

    // Separate pipes for stdout/stderr (not merged) so ExecOutcome's two fields are faithful, not
    // interleaved. Explicit 1 MiB buffer (not the 0-means-"system default" size, historically a
    // few KB) -- exec()'s own architecture drains AFTER wait_or_kill returns, not concurrently, so
    // a hostile child that writes far more than the kernel pipe buffer holds simply blocks in
    // write() until wall_ms kills it; a too-small buffer would make output_bytes containment
    // (C3's unbounded-output abuse case, 008 §2 item 2) look artificially tight regardless of the
    // configured cap, since only the OS's own small default would ever accumulate to drain.
    constexpr DWORD kPipeBufferBytes = 1024 * 1024;
    SECURITY_ATTRIBUTES pipe_sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleGuard stdout_read, stdout_write, stderr_read, stderr_write;
    if (!CreatePipe(&stdout_read.h, &stdout_write.h, &pipe_sa, kPipeBufferBytes) ||
        !CreatePipe(&stderr_read.h, &stderr_write.h, &pipe_sa, kPipeBufferBytes)) {
        return win32_error("CreatePipe", failure_class::fatal, "native_jail.pipe_create_failed");
    }
    SetHandleInformation(stdout_read.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = stdout_write.h;
    si.StartupInfo.hStdError = stderr_write.h;
    si.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi{};
    // Fixed minimal block, never nullptr -- see build_minimal_environment_block()'s own comment
    // (008 SS9 G3, C5): nullptr would inherit this HOST process's full environment.
    std::wstring env_block = build_minimal_environment_block();
    // CREATE_SUSPENDED: assign to the Job Object before the entry point runs (job_object_limits.hpp's
    // own documented usage), so every ResourceLimits axis applies from the guest's very first
    // instruction, not after some head start.
    BOOL created = CreateProcessW(
        nullptr, mutable_cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        env_block.data(), inst.cwd.empty() ? nullptr : inst.cwd.c_str(),
        reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
    // Our copies of the write ends must close regardless of outcome -- the child's own inherited
    // duplicates are what keep the pipes alive for it to write into; ours would otherwise wedge
    // drain_pipe_bounded's EOF wait forever.
    stdout_write.close_now();
    stderr_write.close_now();
    if (!created) {
        return win32_error("CreateProcessW", failure_class::fatal, "native_jail.create_process_failed");
    }
    HandleGuard process_guard{pi.hProcess};
    HandleGuard thread_guard{pi.hThread};

    auto assigned = inst.job.assign_process(pi.hProcess);
    if (!assigned.has_value()) {
        TerminateProcess(pi.hProcess, 1);
        return std::unexpected(assigned.error());
    }

    ResumeThread(pi.hThread);

    auto wait_result = inst.job.wait_or_kill(
        pi.hProcess, std::chrono::milliseconds{static_cast<std::int64_t>(inst.limits.wall_ms)});
    if (!wait_result.has_value()) return std::unexpected(wait_result.error());
    JobWaitOutcome const& wait_outcome = *wait_result;

    ExecOutcome outcome;
    outcome.stdout_text = drain_pipe_bounded(stdout_read.h, inst.limits.output_bytes);
    outcome.stderr_text = drain_pipe_bounded(stderr_read.h, inst.limits.output_bytes);

    if (wait_outcome.kill_reason == job_kill_reason::wall_clock_timeout) {
        outcome.klass = exec_outcome_class::timeout;
    } else if (wait_outcome.exit_code == 0) {
        outcome.klass = exec_outcome_class::ok;
    } else {
        // Job Objects give no completion-port-free way to distinguish "killed for exceeding
        // memory_bytes" from "any other unhandled-exception abort" (job_object_limits.hpp's own
        // documented limitation) -- a peak-usage-vs-configured-cap heuristic is the best available
        // signal without wiring a completion port (ADR-004 §9.3/§10.4, still unbuilt). The 90%
        // threshold (not an exact >=) accounts for VirtualAlloc commit granularity landing peak
        // usage just under the configured cap even when the cap is what actually stopped the
        // allocation. Honest, approximate classification, not silently mislabeled as "ok" or
        // omitted.
        auto usage = inst.job.query_usage();
        bool likely_oom = inst.limits.memory_bytes > 0 && usage.has_value() &&
                           usage->peak_job_memory_bytes >= (inst.limits.memory_bytes * 9) / 10;
        outcome.klass = likely_oom ? exec_outcome_class::oom : exec_outcome_class::crash;
    }
    return outcome;
}

void NativeJailBackend::destroy(SandboxHandle& handle) {
    // Erasing the map entry destroys the Instance, whose JobObjectLimits destructor closes the
    // Job Object handle -- JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (always set, job_object_limits.cpp)
    // terminates any process still assigned to it. This IS 008 §2 clause 4's "Full teardown" for
    // whatever this SandboxHandle was holding. The shared AppContainer profile/SID is NOT
    // destroyed here -- it is deployment-scoped, not session-scoped (ADR-004 §3), so a session
    // ending must not tear it down out from under any sibling session.
    instances_.erase(handle.opaque_id);
}

}  // namespace agentengine::native_jail
