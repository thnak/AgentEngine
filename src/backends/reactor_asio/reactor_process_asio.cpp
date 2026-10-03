// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 "Processes" (and "Handle
// hygiene becomes mandatory", red-team round 1 G4), §4.4 "Process cancellation" and §2 H-e -- the standalone-
// Asio implementation of pal/reactor_process.hpp. It follows asio_reactor.hpp's four-step recipe for every
// operation: post the start; on the reactor thread complete `canceled` at once if a cancel is already
// recorded or the reactor is shutting down (sticky cancel, round-2 M2); store a BackendOpState whose cancel()
// cancels that Asio operation and register the op as pending; in the completion handler deregister, reset the
// state and report the BACKEND's outcome (round-1 M1).
//
// Platform layer (this is a pal backend, so OS specifics live here):
//   Windows  CreateProcessW + STARTUPINFOEXW with PROC_THREAD_ATTRIBUTE_JOB_LIST (the child is in a
//            KILL_ON_JOB_CLOSE Job from creation, no create-then-assign window) and
//            PROC_THREAD_ATTRIBUTE_HANDLE_LIST naming exactly the child's three standard handles (G4: a
//            concurrent sibling, or any other inheritable handle in the host, is never inherited).
//            Parent pipe ends are overlapped named pipes (anonymous pipes cannot be overlapped) driven by
//            asio::windows::stream_handle; the child's ends are opened WITHOUT FILE_FLAG_OVERLAPPED, since a
//            child doing synchronous I/O on an overlapped handle is undefined. Exit: asio::windows::
//            object_handle on the process handle -- Job completion-port messages are not guaranteed to be
//            delivered (research 2026-10-03 §3), so they are not used. Kill: TerminateJobObject.
//   Linux    pipe2(O_CLOEXEC) for every pipe, and posix_spawn file actions that additionally close every
//            descriptor >= 3 in the child (closefrom), so a descriptor the HOST leaked without O_CLOEXEC is
//            not inherited either. posix_spawn (vfork-style; no allocation between the fork and the exec) puts
//            the child in its own process group (POSIX_SPAWN_SETPGROUP), resets its signal mask (the reactor
//            thread blocks SIGPIPE, below) and SIGPIPE's disposition. With glibc >= 2.39 the child is created
//            by pidfd_spawn (clone3 CLONE_PIDFD: the pidfd exists before the child can exit, no pid-reuse
//            window); otherwise (or with AE_PROCESS_FORCE_PIDFD_OPEN, which the test build uses to exercise
//            it) posix_spawn + pidfd_open. No pidfd (kernel < 5.3) is REPORTED as
//            `process_error::unsupported` (the child is killed and reaped), never degraded to a racy pid wait.
//            Exit: the pidfd's readability on the reactor (asio::posix::stream_descriptor), then
//            waitid(P_PIDFD, WEXITED | WNOWAIT): the child stays a zombie until the Process is closed, so its
//            pid -- and therefore its process-group id -- cannot be reused while kill(-pgid) may still target
//            it. Close kills the group and reaps (asynchronously; bounded and blocking only at reactor
//            shutdown). Residual: a host that sets SIGCHLD to SIG_IGN has its children auto-reaped, so the
//            status reads `known = false` (ECHILD) -- reported, not invented.
//            SIGPIPE: pipe writes use write(2), which raises SIGPIPE on a closed reader; the reactor thread
//            blocks SIGPIPE before its first pipe write (pthread_sigmask, thread-directed so it stays pending
//            on that thread) and consumes the pending one after EPIPE (sigtimedwait), so EPIPE is a value.
//
// Lifetimes: the Asio objects of one child (IoObjects) are shared by the process state and every in-flight
// handler, and are destroyed on the reactor thread when the last of them lets go -- never from the caller's
// thread, never after the io_context. Each Process registers one internal "life" op as pending from its
// creation, so reactor shutdown reaches it: the sweep kills the child, closes its pipes (pending reads and
// waits complete `canceled`) and marks it torn down, after which destroying the Process posts nothing.

#include "asio_reactor.hpp"

#include "agentengine/pal/reactor_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>

#ifdef _WIN32
#include <asio/windows/object_handle.hpp>
#include <asio/windows/stream_handle.hpp>
#include <windows.h>

#include <cwchar>
#include <random>
#else
#include <asio/posix/stream_descriptor.hpp>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 39))
// glibc's <sys/pidfd.h> (2.43 checked) declares pidfd_getpid without C linkage for a C++ includer.
extern "C" {
#include <sys/pidfd.h>
}
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere in a header
#ifndef P_PIDFD
#define P_PIDFD 3  // NOLINT: Linux 5.4 uapi value; older glibc headers lack it
#endif
#endif

