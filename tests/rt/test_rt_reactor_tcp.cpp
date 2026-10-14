// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md, implementation step 2: TCP streams on
// the I/O reactor -- the seam (pal/reactor_tcp.hpp), its Asio backend (src/backends/reactor_asio/
// reactor_tcp_asio.cpp) and the awaitables (rt/tcp.hpp). Each claim names the ADR rule it proves and the
// mutant that kills it. Every mutant named below was run (2026-10-03, Windows/clang; the ASan ones on Linux/gcc
// 15) and failed the named check; T8's buffer claim is killed only under ASan, by mutant "G+FRAME" (abandon
// does not cancel AND the read buffer lives in the awaiter: heap-use-after-free in recv), while "G" alone
// (abandon does not cancel, op-owned buffer, data landing after the frame died) stays ASan-clean.
//
//   T1  connect + write_all + read_some round trip against a loopback echo peer; every await resumes on the
//       block_on() thread (its home), never on the reactor thread (§4.2). Mutant: wake inline in on_complete
//       -> T1 never finishes (the watchdog fails the run).
//   T2  Under an ADR-219 ScopedResumer a read resumes on the host Resumer's thread.
//   T3  A homeless waiter (raw resume) is REFUSED and counted, never resumed (§4.2). Mutant: drop the refusal.
//   T4  A stop requested mid-read (a read that would block forever) resumes it promptly `canceled`, on its
//       home; the stream is then closed -- the peer sees EOF and a later read reports `closed` (§4.4: a
//       stream whose op was canceled is unusable). Mutants: no stop_callback; no close-on-cancel.
//   T5  Sticky cancel (round-2 M2), deterministically at the seam: an op cancelled BEFORE its start completes
//       `canceled` at once, never touches the socket, and leaves the stream usable. Mutant: ignore the flag.
//   T5b A stop already requested completes `canceled` without starting anything.
//   T6  Two-phase, real outcome (round-1 M1): a read that had completed when its cancel was requested reports
//       `completed` with its bytes -- staged deterministically on the reactor's queue -- and those bytes are
//       really consumed. Mutant: report `canceled` whenever a cancel was requested.
//   T7  Connection refused is a value (`tcp_error::connection_refused`), IPv4 and IPv6; numeric parsing only.
//   T8  A frame destroyed during a pending read: no continuation is ever posted, nothing crashes, the op is
//       cancelled early (the stream closes: the peer sees EOF), and data the peer sends afterwards lands in
//       op-owned memory (§6.3 round-3 gap 6; run under ASan on Linux). Mutant: no cancel on abandon.
//   T8b At the seam: an op whose every user reference is dropped while pending still completes into its own
//       buffer when data arrives.
//   T9  Destroying the reactor completes a pending read `canceled` before joining (§4.5 rule 4), and the
//       stream handle may be dropped after the reactor is gone. Mutants: ignore shutdown in the outcome;
//       post on a dead reactor.
//   T10 wait_readable (zero-byte / readiness, §6.3 G6) then read_some: the wait consumes nothing; at EOF the
//       wait completes and read_some reports `eof`.
//   T11 64 concurrent connections, each an echo round trip from its own thread, on one reactor thread.
//   T12 Descriptor exhaustion is a `resource` error (§6.3 round-2 G-g): EMFILE/ENFILE/WSAEMFILE classify as
//       `too_many_open_files`; on Linux a real connect under a lowered RLIMIT_NOFILE reports it.
//       Mutant: drop the classification.
//   T13 One serial context per stream: a second read-side op while one is outstanding completes `busy`.
//   T14 tcp_write_all moves 8 MiB through write_some to a slow reader, every byte in order.
//   T15 100 racing stops on reads: every one ends `canceled`, promptly.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "agentengine/pal/net.hpp"
#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tcp.hpp"
#include "../support/crt_fail_fast.hpp"

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 10s) {
    auto const end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

std::vector<std::byte> bytes_of(std::string const& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}
std::string string_of(std::vector<std::byte> const& v) {
    return std::string(reinterpret_cast<char const*>(v.data()), v.size());
}

constexpr std::uint64_t kLoopback = 0x7F000001;

// ---- the test's peer: plain non-blocking pal/net.hpp sockets, polled ---------------------------------

bool peer_send(pal::fd_t fd, std::string const& s) {
    std::size_t off = 0;
    auto const  end = std::chrono::steady_clock::now() + 10s;
    while (off < s.size()) {
        auto r = pal::send_some(fd, reinterpret_cast<std::byte const*>(s.data()) + off, s.size() - off);
        if (r) {
            off += *r;
        } else if (r.error() == pal::would_block()) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(1ms);
        } else {
            return false;
        }
    }
    return true;
}

