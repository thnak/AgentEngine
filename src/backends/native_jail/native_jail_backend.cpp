// Implements native_jail_backend.hpp. See that header for the spec/ADR citations this satisfies.

#include "backends/native_jail/native_jail_backend.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include "agentengine/core/worktree_mount_fs.hpp"  // open_within_mount_root/list_within_mount_root/
                                                      // mount_root_usage -- HandleRelay design draft §1
#include "agentengine/sandbox/net_egress_proxy.hpp"  // resolve_and_validate -- HandleRelay design draft §2
#include "backends/native_jail/agent_tools_codegen.hpp"
#include "backends/native_jail/app_container_profile.hpp"
#include "backends/native_jail/jailed_worker_rpc.hpp"
#include "backends/native_jail/job_object_limits.hpp"
#include "backends/native_jail/mediated_python_worker_protocol.hpp"
#include "backends/native_jail/relay_base64.hpp"  // HandleRelay design draft §2

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

// UTF-16 -> UTF-8, the inverse of widen() above -- needed by create_python_worker() (below) to put
// python_home/extra_sys_path (stored as std::wstring in PythonWorkerSessionConfig, native_jail_backend.hpp)
// into init_request's JSON text.
std::string narrow(std::wstring const& utf16) {
    if (utf16.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), needed,
                         nullptr, nullptr);
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
    // create_python_worker() (below) needs to hand off ownership of the host-side pipe ends into
    // PythonWorkerState -- the existing per_exec exec() path (above) never needed this, so it is
    // additive, not a change to that path's own use of this type.
    HANDLE release() {
        HANDLE tmp = h;
        h = nullptr;
        return tmp;
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
// stable identity). The fallible part
// (CreateAppContainerProfile/DeriveAppContainerSidFromAppContainerName) is returned as an error
// rather than thrown, matching this project's `result<T>`-everywhere convention.
//
// This was a magic static holding BOTH the profile and a cached `init_error`, which had a defect
// worth naming because it turned a rare transient into a total one. Profile creation can fail
// transiently under cross-process contention (measured -- app_container_profile.cpp's
// CrossProcessLock, tests/experiments/appcontainer_profile_race.cpp). A magic static runs its
// initializer exactly once, so ONE such failure was latched for the lifetime of the process and
// every subsequent create() returned the stale error. That is why the observed symptom was never
// "one create() failed" but "all nine create()s in this test failed", while a concurrent process
// was perfectly healthy.
//
// Success is still cached forever -- that part was right, and `profile` is never mutated again once
// engaged, so handing out a pointer to it is safe. Only failure is retried. Callers that genuinely
// cannot create a profile still fail closed, one error per attempt, which is what they should have
// been getting all along.
[[nodiscard]] result<AppContainerProfile const*> shared_profile() {
    static std::mutex profile_mutex;
    static std::optional<AppContainerProfile> profile;

    std::lock_guard<std::mutex> guard(profile_mutex);
    if (!profile.has_value()) {
        auto created = AppContainerProfile::ensure(
            L"AgentEngine.NativeJail", L"AgentEngine Native Jail",
            L"AgentEngine native-jail sandbox AppContainer profile (008 SS1b, ADR-004)");
        if (!created.has_value()) return std::unexpected(created.error());
        profile.emplace(std::move(*created));
    }
    return &*profile;
}

// Correction (2026-08-23, independent red-team pass, REAL GAP finding): `AppContainerProfile::
// grant_path()` is explicitly documented as additive, non-idempotent -- "callers grant each mount
// exactly once." `create_python_worker()`'s read-only grants on the worker binary directory,
// `python_home`, and `extra_sys_path` are host-DEPLOYMENT-fixed paths, identical across every
// session, not session-scoped mount paths -- calling `grant_path()` on them from every session
// creation appended a fresh, redundant ACE to the SAME shared profile's DACL every time, growing it
// without bound over a long-running host's lifetime purely from ordinary session churn (no attack
// needed). Deduped here, matching `shared_profile()`'s own "one profile, reused across sessions"
// lifetime model one field over -- a path is granted at most once for the life of this process.
[[nodiscard]] result<void> grant_ro_deduped(AppContainerProfile const& profile, std::wstring const& path) {
    if (path.empty()) return {};
    static std::mutex granted_mutex;
    static std::unordered_set<std::wstring> granted;

    std::lock_guard<std::mutex> guard(granted_mutex);
    if (granted.contains(path)) return {};
    auto result = profile.grant_path(path, /*read_write=*/false);
    if (!result.has_value()) return result;
    granted.insert(path);
    return {};
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
    // docs/planning/sandbox-spec-capability-enforcement-design-draft.md (Slice 3): capabilities are
    // the real authority behind mounts/net, checked FIRST -- a no-op for every caller that doesn't
    // hold a SandboxMount/SandboxNetOut grant (see authorize_spec()'s own comment, sandbox.hpp).
    if (auto authorized = authorize_spec(spec); !authorized.has_value()) {
        return std::unexpected(authorized.error());
    }
    // decisions/ADR-087-sandbox-spec-capability-enforcement.md: closes a real per-backend divergence
    // this design's own red-team pass found (finding B3) -- KataBackend::create() already fails
    // closed on any NetPolicy beyond deny_all=true (ADR-086); this backend previously ignored
    // NetPolicy entirely. Independent of capabilities/authorize_spec() above: this backend has no
    // CNI/egress-proxy of any kind wired to honor a real allowlist, so the same identical spec would
    // otherwise silently mean two different things depending on which backend a host selected.
    if (!spec.net.deny_all || !spec.net.allowlist.empty()) {
        return std::unexpected(ae::error{
            failure_class::policy,
            "native_jail: NetPolicy with deny_all=false or a nonempty allowlist is not supported "
            "yet -- this backend has no CNI/egress-proxy wired to honor a real allowlist",
            "native_jail.net_allowlist_unsupported"});
    }

    auto profile = shared_profile();
    if (!profile.has_value()) return std::unexpected(profile.error());

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
        auto granted = (*profile)->grant_path(host_path, mount.read_write);
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

    auto profile = shared_profile();
    if (!profile.has_value()) return std::unexpected(profile.error());

    // 008 §2's "static constexpr ProfileTraits traits" line documents ExecRequest::source as this
    // backend's M2-only convention: a fully-resolved Win32 command line, not a name a Runner/Tool
    // registry has yet to mediate (see this file's header comment for the full scope statement).
    std::wstring cmdline = widen(request.source);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

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

    // Correction (2026-08-23, independent red-team pass, BLOCKING finding): `hStdInput` used to be
    // `GetStdHandle(STD_INPUT_HANDLE)` -- the HOST process's own real stdin, handed to the guest
    // with no capability grant (ExecRequest/SandboxSpec has no stdin concept at all). An
    // explicitly-created, always-empty NUL handle replaces it -- `ExecRequest` still has no stdin
    // axis to wire a real one through, so "no input" is the only correct default, not "whatever the
    // host happens to have open." Inheritable via the same `pipe_sa`, closed the same way the pipe
    // write-ends are below.
    HandleGuard nul_input;
    nul_input.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &pipe_sa, OPEN_EXISTING, 0, nullptr);
    if (nul_input.h == INVALID_HANDLE_VALUE) {
        return win32_error("CreateFileW(NUL)", failure_class::fatal, "native_jail.nul_stdin_open_failed");
    }

    SECURITY_CAPABILITIES sec_cap{};
    sec_cap.AppContainerSid = (*profile)->sid();
    sec_cap.Capabilities = nullptr;
    sec_cap.CapabilityCount = 0;

    // Correction (2026-08-23, independent red-team pass, BLOCKING finding, AC-S1 falsified by
    // execution): this attribute list used to be 2 entries (SECURITY_CAPABILITIES,
    // CHILD_PROCESS_POLICY) with NO PROC_THREAD_ATTRIBUTE_HANDLE_LIST -- `CreateProcessW`'s
    // `bInheritHandles=TRUE` with no handle list duplicates EVERY inheritable handle in the host
    // process into the child, not just the three named below. A live, unrelated inheritable handle
    // in the host at exec() time (a socket, another session's pipe) was reachable from inside a
    // zero-capability AppContainer child with no grant at all -- reproduced directly: a
    // zero-capability child inherited and read an unrelated secret through such a handle. Now 3
    // entries, matching create_python_worker()'s own already-correct HANDLE_LIST pattern below --
    // exactly {stdout_write.h, stderr_write.h, nul_input.h} are inheritable; nothing else is.
    HANDLE inherit_list[3] = {stdout_write.h, stderr_write.h, nul_input.h};

    SIZE_T attr_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 3, 0, &attr_list_size);
    std::vector<std::byte> attr_buf(attr_list_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 3, 0, &attr_list_size)) {
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
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list,
                                    sizeof(inherit_list), nullptr, nullptr)) {
        return win32_error("UpdateProcThreadAttribute(HANDLE_LIST)", failure_class::fatal,
                            "native_jail.attr_handle_list_failed");
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = stdout_write.h;
    si.StartupInfo.hStdError = stderr_write.h;
    si.StartupInfo.hStdInput = nul_input.h;
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
    // drain_pipe_bounded's EOF wait forever. Same reasoning for nul_input: the child's own
    // inherited duplicate (if it ever reads stdin at all) is what it actually uses.
    stdout_write.close_now();
    stderr_write.close_now();
    nul_input.close_now();
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
    } else if (wait_outcome.kill_reason == job_kill_reason::memory_limit) {
        // A REAL kernel signal now (job_object_limits.hpp's completion-port wiring, closing the gap
        // ADR-004 §9.3/§10.4 left "still unbuilt"): JOB_OBJECT_MSG_JOB_MEMORY_LIMIT was posted for
        // this exec()'s job, so this classification is direct, not inferred from usage stats.
        outcome.klass = exec_outcome_class::oom;
    } else {
        // No completion-port signal fired (either the port failed to associate at create() time --
        // best-effort -- or this nonzero exit is for an unrelated reason). Fall back to the
        // peak-usage-vs-configured-cap heuristic this classification used exclusively before the
        // completion port existed. The 90% threshold (not an exact >=) accounts for VirtualAlloc
        // commit granularity landing peak usage just under the configured cap even when the cap is
        // what actually stopped the allocation. Honest, approximate classification, not silently
        // mislabeled as "ok" or omitted.
        auto usage = inst.job.query_usage();
        bool likely_oom = inst.limits.memory_bytes > 0 && usage.has_value() &&
                           usage->peak_job_memory_bytes >= (inst.limits.memory_bytes * 9) / 10;
        outcome.klass = likely_oom ? exec_outcome_class::oom : exec_outcome_class::crash;
    }
    return outcome;
}