namespace agentengine::pal::asio_backend {
namespace {

#ifdef _WIN32
using PipeHandle = asio::windows::stream_handle;
using ExitHandle = asio::windows::object_handle;
// The exit code TerminateJobObject gives every process of a killed job: 128 + SIGKILL, the conventional
// "killed" status, so a caller reading only `code` still sees a non-zero, recognisable value.
constexpr DWORD kKilledExitCode = 137;
#else
using PipeHandle = asio::posix::stream_descriptor;
using ExitHandle = asio::posix::stream_descriptor;  // the pidfd
#endif

constexpr std::size_t kDrainChunk = 64 * 1024;

// The Asio objects of one child. Destroyed on the reactor thread by the last holder: the process state (reset
// at close) or an in-flight handler.
struct IoObjects {
    explicit IoObjects(asio::io_context& io) : in(io), out(io), err(io), exit_wait(io) {}
    PipeHandle in;
    PipeHandle out;
    PipeHandle err;
    ExitHandle exit_wait;
    PipeHandle& stream(process_stream w) noexcept { return w == process_stream::out ? out : err; }
};

// Reactor-thread state of one Process (every field except `torn_down`, which ~AsioProcess reads).
struct ProcState {
    explicit ProcState(AsioReactor& r) noexcept : reactor(&r) {}
    ~ProcState();
    ProcState(ProcState const&)            = delete;
    ProcState& operator=(ProcState const&) = delete;

