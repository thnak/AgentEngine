#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.1/§6.3 "Processes" -- child
// processes on the I/O reactor (pal/reactor.hpp), the primitive every spawner moves onto in later steps --
// with §4.4 "Process cancellation", §2 H-e (pipes are read concurrently with the exit wait, never drained
// after it) and §6.3 "Handle hygiene becomes mandatory" (red-team round 1 G4, gate §8.2 1d).
//
// The seam is std-only, like pal/reactor.hpp. The production implementation is the standalone-Asio backend
// (src/backends/reactor_asio/reactor_process_asio.cpp); `make_process` returns null for any other reactor.
//
// SHAPE. A `Process` is created from a reactor and spawns at most one child. Every operation on it is a
// `ReactorOp` subclass (below) carrying its inputs and its RESULT: the backend fills the result fields,
// then calls `on_complete` -- so a waiter reads what happened (bytes, EOF, exit code), not only
// `op_status`. The same threading contract as timers holds (pal/reactor.hpp): every operation is initiated,
// completed and cancelled on the reactor thread; callers only post; `on_complete` runs exactly once; cancel
// is two-phase and sticky through `Reactor::cancel(op)`; reactor shutdown cancels every pending operation.
//
// BUFFERS ARE OWNED BY THE OPERATION (§6.3, round-3 gap 6): a read fills `PipeReadOp::result.data`, a write
// sends `PipeWriteOp::data` -- members of the refcounted op record, never storage in a coroutine frame, so
// a frame destroyed while the kernel still owns the buffer frees nothing the kernel can write into.
//
// CONTAINMENT. The child is in its own kill scope from creation: a Windows Job Object (KILL_ON_JOB_CLOSE,
// assigned at creation by PROC_THREAD_ATTRIBUTE_JOB_LIST) or, on Linux, its own process group
// (POSIX_SPAWN_SETPGROUP). `kill()` terminates that whole scope -- grandchildren included, unless one leaves
// the group (setsid) on Linux, which a group cannot prevent; that residual is the jail's (cgroups), not
// this primitive's. Destroying the `Process` kills the scope too, so a dropped child never outlives it.
//
// HANDLE HYGIENE (G4). The child receives exactly its own three standard handles and nothing else:
// Windows passes PROC_THREAD_ATTRIBUTE_HANDLE_LIST with exactly those (bInheritHandles=TRUE restricted by
// the list); Linux creates every pipe O_CLOEXEC (pipe2) and additionally closes every descriptor >= 3 in
// the child (posix_spawn_file_actions_addclosefrom_np), so even a descriptor another component of the host
// leaked without O_CLOEXEC does not reach it. A sibling spawned concurrently therefore never holds another
// child's pipe end, and "read until EOF" ends at the child's own exit.
//
// ENVIRONMENT. Explicit: the child sees `ProcessSpec::env` and nothing else unless `inherit_environment`
// asks for the host's (008 §9 G3, I2 -- no ambient authority by default).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/pal/reactor.hpp"

namespace agentengine::pal {

// ae-naming-lint: allow ProcessSpec — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProcessSpec {
    std::string              program;  // path of the executable; never searched on PATH
    std::vector<std::string> argv;     // argv[0] included; empty -> { program }
    std::string              cwd;      // empty: the host process's current directory
    std::vector<std::string> env;      // "NAME=value" -- the child's WHOLE environment ...
    bool inherit_environment = false;  // ... unless this is set: then the host's environment, `env` ignored
    bool stdin_pipe          = false;  // true: a pipe written with start_write_stdin; false: stdin at EOF
    bool capture_stdout      = true;   // false: the null device
    bool capture_stderr      = true;   // false: the null device
};

// ae-naming-lint: allow process_stream — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class process_stream : std::uint8_t { out, err };

// Why an operation could not do its work (it still completes `completed`; `canceled` is only ever a cancel
// request or reactor shutdown). Errors are values (001 §6).
// ae-naming-lint: allow process_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class process_error : std::uint8_t {
    none,
    invalid_spec,     // spawn: empty program, or a NUL inside a string
    already_spawned,  // spawn: one child per Process
    unsupported,      // spawn: the platform primitive is missing (Linux without pidfd, < 5.3) -- reported,
                      // never silently degraded to a racy pid wait
    not_running,      // no successful spawn yet, or the Process was closed
    not_captured,     // read of a stream the spec did not capture / write without stdin_pipe
    busy,             // an operation of the same kind is already pending on that stream / the exit
    closed,           // the stream was closed (close_stdin / close_output) before the operation
    os_error,         // see `os_error` (errno / GetLastError)
};

// ae-naming-lint: allow ProcessExit — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProcessExit {
    bool         known  = false;  // false: the status could not be read (a host that reaps children itself)
    std::int64_t code   = -1;     // exit code (Windows: GetExitCodeProcess; Linux: when it exited normally)
    int          signal = 0;      // Linux: the terminating signal, 0 if it exited normally
    bool         killed = false;  // it ended because kill() (or closing the Process) terminated it
};