void NativeJailBackend::destroy(SandboxHandle& handle) {
    // Jailed-Python-worker design: a per_session Python worker gets an orderly teardown FIRST (stop
    // the watchdog thread, best-effort `shutdown` handshake, bounded wait) -- the ordinary per_exec
    // path below (erase -> JobObjectLimits dtor -> KILL_ON_JOB_CLOSE) is still the unconditional
    // backstop either way, so a worker that never responds is still guaranteed torn down.
    auto it = instances_.find(handle.opaque_id);
    if (it != instances_.end() && it->second->worker.has_value()) {
        Instance& inst = *it->second;
        PythonWorkerState& ws = *inst.worker;
        stop_watchdog(inst);
        if (ws.alive.load() && ws.downstream_write != nullptr) {
            FramedChannel channel(ws.upstream_read, ws.downstream_write);
            json::Value shutdown_msg =
                json::Value::make_object({{"type", json::Value::make_string(worker_protocol::kShutdown)}});
            (void)channel.send(shutdown_msg);  // best-effort -- KILL_ON_JOB_CLOSE below is the real backstop
        }
        if (ws.process.hProcess != nullptr) {
            WaitForSingleObject(ws.process.hProcess, 2000);
        }
        close_worker_handles(inst);
    }

    // Erasing the map entry destroys the Instance, whose JobObjectLimits destructor closes the
    // Job Object handle -- JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (always set, job_object_limits.cpp)
    // terminates any process still assigned to it. This IS 008 §2 clause 4's "Full teardown" for
    // whatever this SandboxHandle was holding. The shared AppContainer profile/SID is NOT
    // destroyed here -- it is deployment-scoped, not session-scoped (ADR-004 §3), so a session
    // ending must not tear it down out from under any sibling session.
    instances_.erase(handle.opaque_id);
}

result<void> NativeJailBackend::grant_ro_path_once(std::wstring const& path) {
    auto profile = shared_profile();
    if (!profile.has_value()) return std::unexpected(profile.error());
    return grant_ro_deduped(**profile, path);
}

// ================================================================================================
// Jailed-Python-worker surface (008 §1b/§3, 010 §2/§6 -- decisions/ADR-081-jailed-python-worker-
// process-slice-1.md is the ADR superseding this file's former "Correction (2026-08-23)" comment on
// the header). See native_jail_backend.hpp's own comments for the per-field rationale; this section
// is the design's §4-§8a made concrete.
// ================================================================================================

namespace {
namespace wp = ::agentengine::native_jail::worker_protocol;
}  // namespace

void NativeJailBackend::terminate_worker(Instance& inst) {
    if (!inst.worker.has_value()) return;
    // 0xE000_0002: a recognizable sentinel exit code, same idiom exec()'s own wait_or_kill() uses
    // (0xE0000001) -- distinct values so a post-mortem exit-code dump can tell which path fired.
    TerminateJobObject(inst.job.native_handle(), static_cast<UINT>(0xE0000002));
}

void NativeJailBackend::stop_watchdog(Instance& inst) {
    if (!inst.worker.has_value()) return;
    PythonWorkerState& ws = *inst.worker;
    ws.phase.store(watchdog_phase::stopping);
    if (ws.stop_event != nullptr) SetEvent(ws.stop_event);
    if (ws.watchdog_thread.joinable()) ws.watchdog_thread.join();
}

void NativeJailBackend::close_worker_handles(Instance& inst) {
    if (!inst.worker.has_value()) return;
    PythonWorkerState& ws = *inst.worker;
    // HandleRelay design draft §4 item 5: every host-owned relayed socket is force-closed here, the
    // ONE point every teardown path (destroy(), and every create_python_worker() post-creation
    // failure path) already funnels through -- no relayed socket ever outlives the worker Instance it
    // belongs to, even if the guest never called connect_close itself.
    for (auto& [socket_id, fd] : ws.live_sockets) agentengine::pal::close_fd(fd);
    ws.live_sockets.clear();
    ws.open_files.clear();  // each SafeFileHandle's own destructor closes its real HANDLE
    if (ws.process.hProcess != nullptr) {
        CloseHandle(ws.process.hProcess);
        ws.process.hProcess = nullptr;
    }
    if (ws.downstream_write != nullptr) {
        CloseHandle(ws.downstream_write);
        ws.downstream_write = nullptr;
    }
    if (ws.upstream_read != nullptr) {
        CloseHandle(ws.upstream_read);
        ws.upstream_read = nullptr;
    }
    if (ws.stop_event != nullptr) {
        CloseHandle(ws.stop_event);
        ws.stop_event = nullptr;
    }
}