    AsioReactor*               reactor;
    std::shared_ptr<IoObjects> io;    // null until spawned, and again once closed
    std::shared_ptr<ReactorOp> life;  // pending from creation until the close finished (shutdown reaches it)
    bool life_done     = false;
    bool spawn_started = false;
    bool spawned       = false;
    bool closed        = false;
    bool captured[2]   = {false, false};  // out, err
    bool open[2]       = {false, false};
    bool stdin_pipe    = false;
    bool stdin_open    = false;
    bool reading[2]    = {false, false};
    bool writing       = false;
    bool waiting       = false;
    bool exit_observed = false;
    bool kill_sent     = false;
    ProcessExit exit;
    std::atomic<bool> torn_down{false};  // closed by reactor shutdown: ~AsioProcess must not post any more
#ifdef _WIN32
    HANDLE job     = nullptr;
    HANDLE process = nullptr;
#else
    pid_t pid     = -1;
    bool  reaped  = false;
    bool  reaping = false;
#endif
};

ProcState::~ProcState() {
#ifdef _WIN32
    // Normally released by close(); this only runs for a state whose close never reached the reactor.
    if (process != nullptr) CloseHandle(process);
    if (job != nullptr) CloseHandle(job);  // KILL_ON_JOB_CLOSE: the child cannot outlive this
#endif
}

std::size_t index_of(process_stream w) noexcept { return w == process_stream::out ? 0 : 1; }

// What every pipe operation stores in op->backend_state. `cancel_requested` makes a cancel sticky across the
// re-arms of a multi-step loop (drain, write): a cancel that lands while a step's completion is already
// queued cancels nothing in Asio, so the loop checks the flag before re-arming.
struct StreamOpState final : BackendOpState {
    StreamOpState(std::shared_ptr<IoObjects> i, PipeHandle* h) noexcept : io(std::move(i)), handle(h) {}
    void cancel() noexcept override {
        cancel_requested = true;
        std::error_code ec;
        handle->cancel(ec);
    }
    std::shared_ptr<IoObjects> io;
    PipeHandle*                handle;
    bool                       cancel_requested = false;
    std::vector<char>          scratch;  // drain only: the read buffer past the cap (op-owned via backend_state)
};

struct ExitOpState final : BackendOpState {
    explicit ExitOpState(std::shared_ptr<IoObjects> i) noexcept : io(std::move(i)) {}
    void cancel() noexcept override {
        std::error_code ec;
        io->exit_wait.cancel(ec);
    }
    std::shared_ptr<IoObjects> io;
};

class LifeOp final : public ReactorOp {
public:
    void on_complete(op_status /*status*/) noexcept override {}
};

void close_process(std::shared_ptr<ProcState> const& st, bool for_shutdown);

struct LifeState final : BackendOpState {
    explicit LifeState(std::weak_ptr<ProcState> s) noexcept : st(std::move(s)) {}
    // Reached only from the reactor's shutdown sweep (nobody else holds the life op).
    void cancel() noexcept override {
        if (auto s = st.lock()) {
            s->torn_down.store(true, std::memory_order_release);
            close_process(s, /*for_shutdown=*/true);
        }
    }
    std::weak_ptr<ProcState> st;
};

// Reactor thread. The life op is deregistered from a posted handler, because close_process can run inside
// the reactor's shutdown sweep, which is iterating the pending set.
void finish_life(std::shared_ptr<ProcState> const& st) {
    if (st->life_done || !st->life) return;
    st->life_done = true;
    AsioReactor* r    = st->reactor;
    auto         life = st->life;
    asio::post(r->io(), [r, life] {
        r->remove_pending(life);
        life->backend_state.reset();
        life->on_complete(op_status::completed);
    });
}

bool is_eof(std::error_code const& ec) noexcept {
    return ec == asio::error::eof || ec == asio::error::broken_pipe
#ifdef _WIN32
           || ec.value() == ERROR_BROKEN_PIPE || ec.value() == ERROR_HANDLE_EOF || ec.value() == ERROR_NO_DATA
#endif
        ;
}

bool is_broken_pipe(std::error_code const& ec) noexcept {
    return ec == asio::error::broken_pipe
#ifdef _WIN32
           || ec.value() == ERROR_BROKEN_PIPE || ec.value() == ERROR_NO_DATA
#else
           || ec.value() == EPIPE
#endif
        ;
}

// ---------------------------------------------------------------------------------------------------------
// Platform: kill, exit status, close, spawn
// ---------------------------------------------------------------------------------------------------------

#ifdef _WIN32

void kill_scope(ProcState& st) noexcept {
    if (!st.spawned || st.job == nullptr) return;
    if (!st.exit_observed) st.kill_sent = true;
    TerminateJobObject(st.job, kKilledExitCode);
}

void observe_exit(ProcState& st) noexcept {
    DWORD code = 0;
    if (GetExitCodeProcess(st.process, &code) != 0 && code != STILL_ACTIVE) {
        st.exit.known = true;
        st.exit.code  = static_cast<std::int64_t>(code);
    }
    st.exit.killed   = st.kill_sent && code == kKilledExitCode;
    st.exit_observed = true;
}

void close_process(std::shared_ptr<ProcState> const& st, bool /*for_shutdown*/) {
    if (!st->closed) {
        st->closed = true;
        kill_scope(*st);
    }
    if (st->io) {
        std::error_code ec;
        st->io->in.close(ec);
        st->io->out.close(ec);
        st->io->err.close(ec);
        st->io->exit_wait.close(ec);
        st->io.reset();
    }
    st->stdin_open = st->open[0] = st->open[1] = false;
    if (st->process != nullptr) {
        CloseHandle(st->process);
        st->process = nullptr;
    }
    if (st->job != nullptr) {
        CloseHandle(st->job);
        st->job = nullptr;
    }
    finish_life(st);
}

struct OwnedHandle {
    HANDLE h = nullptr;
    OwnedHandle() noexcept = default;
    OwnedHandle(OwnedHandle const&)            = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;
    ~OwnedHandle() { reset(); }
    void reset() noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    HANDLE release() noexcept { return std::exchange(h, nullptr); }
};

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int const n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// The Microsoft C runtime's argv-quoting rule, per argument (a deliberate per-backend duplicate of
// native_process_spawn.cpp's detail::quote_one_argument, which tests vectors; this file cannot reach it
// without depending on that backend).
std::wstring quote_argument(std::wstring const& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t  backslashes = 0;
    for (wchar_t const c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// CREATE_UNICODE_ENVIRONMENT block, sorted case-insensitively by name as CreateProcess documents.
std::wstring environment_block(std::vector<std::string> const& env) {
    std::vector<std::wstring> entries;
    entries.reserve(env.size());
    for (auto const& e : env) entries.push_back(widen(e));
    std::sort(entries.begin(), entries.end(), [](std::wstring const& a, std::wstring const& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (auto const& e : entries) {
        block += e;
        block.push_back(L'\0');
    }
    if (entries.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// One pipe: the parent's end is an overlapped named-pipe server end (not inheritable); the child's end is a
// synchronous client end, inheritable so it can go in the HANDLE_LIST. The name is unique and unguessable,
// the instance count is 1 and FILE_FLAG_FIRST_PIPE_INSTANCE fails if anyone squatted it, so a third party
// cannot be the other end (it could at most make our open fail, which reports an error).
DWORD make_pipe(bool parent_reads, OwnedHandle& parent, OwnedHandle& child) {
    static std::atomic<std::uint64_t> counter{0};
    static std::uint64_t const        salt = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }();
    wchar_t name[128];
    std::swprintf(name, std::size(name), L"\\\\.\\pipe\\agentengine.reactor.%lu.%llu.%llx",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)),
                  static_cast<unsigned long long>(salt));
    DWORD const access = (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
                         FILE_FLAG_FIRST_PIPE_INSTANCE;
    parent.h = CreateNamedPipeW(name, access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (parent.h == INVALID_HANDLE_VALUE) {
        parent.h = nullptr;
        return GetLastError();
    }
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    DWORD const child_access = parent_reads ? (GENERIC_WRITE | FILE_READ_ATTRIBUTES)
                                            : (GENERIC_READ | FILE_WRITE_ATTRIBUTES);
    child.h = CreateFileW(name, child_access, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child.h == INVALID_HANDLE_VALUE) {
        child.h = nullptr;
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD open_null(bool for_write, OwnedHandle& child) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    child.h = CreateFileW(L"NUL", for_write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child.h == INVALID_HANDLE_VALUE) {
        child.h = nullptr;
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

SpawnResult os_failure(char const* what, DWORD err) {
    SpawnResult r;
    r.error    = process_error::os_error;
    r.os_error = static_cast<int>(err);
    r.message  = std::string(what) + " failed: Win32 error " + std::to_string(err);
    return r;
}

SpawnResult platform_spawn(ProcState& st, ProcessSpec& spec) {
    OwnedHandle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    if (job.h == nullptr) return os_failure("CreateJobObjectW", GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
        return os_failure("SetInformationJobObject", GetLastError());
    }

    OwnedHandle parent_in, child_in, parent_out, child_out, parent_err, child_err;
    DWORD       e = spec.stdin_pipe ? make_pipe(false, parent_in, child_in) : open_null(false, child_in);
    if (e != ERROR_SUCCESS) return os_failure("stdin pipe", e);
    e = spec.capture_stdout ? make_pipe(true, parent_out, child_out) : open_null(true, child_out);
    if (e != ERROR_SUCCESS) return os_failure("stdout pipe", e);
    e = spec.capture_stderr ? make_pipe(true, parent_err, child_err) : open_null(true, child_err);
    if (e != ERROR_SUCCESS) return os_failure("stderr pipe", e);

    // Exactly the child's own three handles (G4). The list and the job variable must outlive CreateProcessW:
    // the attribute list stores pointers to them.
    std::array<HANDLE, 3> inherit{child_in.h, child_out.h, child_err.h};
    HANDLE                job_handle = job.h;
    SIZE_T                attr_size  = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
    std::vector<std::byte> attr_storage(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size) == 0) {
        return os_failure("InitializeProcThreadAttributeList", GetLastError());
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } const attr_guard{attrs};
    if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                  inherit.size() * sizeof(HANDLE), nullptr, nullptr) == 0) {
        return os_failure("UpdateProcThreadAttribute(HANDLE_LIST)", GetLastError());
    }
    if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(HANDLE), nullptr,
                                  nullptr) == 0) {
        return os_failure("UpdateProcThreadAttribute(JOB_LIST)", GetLastError());
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = child_in.h;
    si.StartupInfo.hStdOutput = child_out.h;
    si.StartupInfo.hStdError  = child_err.h;
    si.lpAttributeList        = attrs;

    std::vector<std::string> const& argv = spec.argv.empty() ? std::vector<std::string>{spec.program} : spec.argv;
    std::wstring                    cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) cmdline.push_back(L' ');
        cmdline += quote_argument(widen(argv[i]));
    }
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');
    std::wstring const program = widen(spec.program);
    std::wstring const cwd     = widen(spec.cwd);
    std::wstring       env     = spec.inherit_environment ? std::wstring{} : environment_block(spec.env);

    PROCESS_INFORMATION pi{};
    BOOL const          created = CreateProcessW(
        program.c_str(), cmd.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        spec.inherit_environment ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DWORD const create_error = created != 0 ? ERROR_SUCCESS : GetLastError();
    child_in.reset();
    child_out.reset();
    child_err.reset();
    if (created == 0) return os_failure("CreateProcessW", create_error);
    CloseHandle(pi.hThread);
    OwnedHandle process;
    process.h = pi.hProcess;

    HANDLE wait_handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), process.h, GetCurrentProcess(), &wait_handle, SYNCHRONIZE, FALSE, 0) ==
        0) {
        DWORD const err = GetLastError();
        TerminateJobObject(job.h, kKilledExitCode);
        return os_failure("DuplicateHandle", err);
    }

    auto            io = std::make_shared<IoObjects>(st.reactor->io());
    std::error_code ec;
    io->exit_wait.assign(wait_handle, ec);
    if (ec) CloseHandle(wait_handle);
    // A handle is released from its guard only once Asio owns it.
    auto adopt = [&ec](PipeHandle& h, OwnedHandle& owned) {
        if (ec || owned.h == nullptr) return;
        h.assign(owned.h, ec);
        if (!ec) (void)owned.release();
    };
    adopt(io->in, parent_in);
    adopt(io->out, parent_out);
    adopt(io->err, parent_err);
    if (ec) {
        TerminateJobObject(job.h, kKilledExitCode);
        return os_failure("associating the pipes with the reactor", static_cast<DWORD>(ec.value()));
    }
    st.io      = std::move(io);
    st.job     = job.release();
    st.process = process.release();
    SpawnResult ok;
    ok.pid = static_cast<std::int64_t>(pi.dwProcessId);
    return ok;
}

#else  // ---- Linux --------------------------------------------------------------------------------------

int pidfd_of(ProcState const& st) noexcept { return st.io ? st.io->exit_wait.native_handle() : -1; }

void kill_scope(ProcState& st) noexcept {
    if (!st.spawned || st.reaped || st.pid <= 0) return;
    if (!st.exit_observed) st.kill_sent = true;
    // The leader is never reaped before close (WNOWAIT), so this group id cannot have been reused.
    ::kill(-st.pid, SIGKILL);
}

void observe_exit(ProcState& st) noexcept {
    siginfo_t info{};
    if (::waitid(static_cast<idtype_t>(P_PIDFD), static_cast<id_t>(pidfd_of(st)), &info, WEXITED | WNOWAIT) == 0 &&
        info.si_pid != 0) {
        st.exit.known = true;
        if (info.si_code == CLD_EXITED) {
            st.exit.code = info.si_status;
        } else {
            st.exit.signal = info.si_status;
        }
    }
    // else ECHILD: a host that ignores SIGCHLD had the child auto-reaped; the status is unknown.
    st.exit.killed   = st.kill_sent && st.exit.signal == SIGKILL;
    st.exit_observed = true;
}

void try_reap(ProcState& st) noexcept {
    siginfo_t info{};
    int const rc = ::waitid(static_cast<idtype_t>(P_PIDFD), static_cast<id_t>(pidfd_of(st)), &info, WEXITED | WNOHANG);
    if ((rc == 0 && info.si_pid != 0) || (rc != 0 && errno == ECHILD)) st.reaped = true;
}

void release_after_reap(std::shared_ptr<ProcState> const& st) {
    if (st->io) {
        std::error_code ec;
        st->io->exit_wait.close(ec);
        st->io.reset();
    }
    finish_life(st);
}

void close_process(std::shared_ptr<ProcState> const& st, bool for_shutdown) {
    if (!st->closed) {
        st->closed = true;
        kill_scope(*st);
        if (st->io) {
            std::error_code ec;
            st->io->in.close(ec);
            st->io->out.close(ec);
            st->io->err.close(ec);
        }
        st->stdin_open = st->open[0] = st->open[1] = false;
    }
    if (!st->spawned || !st->io) {
        st->io.reset();
        finish_life(st);
        return;
    }
    if (!st->reaped) try_reap(*st);
    if (!st->reaped && for_shutdown) {
        // Bounded: a SIGKILLed child normally dies in microseconds; one stuck in uninterruptible sleep is left
        // a zombie of the host rather than hanging the reactor's shutdown.
        pollfd p{pidfd_of(*st), POLLIN, 0};
        (void)::poll(&p, 1, 2000);
        try_reap(*st);
    }
    if (st->reaped || for_shutdown) {
        release_after_reap(st);
        return;
    }
    if (st->reaping) return;
    // Reap when the killed child is gone; the life op stays pending (so shutdown can still finish it).
    st->reaping = true;
    std::error_code ec;
    st->io->exit_wait.cancel(ec);  // a user's pending exit wait completes `canceled`: the Process is closed
    auto io = st->io;
    io->exit_wait.async_wait(asio::posix::descriptor_base::wait_read, [st, io](std::error_code const& wec) {
        if (wec || st->life_done) return;  // shutdown already released it
        try_reap(*st);
        release_after_reap(st);
    });
}

struct OwnedFd {
    int fd = -1;
    OwnedFd() noexcept = default;
    OwnedFd(OwnedFd const&)            = delete;
    OwnedFd& operator=(OwnedFd const&) = delete;
    ~OwnedFd() { reset(); }
    void reset() noexcept {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    int release() noexcept { return std::exchange(fd, -1); }
};

// Keeps a new descriptor off 0/1/2: a dup2 onto its own number would keep FD_CLOEXEC and the child would lose
// it at exec (a host that closed its own stdin makes the next descriptor 0).
int lift(int fd) noexcept {
    if (fd < 0 || fd > 2) return fd;
    int const moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return moved;
}

int make_pipe(OwnedFd& read_end, OwnedFd& write_end) noexcept {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) return errno;
    read_end.fd  = lift(p[0]);
    write_end.fd = lift(p[1]);
    return (read_end.fd < 0 || write_end.fd < 0) ? errno : 0;
}

int open_null(int flags, OwnedFd& out) noexcept {
    out.fd = lift(::open("/dev/null", flags | O_CLOEXEC));
    return out.fd < 0 ? errno : 0;
}

SpawnResult os_failure(char const* what, int err) {
    SpawnResult r;
    r.error    = process_error::os_error;
    r.os_error = err;
    r.message  = std::string(what) + " failed: " + std::system_category().message(err);
    return r;
}

struct SpawnAttrs {
    posix_spawn_file_actions_t fa{};
    posix_spawnattr_t          attr{};
    bool                       fa_ok = false, attr_ok = false;
    SpawnAttrs()                             = default;
    SpawnAttrs(SpawnAttrs const&)            = delete;
    SpawnAttrs& operator=(SpawnAttrs const&) = delete;
    ~SpawnAttrs() {
        if (fa_ok) posix_spawn_file_actions_destroy(&fa);
        if (attr_ok) posix_spawnattr_destroy(&attr);
    }
};

SpawnResult platform_spawn(ProcState& st, ProcessSpec& spec) {
    OwnedFd parent_in, child_in, parent_out, child_out, parent_err, child_err;
    int     e = spec.stdin_pipe ? make_pipe(child_in, parent_in) : open_null(O_RDONLY, child_in);
    if (e != 0) return os_failure("stdin pipe", e);
    e = spec.capture_stdout ? make_pipe(parent_out, child_out) : open_null(O_WRONLY, child_out);
    if (e != 0) return os_failure("stdout pipe", e);
    e = spec.capture_stderr ? make_pipe(parent_err, child_err) : open_null(O_WRONLY, child_err);
    if (e != 0) return os_failure("stderr pipe", e);

    // Everything the child needs is built here, before the spawn: posix_spawn's child side runs no
    // allocator between the clone and the exec (G4: async-signal-safety while reactor/lane threads run).
    std::vector<char*> argv;
    if (spec.argv.empty()) spec.argv.push_back(spec.program);
    argv.reserve(spec.argv.size() + 1);
    for (auto& a : spec.argv) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char*> envp;
    if (!spec.inherit_environment) {
        envp.reserve(spec.env.size() + 1);
        for (auto& v : spec.env) envp.push_back(v.data());
        envp.push_back(nullptr);
    }

    SpawnAttrs sa;
    if ((e = posix_spawn_file_actions_init(&sa.fa)) != 0) return os_failure("posix_spawn_file_actions_init", e);
    sa.fa_ok = true;
    if ((e = posix_spawnattr_init(&sa.attr)) != 0) return os_failure("posix_spawnattr_init", e);
    sa.attr_ok = true;
    e = posix_spawn_file_actions_adddup2(&sa.fa, child_in.fd, 0);
    if (e == 0) e = posix_spawn_file_actions_adddup2(&sa.fa, child_out.fd, 1);
    if (e == 0) e = posix_spawn_file_actions_adddup2(&sa.fa, child_err.fd, 2);
    if (e == 0 && !spec.cwd.empty()) e = posix_spawn_file_actions_addchdir_np(&sa.fa, spec.cwd.c_str());
    if (e == 0) e = posix_spawn_file_actions_addclosefrom_np(&sa.fa, 3);
    if (e != 0) return os_failure("posix_spawn file actions", e);
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    e = posix_spawnattr_setflags(&sa.attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (e == 0) e = posix_spawnattr_setpgroup(&sa.attr, 0);
    if (e == 0) e = posix_spawnattr_setsigmask(&sa.attr, &none);
    if (e == 0) e = posix_spawnattr_setsigdefault(&sa.attr, &defaults);
    if (e != 0) return os_failure("posix_spawnattr", e);

    char* const* const env = spec.inherit_environment ? environ : envp.data();
    pid_t              pid = -1;
    int                pidfd = -1;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 39)) && \
    !defined(AE_PROCESS_FORCE_PIDFD_OPEN)
    e = pidfd_spawn(&pidfd, spec.program.c_str(), &sa.fa, &sa.attr, argv.data(), env);
    if (e == ENOSYS) {
        SpawnResult r = os_failure("pidfd_spawn", e);
        r.error       = process_error::unsupported;
        return r;
    }
    if (e != 0) return os_failure("pidfd_spawn", e);
    pid = pidfd_getpid(pidfd);
#else
    e = posix_spawn(&pid, spec.program.c_str(), &sa.fa, &sa.attr, argv.data(), env);
    if (e != 0) return os_failure("posix_spawn", e);
    pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pidfd < 0) {
        int const err = errno;
        ::kill(-pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        SpawnResult r = os_failure("pidfd_open", err);
        if (err == ENOSYS) r.error = process_error::unsupported;
        return r;
    }
#endif
    child_in.reset();
    child_out.reset();
    child_err.reset();

    auto            io = std::make_shared<IoObjects>(st.reactor->io());
    std::error_code ec;
    io->exit_wait.assign(pidfd, ec);
    if (ec) {
        ::close(pidfd);
        ::kill(-pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        return os_failure("registering the pidfd", ec.value());
    }
    auto adopt = [&ec](PipeHandle& h, OwnedFd& owned) {
        if (ec || owned.fd < 0) return;
        h.assign(owned.fd, ec);
        if (!ec) (void)owned.release();
    };
    adopt(io->in, parent_in);
    adopt(io->out, parent_out);
    adopt(io->err, parent_err);
    st.pid = pid;
    st.io  = std::move(io);
    if (ec) {
        st.spawned = true;  // so the close below kills and reaps it
        return os_failure("registering the pipes", ec.value());
    }
    SpawnResult ok;
    ok.pid = pid;
    return ok;
}

// The reactor thread blocks SIGPIPE before its first pipe write; the signal a write(2) to a closed pipe raises
// is thread-directed, so it stays pending on this thread and is consumed after the EPIPE.
void block_sigpipe_on_reactor_thread() noexcept {
    thread_local bool blocked = false;  // backend-private, reactor thread only
    if (blocked) return;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    blocked = true;
}

void consume_pending_sigpipe() noexcept {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    timespec const zero{0, 0};
    (void)::sigtimedwait(&set, nullptr, &zero);
}

#endif  // platform

// ---------------------------------------------------------------------------------------------------------
// Operations (reactor thread)
// ---------------------------------------------------------------------------------------------------------

bool has_nul(std::string const& s) noexcept { return s.find('\0') != std::string::npos; }

void do_spawn(std::shared_ptr<ProcState> const& st, std::shared_ptr<SpawnOp> const& op) {
    if (op->cancel_requested() || st->reactor->shutting_down()) {
        op->on_complete(op_status::canceled);
        return;
    }
    auto fail = [&](process_error e, char const* msg) {
        op->result.error   = e;
        op->result.message = msg;
        op->on_complete(op_status::completed);
    };
    if (st->spawn_started) return fail(process_error::already_spawned, "this Process already spawned a child");
    if (st->closed) return fail(process_error::not_running, "the Process was closed");
    st->spawn_started = true;
    ProcessSpec& spec = op->spec;
    bool bad          = spec.program.empty() || has_nul(spec.program) || has_nul(spec.cwd);
    for (auto const& a : spec.argv) bad = bad || has_nul(a);
    for (auto const& v : spec.env) bad = bad || has_nul(v) || v.find('=') == std::string::npos;
    if (bad) {
        return fail(process_error::invalid_spec,
                    "ProcessSpec: empty program, a NUL inside a string, or an env entry without '='");
    }
    try {
        op->result = platform_spawn(*st, spec);
    } catch (std::exception const& ex) {  // allocation failure while building the spawn: a value, not a throw
        op->result.error   = process_error::os_error;
        op->result.message = ex.what();
    }
    if (st->io) st->spawned = true;
    if (op->result.ok()) {
        st->stdin_pipe  = spec.stdin_pipe;
        st->stdin_open  = spec.stdin_pipe;
        st->captured[0] = st->open[0] = spec.capture_stdout;
        st->captured[1] = st->open[1] = spec.capture_stderr;
    } else if (st->spawned) {
        close_process(st, /*for_shutdown=*/false);  // half-registered child: kill and reap it
    }
    op->on_complete(op_status::completed);
}

// Shared precondition of the pipe operations. Returns false (and completes the op) if it cannot start.
template <class Op>
bool pipe_op_admitted(ProcState& st, Op& op, bool captured, bool open, bool busy) {
    if (op.cancel_requested() || st.reactor->shutting_down()) {
        op.on_complete(op_status::canceled);
        return false;
    }
    process_error e = process_error::none;
    if (!st.spawned || st.closed || !st.io) {
        e = process_error::not_running;
    } else if (!captured) {
        e = process_error::not_captured;
    } else if (!open) {
        e = process_error::closed;
    } else if (busy) {
        e = process_error::busy;
    }
    if (e == process_error::none) return true;
    op.result.error = e;
    op.on_complete(op_status::completed);
    return false;
}

template <class Op>
void finish_pipe_op(ProcState& st, std::shared_ptr<Op> const& op, op_status status) {
    st.reactor->remove_pending(op);
    op->backend_state.reset();
    op->on_complete(status);
}

void do_read(std::shared_ptr<ProcState> const& st, process_stream w, std::shared_ptr<PipeReadOp> const& op) {
    std::size_t const i = index_of(w);
    if (!pipe_op_admitted(*st, *op, st->captured[i], st->open[i], st->reading[i])) return;
    st->reading[i] = true;
    auto        io = st->io;
    PipeHandle& h  = io->stream(w);
    op->result.data.resize(std::max<std::size_t>(op->max_bytes, 1));  // op-owned buffer (§6.3 gap 6)
    op->backend_state = std::make_shared<StreamOpState>(io, &h);
    st->reactor->add_pending(op);
    h.async_read_some(asio::buffer(op->result.data), [st, io, i, op](std::error_code const& ec, std::size_t n) {
        st->reading[i] = false;
        op->result.data.resize(n);
        op_status status = op_status::completed;
        if (ec && n == 0) {
            if (is_eof(ec)) {
                op->result.eof = true;
            } else if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                status = op_status::canceled;
            } else {
                op->result.error    = process_error::os_error;
                op->result.os_error = ec.value();
            }
        }
        finish_pipe_op(*st, op, status);
    });
}

void drain_step(std::shared_ptr<ProcState> const& st, std::shared_ptr<IoObjects> const& io, std::size_t i,
                std::shared_ptr<PipeDrainOp> const& op, std::shared_ptr<StreamOpState> const& state) {
    PipeHandle& h = *state->handle;
    h.async_read_some(asio::buffer(state->scratch),
                      [st, io, i, op, state](std::error_code const& ec, std::size_t n) {
                          PipeDrainResult& r = op->result;
                          if (n > 0) {
                              r.total_bytes += n;
                              std::size_t const room = op->cap > r.data.size() ? op->cap - r.data.size() : 0;
                              std::size_t const keep = std::min(room, n);
                              r.data.append(state->scratch.data(), keep);
                              if (keep < n) r.truncated = true;
                          }
                          if (!ec && !state->cancel_requested) {
                              drain_step(st, io, i, op, state);  // keep one read outstanding (§6.3 gap 7)
                              return;
                          }
                          st->reading[i]   = false;
                          op_status status = op_status::completed;
                          if (!ec) {
                              status = op_status::canceled;  // sticky cancel between two steps
                          } else if (is_eof(ec)) {
                              r.eof = true;
                          } else if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                              status = op_status::canceled;
                          } else {
                              r.error    = process_error::os_error;
                              r.os_error = ec.value();
                          }
                          finish_pipe_op(*st, op, status);
                      });
}