// Reads until `n` bytes, EOF or the limit.
std::string peer_recv(pal::fd_t fd, std::size_t n, std::chrono::milliseconds limit = 10s) {
    std::string out;
    std::byte   buf[4096];
    auto const  end = std::chrono::steady_clock::now() + limit;
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        auto r = pal::recv_some(fd, buf, std::min(sizeof(buf), n - out.size()));
        if (r && *r > 0) {
            out.append(reinterpret_cast<char const*>(buf), *r);
        } else if (r || r.error() != pal::would_block()) {
            break;  // EOF or error
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    return out;
}

// True once the peer observes the connection closed (EOF or reset) within `limit`.
bool peer_sees_close(pal::fd_t fd, std::chrono::milliseconds limit = 5s) {
    std::byte  buf[256];
    auto const end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        auto r = pal::recv_some(fd, buf, sizeof(buf));
        if (r && *r == 0) return true;
        if (!r && r.error() != pal::would_block()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

struct Listener {
    pal::fd_t     fd   = pal::invalid_fd;
    std::uint16_t port = 0;
    Listener() {
        auto l = pal::tcp_listen(kLoopback, 0, 256);
        if (!l) return;
        fd   = *l;
        port = pal::local_port(fd).value_or(0);
    }
    ~Listener() { pal::close_fd(fd); }
    Listener(Listener const&)            = delete;
    Listener& operator=(Listener const&) = delete;

    pal::fd_t accept_within(std::chrono::milliseconds limit = 10s) const {
        auto const end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            auto a = pal::accept_one(fd);
            if (a) return *a;
            std::this_thread::sleep_for(1ms);
        }
        return pal::invalid_fd;
    }
};

// Echoes every byte back, on every accepted connection, from one polling thread.
class EchoServer {
public:
    EchoServer() : thread_([this] { loop(); }) {}
    ~EchoServer() {
        stop_.store(true);
        thread_.join();
    }
    EchoServer(EchoServer const&)            = delete;
    EchoServer& operator=(EchoServer const&) = delete;
    [[nodiscard]] std::uint16_t port() const { return listener_.port; }

private:
    void loop() {
        std::vector<pal::fd_t> conns;
        std::vector<std::byte> buf(65536);
        while (!stop_.load()) {
            bool idle = true;
            for (;;) {
                auto a = pal::accept_one(listener_.fd);
                if (!a) break;
                conns.push_back(*a);
                idle = false;
            }
            for (auto& c : conns) {
                if (c == pal::invalid_fd) continue;
                auto r = pal::recv_some(c, buf.data(), buf.size());
                if (r && *r > 0) {
                    (void)peer_send(c, std::string(reinterpret_cast<char const*>(buf.data()), *r));
                    idle = false;
                } else if (r || r.error() != pal::would_block()) {
                    pal::close_fd(c);
                    c = pal::invalid_fd;
                }
            }
            if (idle) std::this_thread::sleep_for(1ms);
        }
        for (auto c : conns) pal::close_fd(c);
    }

    Listener          listener_;  // declared before thread_: the loop reads it
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

// ---- coroutines: free functions taking state by pointer (never immediately-invoked lambda coroutines with
// captures -- ADR-237 §14, step 1's use-after-free) -------------------------------------------------------

task<TcpResult> connect_to(pal::TcpStream* s, pal::IpAddress a, std::uint16_t port, std::stop_token stop = {}) {
    co_return co_await tcp_connect(*s, a, port, std::move(stop));
}

task<TcpResult> read_some_of(pal::TcpStream* s, std::size_t n, std::stop_token stop = {}) {
    co_return co_await tcp_read_some(*s, n, std::move(stop));
}

task<TcpResult> wait_readable_of(pal::TcpStream* s, std::stop_token stop = {}) {
    co_return co_await tcp_wait_readable(*s, std::move(stop));
}

// A read the test expects to finish, bounded so that a mutant breaking it fails a check instead of hanging the
// run: past `limit` a stop is requested and the read reports `canceled`.
TcpResult bounded_read(pal::TcpStream* s, std::size_t n, std::chrono::milliseconds limit = 3s) {
    std::stop_source        src;
    std::mutex              m;
    std::condition_variable cv;
    bool                    finished = false;
    std::thread             deadline([&] {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, limit, [&] { return finished; })) src.request_stop();
    });
    TcpResult r = block_on(read_some_of(s, n, src.get_token()));
    {
        std::lock_guard lock(m);
        finished = true;
    }
    cv.notify_one();
    deadline.join();
    return r;
}

struct RoundTrip {
    TcpResult                    connect, write;
    std::string                  echoed;
    pal::tcp_error               read_error = pal::tcp_error::none;
    std::vector<std::thread::id> resumed_on;
};

task<RoundTrip> round_trip(pal::TcpStream* s, std::uint16_t port, std::string msg) {
    RoundTrip rt;
    rt.connect = co_await tcp_connect(*s, pal::IpAddress::loopback_v4(), port);
    rt.resumed_on.push_back(std::this_thread::get_id());
    if (!rt.connect.ok()) co_return rt;
    auto const out = bytes_of(msg);
    rt.write       = co_await tcp_write_all(*s, out);
    rt.resumed_on.push_back(std::this_thread::get_id());
    while (rt.echoed.size() < msg.size()) {
        TcpResult r = co_await tcp_read_some(*s, 4096);
        rt.resumed_on.push_back(std::this_thread::get_id());
        if (!r.ok()) {
            rt.read_error = r.error;
            break;
        }
        rt.echoed += string_of(r.data);
    }
    co_return rt;
}

struct Observed {
    TcpResult       result;
    std::thread::id resumed_on{};
};

task<Observed> observe_read(pal::TcpStream* s, std::size_t n, std::stop_token stop = {}) {
    Observed o;
    o.result     = co_await tcp_read_some(*s, n, std::move(stop));
    o.resumed_on = std::this_thread::get_id();
    co_return o;
}

task<void> read_into(pal::TcpStream* s, Observed* out, std::atomic<bool>* finished) {
    *out = co_await observe_read(s, 4096);
    finished->store(true, std::memory_order_release);
}

task<void> read_then_flag(pal::TcpStream* s, std::atomic<bool>* ran) {
    (void)co_await tcp_read_some(*s, 4096);
    ran->store(true, std::memory_order_release);
}

struct WaitThenRead {
    TcpResult wait, read;
};
task<WaitThenRead> wait_then_read(pal::TcpStream* s) {
    WaitThenRead w;
    w.wait = co_await tcp_wait_readable(*s);
    w.read = co_await tcp_read_some(*s, 4096);
    co_return w;
}

task<TcpResult> write_all_of(pal::TcpStream* s, std::vector<std::byte> const* data) {
    co_return co_await tcp_write_all(*s, *data);
}

// ---- a host Resumer with its own thread (as in test_rt_reactor_timer.cpp) ----------------------------
class ThreadResumer final : public Resumer {
public:
    ThreadResumer() : worker_([this] { loop(); }) {}
    ~ThreadResumer() override { stop(); }
    ThreadResumer(ThreadResumer const&)            = delete;
    ThreadResumer& operator=(ThreadResumer const&) = delete;

    void post(ParkedContinuation c) override {
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(c));
            ++posted_;
        }
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
    [[nodiscard]] std::thread::id id() const { return worker_.get_id(); }
    [[nodiscard]] int             posted() {
        std::lock_guard lock(m_);
        return posted_;
    }

private:
    void loop() {
        for (;;) {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            lock.unlock();
            c.resume();
        }
    }

    std::mutex                     m_;
    std::condition_variable        cv_;
    std::deque<ParkedContinuation> q_;
    int                            posted_ = 0;
    bool                           stop_   = false;
    std::thread                    worker_;
};