void NativeJailBackend::session_watchdog_loop(Instance& inst) {
    PythonWorkerState& ws = *inst.worker;
    for (;;) {
        watchdog_phase const phase = ws.phase.load();
        if (phase == watchdog_phase::stopping) return;

        // Continuous single drainer (final spec §6, closing G-Q3's stale-notification concern
        // structurally): there is never a gap where nothing is draining the Job Object's completion
        // port, unlike a design that only drained inside exec()'s own wait_or_kill().
        if (auto polled = inst.job.poll_memory_limit_once();
            polled.has_value() && *polled == job_kill_reason::memory_limit) {
            TerminateJobObject(inst.job.native_handle(), static_cast<UINT>(0xE0000003));
            ws.kill_reason.store(job_kill_reason::memory_limit);
            ws.alive.store(false);
        }

        auto const now = std::chrono::steady_clock::now();
        switch (phase) {
            case watchdog_phase::awaiting_init:
            case watchdog_phase::call_active:
                if (now >= ws.phase_deadline) {
                    TerminateJobObject(inst.job.native_handle(), static_cast<UINT>(0xE0000001));
                    ws.kill_reason.store(job_kill_reason::wall_clock_timeout);
                    ws.alive.store(false);
                }
                break;
            case watchdog_phase::idle: {
                // RT2 Finding 1: a guest-spawned daemon thread that outlives the call which spawned
                // it would otherwise pin CPU forever, undetected, once no exec_request is
                // outstanding -- this is the fix, a background CPU budget per idle window.
                if (auto usage = inst.job.query_usage(); usage.has_value()) {
                    std::uint64_t const cpu_since_window_start =
                        usage->total_user_time_100ns >= ws.idle_window_start_cpu_100ns
                            ? usage->total_user_time_100ns - ws.idle_window_start_cpu_100ns
                            : 0;
                    std::uint64_t const budget_100ns =
                        static_cast<std::uint64_t>(ws.session_config.idle_cpu_budget_ms.count()) * 10'000ULL;
                    if (cpu_since_window_start > budget_100ns) {
                        TerminateJobObject(inst.job.native_handle(), static_cast<UINT>(0xE0000004));
                        // job_kill_reason has no dedicated "background CPU budget exceeded" member --
                        // process_limit is repurposed here (unused for any other kill reason on this
                        // path), matching the final spec §6's own note.
                        ws.kill_reason.store(job_kill_reason::process_limit);
                        ws.alive.store(false);
                    } else if (now - ws.idle_window_start_time >= ws.session_config.idle_cpu_window_ms) {
                        ws.idle_window_start_time = now;
                        ws.idle_window_start_cpu_100ns = usage->total_user_time_100ns;
                    }
                }
                break;
            }
            default:
                break;
        }

        auto const poll_interval = (phase == watchdog_phase::call_active ||
                                     phase == watchdog_phase::awaiting_init)
                                        ? std::chrono::milliseconds(5)
                                        : std::chrono::milliseconds(100);
        WaitForSingleObject(ws.stop_event, static_cast<DWORD>(poll_interval.count()));
    }
}