void do_drain(std::shared_ptr<ProcState> const& st, process_stream w, std::shared_ptr<PipeDrainOp> const& op) {
    std::size_t const i = index_of(w);
    if (!pipe_op_admitted(*st, *op, st->captured[i], st->open[i], st->reading[i])) return;
    st->reading[i] = true;
    auto io        = st->io;
    auto state     = std::make_shared<StreamOpState>(io, &io->stream(w));
    state->scratch.resize(kDrainChunk);
    op->result.data.reserve(std::min<std::size_t>(op->cap, kDrainChunk));
    op->backend_state = state;
    st->reactor->add_pending(op);
    drain_step(st, io, i, op, state);
}

void write_step(std::shared_ptr<ProcState> const& st, std::shared_ptr<PipeWriteOp> const& op,
                std::shared_ptr<StreamOpState> const& state) {
    PipeWriteResult& r = op->result;
    state->handle->async_write_some(
        asio::buffer(op->data.data() + r.written, op->data.size() - r.written),
        [st, op, state](std::error_code const& ec, std::size_t n) {
            PipeWriteResult& res = op->result;
            res.written += n;
            if (!ec && res.written < op->data.size() && !state->cancel_requested) {
                write_step(st, op, state);
                return;
            }
            st->writing      = false;
            op_status status = op_status::completed;
            if (!ec) {
                if (res.written < op->data.size()) status = op_status::canceled;
            } else if (is_broken_pipe(ec)) {
                res.broken_pipe = true;
#ifndef _WIN32
                consume_pending_sigpipe();
#endif
            } else if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                status = op_status::canceled;
            } else {
                res.error    = process_error::os_error;
                res.os_error = ec.value();
            }
            finish_pipe_op(*st, op, status);
        });
}