// ---- seam-level probes -------------------------------------------------------------------------------

struct ProbeTcpOp final : pal::TcpOp {
    std::atomic<bool>  done{false};
    pal::op_status     status = pal::op_status::completed;
    bool               cancel_seen_at_completion = false;
    std::thread::id    where{};
    std::atomic<int>*  order_counter = nullptr;  // optional: completion order
    int                order         = -1;
    pal::Reactor*      cancel_on_complete_reactor = nullptr;  // optional: request a cancel of `cancel_target`
    std::shared_ptr<pal::ReactorOp> cancel_target;
    std::atomic<bool>* also_flag = nullptr;  // optional: set on completion (for ops nobody else references)

    void on_complete(pal::op_status st) noexcept override {
        status                    = st;
        cancel_seen_at_completion = cancel_requested();
        where                     = std::this_thread::get_id();
        if (order_counter != nullptr) order = order_counter->fetch_add(1);
        if (cancel_on_complete_reactor != nullptr) {
            cancel_on_complete_reactor->cancel(cancel_target);  // only posts: allowed on the reactor thread
            cancel_target.reset();
        }
        if (also_flag != nullptr) also_flag->store(true, std::memory_order_release);
        done.store(true, std::memory_order_release);
    }
};

// A timer op whose completion holds the reactor thread until released -- a test-only staging device to fix
// the order of the reactor's queue (it violates "only enqueue" on purpose, briefly, and nothing else runs).
struct BlockerOp final : pal::ReactorOp {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread::id   where{};
    void on_complete(pal::op_status) noexcept override {
        where = std::this_thread::get_id();
        entered.store(true, std::memory_order_release);
        auto const end = std::chrono::steady_clock::now() + 10s;
        while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) {
            std::this_thread::yield();
        }
    }
};