result<SandboxHandle> NativeJailBackend::create_python_worker(SandboxSpec const& spec,
                                                                PythonWorkerSessionConfig session,
                                                                EffectContext&) {
    // docs/planning/sandbox-spec-capability-enforcement-design-draft.md §6 finding B1: this is a
    // SEPARATE mount-granting entry point from create() above (MediatedPythonRunner::initialize()
    // calls this, never create()) -- the mechanism's own red-team pass found it would otherwise be
    // silently unenforced on the one path that matters most in production (the mediated Python
    // interpreter's real host-directory mounts). A no-op for every caller that doesn't hold a
    // SandboxMount/SandboxNetOut grant, identical to create()'s own call above.
    if (auto authorized = authorize_spec(spec); !authorized.has_value()) {
        return std::unexpected(authorized.error());
    }

    auto profile = shared_profile();
    if (!profile.has_value()) return std::unexpected(profile.error());

    if (session.worker_exe_path.empty()) {
        return std::unexpected(ae::error{
            failure_class::contract,
            "PythonWorkerSessionConfig::worker_exe_path is empty -- the caller must set it "
            "(mediated_python_runner.cpp derives it from the build-injected "
            "AE_PYTHON_WORKER_EXE_PATH macro)",
            "native_jail.worker_exe_path_missing",
        });
    }

    auto instance = std::make_unique<Instance>();
    instance->limits = spec.limits;

    for (MountSpec const& mount : spec.mounts) {
        if (!std::holds_alternative<std::string>(mount.source)) {
            return std::unexpected(ae::error{
                failure_class::policy,
                "MountSpec::source as a BlobRef is not supported by NativeJailBackend yet",
                "native_jail.blob_mount_unsupported",
            });
        }
        std::wstring host_path = widen(std::get<std::string>(mount.source));
        auto granted = (*profile)->grant_path(host_path, mount.read_write);
        if (!granted.has_value()) return std::unexpected(granted.error());
        if (mount.read_write && instance->cwd.empty()) instance->cwd = host_path;
    }

    // The worker binary and the interpreter deployment paths (python_home/extra_sys_path) are NOT
    // SandboxSpec mounts -- they are host deployment config the worker process needs read+execute
    // access to just to START, not guest-visible session data -- so they get their own grant_path()
    // calls here (008 §2's mount contract, applied one layer up from ordinary guest mounts).
    auto grant_ro = [&](std::wstring const& path) -> result<void> {
        return grant_ro_deduped(**profile, path);
    };
    {
        std::filesystem::path exe_dir = std::filesystem::path(session.worker_exe_path).parent_path();
        if (auto g = grant_ro(exe_dir.wstring()); !g.has_value()) return std::unexpected(g.error());
    }
    if (auto g = grant_ro(session.python_home); !g.has_value()) return std::unexpected(g.error());
    for (auto const& p : session.extra_sys_path) {
        if (auto g = grant_ro(p); !g.has_value()) return std::unexpected(g.error());
    }

    auto job_created = instance->job.create(spec.limits);
    if (!job_created.has_value()) return std::unexpected(job_created.error());

    instance->worker.emplace();
    PythonWorkerState& ws = *instance->worker;
    ws.session_config = session;  // COPY, deliberately -- `session` (the parameter) is still read
                                    // below (tool_bridge, extra_sys_path, ...) to build init_request;
                                    // ws.session_config is the watchdog thread's own, separately-read
                                    // copy (idle_cpu_*/init_timeout_ms), never mutated after this line.

    SECURITY_ATTRIBUTES pipe_sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleGuard down_read, down_write, up_read, up_write;
    if (!CreatePipe(&down_read.h, &down_write.h, &pipe_sa, 0) ||
        !CreatePipe(&up_read.h, &up_write.h, &pipe_sa, 0)) {
        return win32_error("CreatePipe", failure_class::fatal, "native_jail.worker_pipe_create_failed");
    }
    // Host-side ends must NOT be inherited by the child -- only the two child-side handles below,
    // named explicitly in the PROC_THREAD_ATTRIBUTE_HANDLE_LIST attribute (the "hardened inheritance"
    // final spec §2 calls for, tighter than exec()'s own bInheritHandles=TRUE + SetHandleInformation
    // idiom, appropriate here because this child is long-lived, not a short-lived per-exec probe).
    SetHandleInformation(down_write.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(up_read.h, HANDLE_FLAG_INHERIT, 0);

    SECURITY_CAPABILITIES sec_cap{};
    sec_cap.AppContainerSid = (*profile)->sid();
    sec_cap.Capabilities = nullptr;
    sec_cap.CapabilityCount = 0;

    HANDLE inherit_list[2] = {down_read.h, up_write.h};

    SIZE_T attr_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 3, 0, &attr_list_size);
    std::vector<std::byte> attr_buf(attr_list_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 3, 0, &attr_list_size)) {
        return win32_error("InitializeProcThreadAttributeList", failure_class::fatal,
                            "native_jail.worker_attr_list_init_failed");
    }
    struct WorkerAttrListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~WorkerAttrListGuard() { DeleteProcThreadAttributeList(list); }
    } attr_guard{attr_list};

    DWORD child_process_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sec_cap,
                                    sizeof(sec_cap), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                    &child_process_policy, sizeof(child_process_policy), nullptr,
                                    nullptr) ||
        !UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list,
                                    sizeof(inherit_list), nullptr, nullptr)) {
        return win32_error("UpdateProcThreadAttribute", failure_class::fatal,
                            "native_jail.worker_attr_update_failed");
    }

    std::wstring cmdline = L"\"" + session.worker_exe_path + L"\" " +
                            std::to_wstring(reinterpret_cast<std::intptr_t>(down_read.h)) + L" " +
                            std::to_wstring(reinterpret_cast<std::intptr_t>(up_write.h));
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi{};
    std::wstring env_block = build_minimal_environment_block();
    BOOL created = CreateProcessW(
        nullptr, mutable_cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env_block.data(),
        nullptr, reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
    down_read.close_now();
    up_write.close_now();
    if (!created) {
        return win32_error("CreateProcessW", failure_class::fatal,
                            "native_jail.worker_create_process_failed");
    }

    auto assigned = instance->job.assign_process(pi.hProcess);
    if (!assigned.has_value()) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return std::unexpected(assigned.error());
    }

    ws.process = pi;
    ws.downstream_write = down_write.release();
    ws.upstream_read = up_read.release();
    ws.stop_event = CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE, nullptr);
    ws.phase.store(watchdog_phase::awaiting_init);
    ws.phase_deadline = std::chrono::steady_clock::now() + ws.session_config.init_timeout_ms;

    // Watchdog starts BEFORE ResumeThread (final spec §6, RT2 Finding 4) -- an init-phase hang has a
    // kill path from the guest's very first instruction, not just from exec_session()'s own deadline.
    Instance* inst_ptr = instance.get();
    ws.watchdog_thread = std::thread([this, inst_ptr] { session_watchdog_loop(*inst_ptr); });

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    ws.process.hThread = nullptr;

    FramedChannel channel(ws.upstream_read, ws.downstream_write);

    // I2: the worker never receives a ToolDescriptor (a live closure over host state) -- only the
    // RENDERED Python source text agent_tools_codegen.hpp produces from it, host-side, right here.
    std::string agent_tools_module_source;
    if (session.tool_bridge.has_value()) {
        auto rendered = generate_agent_tools_module_source(session.tool_bridge->bridged_tools.descriptors());
        if (!rendered.has_value()) {
            stop_watchdog(*instance);
            terminate_worker(*instance);
            close_worker_handles(*instance);
            return std::unexpected(rendered.error());
        }
        agent_tools_module_source = std::move(*rendered);
    }

    std::vector<json::Value> extra_sys_path_json;
    for (auto const& p : session.extra_sys_path) extra_sys_path_json.push_back(json::Value::make_string(narrow(p)));
    std::vector<json::Value> allowlist_json;
    for (auto const& n : session.package_policy_allowlist) allowlist_json.push_back(json::Value::make_string(n));
    std::vector<json::Value> gated_json;
    for (auto const& n : session.caller_gated_modules) gated_json.push_back(json::Value::make_string(n));

    json::Value init_req = json::Value::make_object({
        {"type", json::Value::make_string(wp::kInitRequest)},
        {"python_home", json::Value::make_string(narrow(session.python_home))},
        {"extra_sys_path", json::Value::make_array(std::move(extra_sys_path_json))},
        {"package_policy_allowlist", json::Value::make_array(std::move(allowlist_json))},
        {"caller_gated_modules", json::Value::make_array(std::move(gated_json))},
        {"expose_agent_files_data", json::Value::make_bool(session.expose_agent_files_data)},
        {"expose_agent_ask", json::Value::make_bool(session.expose_agent_ask)},
        {"output_cap_bytes", json::Value::make_number(static_cast<double>(session.output_cap_bytes))},
        {"agent_tools_module_source", json::Value::make_string(agent_tools_module_source)},
    });

    if (auto sent = channel.send(init_req); !sent.has_value()) {
        stop_watchdog(*instance);
        terminate_worker(*instance);
        close_worker_handles(*instance);
        return std::unexpected(sent.error());
    }

    auto init_resp = channel.recv();
    if (!init_resp.has_value()) {
        // Broken pipe: either the worker crashed on its own or the watchdog already killed it
        // (awaiting_init's own deadline, or a memory-limit notification during startup).
        ws.alive.store(false);
        job_kill_reason const reason = ws.kill_reason.load();
        stop_watchdog(*instance);
        close_worker_handles(*instance);
        return std::unexpected(ae::error{
            failure_class::fatal,
            reason == job_kill_reason::wall_clock_timeout
                ? "python worker did not respond to init_request within init_timeout_ms"
                : "python worker process died during initialization",
            "native_jail.worker_init_failed",
        });
    }
    if (!wp::get_bool(*init_resp, "ok")) {
        std::string const msg = wp::get_string(*init_resp, "error_message");
        stop_watchdog(*instance);
        terminate_worker(*instance);
        close_worker_handles(*instance);
        return std::unexpected(ae::error{failure_class::fatal, "python worker initialize() failed: " + msg,
                                          "native_jail.worker_init_rejected"});
    }

    ws.phase.store(watchdog_phase::idle);
    ws.idle_window_start_time = std::chrono::steady_clock::now();
    if (auto usage = instance->job.query_usage(); usage.has_value()) {
        ws.idle_window_start_cpu_100ns = usage->total_user_time_100ns;
    }

    static std::atomic<std::uint64_t> counter{0};
    std::string id = "native_jail_worker-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    instances_.emplace(id, std::move(instance));
    return SandboxHandle{id};
}

// ================================================================================================
// HandleRelay (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md) -- real
// host-side handling for the "open"/"listdir"/"connect_authorize"/"connect_send"/"connect_recv"/
// "connect_close" worker_query kinds dispatch_worker_query (below) routes to. `deny`/`parse_open_mode`
// (this anonymous namespace) are ordinary free helpers; the six `dispatch_*` functions further below
// are PRIVATE STATIC methods of `NativeJailBackend`, not free functions, purely because their
// signatures name `PythonWorkerState` -- a private nested type (native_jail_backend.hpp) -- so only an
// actual member (declared `static` there since none of them need `this`) has access.
// ================================================================================================