void do_write(std::shared_ptr<ProcState> const& st, std::shared_ptr<PipeWriteOp> const& op) {
    if (!pipe_op_admitted(*st, *op, st->stdin_pipe, st->stdin_open, st->writing)) return;
    if (op->data.empty()) {
        op->on_complete(op_status::completed);
        return;
    }
#ifndef _WIN32
    block_sigpipe_on_reactor_thread();
#endif
    st->writing       = true;
    auto io           = st->io;
    auto state        = std::make_shared<StreamOpState>(io, &io->in);
    op->backend_state = state;
    st->reactor->add_pending(op);
    write_step(st, op, state);
}

void do_wait(std::shared_ptr<ProcState> const& st, std::shared_ptr<ExitWaitOp> const& op) {
    if (op->cancel_requested() || st->reactor->shutting_down()) {
        op->on_complete(op_status::canceled);
        return;
    }
    if (st->exit_observed) {  // the status is kept: a later wait completes at once
        op->result.exit = st->exit;
        op->on_complete(op_status::completed);
        return;
    }
    process_error e = process_error::none;
    if (!st->spawned || !st->io || st->closed) {
        e = process_error::not_running;
    } else if (st->waiting) {
        e = process_error::busy;
    }
    if (e != process_error::none) {
        op->result.error = e;
        op->on_complete(op_status::completed);
        return;
    }
    st->waiting       = true;
    auto io           = st->io;
    op->backend_state = std::make_shared<ExitOpState>(io);
    st->reactor->add_pending(op);
    auto handler = [st, io, op](std::error_code const& ec) {
        st->waiting      = false;
        op_status status = op_status::canceled;
        if (!ec) {
            if (!st->exit_observed) observe_exit(*st);
            op->result.exit = st->exit;
            status          = op_status::completed;
        }
        finish_pipe_op(*st, op, status);
    };
#ifdef _WIN32
    io->exit_wait.async_wait(std::move(handler));
#else
    io->exit_wait.async_wait(asio::posix::descriptor_base::wait_read, std::move(handler));
#endif
}