std::thread::id reactor_thread_of(pal::Reactor& r) {
    auto b = std::make_shared<BlockerOp>();
    b->release.store(true);
    r.start_timer(b, pal::Reactor::clock::now());
    (void)wait_until([&] { return b->entered.load(std::memory_order_acquire); });
    std::this_thread::sleep_for(5ms);
    return b->where;
}

std::shared_ptr<pal::TcpBuffer::element_type> buffer_of(std::size_t n) {
    return std::make_shared<std::vector<std::byte>>(n);
}

// A connected client stream plus the accepted peer fd.
struct Connected {
    std::shared_ptr<pal::TcpStream> stream;
    pal::fd_t                       peer = pal::invalid_fd;
    Connected()                      = default;
    Connected(Connected&& o) noexcept : stream(std::move(o.stream)), peer(std::exchange(o.peer, pal::invalid_fd)) {}
    Connected& operator=(Connected&&) = delete;
    ~Connected() { pal::close_fd(peer); }
};

Connected connect_pair(pal::Reactor& r, Listener const& l) {
    Connected c;
    c.stream    = pal::make_tcp_stream(r);
    TcpResult k = block_on(connect_to(c.stream.get(), pal::IpAddress::loopback_v4(), l.port));
    if (k.ok()) c.peer = l.accept_within();
    return c;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(150s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 150 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main
    pal::ensure_winsock();

    auto                  reactor   = pal::make_default_reactor();
    std::thread::id const reactor_t = reactor_thread_of(*reactor);
    check(reactor_t != std::thread::id{} && reactor_t != std::this_thread::get_id(),
          "setup: the reactor runs its own thread");
    Listener listener;
    check(listener.fd != pal::invalid_fd && listener.port != 0, "setup: a loopback listener");
    EchoServer echo;

    // T1 ------------------------------------------------------------------------------------------------
    {
        auto      s  = pal::make_tcp_stream(*reactor);
        RoundTrip rt = block_on(round_trip(s.get(), echo.port(), "hello, reactor"));
        check(rt.connect.ok(), "T1: connect to the loopback echo peer");
        check(rt.write.ok() && rt.write.bytes == 14, "T1: write_all wrote every byte");
        check(rt.echoed == "hello, reactor", "T1: read_some brought the echo back intact");
        bool home = !rt.resumed_on.empty();
        bool off_reactor = true;
        for (auto id : rt.resumed_on) {
            home        = home && id == std::this_thread::get_id();
            off_reactor = off_reactor && id != reactor_t;
        }
        check(home, "T1: every await resumed on the block_on() thread (its home) -- " +
                        std::to_string(rt.resumed_on.size()) + " resumptions");
        check(off_reactor, "T1: none resumed on the reactor thread");
    }

    // T2 ------------------------------------------------------------------------------------------------
    {
        Connected         c = connect_pair(*reactor, listener);
        auto              resumer = std::make_shared<ThreadResumer>();
        std::atomic<bool> finished{false};
        Observed          o;
        auto              t = read_into(c.stream.get(), &o, &finished);
        {
            ScopedResumer scope(resumer);
            t.resume();  // raw drive under a host resumer: parks on the reactor
        }
        (void)peer_send(c.peer, "via resumer");
        check(wait_until([&] { return finished.load(std::memory_order_acquire); }), "T2: the reader finished");
        check(o.result.ok() && string_of(o.result.data) == "via resumer", "T2: it read the peer's bytes");
        check(o.resumed_on == resumer->id(), "T2: resumed on the host Resumer's thread");
        check(o.resumed_on != reactor_t && resumer->posted() >= 1, "T2: through Resumer::post(), not the reactor");
        resumer->stop();
    }

    // T3 ------------------------------------------------------------------------------------------------
    {
        Connected           c      = connect_pair(*reactor, listener);
        std::uint64_t const before = reactor->homeless_refusals();
        std::atomic<bool>   ran{false};
        auto                t = read_then_flag(c.stream.get(), &ran);
        t.resume();  // homeless: no block_on, no ScopedResumer
        (void)peer_send(c.peer, "x");
        check(wait_until([&] { return reactor->homeless_refusals() == before + 1; }, 5s),
              "T3: the homeless wake-up was refused and counted");
        std::this_thread::sleep_for(50ms);
        check(!ran.load(std::memory_order_acquire), "T3: the homeless reader was never resumed");
    }

    // T4 ------------------------------------------------------------------------------------------------
    {
        Connected        c = connect_pair(*reactor, listener);
        std::stop_source  src;
        std::atomic<bool> finished{false};
        std::thread       stopper([&] {
            std::this_thread::sleep_for(50ms);
            src.request_stop();
        });
        // Safety net so a mutant that loses the stop fails this check (as `closed`) instead of hanging the run.
        std::thread net([&finished, s = c.stream] {
            if (!wait_until([&] { return finished.load(); }, 5s)) s->close();
        });
        auto const t0 = std::chrono::steady_clock::now();
        Observed   o  = block_on(observe_read(c.stream.get(), 4096, src.get_token()));
        auto const dt = std::chrono::steady_clock::now() - t0;
        finished.store(true);
        stopper.join();
        check(o.result.error == pal::tcp_error::canceled, "T4: a stop mid-read ends it `canceled`");
        check(dt < 3s, "T4: promptly");
        check(o.resumed_on == std::this_thread::get_id(), "T4: resumed on its home thread");
        check(peer_sees_close(c.peer, 3s), "T4: the canceled stream was closed (the peer sees EOF)");
        TcpResult again = bounded_read(c.stream.get(), 16);
        check(again.error == pal::tcp_error::closed, "T4: a later read on it reports `closed`");
        net.join();
    }

    // T5 ------------------------------------------------------------------------------------------------
    {
        Connected c     = connect_pair(*reactor, listener);
        auto      probe = std::make_shared<ProbeTcpOp>();
        probe->set_buffer(buffer_of(16));
        reactor->cancel(probe);  // before the start: only the sticky flag can carry it
        c.stream->start_read_some(probe);
        check(wait_until([&] { return probe->done.load(std::memory_order_acquire); }, 3s),
              "T5: a read cancelled before its start completes at once (sticky cancel)");
        check(probe->done.load() && probe->status == pal::op_status::canceled &&
                  probe->result().error == pal::tcp_error::canceled && probe->where == reactor_t,
              "T5: ... `canceled`, on the reactor thread");
        (void)peer_send(c.peer, "still open");
        TcpResult r = bounded_read(c.stream.get(), 64);
        check(r.ok() && string_of(r.data) == "still open",
              "T5: a pre-start cancel never touched the socket -- the stream still reads");
    }

    // T5b -----------------------------------------------------------------------------------------------
    {
        Connected        c = connect_pair(*reactor, listener);
        std::stop_source src;
        src.request_stop();
        TcpResult r = block_on(read_some_of(c.stream.get(), 64, src.get_token()));
        check(r.error == pal::tcp_error::canceled, "T5b: an already-requested stop completes `canceled`");
        (void)peer_send(c.peer, "ok");
        TcpResult r2 = bounded_read(c.stream.get(), 64);
        check(r2.ok() && string_of(r2.data) == "ok", "T5b: ... without starting anything (stream still usable)");
    }

    // T6 ------------------------------------------------------------------------------------------------
    {
        Connected a = connect_pair(*reactor, listener);
        Connected b = connect_pair(*reactor, listener);
        (void)peer_send(a.peer, "hello");
        (void)peer_send(b.peer, "b");
        bool const ready = block_on(wait_readable_of(a.stream.get())).ok() &&
                           block_on(wait_readable_of(b.stream.get())).ok();
        check(ready, "T6 setup: both streams have data in the kernel");

        std::atomic<int> order{0};
        auto             op_a = std::make_shared<ProbeTcpOp>();
        op_a->set_buffer(buffer_of(64));
        op_a->order_counter = &order;
        auto op_b = std::make_shared<ProbeTcpOp>();
        op_b->set_buffer(buffer_of(64));
        op_b->order_counter              = &order;
        op_b->cancel_on_complete_reactor = reactor.get();
        op_b->cancel_target              = op_a;  // B's completion requests A's cancel

        // Hold the reactor, queue start(B), start(A), release. The reactor then runs start(B) (B's read
        // completes at once and its handler is queued), start(A) (same), B's handler -- which requests A's
        // cancel, queued behind A's handler -- then A's handler, with A's cancel requested but its bytes read.
        auto blocker = std::make_shared<BlockerOp>();
        reactor->start_timer(blocker, pal::Reactor::clock::now());
        (void)wait_until([&] { return blocker->entered.load(std::memory_order_acquire); });
        b.stream->start_read_some(op_b);
        a.stream->start_read_some(op_a);
        blocker->release.store(true, std::memory_order_release);
        check(wait_until([&] { return op_a->done.load(std::memory_order_acquire) &&
                                      op_b->done.load(std::memory_order_acquire); }, 5s),
              "T6: both reads completed");
        check(op_b->order == 0 && op_a->order == 1 && op_a->cancel_seen_at_completion,
              "T6 staging: A's cancel was requested before A's completion ran");
        check(op_a->status == pal::op_status::completed && op_a->result().error == pal::tcp_error::none,
              "T6: a read that completed before its cancel landed reports `completed` (two-phase, real outcome)");
        check(op_a->result().transferred == 5 &&
                  std::string(reinterpret_cast<char const*>(op_a->buffer()->data()), 5) == "hello",
              "T6: ... with its 5 bytes, in the op's own buffer");
        (void)peer_send(a.peer, "z");
        TcpResult next = bounded_read(a.stream.get(), 64);
        check(next.ok() && string_of(next.data) == "z",
              "T6: those bytes were really consumed, and the stream was not closed");
    }

    // T7 ------------------------------------------------------------------------------------------------
    {
        std::uint16_t dead_port = 0;
        {
            Listener tmp;
            dead_port = tmp.port;
        }  // closed: nobody listens there now
        auto      s = pal::make_tcp_stream(*reactor);
        TcpResult r = block_on(connect_to(s.get(), pal::IpAddress::loopback_v4(), dead_port));
        check(r.error == pal::tcp_error::connection_refused,
              "T7: connect to a closed port is `connection_refused`, as a value (native " +
                  std::to_string(r.native.value()) + ")");
        auto      s6 = pal::make_tcp_stream(*reactor);
        TcpResult r6 = block_on(connect_to(s6.get(), pal::IpAddress::loopback_v6(), dead_port));
        check(r6.error == pal::tcp_error::connection_refused, "T7: ... over IPv6 ::1 too");
        auto const v4 = pal::parse_ip_address("127.0.0.1");
        auto const v6 = pal::parse_ip_address("::1");
        check(v4 && *v4 == pal::IpAddress::loopback_v4() && v6 && *v6 == pal::IpAddress::loopback_v6(),
              "T7: numeric addresses parse");
        check(!pal::parse_ip_address("localhost") && !pal::parse_ip_address("example.com"),
              "T7: a host name is refused (no DNS in this seam, ADR-011)");
    }

    // T8 ------------------------------------------------------------------------------------------------
    {
        Connected c       = connect_pair(*reactor, listener);
        auto      resumer = std::make_shared<ThreadResumer>();
        {
            std::atomic<bool> ran{false};
            auto              t = read_then_flag(c.stream.get(), &ran);
            ScopedResumer     scope(resumer);
            t.resume();
            std::this_thread::sleep_for(20ms);  // the read is pending in the kernel
        }  // frame destroyed while the read is pending
        check(peer_sees_close(c.peer, 3s), "T8: destroying the frame cancelled its read early (stream closed)");
        (void)peer_send(c.peer, std::string(4096, 'q'));  // anything still landing goes to op-owned memory
        std::this_thread::sleep_for(100ms);
        check(resumer->posted() == 0, "T8: no continuation was ever posted for the destroyed frame");
        resumer->stop();
    }

    // T8b -----------------------------------------------------------------------------------------------
    {
        Connected         c = connect_pair(*reactor, listener);
        std::atomic<bool> landed{false};
        {
            auto op = std::make_shared<ProbeTcpOp>();
            op->set_buffer(buffer_of(4096));
            op->also_flag = &landed;
            c.stream->start_read_some(std::move(op));
        }  // every user reference to the op and its buffer is gone; only the backend holds them
        std::this_thread::sleep_for(20ms);
        (void)peer_send(c.peer, std::string(1000, 'w'));
        check(wait_until([&] { return landed.load(std::memory_order_acquire); }, 5s),
              "T8b: a pending read nobody references completes into its own (op-owned) buffer");
    }

    // T9 ------------------------------------------------------------------------------------------------
    {
        auto              doomed = pal::make_default_reactor();
        pal::Reactor*     raw    = doomed.get();  // the waiter must not read the unique_ptr main resets (TSan)
        Connected         c      = connect_pair(*raw, listener);
        std::atomic<bool> done{false};
        std::atomic<bool> reactor_gone{false};
        Observed          o;
        std::thread       waiter([&o, &done, &reactor_gone, s = std::move(c.stream)]() mutable {
            o = block_on(observe_read(s.get(), 4096));
            done.store(true, std::memory_order_release);
            while (!reactor_gone.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
            s.reset();  // the stream outlives its reactor, then is dropped
        });
        std::this_thread::sleep_for(50ms);
        auto const t0 = std::chrono::steady_clock::now();
        doomed.reset();  // shutdown with a pending read
        check(std::chrono::steady_clock::now() - t0 < 5s, "T9: reactor shutdown returned promptly");
        reactor_gone.store(true, std::memory_order_release);
        waiter.join();
        check(done.load() && o.result.error == pal::tcp_error::canceled,
              "T9: the pending read was completed `canceled` by shutdown, not dropped");
        // The waiter dropped its stream after the reactor was destroyed: that this process is still running
        // is the claim (mutant I -- post to the dead io_context -- crashes here).
    }

    // T10 -----------------------------------------------------------------------------------------------
    {
        Connected   c = connect_pair(*reactor, listener);
        std::thread sender([&] {
            std::this_thread::sleep_for(30ms);
            (void)peer_send(c.peer, "ready");
        });
        WaitThenRead w = block_on(wait_then_read(c.stream.get()));
        sender.join();
        check(w.wait.ok() && w.wait.bytes == 0, "T10: wait_readable completes when data arrives, reading nothing");
        check(w.read.ok() && string_of(w.read.data) == "ready", "T10: read_some then gets every byte");
        pal::close_fd(c.peer);
        c.peer        = pal::invalid_fd;
        WaitThenRead e = block_on(wait_then_read(c.stream.get()));
        check(e.wait.ok(), "T10: at the peer's close, wait_readable completes");
        check(e.read.error == pal::tcp_error::eof && e.read.data.empty(), "T10: ... and read_some reports `eof`");
    }

    // T11 -----------------------------------------------------------------------------------------------
    {
        constexpr int            kConns = 64;
        std::atomic<int>         right{0};
        std::vector<std::thread> threads;
        threads.reserve(kConns);
        for (int i = 0; i < kConns; ++i) {
            threads.emplace_back([&, i] {
                auto              s   = pal::make_tcp_stream(*reactor);
                std::string const msg = "conn-" + std::to_string(i) + std::string(static_cast<std::size_t>(i) * 37, '.');
                RoundTrip         rt  = block_on(round_trip(s.get(), echo.port(), msg));
                bool home = true;
                for (auto id : rt.resumed_on) home = home && id == std::this_thread::get_id();
                if (rt.connect.ok() && rt.write.ok() && rt.echoed == msg && home) right.fetch_add(1);
            });
        }
        for (auto& t : threads) t.join();
        check(right.load() == kConns, "T11: 64 concurrent connections on one reactor thread, every echo intact on "
                                      "its own home -- " + std::to_string(right.load()) + "/64");
    }

    // T12 -----------------------------------------------------------------------------------------------
    {
        bool const emfile = pal::classify_tcp_error(std::make_error_code(std::errc::too_many_files_open)) ==
                            pal::tcp_error::too_many_open_files;
        bool const enfile =
            pal::classify_tcp_error(std::make_error_code(std::errc::too_many_files_open_in_system)) ==
            pal::tcp_error::too_many_open_files;
#if defined(_WIN32)
        bool const native = pal::classify_tcp_error(std::error_code(10024, std::system_category())) ==
                            pal::tcp_error::too_many_open_files;  // WSAEMFILE
#else
        bool const native = pal::classify_tcp_error(std::error_code(EMFILE, std::system_category())) ==
                                pal::tcp_error::too_many_open_files &&
                            pal::classify_tcp_error(std::error_code(ENFILE, std::system_category())) ==
                                pal::tcp_error::too_many_open_files;
#endif
        check(emfile && enfile && native, "T12: EMFILE / ENFILE / WSAEMFILE classify as `too_many_open_files`");
        check(pal::is_resource_error(pal::tcp_error::too_many_open_files) &&
                  !pal::is_resource_error(pal::tcp_error::connection_refused),
              "T12: ... which is a `resource` error");
#if !defined(_WIN32)
        {
            ::rlimit saved{};
            (void)::getrlimit(RLIMIT_NOFILE, &saved);
            auto s = pal::make_tcp_stream(*reactor);
            // Nothing else opens descriptors meanwhile: the echo server only polls, and no test runs.
            ::rlimit low = saved;
            low.rlim_cur = 3;
            (void)::setrlimit(RLIMIT_NOFILE, &low);
            TcpResult r = block_on(connect_to(s.get(), pal::IpAddress::loopback_v4(), echo.port()));
            (void)::setrlimit(RLIMIT_NOFILE, &saved);
            check(r.error == pal::tcp_error::too_many_open_files,
                  "T12: a real connect under RLIMIT_NOFILE=3 reports `too_many_open_files` (native " +
                      std::to_string(r.native.value()) + ")");
        }
#endif
    }

    // T13 -----------------------------------------------------------------------------------------------
    {
        Connected c     = connect_pair(*reactor, listener);
        auto      first = std::make_shared<ProbeTcpOp>();
        auto      second = std::make_shared<ProbeTcpOp>();
        second->set_buffer(buffer_of(16));
        c.stream->start_wait_readable(first);
        c.stream->start_read_some(second);
        check(wait_until([&] { return second->done.load(std::memory_order_acquire); }, 3s) &&
                  second->result().error == pal::tcp_error::busy,
              "T13: a second read-side op while one is outstanding completes `busy`");
        check(!first->done.load(), "T13: ... and the first is undisturbed");
        reactor->cancel(first);
        check(wait_until([&] { return first->done.load(std::memory_order_acquire); }, 3s) &&
                  first->status == pal::op_status::canceled,
              "T13: the first is then cancelled at the seam");
    }

    // T14 -----------------------------------------------------------------------------------------------
    {
        Connected              c = connect_pair(*reactor, listener);
        std::vector<std::byte> big(8u << 20);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>((i * 131u) >> 3);
        std::atomic<bool> intact{false};
        std::thread       reader([&] {
            std::string got = peer_recv(c.peer, big.size(), 60s);
            intact.store(got.size() == big.size() && std::memcmp(got.data(), big.data(), big.size()) == 0);
        });
        TcpResult w = block_on(write_all_of(c.stream.get(), &big));
        reader.join();
        check(w.ok() && w.bytes == big.size(), "T14: write_all wrote 8 MiB through write_some");
        check(intact.load(), "T14: the peer received every byte, in order");
    }

    // T15 -----------------------------------------------------------------------------------------------
    {
        int        canceled = 0;
        auto const t0       = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            auto      s = pal::make_tcp_stream(*reactor);
            TcpResult k = block_on(connect_to(s.get(), pal::IpAddress::loopback_v4(), echo.port()));
            if (!k.ok()) continue;
            std::stop_source src;
            std::thread      stopper([&src, i] {
                if (i % 3 == 1) std::this_thread::yield();
                if (i % 3 == 2) std::this_thread::sleep_for(std::chrono::microseconds(50));
                src.request_stop();
            });
            Observed o = block_on(observe_read(s.get(), 64, src.get_token()));
            stopper.join();
            if (o.result.error == pal::tcp_error::canceled) ++canceled;
        }
        check(canceled == 100, "T15: every one of 100 racing stops on a read was honoured -- " +
                                   std::to_string(canceled) + "/100");
        check(std::chrono::steady_clock::now() - t0 < 60s, "T15: promptly");
    }

    reactor.reset();
    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