namespace {

using Fields = std::vector<std::pair<std::string, json::Value>>;

constexpr std::size_t kMaxRelayChunkBytes = 1024ull * 1024;  // design draft §4 item 4
// Red-team finding (independent review, this pass): `kMaxRelayChunkBytes` was only applied AFTER
// `relay_base64::decode`, so a single connect_send/file_write call could force decoding (and the
// allocation that implies) of up to the wire frame's own 64 MiB ceiling (jailed_worker_rpc.hpp's
// kMaxFrameBytes) before 63/64 of it was discarded -- the stated "the host's own ceiling applies
// regardless of what the guest sent" claim held for the RESULT, not the WORK done to produce it.
// Checked on the base64 TEXT length, before decode ever runs: base64 expands 3 bytes to 4 characters,
// so this is `ceil(kMaxRelayChunkBytes / 3) * 4`, the largest encoded length that could ever decode to
// a payload at or under the real limit.
constexpr std::size_t kMaxRelayBase64Chars = ((kMaxRelayChunkBytes + 2) / 3) * 4;

// TEST-ONLY seam, see NativeJailBackend::set_test_connect_resolver_override's own header comment for
// the full rationale -- nullptr (the default) means "use the real sandbox::resolve_and_validate".
std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>
    g_test_connect_resolver_override;

Fields deny(std::string const& error_code, std::string const& message, int native_code = 0) {
    Fields f = {
        {"ok", json::Value::make_bool(false)},
        {"error_code", json::Value::make_string(error_code)},
        {"message", json::Value::make_string(message)},
    };
    if (native_code != 0) f.emplace_back("native_code", json::Value::make_number(static_cast<double>(native_code)));
    return f;
}

// Design draft §1: file-open mode parsing, ported from the pre-worker-process design
// (git show a60fc3d^:.../mediated_python_runner.cpp's own parse_open_mode/ParsedMode) -- same
// six-entry table, plus an explicit `append` flag (the old code derived it worker-side from
// `creation_disposition == OPEN_ALWAYS`; this design sends it explicitly over the wire instead,
// since the worker no longer sees `creation_disposition` at all).
struct ParsedOpenMode {
    bool for_write = false;
    bool binary = false;
    bool append = false;
    DWORD desired_access = 0;
    DWORD creation_disposition = 0;
};

result<ParsedOpenMode> parse_open_mode(std::string const& mode) {
    ParsedOpenMode m;
    if (mode == "r") {
        m.desired_access = GENERIC_READ;
        m.creation_disposition = OPEN_EXISTING;
    } else if (mode == "rb") {
        m.binary = true;
        m.desired_access = GENERIC_READ;
        m.creation_disposition = OPEN_EXISTING;
    } else if (mode == "w") {
        m.for_write = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = CREATE_ALWAYS;
    } else if (mode == "wb") {
        m.for_write = true;
        m.binary = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = CREATE_ALWAYS;
    } else if (mode == "a") {
        m.for_write = true;
        m.append = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = OPEN_ALWAYS;
    } else if (mode == "ab") {
        m.for_write = true;
        m.binary = true;
        m.append = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = OPEN_ALWAYS;
    } else {
        return std::unexpected(ae::error{failure_class::policy,
                                          "unsupported open() mode '" + mode +
                                              "' (supported: r, rb, w, wb, a, ab)",
                                          "python.open_bad_mode"});
    }
    return m;
}

}  // namespace

// Design draft §1, REVISED during implementation from DuplicateHandle-based relay to a per-call RPC
// relay (the same shape §2 already uses for sockets): a real, reproduced red-team-worthy finding, not
// a design choice made up front -- `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` into the
// AppContainer'd worker process SUCCEEDS (the duplicate is even confirmed present and open via
// `GetHandleInformation` inside the worker), but real I/O against it (`io.FileIO` construction) fails
// with `ERROR_INVALID_HANDLE`, reproducibly, for an ordinary user-created file with no explicit
// AppContainer-SID ACE -- a real Windows AppContainer limitation on duplicated named-file-object
// handles, not a bug in the relay wiring (verified: the exact numeric handle value the host duplicated
// is confirmed, byte-for-byte, to be the one the worker receives and treats as "open" per
// `GetHandleInformation`, yet is unusable for actual I/O). This falsifies the original design's own
// claim ("a real HANDLE, once verified and duplicated with the exact granted access mask, is a
// strictly narrower and cheaper mechanism than reimplementing read/write/seek over JSON RPC") --
// consistent with ADR-004 §6 finding 1's own headline ("AppContainer's ACL model is not a sufficient
// filesystem boundary"), now shown to cut the OTHER direction too: not just "some files are readable
// that shouldn't be" but "a legitimately-duplicated handle to an ordinary file can be flatly unusable".
// The host now keeps the real, capability-checked, size-cap-checked `SafeFileHandle` open in its OWN
// process (`PythonWorkerState::open_files`, mirroring `live_sockets`' own id-keyed map) and answers
// every `Internal_open`/`_files_input`/etc. call with an opaque `file_id`; `dispatch_file_read`/
// `dispatch_file_write`/`dispatch_file_close` (below) relay the actual I/O, exactly like
// `dispatch_connect_send`/`dispatch_connect_recv`/`dispatch_connect_close` already do for sockets.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_open(PythonWorkerState& ws, EffectContext& ctx,
                                                                   json::Value const& payload) {
    std::string const mount_id = wp::get_string(payload, "mount_id");
    std::string const mount_relative = wp::get_string(payload, "mount_relative");
    std::string const mode_str = wp::get_string(payload, "mode", "r");

    auto mode = parse_open_mode(mode_str);
    if (!mode) return deny("python.open_bad_mode", mode.error().message);

    auto mount_it = ws.session_config.mount_roots.find(mount_id);
    if (mount_it == ws.session_config.mount_roots.end()) {
        return deny("tool.capability_not_held",
                     "no mount named '" + mount_id + "' is available in this session");
    }
    if (!ctx.capabilities) {
        return deny("tool.capability_not_held", "no capability context available for file access");
    }

    std::optional<cap::FsWrite> granted_write;
    std::optional<cap::FsRead> granted_read;
    if (mode->for_write) {
        granted_write = ctx.capabilities->find_fs_write(mount_id, mount_relative);
        if (!granted_write) {
            return deny("tool.capability_not_held",
                         "no capability grants write access to '/" + mount_id + "/" + mount_relative + "'");
        }
    } else {
        granted_read = ctx.capabilities->find_fs_read(mount_id, mount_relative);
        if (!granted_read) {
            return deny("tool.capability_not_held",
                         "no capability grants read access to '/" + mount_id + "/" + mount_relative + "'");
        }
    }

    if (granted_write &&
        (granted_write->quota_bytes.has_value() || granted_write->file_count_cap.has_value())) {
        auto usage = mount_root_usage(mount_it->second);
        if (!usage) return deny("python.open_os_error", usage.error().message, usage.error().native_code);
        bool const over_quota =
            granted_write->quota_bytes.has_value() && usage->total_bytes > *granted_write->quota_bytes;
        bool const over_count = granted_write->file_count_cap.has_value() &&
                                 usage->file_count > *granted_write->file_count_cap;
        if (over_quota || over_count) {
            return deny(wp::kErrorPythonOpenOsError, "No space left on device");
        }
    }

    auto handle =
        open_within_mount_root(mount_it->second, mount_relative, mode->desired_access, mode->creation_disposition);
    if (!handle) return deny(wp::kErrorPythonOpenOsError, handle.error().message, handle.error().native_code);

    if (granted_read && granted_read->size_cap_bytes.has_value()) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle->get(), &size)) {
            return deny(wp::kErrorPythonOpenOsError, "GetFileSizeEx failed",
                         static_cast<int>(GetLastError()));
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > *granted_read->size_cap_bytes) {
            return deny("tool.capability_not_held",
                         "open: the requested file exceeds this capability's size cap");
        }
    }

    if (mode->append) {
        // Append mode positions at end-of-file -- the pre-worker-process design did this via a
        // worker-side `_lseeki64` after `_open_osfhandle`; now the host owns the handle for the
        // handle's whole lifetime, so it does the equivalent seek itself, once, here.
        if (!SetFilePointerEx(handle->get(), LARGE_INTEGER{}, nullptr, FILE_END)) {
            return deny(wp::kErrorPythonOpenOsError, "SetFilePointerEx(FILE_END) failed",
                         static_cast<int>(GetLastError()));
        }
    }

    if (ws.open_files.size() >= NativeJailBackend::PythonWorkerState::kMaxLiveSockets) {
        // Red-team correction (independent review, this pass): NOT a shared budget with
        // `live_sockets` -- each map is checked against its OWN size, so a session can hold
        // kMaxLiveSockets files AND kMaxLiveSockets sockets concurrently (two independent 16-entry
        // ceilings, not one 16-entry ceiling split between them). The constant NAME is reused
        // (design draft §4 item 5's own cardinality-cap reasoning applies identically to both
        // resource kinds), not the counter itself.
        return deny(wp::kErrorNetTooManySockets, "too many open relayed files for this session");
    }
    std::uint64_t const file_id = ws.next_socket_id++;
    ws.open_files.emplace(file_id, std::move(*handle));

    return {
        {"ok", json::Value::make_bool(true)},
        {"file_id", json::Value::make_number(static_cast<double>(file_id))},
        {"for_write", json::Value::make_bool(mode->for_write)},
        {"binary", json::Value::make_bool(mode->binary)},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_read(PythonWorkerState& ws,
                                                                        json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    auto it = ws.open_files.find(file_id);
    if (it == ws.open_files.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "file_id " + std::to_string(file_id) + " is not a live relayed file");
    }
    std::size_t const want = std::min(
        static_cast<std::size_t>(std::max(0.0, wp::get_number(payload, "max_bytes"))), kMaxRelayChunkBytes);
    std::vector<std::byte> buf(want == 0 ? 1 : want);
    DWORD read_bytes = 0;
    if (!ReadFile(it->second.get(), buf.data(), static_cast<DWORD>(buf.size()), &read_bytes, nullptr)) {
        DWORD const err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            return {{"ok", json::Value::make_bool(true)}, {"data_base64", json::Value::make_string("")}};
        }
        return deny(wp::kErrorPythonOpenOsError, "ReadFile failed", static_cast<int>(err));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"data_base64", json::Value::make_string(relay_base64::encode(buf.data(), read_bytes))},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_write(PythonWorkerState& ws,
                                                                         json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    auto it = ws.open_files.find(file_id);
    if (it == ws.open_files.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "file_id " + std::to_string(file_id) + " is not a live relayed file");
    }
    std::string const data_b64 = wp::get_string(payload, "data_base64");
    if (data_b64.size() > kMaxRelayBase64Chars) {
        return deny(wp::kErrorNetSocketClosed, "file_write payload exceeds the per-call relay ceiling");
    }
    auto decoded = relay_base64::decode(data_b64);
    if (!decoded) return deny(wp::kErrorNetSocketClosed, "malformed base64 in file_write payload");

    std::size_t const n = std::min(decoded->size(), kMaxRelayChunkBytes);
    DWORD written = 0;
    if (n > 0 && !WriteFile(it->second.get(), decoded->data(), static_cast<DWORD>(n), &written, nullptr)) {
        return deny(wp::kErrorPythonOpenOsError, "WriteFile failed", static_cast<int>(GetLastError()));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"written", json::Value::make_number(static_cast<double>(written))},
    };
}