// ---------------------------------------------------------------------------------------------------------

class AsioProcess final : public Process {
public:
    explicit AsioProcess(AsioReactor& r) : reactor_(&r), st_(std::make_shared<ProcState>(r)) {
        auto life        = std::make_shared<LifeOp>();
        life->backend_state = std::make_shared<LifeState>(st_);
        st_->life        = life;
        asio::post(r.io(), [st = st_, life] {
            if (st->reactor->shutting_down()) {  // created after the shutdown sweep: torn down at birth
                st->torn_down.store(true, std::memory_order_release);
                st->closed = st->life_done = true;
                life->backend_state.reset();
                life->on_complete(op_status::canceled);
                return;
            }
            st->reactor->add_pending(life);
        });
    }

    ~AsioProcess() override {
        if (st_->torn_down.load(std::memory_order_acquire)) return;  // the reactor's shutdown closed it
        try {
            asio::post(reactor_->io(), [st = std::move(st_)] { close_process(st, /*for_shutdown=*/false); });
        } catch (...) {  // NOLINT(bugprone-empty-catch): a failed post leaves the job/group to the reactor's shutdown
        }
    }

    AsioProcess(AsioProcess const&)            = delete;
    AsioProcess& operator=(AsioProcess const&) = delete;

    [[nodiscard]] Reactor& reactor() const noexcept override { return *reactor_; }

