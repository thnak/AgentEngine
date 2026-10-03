#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 "Processes", §4.4 "Process
// cancellation" and "Deadlines are timers", §2 H-e -- coroutine awaitables over pal/reactor_process.hpp, and
// `rt::run_process`, the whole-child convenience every spawner moves onto in later steps of the ADR.
//
// Every awaitable here follows rt/sleep.hpp exactly (the reference discipline):
//   - capture_parked() first (the only throwing step that can touch the handle), then the stop_callback is
//     registered, then the operation is started -- and nothing in the frame is touched after the start;
//   - the completion (reactor thread) only enqueues the waiter to its home: an ADR-175 block_on home or an
//     ADR-219 host Resumer. A homeless waiter is REFUSED and counted (Reactor::homeless_refusals), never
//     resumed on the reactor thread;
//   - a stop posts a cancel; the waiter is resumed from the operation's own completion and reports what
//     actually happened (a read that finished before the cancel landed returns its bytes);
//   - a frame destroyed while waiting abandons the operation (its completion touches nothing) and cancels it.
// Buffers live in the operation record, never in the frame (§6.3, round-3 gap 6): a result is MOVED out of
// the record in await_resume, after the backend is done with it.
//
// rt::run_process -- concurrency without detached tasks. The reads of stdout and stderr, the stdin write,
// the exit wait and the deadline timers are all reactor operations started from ONE coroutine; their
// completions are joined by a small internal join (process_detail::RunJoin): each completion sets a bit and
// wakes the coroutine at its home, which reacts (exit -> start the drain grace; deadline -> kill; stop ->
// kill) and waits again until every started operation has completed. There is no when_all/task_scope yet
// (§4.5 rule 2, a later step); this join is that rule in miniature -- every started operation is awaited to
// its completion before run_process returns, and a frame destroyed mid-run cancels each one and kills the
// child. The decisions run on the waiter's home, never on the reactor thread (§4.2).
//
// "Finished" is exit AND end-of-stream on both pipes (H-e): the pipes are drained concurrently with the exit
// wait, so a child that writes more than the pipe buffer before exiting completes normally. A pipe that does
// not reach EOF within `exit_drain_grace` of the exit (a grandchild still holding it) gets the job/group
// killed and the pipe closed (`output_incomplete`). Output past `output_cap_bytes` is read and discarded
// (`truncated`), never left in the pipe.
//
// No `failure_class` is involved yet (D6 arrives with the seam conversion): outcomes are plain values.

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_process.hpp"
#include "agentengine/rt/reactor_await.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// One operation's outcome: `canceled` (a stop, or reactor shutdown, before it finished) plus whatever the
// backend reported -- a canceled drain still carries the bytes it had read.
template <class R>
// ae-naming-lint: allow ProcessOpOutcome — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProcessOpOutcome {
    bool canceled = false;
    R    result{};
};

namespace process_detail {

// The shared reactor operation record (rt/reactor_await.hpp), over any pal operation record.
template <class Base>
using AwaitedOp = reactor_detail::AwaitedOp<Base>;
using Canceler  = reactor_detail::Canceler;

// The awaiter shape shared by every single-operation awaitable. `Derived` supplies fill(op) (inputs, may
// throw: runs before anything refers to the handle's registration) and start(process, op).
template <class Base, class Derived>
class OpAwaiter {
public:
    using Result = decltype(std::declval<Base&>().result);

    OpAwaiter(pal::Process& process, std::stop_token stop) noexcept : process_(&process), stop_(std::move(stop)) {}
    OpAwaiter(OpAwaiter const&)            = delete;
    OpAwaiter& operator=(OpAwaiter const&) = delete;

    ~OpAwaiter() {
        if (!op_) return;
        cb_.reset();
        if (op_->abandon()) Canceler{&process_->reactor(), op_}();  // frame destroyed while waiting
    }

    // A stop already requested completes `canceled` without a reactor round trip.
    [[nodiscard]] bool await_ready() const noexcept { return stop_.stop_requested(); }