// Idempotent success, same reasoning as dispatch_connect_close (design draft §2 item 4).
NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_close(PythonWorkerState& ws,
                                                                         json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    ws.open_files.erase(file_id);  // SafeFileHandle's destructor closes the real HANDLE
    return {{"ok", json::Value::make_bool(true)}};
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_listdir(PythonWorkerState& ws, EffectContext& ctx,
                                                                      json::Value const& payload) {
    std::string const mount_id = wp::get_string(payload, "mount_id");
    std::string const mount_relative = wp::get_string(payload, "mount_relative");

    auto mount_it = ws.session_config.mount_roots.find(mount_id);
    if (mount_it == ws.session_config.mount_roots.end()) {
        return deny("tool.capability_not_held",
                     "no mount named '" + mount_id + "' is available in this session");
    }
    if (!ctx.capabilities || !ctx.capabilities->find_fs_read(mount_id, mount_relative)) {
        return deny("tool.capability_not_held",
                     "no capability grants read access to '/" + mount_id + "/" + mount_relative + "'");
    }

    auto entries = list_within_mount_root(mount_it->second, mount_relative);
    if (!entries) return deny(wp::kErrorPythonOpenOsError, entries.error().message, entries.error().native_code);

    std::vector<json::Value> items;
    items.reserve(entries->size());
    for (auto const& entry : *entries) {
        items.push_back(json::Value::make_object({
            {"name", json::Value::make_string(entry.name)},
            {"is_dir", json::Value::make_bool(entry.is_directory)},
            {"size", json::Value::make_number(static_cast<double>(entry.size_bytes))},
        }));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"entries_json", json::Value::make_string(json::dump(json::Value::make_array(std::move(items))))},
    };
}

// Design draft §2 item 1: capability-checked, resolve_and_validate-checked (NEW hardening beyond the
// old in-process design, named explicitly in the draft), then a REAL connect performed host-side
// (the worker cannot -- ADR-004 AC-S1) with its own bounded wait, never an unbounded one (this
// function must always return promptly; there is no outer mechanism that can unstick a host thread
// blocked on a live TCP handshake the way the watchdog unsticks a stuck WORKER process).
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_authorize(PythonWorkerState& ws,
                                                                                EffectContext& ctx,
                                                                                json::Value const& payload) {
    std::string const host = wp::get_string(payload, "host");
    // Red-team finding (independent review, this pass): the old, unpatched socket.connect() validated
    // the port was 0-65535 (OverflowError) before ever reaching C; _ae_connect's relay skips that,
    // passing any Python int straight through -- a double outside uint16_t's range converted via
    // static_cast is undefined behavior per the standard, not merely truncation, and was trivially
    // reachable via an ordinary guest typo (`s.connect((host, 99999))`), not just an attacker. Range-
    // checked explicitly here, before any cast, and denied the same ordinary way an unheld capability
    // already is.
    double const port_raw = wp::get_number(payload, "port");
    if (port_raw < 0.0 || port_raw > 65535.0) {
        return deny(wp::kErrorNetAddressBlocked, "port out of range (0-65535)");
    }
    auto const port = static_cast<std::uint16_t>(port_raw);
    std::string const entry = host + ":" + std::to_string(port) + ":tcp";

    if (!ctx.capabilities || !ctx.capabilities->contains(Capability{cap::NetOut{{entry}, std::nullopt, {}}})) {
        return deny(wp::kErrorNetAddressBlocked, "no capability grants network access to '" + entry + "'");
    }
    if (ws.live_sockets.size() >= NativeJailBackend::PythonWorkerState::kMaxLiveSockets) {
        return deny(wp::kErrorNetTooManySockets, "too many open relayed sockets for this session");
    }

    auto endpoint = g_test_connect_resolver_override ? g_test_connect_resolver_override(host, port)
                                                       : agentengine::sandbox::resolve_and_validate(host, port);
    if (!endpoint) return deny(wp::kErrorNetHostUnresolvable, endpoint.error().message);

    auto connected = agentengine::pal::tcp_connect(endpoint->ipv4_host_order, endpoint->port);
    if (!connected) return deny(wp::kErrorNetHostUnresolvable, connected.error().message());
    agentengine::pal::fd_t const fd = *connected;

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(fd, &write_set);
    fd_set err_set = write_set;
    timeval tv{};
    tv.tv_sec = 5;  // kConnectTimeoutMs (design draft §2 item 1) -- generous but always finite
    int const sel = select(0, nullptr, &write_set, &err_set, &tv);
    bool const connect_ok =
        sel > 0 && FD_ISSET(fd, &write_set) && !FD_ISSET(fd, &err_set) && agentengine::pal::connect_result(fd).has_value();
    if (!connect_ok) {
        agentengine::pal::close_fd(fd);
        return deny(wp::kErrorNetHostUnresolvable, "connect to '" + entry + "' did not complete");
    }

    std::uint64_t const socket_id = ws.next_socket_id++;
    ws.live_sockets.emplace(socket_id, fd);
    return {
        {"ok", json::Value::make_bool(true)},
        {"socket_id", json::Value::make_number(static_cast<double>(socket_id))},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_send(PythonWorkerState& ws,
                                                                           json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it == ws.live_sockets.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "socket_id " + std::to_string(socket_id) + " is not a live relayed socket");
    }
    std::string const data_b64 = wp::get_string(payload, "data_base64");
    if (data_b64.size() > kMaxRelayBase64Chars) {
        return deny(wp::kErrorNetSocketClosed, "connect_send payload exceeds the per-call relay ceiling");
    }
    auto decoded = relay_base64::decode(data_b64);
    if (!decoded) return deny(wp::kErrorNetSocketClosed, "malformed base64 in connect_send payload");

    std::size_t const n = std::min(decoded->size(), kMaxRelayChunkBytes);
    std::size_t total_sent = 0;
    while (total_sent < n) {
        auto sent = agentengine::pal::send_some(it->second, decoded->data() + total_sent, n - total_sent);
        if (!sent) {
            if (sent.error() == agentengine::pal::would_block()) break;  // kernel send buffer full --
                                                                            // report the short write so
                                                                            // far, matching Python's own
                                                                            // socket.send() contract
            agentengine::pal::close_fd(it->second);
            ws.live_sockets.erase(it);
            return deny(wp::kErrorNetSocketClosed, "send failed: " + sent.error().message());
        }
        if (*sent == 0) break;
        total_sent += *sent;
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"sent", json::Value::make_number(static_cast<double>(total_sent))},
    };
}