    void start_spawn(std::shared_ptr<SpawnOp> op) override {
        asio::post(reactor_->io(), [st = st_, op = std::move(op)] { do_spawn(st, op); });
    }
    void start_read(process_stream which, std::shared_ptr<PipeReadOp> op) override {
        asio::post(reactor_->io(), [st = st_, which, op = std::move(op)] { do_read(st, which, op); });
    }
    void start_drain(process_stream which, std::shared_ptr<PipeDrainOp> op) override {
        asio::post(reactor_->io(), [st = st_, which, op = std::move(op)] { do_drain(st, which, op); });
    }
    void start_write_stdin(std::shared_ptr<PipeWriteOp> op) override {
        asio::post(reactor_->io(), [st = st_, op = std::move(op)] { do_write(st, op); });
    }
    void start_wait_exit(std::shared_ptr<ExitWaitOp> op) override {
        asio::post(reactor_->io(), [st = st_, op = std::move(op)] { do_wait(st, op); });
    }
    void close_stdin() override {
        asio::post(reactor_->io(), [st = st_] {
            if (!st->io || !st->stdin_open) return;
            st->stdin_open = false;
            std::error_code ec;
            st->io->in.close(ec);  // a pending write completes `canceled`
        });
    }
    void close_output(process_stream which) override {
        asio::post(reactor_->io(), [st = st_, which] {
            std::size_t const i = index_of(which);
            if (!st->io || !st->open[i]) return;
            st->open[i] = false;
            std::error_code ec;
            st->io->stream(which).close(ec);  // a pending read/drain completes `canceled` with what it had
        });
    }
    void kill() override {
        asio::post(reactor_->io(), [st = st_] {
            if (!st->closed) kill_scope(*st);
        });
    }

private:
    AsioReactor*               reactor_;
    std::shared_ptr<ProcState> st_;
};

}  // namespace
}  // namespace agentengine::pal::asio_backend

namespace agentengine::pal {

std::shared_ptr<Process> make_process(Reactor& reactor) {
    asio_backend::AsioReactor* const r = asio_backend::AsioReactor::from(reactor);
    if (r == nullptr) return nullptr;
    return std::make_shared<asio_backend::AsioProcess>(*r);
}

}  // namespace agentengine::pal