    void await_suspend(std::coroutine_handle<> h) {
        pal::Reactor& reactor = process_->reactor();
        auto          op      = std::make_shared<AwaitedOp<Base>>(reactor, detail::capture_parked(h));
        static_cast<Derived*>(this)->fill(*op);
        if (stop_.stop_possible()) cb_.emplace(stop_, Canceler{&reactor, op});
        op_ = op;
        try {
            // start() reads the awaiter's inputs before it posts, and touches nothing after.
            static_cast<Derived*>(this)->start(*process_, std::move(op));
        } catch (...) {
            cb_.reset();
            op_.reset();
            throw;
        }
        // Nothing in this frame may be touched from here on: the continuation may already be running.
    }

    [[nodiscard]] ProcessOpOutcome<Result> await_resume() noexcept {
        ProcessOpOutcome<Result> out;
        if (!op_) {
            out.canceled = true;
            return out;
        }
        cb_.reset();
        out.canceled = op_->status() == pal::op_status::canceled;
        out.result   = std::move(op_->result);
        op_.reset();
        return out;
    }

private:
    pal::Process*                               process_;
    std::stop_token                             stop_;
    std::shared_ptr<AwaitedOp<Base>>            op_;
    std::optional<std::stop_callback<Canceler>> cb_;
};

class SpawnAwaiter : public OpAwaiter<pal::SpawnOp, SpawnAwaiter> {
public:
    SpawnAwaiter(pal::Process& p, pal::ProcessSpec spec, std::stop_token stop) noexcept
        : OpAwaiter(p, std::move(stop)), spec_(std::move(spec)) {}
    void fill(pal::SpawnOp& op) { op.spec = std::move(spec_); }
    void start(pal::Process& p, std::shared_ptr<pal::SpawnOp> op) { p.start_spawn(std::move(op)); }

private:
    pal::ProcessSpec spec_;
};

class ReadAwaiter : public OpAwaiter<pal::PipeReadOp, ReadAwaiter> {
public:
    ReadAwaiter(pal::Process& p, pal::process_stream which, std::size_t max_bytes, std::stop_token stop) noexcept
        : OpAwaiter(p, std::move(stop)), which_(which), max_bytes_(max_bytes) {}
    void fill(pal::PipeReadOp& op) noexcept { op.max_bytes = max_bytes_; }
    void start(pal::Process& p, std::shared_ptr<pal::PipeReadOp> op) { p.start_read(which_, std::move(op)); }

private:
    pal::process_stream which_;
    std::size_t         max_bytes_;
};

class DrainAwaiter : public OpAwaiter<pal::PipeDrainOp, DrainAwaiter> {
public:
    DrainAwaiter(pal::Process& p, pal::process_stream which, std::size_t cap, std::stop_token stop) noexcept
        : OpAwaiter(p, std::move(stop)), which_(which), cap_(cap) {}
    void fill(pal::PipeDrainOp& op) noexcept { op.cap = cap_; }
    void start(pal::Process& p, std::shared_ptr<pal::PipeDrainOp> op) { p.start_drain(which_, std::move(op)); }

private:
    pal::process_stream which_;
    std::size_t         cap_;
};

class WriteAwaiter : public OpAwaiter<pal::PipeWriteOp, WriteAwaiter> {
public:
    WriteAwaiter(pal::Process& p, std::string data, std::stop_token stop) noexcept
        : OpAwaiter(p, std::move(stop)), data_(std::move(data)) {}
    void fill(pal::PipeWriteOp& op) noexcept { op.data = std::move(data_); }  // op-owned from here on
    void start(pal::Process& p, std::shared_ptr<pal::PipeWriteOp> op) { p.start_write_stdin(std::move(op)); }

private:
    std::string data_;
};

class ExitAwaiter : public OpAwaiter<pal::ExitWaitOp, ExitAwaiter> {
public:
    ExitAwaiter(pal::Process& p, std::stop_token stop) noexcept : OpAwaiter(p, std::move(stop)) {}
    void fill(pal::ExitWaitOp& /*op*/) noexcept {}
    void start(pal::Process& p, std::shared_ptr<pal::ExitWaitOp> op) { p.start_wait_exit(std::move(op)); }
};

// ---- run_process's join ---------------------------------------------------------------------------------

// Completions of several reactor operations, delivered to ONE waiting coroutine as a bit set. Shared by the
// frame and every child operation (the last to let go frees it).
class JoinState {
public:
    explicit JoinState(pal::Reactor& reactor) noexcept : reactor_(&reactor) {}