// Design draft §2 item 3, REVISED during implementation from a blocking-with-inner-timeout design to
// a single non-blocking attempt: a host-thread wait here has NO outer rescue mechanism (unlike a
// stuck WORKER process, which the watchdog can kill) -- and worse, a fixed inner poll deadline that
// gives up would be indistinguishable, on the wire, from a real orderly peer close (both are "empty
// data"), which would falsely signal EOF to guest code still waiting on a slow-but-live peer. The
// `would_block` field makes the three outcomes (real data / real EOF / try again) wire-distinguishable;
// the worker's own `_ae_recv` wrapper (python_worker_mediation.cpp bootstrap source) loops on
// `would_block`, so the actual "keep waiting" behavior lives in many short, host-thread-safe round
// trips instead of one host-side blocking wait -- exec_session()'s outer wall_ms watchdog is then the
// ONLY timeout for "guest is waiting on data that will never come," the same bound a CPU busy-loop
// already relies on.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_recv(PythonWorkerState& ws,
                                                                           json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it == ws.live_sockets.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "socket_id " + std::to_string(socket_id) + " is not a live relayed socket");
    }
    std::size_t const bufsize = std::min(
        static_cast<std::size_t>(std::max(0.0, wp::get_number(payload, "bufsize"))), kMaxRelayChunkBytes);
    std::vector<std::byte> buf(bufsize == 0 ? 1 : bufsize);

    auto received = agentengine::pal::recv_some(it->second, buf.data(), buf.size());
    if (received) {
        return {
            {"ok", json::Value::make_bool(true)},
            {"data_base64", json::Value::make_string(relay_base64::encode(buf.data(), *received))},
            {"would_block", json::Value::make_bool(false)},
        };
    }
    if (received.error() == agentengine::pal::would_block()) {
        return {
            {"ok", json::Value::make_bool(true)},
            {"data_base64", json::Value::make_string("")},
            {"would_block", json::Value::make_bool(true)},
        };
    }
    agentengine::pal::close_fd(it->second);
    ws.live_sockets.erase(it);
    return deny(wp::kErrorNetSocketClosed, "recv failed: " + received.error().message());
}

// Idempotent success by design (design draft §2 item 4) -- closing an already-closed/foreign id is a
// no-op, matching Python socket.close()'s own idempotent contract.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_close(PythonWorkerState& ws,
                                                                            json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it != ws.live_sockets.end()) {
        agentengine::pal::close_fd(it->second);
        ws.live_sockets.erase(it);
    }
    return {{"ok", json::Value::make_bool(true)}};
}

void NativeJailBackend::dispatch_worker_query(Instance& inst, json::Value const& query_frame,
                                               EffectContext& ctx) {
    PythonWorkerState& ws = *inst.worker;
    double const call_id = wp::get_number(query_frame, "call_id");
    double const exec_seq = wp::get_number(query_frame, "exec_seq");
    std::string const kind = wp::get_string(query_frame, "kind");
    json::Value const* payload = query_frame.find("payload");

    // Built as a plain field list, not a json::Value, so the envelope fields (type/call_id/exec_seq)
    // below can be prepended without a second, mutate-after-construct pass -- core/json_value.hpp's
    // `Value` is immutable once built (matching this codebase's json value type), so "build once with
    // everything already in it" is the natural shape here, not "build then merge."
    std::vector<std::pair<std::string, json::Value>> fields;
    if (kind == wp::kQueryCallTool && ws.session_config.tool_bridge.has_value() && payload != nullptr) {
        std::string const tool_name = wp::get_string(*payload, "tool_name");
        std::string const args_json = wp::get_string(*payload, "args_json", "{}");
        auto parsed_args = json::parse(args_json);
        if (!parsed_args.has_value()) {
            fields = {
                {"ok", json::Value::make_bool(false)},
                {"error_code", json::Value::make_string("tool.malformed_arguments")},
                {"message", json::Value::make_string("malformed JSON arguments: " + parsed_args.error().message)},
            };
        } else {
            ToolCallRequest request{"pycall-" + std::to_string(++ws.next_call_id), tool_name,
                                     *parsed_args, /*arguments_tainted=*/false};
            ToolInvocationAudit audit;
            ToolResult result =
                bridge_tool_call(*ws.session_config.tool_bridge, request, ctx, &audit);
            if (result.is_error) {
                std::string message = result.content.empty()
                                           ? "tool call failed"
                                           : std::get<Error>(result.content[0].value).message;
                fields = {
                    {"ok", json::Value::make_bool(false)},
                    {"error_code", json::Value::make_string(audit.error_code)},
                    {"message", json::Value::make_string(message)},
                };
            } else {
                std::string reply_json =
                    result.content.empty() ? "null" : std::get<Data>(result.content[0].value).json;
                fields = {
                    {"ok", json::Value::make_bool(true)},
                    {"reply_json", json::Value::make_string(reply_json)},
                };
            }
        }
    } else if (kind == wp::kQueryCallTool) {
        fields = {
            {"ok", json::Value::make_bool(false)},
            {"error_code", json::Value::make_string(wp::kErrorNotImplementedThisSlice)},
            {"message", json::Value::make_string("no tools are bridged for this session")},
        };
    } else if (kind == wp::kQueryOpen && payload != nullptr) {
        fields = dispatch_open(ws, ctx, *payload);
    } else if (kind == wp::kQueryListdir && payload != nullptr) {
        fields = dispatch_listdir(ws, ctx, *payload);
    } else if (kind == wp::kQueryFileRead && payload != nullptr) {
        fields = dispatch_file_read(ws, *payload);
    } else if (kind == wp::kQueryFileWrite && payload != nullptr) {
        fields = dispatch_file_write(ws, *payload);
    } else if (kind == wp::kQueryFileClose && payload != nullptr) {
        fields = dispatch_file_close(ws, *payload);
    } else if (kind == wp::kQueryConnectAuthorize && payload != nullptr) {
        fields = dispatch_connect_authorize(ws, ctx, *payload);
    } else if (kind == wp::kQueryConnectSend && payload != nullptr) {
        fields = dispatch_connect_send(ws, *payload);
    } else if (kind == wp::kQueryConnectRecv && payload != nullptr) {
        fields = dispatch_connect_recv(ws, *payload);
    } else if (kind == wp::kQueryConnectClose && payload != nullptr) {
        fields = dispatch_connect_close(ws, *payload);
    } else {
        // An unknown kind, or a known HandleRelay kind with no payload object at all -- a malformed
        // frame, never dispatched (RT1's own "never trust an unparseable frame" posture).
        fields = {
            {"ok", json::Value::make_bool(false)},
            {"error_code", json::Value::make_string(wp::kErrorNotImplementedThisSlice)},
            {"message", json::Value::make_string("unknown or malformed worker_query kind '" + kind + "'")},
        };
    }

    std::vector<std::pair<std::string, json::Value>> envelope = {
        {"type", json::Value::make_string(wp::kWorkerQueryResponse)},
        {"call_id", json::Value::make_number(call_id)},
        {"exec_seq", json::Value::make_number(exec_seq)},
    };
    envelope.insert(envelope.end(), std::make_move_iterator(fields.begin()),
                     std::make_move_iterator(fields.end()));
    json::Value response = json::Value::make_object(std::move(envelope));

    FramedChannel channel(ws.upstream_read, ws.downstream_write);
    (void)channel.send(response);
}