// ae-naming-lint: allow SpawnResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct SpawnResult {
    process_error error    = process_error::none;
    int           os_error = 0;
    std::string   message;  // human-readable detail for `error`
    std::int64_t  pid      = 0;
    [[nodiscard]] bool ok() const noexcept { return error == process_error::none; }
};

// ae-naming-lint: allow PipeReadResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct PipeReadResult {
    process_error error    = process_error::none;
    int           os_error = 0;
    std::string   data;         // what this read returned (empty at EOF)
    bool          eof = false;  // the child side is closed: no more data will come
};

// ae-naming-lint: allow PipeDrainResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct PipeDrainResult {
    process_error error    = process_error::none;
    int           os_error = 0;
    std::string   data;               // the first `cap` bytes
    std::uint64_t total_bytes = 0;    // everything read, kept or not
    bool          truncated   = false;  // more than `cap` arrived; the rest was read and discarded
    bool          eof         = false;  // reached end-of-stream (false if canceled / closed first)
};

// ae-naming-lint: allow PipeWriteResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct PipeWriteResult {
    process_error error    = process_error::none;
    int           os_error = 0;
    std::size_t   written     = 0;
    bool          broken_pipe = false;  // the child closed its stdin (no SIGPIPE is raised -- EPIPE is a value)
};

// ae-naming-lint: allow ExitWaitResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ExitWaitResult {
    process_error error = process_error::none;
    ProcessExit   exit;
};

// ---- operation records: inputs set before the start, `result` filled by the backend before on_complete ----

// ae-naming-lint: allow SpawnOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class SpawnOp : public ReactorOp {
public:
    ProcessSpec spec;
    SpawnResult result;
};

// One read of at most `max_bytes` (at least 1 byte or EOF).
// ae-naming-lint: allow PipeReadOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class PipeReadOp : public ReactorOp {
public:
    std::size_t    max_bytes = 64 * 1024;
    PipeReadResult result;
};

// Reads until end-of-stream, keeping the first `cap` bytes and discarding (but still reading) the rest, so
// a child that writes more than the cap never blocks on a full pipe (H-e). The backend re-arms each read
// on the reactor thread; the waiter is woken once, at EOF (§6.3 round-3 gap 7: not once per chunk).
// ae-naming-lint: allow PipeDrainOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class PipeDrainOp : public ReactorOp {
public:
    std::size_t     cap = 16u * 1024 * 1024;
    PipeDrainResult result;
};

// Writes all of `data` to the child's stdin.
// ae-naming-lint: allow PipeWriteOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class PipeWriteOp : public ReactorOp {
public:
    std::string     data;
    PipeWriteResult result;
};

// Completes when the child has exited (the status is kept: a second wait after exit completes at once).
// Canceling the wait does not kill the child.
// ae-naming-lint: allow ExitWaitOp — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class ExitWaitOp : public ReactorOp {
public:
    ExitWaitResult result;
};

// One child process on one reactor. Every member is thread-safe: it only posts to the reactor thread.
// Destroying the Process (the last shared_ptr) posts its close: the job / process group is killed, the
// pipes are closed (pending reads complete), and on Linux the child is reaped asynchronously. Reactor
// shutdown closes every Process the same way. A Process must not be used CONCURRENTLY with its reactor's
// destruction, and nothing but its destructor may be called after it; destroying it after the reactor is gone
// is safe (the shutdown already closed it, so it posts nothing).
// ae-naming-lint: allow Process — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class Process {
public:
    virtual ~Process() = default;
    Process(Process const&)            = delete;
    Process& operator=(Process const&) = delete;

    [[nodiscard]] virtual Reactor& reactor() const noexcept = 0;

    virtual void start_spawn(std::shared_ptr<SpawnOp> op)                             = 0;
    virtual void start_read(process_stream which, std::shared_ptr<PipeReadOp> op)     = 0;
    virtual void start_drain(process_stream which, std::shared_ptr<PipeDrainOp> op)   = 0;
    virtual void start_write_stdin(std::shared_ptr<PipeWriteOp> op)                   = 0;
    virtual void start_wait_exit(std::shared_ptr<ExitWaitOp> op)                      = 0;

    // Closes the parent's end of stdin (the child reads EOF). A pending write completes `canceled`.
    virtual void close_stdin() = 0;
    // Closes the parent's end of stdout/stderr. A pending read/drain completes `canceled` with what it had.
    virtual void close_output(process_stream which) = 0;
    // Terminates the job (Windows: TerminateJobObject) / process group (Linux: SIGKILL to the group).
    // Idempotent; a no-op before spawn and after close. Pipes stay open: readers see EOF once every holder
    // of the write ends is gone.
    virtual void kill() = 0;

protected:
    Process() noexcept = default;
};

// A new, not yet spawned, Process on `reactor`; null if `reactor` has no process support (any reactor but
// the default one, e.g. a test's manual reactor).
[[nodiscard]] std::shared_ptr<Process> make_process(Reactor& reactor);

}  // namespace agentengine::pal