    // Reactor thread (a child's on_complete). Only enqueues (§4.2).
    void deliver(unsigned bit) noexcept {
        detail::ParkedResumer to_wake;
        {
            std::lock_guard const lock(m_);
            ready_ |= bit;
            if (!parked_) return;
            to_wake = std::move(*parked_);
            parked_.reset();
        }
        if (!to_wake.homed()) {
            reactor_->note_homeless_refusal();  // never resumed on the reactor thread
            return;
        }
        detail::wake(std::move(to_wake));
    }

    [[nodiscard]] unsigned take() noexcept {
        std::lock_guard const lock(m_);
        return std::exchange(ready_, 0U);
    }

    // False (nothing parked) if a completion is already waiting to be taken.
    [[nodiscard]] bool park(detail::ParkedResumer p) noexcept {
        std::lock_guard const lock(m_);
        if (ready_ != 0) return false;
        ticket_ = p.ticket;
        parked_.emplace(std::move(p));
        return true;
    }

    // The waiting frame is being destroyed.
    void abandon() noexcept {
        {
            std::lock_guard const lock(m_);
            if (parked_) {
                parked_.reset();
                return;
            }
        }
        (void)detail::abandon_woken(ticket_);  // already woken: claim a continuation queued at a host Resumer
    }

private:
    pal::Reactor*                          reactor_;
    std::mutex                             m_;
    unsigned                               ready_ = 0;
    std::optional<detail::ParkedResumer>   parked_;
    std::shared_ptr<detail::ResumerTicket> ticket_;
};

template <class Base>
class JoinChild final : public Base {
public:
    JoinChild(std::shared_ptr<JoinState> join, unsigned bit) noexcept : join_(std::move(join)), bit_(bit) {}
    void on_complete(pal::op_status status) noexcept override {
        status_.store(status, std::memory_order_relaxed);  // published by deliver()'s lock
        join_->deliver(bit_);
    }
    [[nodiscard]] pal::op_status status() const noexcept { return status_.load(std::memory_order_relaxed); }

private:
    std::shared_ptr<JoinState>  join_;
    unsigned                    bit_;
    std::atomic<pal::op_status> status_{pal::op_status::canceled};
};

class JoinNext {
public:
    explicit JoinNext(std::shared_ptr<JoinState> join) noexcept : join_(std::move(join)) {}
    JoinNext(JoinNext const&)            = delete;
    JoinNext& operator=(JoinNext const&) = delete;
    ~JoinNext() {
        if (parked_) join_->abandon();  // frame destroyed while waiting
    }

    [[nodiscard]] bool await_ready() noexcept {
        got_ = join_->take();
        return got_ != 0;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
        detail::ParkedResumer p = detail::capture_parked(h);  // may throw; nothing registered yet
        // A stack copy keeps the join alive through park()'s unlock even if the continuation runs and
        // destroys the frame (and with it this awaiter) before park() returns.
        std::shared_ptr<JoinState> const join = join_;
        parked_                               = true;  // written BEFORE the handle is published
        if (join->park(std::move(p))) return true;   // nothing in this frame is touched from here on
        parked_ = false;
        got_    = join->take();
        return false;
    }