result<ExecOutcome> NativeJailBackend::exec_session(SandboxHandle const& handle, ExecRequest request,
                                                       ExecState& state, EffectContext& ctx) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end() || !it->second->worker.has_value()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "exec_session() called on an unknown SandboxHandle or one "
                                          "created via create() rather than create_python_worker()",
                                          "native_jail.unknown_worker_handle"});
    }
    Instance& inst = *it->second;
    PythonWorkerState& ws = *inst.worker;

    if (!ws.alive.load()) {
        return std::unexpected(ae::error{failure_class::resource,
                                          "this session's python worker has already been terminated "
                                          "(watchdog kill or a prior protocol violation) -- no silent "
                                          "respawn",
                                          "native_jail.session_terminated"});
    }

    std::unique_lock<std::mutex> lock(ws.call_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "exec_session() is already in progress on this handle "
                                          "(single-flight per session)",
                                          "native_jail.already_in_progress"});
    }

    std::uint64_t const seq = ws.next_exec_seq++;
    ws.active_exec_seq.store(seq);
    ws.phase_deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(static_cast<std::int64_t>(inst.limits.wall_ms));
    ws.phase.store(watchdog_phase::call_active);

    FramedChannel channel(ws.upstream_read, ws.downstream_write);

    std::vector<json::Value> preseeded_json;
    for (auto const& a : request.preseeded_answers) preseeded_json.push_back(json::Value::make_string(a));
    json::Value exec_req = json::Value::make_object({
        {"type", json::Value::make_string(wp::kExecRequest)},
        {"exec_seq", json::Value::make_number(static_cast<double>(seq))},
        {"source", json::Value::make_string(request.source)},
        {"preseeded_answers", json::Value::make_array(std::move(preseeded_json))},
        {"cwd", json::Value::make_string(state.cwd)},
        {"env", wp::make_string_map(state.env)},
    });

    ExecOutcome outcome;
    bool got_final = false;
    if (auto sent = channel.send(exec_req); !sent.has_value()) {
        ws.alive.store(false);
        outcome.klass = exec_outcome_class::crash;
        outcome.stderr_text = "internal error: could not send exec_request to the python worker: " +
                               sent.error().message;
        got_final = true;
    }

    while (!got_final) {
        auto frame = channel.recv();
        if (!frame.has_value()) {
            // Broken pipe -- the worker died. Classify from the watchdog's own observed kill_reason
            // (a real, external signal), never from anything the worker itself claimed (I3).
            ws.alive.store(false);
            job_kill_reason const reason = ws.kill_reason.load();
            outcome = ExecOutcome{};
            outcome.klass = reason == job_kill_reason::wall_clock_timeout ? exec_outcome_class::timeout
                             : reason == job_kill_reason::memory_limit    ? exec_outcome_class::oom
                             : reason == job_kill_reason::process_limit
                                 ? exec_outcome_class::policy_violation  // idle-phase CPU-budget kill
                                 : exec_outcome_class::crash;
            got_final = true;
            break;
        }
        std::string const type = wp::get_string(*frame, "type");
        if (type == wp::kWorkerQuery) {
            if (wp::get_exec_seq(*frame) != seq) {
                // RT1 Finding 1 -- a worker_query claiming a different exec_seq than the call
                // actually in flight: replayed, queued, or otherwise misattributed. Fail closed: the
                // query is NEVER dispatched, the worker is terminated outright, never trusted to
                // self-correct (final spec §8a).
                terminate_worker(inst);
                ws.alive.store(false);
                outcome = ExecOutcome{};
                outcome.klass = exec_outcome_class::policy_violation;
                outcome.stderr_text = "protocol violation: worker_query exec_seq mismatch "
                                       "(expected " +
                                       std::to_string(seq) + ")";
                got_final = true;
                break;
            }
            dispatch_worker_query(inst, *frame, ctx);
            continue;
        }
        if (type == wp::kExecResponse && wp::get_exec_seq(*frame) == seq) {
            outcome.klass = wp::get_string(*frame, "klass") == "ask_pending" ? exec_outcome_class::ask_pending
                                                                              : exec_outcome_class::ok;
            outcome.stdout_text = wp::get_string(*frame, "stdout_text");
            outcome.stderr_text = wp::get_string(*frame, "stderr_text");
            outcome.result_repr = wp::get_string(*frame, "result_repr");
            outcome.ask_prompt = wp::get_string(*frame, "ask_prompt");
            state.cwd = wp::get_string(*frame, "cwd");
            state.env = wp::get_string_map(*frame, "env");
            got_final = true;
            break;
        }
        // Any other type, or an exec_response with a mismatched exec_seq -- same protocol-violation
        // path as the worker_query mismatch above (final spec §8a: uniform response).
        terminate_worker(inst);
        ws.alive.store(false);
        outcome = ExecOutcome{};
        outcome.klass = exec_outcome_class::policy_violation;
        outcome.stderr_text = "protocol violation: unexpected frame type '" + type + "' while awaiting "
                               "exec_response for exec_seq " + std::to_string(seq);
        got_final = true;
    }

    ws.active_exec_seq.store(0);
    if (ws.alive.load()) {
        ws.phase.store(watchdog_phase::idle);
        ws.idle_window_start_time = std::chrono::steady_clock::now();
        if (auto usage = inst.job.query_usage(); usage.has_value()) {
            ws.idle_window_start_cpu_100ns = usage->total_user_time_100ns;
        }
    }
    return outcome;
}

result<void> NativeJailBackend::refresh_python_tools(SandboxHandle const& handle, ToolBridgeConfig config) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end() || !it->second->worker.has_value()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "refresh_python_tools() called on an unknown or non-worker "
                                          "SandboxHandle",
                                          "native_jail.unknown_worker_handle"});
    }
    Instance& inst = *it->second;
    PythonWorkerState& ws = *inst.worker;
    if (!ws.alive.load()) {
        return std::unexpected(ae::error{failure_class::resource,
                                          "this session's python worker has already been terminated",
                                          "native_jail.session_terminated"});
    }

    auto rendered = generate_agent_tools_module_source(config.bridged_tools.descriptors());
    if (!rendered.has_value()) return std::unexpected(rendered.error());

    ws.session_config.tool_bridge = std::move(config);  // consulted by dispatch_worker_query from now on

    FramedChannel channel(ws.upstream_read, ws.downstream_write);
    json::Value req = json::Value::make_object({
        {"type", json::Value::make_string(wp::kRefreshToolsRequest)},
        {"module_source", json::Value::make_string(*rendered)},
    });
    if (auto sent = channel.send(req); !sent.has_value()) {
        ws.alive.store(false);
        return std::unexpected(sent.error());
    }
    auto resp = channel.recv();
    if (!resp.has_value()) {
        ws.alive.store(false);
        return std::unexpected(ae::error{failure_class::fatal,
                                          "python worker died during refresh_agent_tools",
                                          "native_jail.worker_refresh_failed"});
    }
    if (!wp::get_bool(*resp, "ok")) {
        return std::unexpected(ae::error{failure_class::fatal,
                                          "python worker refresh_agent_tools failed: " +
                                              wp::get_string(*resp, "error_message"),
                                          "native_jail.worker_refresh_rejected"});
    }
    return {};
}

void NativeJailBackend::set_test_connect_resolver_override(
    std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)> fn) {
    g_test_connect_resolver_override = std::move(fn);
}

}  // namespace agentengine::native_jail