    [[nodiscard]] unsigned await_resume() noexcept {
        if (parked_) {
            parked_ = false;
            got_    = join_->take();
        }
        return got_;
    }

private:
    std::shared_ptr<JoinState> join_;
    unsigned                   got_    = 0;
    bool                       parked_ = false;
};

// Frame-owned: the operations run_process started and has not yet seen complete. Its destructor (a frame
// destroyed mid-run) cancels each of them; their completions then touch only the shared JoinState.
class RunJoin {
public:
    static constexpr unsigned kSlots = 6;

    explicit RunJoin(pal::Reactor& reactor) : reactor_(&reactor), state_(std::make_shared<JoinState>(reactor)) {}
    RunJoin(RunJoin const&)            = delete;
    RunJoin& operator=(RunJoin const&) = delete;
    ~RunJoin() {
        for (unsigned i = 0; i < kSlots; ++i) cancel(1U << i);
    }

    template <class Base>
    [[nodiscard]] std::shared_ptr<JoinChild<Base>> make(unsigned bit) {
        return std::make_shared<JoinChild<Base>>(state_, bit);
    }

    // Marks `bit` outstanding, then starts it; a start that throws is unmarked again.
    template <class Op, class Start>
    void start(unsigned bit, std::shared_ptr<Op> const& op, Start&& start_fn) {
        outstanding_ |= bit;
        ops_[slot(bit)] = op;
        try {
            std::forward<Start>(start_fn)(op);
        } catch (...) {
            outstanding_ &= ~bit;
            ops_[slot(bit)].reset();
            throw;
        }
    }

    void cancel(unsigned bit) noexcept {
        if ((outstanding_ & bit) != 0 && ops_[slot(bit)]) Canceler{reactor_, ops_[slot(bit)]}();
    }

    [[nodiscard]] bool     outstanding(unsigned bit) const noexcept { return (outstanding_ & bit) != 0; }
    [[nodiscard]] bool     any_outstanding() const noexcept { return outstanding_ != 0; }
    [[nodiscard]] JoinNext next() const noexcept { return JoinNext(state_); }
    void                   completed(unsigned got) noexcept { outstanding_ &= ~got; }

private:
    static unsigned slot(unsigned bit) noexcept {
        unsigned i = 0;
        while (i + 1 < kSlots && (bit >> i) != 1U) ++i;
        return i;
    }

    pal::Reactor*                   reactor_;
    std::shared_ptr<JoinState>      state_;
    unsigned                        outstanding_ = 0;
    std::shared_ptr<pal::ReactorOp> ops_[kSlots];
};

// Kill the job/group and close every pipe: what a stop, the deadline and the drain grace all do. Only posts.
inline void kill_and_close(pal::Process& p) {
    p.kill();
    p.close_output(pal::process_stream::out);
    p.close_output(pal::process_stream::err);
    p.close_stdin();
}

// The stop callback of a run. Only posts (§4.2; §7.5: a stop_callback never touches session state).
struct KillOnStop {
    pal::Process* process;
    void operator()() const noexcept {
        try {
            kill_and_close(*process);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a failed post leaves the deadline to end the run
        }
    }
};

}  // namespace process_detail

// ---- single operations ----------------------------------------------------------------------------------

[[nodiscard]] inline process_detail::SpawnAwaiter spawn_process(pal::Process& process, pal::ProcessSpec spec,
                                                                std::stop_token stop = {}) noexcept {
    return process_detail::SpawnAwaiter(process, std::move(spec), std::move(stop));
}

// One chunk (at most `max_bytes`, at least one byte unless at EOF).
[[nodiscard]] inline process_detail::ReadAwaiter read_output(pal::Process& process, pal::process_stream which,
                                                             std::size_t     max_bytes = 64 * 1024,
                                                             std::stop_token stop      = {}) noexcept {
    return process_detail::ReadAwaiter(process, which, max_bytes, std::move(stop));
}

// Everything until EOF, the first `cap` bytes kept.
[[nodiscard]] inline process_detail::DrainAwaiter read_all_output(pal::Process& process, pal::process_stream which,
                                                                  std::size_t     cap,
                                                                  std::stop_token stop = {}) noexcept {
    return process_detail::DrainAwaiter(process, which, cap, std::move(stop));
}

[[nodiscard]] inline process_detail::WriteAwaiter write_stdin(pal::Process& process, std::string data,
                                                              std::stop_token stop = {}) noexcept {
    return process_detail::WriteAwaiter(process, std::move(data), std::move(stop));
}

// Waits for the child's exit (a stop ends the WAIT, `canceled`; it does not kill the child).
[[nodiscard]] inline process_detail::ExitAwaiter wait_exit(pal::Process& process, std::stop_token stop = {}) noexcept {
    return process_detail::ExitAwaiter(process, std::move(stop));
}

// ---- the whole child ------------------------------------------------------------------------------------

// ae-naming-lint: allow ProcessRunLimits — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProcessRunLimits {
    std::size_t               output_cap_bytes = 16u * 1024 * 1024;  // per stream; the rest is read and dropped
    std::chrono::milliseconds wall{5 * 60 * 1000};         // deadline from the call; <= 0: none (not advised)
    std::chrono::milliseconds exit_drain_grace{2000};      // after the exit, how long a pipe may stay open
};

// ae-naming-lint: allow run_end — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class run_end : std::uint8_t {
    exited,        // the child exited on its own (see `exit`)
    spawn_failed,  // see `spawn`
    canceled,      // the stop token (or reactor shutdown) ended it; the job/group was killed
    timed_out,     // the wall deadline ended it; the job/group was killed
};

// ae-naming-lint: allow ProcessRunResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ProcessRunResult {
    run_end          end = run_end::spawn_failed;
    pal::SpawnResult spawn;  // pid on success, the failure otherwise
    pal::ProcessExit exit;
    std::string      stdout_text;
    std::string      stderr_text;
    bool             stdout_truncated  = false;
    bool             stderr_truncated  = false;
    bool             output_incomplete = false;  // a pipe was closed before EOF (stop, deadline, drain grace)
};

// Spawns `spec`, writes `stdin_bytes` (if any; stdin then closes), reads stdout and stderr concurrently with
// the exit wait, and returns when the child has exited and both pipes have ended. A stop or the wall
// deadline kills the job/group, closes the pipes and still waits for the exit, so nothing is left running
// or registered when it returns. Parameters are taken by value (they live in the frame); `reactor` must
// outlive the call -- the run must finish before its reactor is destroyed (§4.5 rule 4's shutdown order: a
// reactor shutdown mid-run completes every operation `canceled`, but the run's follow-up posts would then
// reach a reactor that is going away).
// ae-naming-lint: allow run_process — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
inline task<ProcessRunResult> run_process(pal::Reactor& reactor, pal::ProcessSpec spec, std::stop_token stop = {},
                                          ProcessRunLimits           limits      = {},
                                          std::optional<std::string> stdin_bytes = std::nullopt) {
    using clock               = pal::Reactor::clock;
    constexpr unsigned kOut      = 1;
    constexpr unsigned kErr      = 2;
    constexpr unsigned kIn       = 4;
    constexpr unsigned kExit     = 8;
    constexpr unsigned kDeadline = 16;
    constexpr unsigned kGrace    = 32;

    ProcessRunResult out;
    auto const       deadline = clock::now() + limits.wall;
    if (stdin_bytes) spec.stdin_pipe = true;
    bool const want_out = spec.capture_stdout;
    bool const want_err = spec.capture_stderr;

    std::shared_ptr<pal::Process> const proc = pal::make_process(reactor);
    if (!proc) {
        out.spawn.error   = pal::process_error::unsupported;
        out.spawn.message = "this reactor has no process support";
        co_return out;
    }
    pal::Process& p       = *proc;
    auto          spawned = co_await spawn_process(p, std::move(spec), stop);
    out.spawn             = std::move(spawned.result);
    if (spawned.canceled) {
        out.end = run_end::canceled;
        co_return out;
    }
    if (!out.spawn.ok()) co_return out;  // spawn_failed

    // Registered BEFORE any operation starts (rt/sleep.hpp's order); destroyed before `proc`.
    std::optional<std::stop_callback<process_detail::KillOnStop>> on_stop;
    if (stop.stop_possible()) on_stop.emplace(stop, process_detail::KillOnStop{&p});

    process_detail::RunJoin join(reactor);
    auto out_op  = join.make<pal::PipeDrainOp>(kOut);
    auto err_op  = join.make<pal::PipeDrainOp>(kErr);
    auto in_op   = join.make<pal::PipeWriteOp>(kIn);
    auto exit_op = join.make<pal::ExitWaitOp>(kExit);
    auto dl_op   = join.make<pal::ReactorOp>(kDeadline);
    auto gr_op   = join.make<pal::ReactorOp>(kGrace);
    out_op->cap  = limits.output_cap_bytes;
    err_op->cap  = limits.output_cap_bytes;

    if (want_out) join.start(kOut, out_op, [&p](auto const& op) { p.start_drain(pal::process_stream::out, op); });
    if (want_err) join.start(kErr, err_op, [&p](auto const& op) { p.start_drain(pal::process_stream::err, op); });
    if (stdin_bytes && !stdin_bytes->empty()) {
        in_op->data = std::move(*stdin_bytes);  // op-owned
        join.start(kIn, in_op, [&p](auto const& op) { p.start_write_stdin(op); });
    } else if (stdin_bytes) {
        p.close_stdin();
    }
    join.start(kExit, exit_op, [&p](auto const& op) { p.start_wait_exit(op); });
    if (limits.wall.count() > 0) {
        join.start(kDeadline, dl_op, [&reactor, deadline](auto const& op) { reactor.start_timer(op, deadline); });
    }

    bool timed_out = false;
    bool exited    = false;
    bool finished  = false;
    while (join.any_outstanding()) {
        unsigned const got = co_await join.next();
        join.completed(got);
        if ((got & kIn) != 0) p.close_stdin();  // all written (or the child closed it): EOF for the child
        if ((got & kExit) != 0) {
            exited = true;
            if ((join.outstanding(kOut) || join.outstanding(kErr)) && limits.exit_drain_grace.count() > 0 &&
                exit_op->status() == pal::op_status::completed) {
                auto const grace_end = clock::now() + limits.exit_drain_grace;
                join.start(kGrace, gr_op, [&reactor, grace_end](auto const& op) { reactor.start_timer(op, grace_end); });
            }
        }
        if ((got & kDeadline) != 0 && dl_op->status() == pal::op_status::completed && !finished) {
            timed_out = !exited;  // a deadline after the exit only cuts the drain short
            process_detail::kill_and_close(p);
        }
        if ((got & kGrace) != 0 && gr_op->status() == pal::op_status::completed && !finished) {
            process_detail::kill_and_close(p);  // something still holds a pipe past the exit: end it
        }
        if (!finished && exited && !join.outstanding(kOut) && !join.outstanding(kErr) && !join.outstanding(kIn)) {
            finished = true;
            join.cancel(kDeadline);  // awaited to their completion too: nothing outlives the run
            join.cancel(kGrace);
        }
    }
    on_stop.reset();

    out.exit             = exit_op->result.exit;
    out.stdout_text      = std::move(out_op->result.data);
    out.stderr_text      = std::move(err_op->result.data);
    out.stdout_truncated = out_op->result.truncated;
    out.stderr_truncated = err_op->result.truncated;
    out.output_incomplete = (want_out && !out_op->result.eof) || (want_err && !err_op->result.eof);
    if (timed_out) {
        out.end = run_end::timed_out;
    } else if (exit_op->status() == pal::op_status::canceled || !out.exit.known ||
               (stop.stop_requested() && out.exit.killed)) {
        out.end = run_end::canceled;
    } else {
        out.end = run_end::exited;
    }
    co_return out;
}

}  // namespace agentengine::rt
